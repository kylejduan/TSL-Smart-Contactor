// Browser behavior against synthetic, loopback-only fixtures. No device or Tesla I/O.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const path = require('node:path');
const fs = require('node:fs/promises');

(async () => {
  const server = spawn('python3', [path.join(__dirname, 'server.py')], { stdio: ['ignore', 'pipe', 'inherit'] });
  let browser, origin;
  const results = [];
  try {
    const [line] = await once(server.stdout, 'data'); origin = 'http://127.0.0.1:' + line.toString().trim();
    browser = await chromium.launch({ headless: true });
    const context = await browser.newContext({ acceptDownloads: true });
    const page = await context.newPage(); const errors = [], outside = [];
    page.on('pageerror', e => errors.push(e.message));
    page.on('request', r => { if (!r.url().startsWith(origin) && !r.url().startsWith('blob:')) outside.push(r.url()); });
    const fixture = async data => { const r = await fetch(origin + '/__test', { method: 'POST', body: JSON.stringify(data) }); assert.equal(r.status, 200); };
    const read = async () => (await fetch(origin + '/__test')).json();
    const waitText = async (id, value) => page.waitForFunction(([id, value]) => document.getElementById(id).textContent.includes(value), [id, value]);
    const signIn = async () => {
      await page.locator('#password').fill('synthetic-admin-password');
      await page.locator('#sign-in').click(); await page.locator('#controls').waitFor({ state: 'visible' });
      await page.waitForFunction(() => !document.getElementById('refresh').disabled);
    };
    const reset = async state => { await fixture({ reset: true, state }); await page.goto(origin); await page.locator('#login').waitFor({ state: 'visible' }); await signIn(); };
    const run = async (name, fn) => { await fn(); results.push(name); console.log('PASS ' + name); };
    const refresh = async () => { await page.locator('#refresh').click(); await page.waitForFunction(() => !document.getElementById('refresh').disabled); };
    await run('login failures, commissioning gates, and same-origin CSP', async () => {
      const response = await page.goto(origin); assert.match(response.headers()['content-security-policy'], /script-src 'self'/);
      await page.locator('#password').fill('synthetic-invalid-password'); await page.locator('#sign-in').click();
      await waitText('message', 'Sign-in failed'); assert.equal(await page.locator('#password').inputValue(), '');
      await signIn(); assert.equal(await page.locator('#timed').isDisabled(), true);
      assert.equal(await page.locator('#check').isDisabled(), true);
      assert.equal(await page.locator('#off').isEnabled(), true);
      await waitText('gps-state', 'Invalid time'); await waitText('distance', '2.3 m');
      assert.equal(await page.locator('#events li').count(), 1);
    });
    await run('countdowns never cause background requests', async () => {
      const before = (await read()).requests.length;
      await page.waitForTimeout(2200); assert.equal((await read()).requests.length, before);
      assert.equal(outside.length, 0);
    });
    await run('settings validation and unsaved edits survive refresh', async () => {
      await page.locator('#setting-home_lat').fill('12.5'); await refresh();
      assert.equal(await page.locator('#setting-home_lat').inputValue(), '12.5');
      await page.locator('#setting-home_lat').fill(''); const before = (await read()).requests.length;
      await page.locator('#save').click(); assert.equal((await read()).requests.length, before);
      await page.locator('#discard').click();
      await page.locator('#policy-settings').evaluate(el => el.closest('details').open = true);
      await page.locator('#setting-disable_m').fill('50');
      await page.locator('#policy-settings').evaluate(el => el.closest('details').open = false);
      await page.locator('#save').click();
      assert.match(await page.locator('#setting-disable_m').evaluate(el => el.validationMessage), /larger/);
      assert.equal(await page.locator('#confirmation').isVisible(), false);
      await page.locator('#discard').click(); await page.locator('#setting-dry_run').uncheck();
      await page.locator('#save').click(); await waitText('message', 'USB bench procedure');
      await page.locator('#discard').click();
      await page.locator('#setting-enable_m').fill('110'); await page.locator('#save').click();
      await page.locator('#confirm-accept').click(); await waitText('message', 'accepted');
      await page.waitForFunction(() => document.getElementById('settings-dirty').hidden && !document.getElementById('save').disabled);
      assert.equal((await read()).state.settings.enable_m, 110);
    });
    await run('AUTO and timed override require deliberate confirmation; OFF wins', async () => {
      await reset({ commissioned: true, reason: 'user_disabled' });
      await page.locator('#auto').click(); await page.locator('#confirmation button[value="cancel"]').click();
      assert.equal((await read()).state.mode, 'DISABLED');
      await page.locator('#auto').click(); await page.locator('#confirm-accept').click(); await waitText('mode', 'AUTO');
      await page.waitForFunction(() => !document.getElementById('timed').disabled);
      await page.locator('#hours').fill('0.25'); await page.locator('#timed').click();
      await page.locator('#confirm-accept').click(); await waitText('mode', 'TIMED_ON');
      assert.equal((await read()).state.override_s, 900);
      await page.waitForFunction(() => !document.getElementById('auto').disabled);
      await page.locator('#auto').click();
      await page.locator('#confirm-off').click(); await waitText('mode', 'DISABLED');
      assert.equal((await read()).state.override_s, 0);
    });
    await run('late status cannot overwrite an OFF result', async () => {
      await reset({ mode: 'AUTO', commissioned: true, auto_home: true, lease_s: 900, gpio_command: 'ON commanded', reason: 'auto_home_lease' });
      await fixture({ status_delay: 1 }); await page.locator('#refresh').click();
      await page.waitForTimeout(150); await page.locator('#off').click();
      await waitText('command', 'OFF commanded'); await page.waitForTimeout(1400);
      await waitText('mode', 'DISABLED'); await waitText('command', 'OFF commanded');
    });
    await run('redacted downloads omit identifiers, coordinates, and session data', async () => {
      await reset({}); const downloading = page.waitForEvent('download'); await page.locator('#export').click();
      const download = await downloading; const raw = await fs.readFile(await download.path(), 'utf8');
      const report = JSON.parse(raw); assert.equal(report.status.gps_source_text, '-123456789');
      assert.equal(report.status.fleet_txid, 'synthetic-request-id');
      for (const secret of ['5YJ3E1EA7KF000001', 'home_lat', 'home_lon', 'synthetic-csrf', 'synthetic-admin-password', 'reported_distance_m']) assert.equal(raw.includes(secret), false);
      assert.equal(report.events.length, 1);
    });
    await run('hostile metadata stays text; fault recovery and event errors are visible', async () => {
      await fixture({ state: { fault: true, reason: 'critical_local_fault', fault_source: 'configuration_write', fleet_detail: '<img src=x onerror=alert(1)>', reauthorization_needed: true }, events_fail: true });
      await refresh(); await waitText('alert', 'Local fault');
      assert.equal(await page.locator('#auto').isDisabled(), true); assert.equal(await page.locator('#off').isEnabled(), true);
      assert.equal(await page.locator('#fleet-status img').count(), 0); await waitText('events-empty', 'unavailable');
      await page.locator('#logout').click(); await page.locator('#login').waitFor({ state: 'visible' });
      assert.equal((await read()).state.fault, true); assert.equal(await page.locator('#setting-vin').inputValue(), '');
    });
    await run('session expiry clears private UI and requires another login', async () => {
      await reset({ session_left_s: 1 }); await page.locator('#login').waitFor({ state: 'visible', timeout: 4000 });
      await waitText('message', 'session expired'); assert.equal(await page.locator('#setting-vin').inputValue(), '');
    });
    await run('desktop and mobile layouts fit, with no browser storage or external resources', async () => {
      await reset({});
      for (const width of [1280, 390]) {
        await page.setViewportSize({ width, height: 900 });
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
        if (process.env.TSL_SCREENSHOT_DIR) { await fs.mkdir(process.env.TSL_SCREENSHOT_DIR, { recursive: true }); await page.screenshot({ path: path.join(process.env.TSL_SCREENSHOT_DIR, `dashboard-${width}.png`), fullPage: true }); }
      }
      assert.deepEqual(await page.evaluate(() => [localStorage.length, sessionStorage.length]), [0,0]);
      assert.deepEqual(errors, []); assert.deepEqual(outside, []);
    });
    console.log(`${results.length} browser scenarios passed; all responses synthetic.`);
  } finally {
    if (browser) await browser.close();
    const exited = once(server, 'exit'); server.kill('SIGTERM'); await exited;
    if (origin) { let open = false; try { await fetch(origin); open = true; } catch {} assert.equal(open, false, 'fixture listener must close'); }
  }
})().catch(e => { console.error(e); process.exitCode = 1; });
