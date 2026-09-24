'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const reasons = {
    uncommissioned: 'Bench checks have not been confirmed. Physical output is inhibited.',
    user_disabled: 'You selected OFF. Automatic requests cannot enable the outlet.',
    critical_local_fault: 'A local control or storage fault inhibits output. Inspect USB diagnostics.',
    no_auto_authorization: 'No valid home authorization. Waiting for qualifying location evidence.',
    auto_home_lease: 'A valid home lease authorizes the outlet.',
    timed_override_bypasses_presence: 'A timed override is bypassing vehicle presence and connectivity.',
    minimum_off_dwell: 'Waiting for the minimum continuous OFF interval before energizing.',
    dry_run_output_inhibited: 'The policy requests ON, but dry-run keeps the physical relay OFF.',
    utc_not_ready: 'Waiting for UTC synchronization before accepting location evidence.'
  };
  const errors = {
    login_failed: 'Sign-in failed. Check your local administrator password.',
    login_throttled: 'Sign-in is temporarily throttled. Wait before trying again.',
    authentication_required: 'Your session expired. Sign in again.',
    authentication_or_csrf: 'The session or request could not be verified. Refresh or sign in again.',
    check_requires_auto: 'Tesla polling is paused in DISABLED. Select AUTO to allow a check.',
    connection_not_ready: 'Wait for Wi-Fi and UTC synchronization before checking Tesla.',
    check_throttled: 'A check is already running, or its rate limit/backoff has not expired.',
    timed_on_rejected: 'Timed ON requires commissioning, AUTO selected, and a duration up to 8 hours.',
    usb_commissioning_required: 'Physical-output enablement is available only through the USB bench procedure.',
    usb_recovery_required: 'A local fault or provisioning state requires USB recovery.',
    invalid_settings: 'Settings were rejected. Check the field limits and radius/lease relationships.',
    persistence_failed: 'Settings could not be saved. Output is inhibited; inspect USB diagnostics.',
    off_inhibited_persistence_failed: 'OFF was requested, but saving DISABLED failed. Inspect USB diagnostics.',
    malformed_data: 'Tesla returned unusable data. Existing authorization will not be renewed.',
    reauthorization_required: 'Tesla consent must be renewed through the USB onboarding helper.',
    missing_permission: 'Tesla read-only permissions are missing. Review consent through the helper.',
    billing: 'Tesla rejected billing. Review the developer account before requesting another check.',
    local_request_cap: 'The local request budget is exhausted. Polling cannot renew authorization.',
    rate_limit: 'Tesla rate-limited this request. The controller will respect its backoff.'
  };
  const fields = [
    ['vin', 'Selected VIN', 'text', 'location', null, null, 'Exactly 17 characters; never selected automatically.'],
    ['home_lat', 'Home latitude', 'number', 'location', -90, 90, 'Decimal degrees.'],
    ['home_lon', 'Home longitude', 'number', 'location', -180, 180, 'Decimal degrees.'],
    ['enable_m', 'Enable radius · metres', 'number', 'policy', 10, 9999, 'Must be smaller than the disable radius.'],
    ['disable_m', 'Disable radius · metres', 'number', 'policy', 11, 10000, 'Outside this boundary, valid AWAY evidence clears AUTO.'],
    ['max_age_s', 'Maximum GPS age · seconds', 'number', 'policy', 1, 120, 'Age of the source fix, not the HTTP response.'],
    ['future_s', 'Future tolerance · seconds', 'number', 'policy', 0, 30, 'A tolerated future timestamp adds no lease time.'],
    ['lease_s', 'Authorization lease · seconds', 'number', 'policy', 60, 900, 'Failures and duplicate fixes do not renew it.'],
    ['sleep_s', 'Sleeping-home ceiling · seconds', 'number', 'policy', 60, 86400, 'At least the lease; measured from the last qualifying source fix.'],
    ['poll_s', 'Poll interval · seconds', 'number', 'policy', 60, 3600, 'Default 600. Arrival/departure detection waits for polling.'],
    ['dwell_s', 'Minimum OFF dwell · seconds', 'number', 'policy', 30, 600, 'Never delays turning OFF.'],
    ['daily_cap', 'Daily request cap', 'number', 'policy', 1, 10000, 'All endpoints combined, on the UTC day.'],
    ['monthly_cap', 'Monthly request cap', 'number', 'policy', 1, 310000, 'All endpoints combined, on the UTC calendar month.']
  ];
  let csrf = '', state = null, events = [], receivedAt = 0, dirty = false;
  let epoch = 0, refreshId = 0, pending = false, loginPending = false;
  let eventAvailable = false;
  async function loginMaterial(password, saltHex, iterations) {
    if (!crypto.subtle || !/^[0-9a-f]{32}$/.test(saltHex) || iterations !== 100000)
      throw Error('Trusted HTTPS and valid controller login parameters are required.');
    const secret = new TextEncoder().encode(password);
    if (secret.length < 16 || secret.length > 128) {
      secret.fill(0); throw Error('Password must be 16–128 UTF-8 bytes.');
    }
    const salt = Uint8Array.from(saltHex.match(/../g), pair => parseInt(pair, 16));
    try {
      const key = await crypto.subtle.importKey('raw', secret, 'PBKDF2', false, ['deriveBits']);
      const material = new Uint8Array(await crypto.subtle.deriveBits(
        { name: 'PBKDF2', salt, iterations, hash: 'SHA-256' }, key, 256));
      const hex = Array.from(material, byte => byte.toString(16).padStart(2, '0')).join('');
      material.fill(0); return hex;
    } finally { secret.fill(0); salt.fill(0); }
  }
  for (const [key, label, type, group, min, max, hint] of fields) {
    const row = document.createElement('label');
    row.textContent = label;
    const input = document.createElement('input');
    input.id = 'setting-' + key; input.name = key; input.type = type; input.required = true;
    if (type === 'number') {
      input.min = min; input.max = max; input.step = key.startsWith('home_') ? 'any' : '1';
    } else { input.minLength = 17; input.maxLength = 17; input.pattern = '[A-HJ-NPR-Z0-9]{17}'; }
    const note = document.createElement('small'); note.className = 'field-hint'; note.textContent = hint;
    row.append(input, note); $(group + '-settings').append(row);
  }
  const regionLabel = document.createElement('label'); regionLabel.textContent = 'Tesla region';
  const region = document.createElement('select'); region.id = 'setting-region';
  for (const [value, label] of [['NA', 'North America / APAC'], ['EU', 'Europe / Middle East / Africa']]) {
    const option = document.createElement('option'); option.value = value; option.textContent = label; region.append(option);
  }
  regionLabel.append(region); $('location-settings').append(regionLabel);
  function message(text, error = false) {
    $('message').hidden = !text; $('message').textContent = text;
    $('message').className = error ? 'notice error' : 'notice';
  }
  function duration(seconds) {
    const n = Math.max(0, Math.ceil(seconds));
    if (n >= 3600) return Math.floor(n / 3600) + 'h ' + Math.floor(n % 3600 / 60) + 'm';
    if (n >= 60) return Math.floor(n / 60) + 'm ' + n % 60 + 's';
    return n + 's';
  }
  function elapsed() { return state ? Math.max(0, (performance.now() - receivedAt) / 1000) : 0; }
  function readable(value) { return String(value ?? 'Unavailable').replaceAll('_', ' '); }
  function clearSession() {
    ++epoch; ++refreshId; csrf = ''; state = null; events = []; dirty = false; pending = false;
    $('controls').hidden = true; $('login').hidden = false; $('password').value = '';
    $('connection').textContent = 'Signed out';
    $('settings-form').reset();
    for (const [key] of fields) $('setting-' + key).value = '';
    for (const id of ['status', 'fleet-status', 'usage', 'events', 'readiness']) $(id).replaceChildren();
  }
  async function call(path, body, timeout = 12000) {
    const abort = new AbortController(); const timer = setTimeout(() => abort.abort(), timeout);
    try {
      const r = await fetch(path, {
        method: body === undefined ? 'GET' : 'POST', credentials: 'same-origin', cache: 'no-store', signal: abort.signal,
        headers: body === undefined ? {} : { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf },
        body: body === undefined ? undefined : JSON.stringify(body)
      });
      const data = await r.json();
      if (!r.ok) {
        if (r.status === 401 && path !== '/api/login') clearSession();
        throw Error(errors[data.error] || 'Controller rejected the request (' + readable(data.error) + ').');
      }
      const token = r.headers.get('X-CSRF-Token'); if (token) csrf = token;
      return data;
    } catch (e) {
      if (e.name === 'AbortError' || e instanceof TypeError)
        throw Error('Controller did not respond. The last snapshot may be stale. For a command, refresh to verify its outcome; use USB OFF if needed.');
      throw e;
    } finally { clearTimeout(timer); }
  }
  function facts(id, pairs) {
    $(id).replaceChildren();
    for (const [key, value] of pairs) {
      const dt = document.createElement('dt'), dd = document.createElement('dd');
      dt.textContent = key; dd.textContent = value; $(id).append(dt, dd);
    }
  }
  function loadSettings() {
    if (!state) return;
    for (const [key] of fields) { $('setting-' + key).value = state.settings[key]; $('setting-' + key).setCustomValidity(''); }
    region.value = state.settings.region; $('setting-dry_run').checked = state.settings.dry_run;
    dirty = false; $('settings-dirty').hidden = true;
  }
  function updateButtons() {
    if (!state) return;
    const fault = state.fault || !state.ready;
    $('auto').disabled = pending || fault || state.mode === 'AUTO';
    $('timed').disabled = pending || fault || !state.commissioned || state.mode === 'DISABLED';
    $('check').disabled = pending || fault || state.mode === 'DISABLED' || state.poll_busy || !state.wifi_connected || !state.utc_ready;
    $('save').disabled = pending || fault; $('discard').disabled = pending;
    $('off').disabled = false; // OFF must remain available while another request waits.
    $('dry-help').textContent = state.dry_run ? 'Dry-run is active. Only USB can enable physical output.' : 'Selecting dry-run inhibits physical output. Returning to physical output requires USB.';
  }
  function render() {
    const s = state;
    $('controls').hidden = false; $('login').hidden = true;
    $('connection').textContent = 'Controller reachable'; $('connection').className = 'badge good';
    $('command').textContent = s.gpio_command; $('reason').textContent = reasons[s.reason] || readable(s.reason);
    $('mode').textContent = s.mode;
    $('dry-run').textContent = s.dry_run ? 'Dry-run · relay inhibited' : 'Physical output enabled';
    $('armed').textContent = s.commissioned ? 'Commissioned' : 'Not commissioned';
    $('auto-state').textContent = s.auto_home ? 'HOME authorized' : 'Not authorized';
    $('vehicle').textContent = readable(s.vehicle);
    $('distance').textContent = s.reported_distance_m < 0 ? 'Unavailable' : s.reported_distance_m.toFixed(1) + ' m';
    $('position-note').textContent = 'From configured home · freshness unverified';
    const gpsInvalid = s.fleet_endpoint === 'location' && s.fleet_detail.startsWith('gps_as_of_') && s.error === 'malformed_data';
    $('gps-state').textContent = gpsInvalid ? 'Invalid time' : s.location_age_s >= 0 ? 'Accepted fix' : 'No accepted fix';
    $('gps-note').textContent = gpsInvalid ? 'Reported position cannot renew AUTO.' : s.location_age_s >= 0 ? 'Source age at snapshot: ' + duration(s.location_age_s) : 'Waiting for qualifying source-time evidence.';
    const alert = s.fault ? 'Local fault: ' + readable(s.fault_source) + '. Output is inhibited. Use USB recovery.' :
      s.reauthorization_needed ? 'Tesla reauthorization or permission repair is needed. Use the USB helper; never replay an old token backup.' :
      gpsInvalid ? 'Tesla supplied an invalid GPS timestamp. A position near home alone cannot authorize the outlet.' :
      s.mode === 'TIMED_ON' ? 'Timed ON is active and deliberately bypasses vehicle presence and connectivity.' :
      !s.commissioned ? 'Commissioning is incomplete. Complete the USB-only physical bench checks before arming.' : '';
    $('alert').textContent = alert; $('alert').hidden = !alert;
    $('auto-help').textContent = s.mode === 'AUTO' ? 'AUTO is selected. Output still requires valid evidence and all local checks.' : 'Selecting AUTO permits scheduled Tesla requests. It does not bypass commissioning or dry-run.';
    $('timed-help').textContent = !s.commissioned ? 'Unavailable until USB bench commissioning is complete.' : s.mode === 'DISABLED' ? 'Select AUTO before requesting a timed override.' : 'Default 1 hour; maximum 8 hours. Minimum OFF dwell still applies.';
    facts('status', [
      ['Wi-Fi', s.wifi_connected ? 'Connected · ' + s.rssi_dbm + ' dBm' : 'Disconnected'],
      ['UTC synchronized', s.utc_ready ? 'Yes' : 'No'], ['Uptime at snapshot', duration(s.uptime_s)],
      ['Last accepted poll', s.last_success_uptime_s ? 'At uptime ' + duration(s.last_success_uptime_s) : 'None this boot'],
      ['Last error', s.error === 'none' ? 'None' : errors[s.error] || readable(s.error)],
      ['Control loop maximum gap', s.control_max_gap_ms + ' ms'], ['Free internal memory', Math.round(s.internal_heap_free / 1024) + ' KiB'],
      ['Local fault', s.fault ? readable(s.fault_source) : 'None'], ['Policy desired ON', s.desired_on ? 'Yes' : 'No'],
      ['Accepted-fix distance', s.distance_m < 0 ? 'Unavailable' : s.distance_m.toFixed(1) + ' m']
    ]);
    facts('fleet-status', [
      ['Endpoint / HTTP status', s.fleet_endpoint + ' / ' + s.fleet_http_status], ['Parser detail', s.fleet_detail],
      ['GPS source token', s.gps_source_text || 'Unavailable'], ['Report timestamp (not GPS fix time)', s.report_timestamp_text || 'Unavailable'],
      ['HTTP Date', s.fleet_date || 'Unavailable'], ['Request ID', s.fleet_txid || 'Unavailable'],
      ['API version (not vehicle firmware)', s.api_version < 0 ? 'Unavailable' : s.api_version]
    ]);
    $('usage').replaceChildren();
    ['Status', 'Location', 'Token refresh'].forEach((name, i) => {
      const row = document.createElement('tr');
      for (const value of [name, s.attempts_this_boot[i], s.reserved_today[i], s.reserved_month[i]]) {
        const cell = document.createElement('td'); cell.textContent = value; row.append(cell);
      }
      $('usage').append(row);
    });
    $('budget').textContent = s.reserved_today.reduce((a, b) => a + b, 0) + ' / ' + s.settings.daily_cap + ' reserved today · ' + s.reserved_month.reduce((a, b) => a + b, 0) + ' / ' + s.settings.monthly_cap + ' this month. Reservations are conservative upper bounds.';
    $('cost').textContent = 'Estimated location cost: $' + s.estimated_location_usd.toFixed(3) + ' this month before discounts. Tesla billing remains authoritative.';
    $('readiness').replaceChildren();
    for (const [ok, title, note] of [
      [s.ready && !s.fault, 'Configuration', s.fault ? 'USB recovery required' : 'Profile loaded'],
      [s.wifi_connected && s.utc_ready, 'Connectivity', 'Wi-Fi and synchronized UTC'],
      [s.commissioned, 'Bench checks', 'Physical measurements acknowledged through USB'],
      [!s.dry_run, 'Physical output', s.dry_run ? 'Inhibited by dry-run' : 'Enabled through USB'],
      [s.auto_home, 'AUTO evidence', 'Live, source-anchored HOME authorization']
    ]) {
      const li = document.createElement('li'), mark = document.createElement('strong'), text = document.createElement('span');
      mark.textContent = ok ? 'Ready' : 'Pending'; mark.className = ok ? 'good' : '';
      text.textContent = title + ' — ' + note; li.append(mark, text); $('readiness').append(li);
    }
    $('version').textContent = 'Firmware ' + s.firmware_version + ' · ' + s.sdk_version;
    if (!dirty) loadSettings(); updateButtons(); tick();
  }
  function renderEvents() {
    $('events').replaceChildren(); $('events-empty').hidden = eventAvailable && events.length > 0;
    $('events-empty').textContent = eventAvailable ? 'No decision changes recorded.' : 'Event history unavailable. Refresh local status to retry.';
    for (const e of events) {
      const li = document.createElement('li'), time = document.createElement('time');
      time.textContent = duration(e.uptime_s); time.title = 'Uptime when recorded';
      const command = document.createElement('span'), reason = document.createElement('span');
      command.textContent = e.commanded_on ? 'ON commanded' : 'OFF commanded';
      reason.textContent = reasons[e.reason] || readable(e.reason); li.append(time, command, reason); $('events').append(li);
    }
  }
  function tick() {
    if (!state) return;
    const age = elapsed(), s = state;
    $('updated').textContent = 'Snapshot ' + duration(age) + ' ago. Refresh to verify subsequent changes.';
    $('lease').textContent = s.lease_s > age ? duration(s.lease_s - age) + ' until recorded lease expires' : 'No remaining lease in this snapshot';
    $('override').textContent = s.override_s > age ? duration(s.override_s - age) + ' until recorded override expires' : '';
    $('check-help').textContent = s.mode === 'DISABLED' ? 'Polling paused while DISABLED.' : s.poll_busy ? 'A Tesla request is in progress.' : s.next_poll_s < 0 ? 'Automatic polling is paused. Resolve the error before checking again.' : 'Next scheduled poll in ' + duration(s.next_poll_s - age) + ' (as of this snapshot).';
    $('session').textContent = 'Session: ' + duration(s.session_left_s - age) + ' remaining';
    if (age >= s.session_left_s) { clearSession(); message('Your session expired. Sign in again.'); }
  }
  async function refresh(reset = false) {
    const ticket = ++refreshId, startedEpoch = epoch;
    $('refresh').disabled = true;
    try {
      const next = await call('/api/status');
      if (ticket !== refreshId || startedEpoch !== epoch) return;
      state = next; receivedAt = performance.now(); if (reset) dirty = false; render();
      try {
        const history = await call('/api/events');
        if (ticket !== refreshId || startedEpoch !== epoch) return;
        events = history.events; eventAvailable = true;
      } catch (e) { if (startedEpoch !== epoch) return; events = []; eventAvailable = false; }
      renderEvents();
    } catch (e) {
      if (startedEpoch === epoch) { $('connection').textContent = 'Snapshot may be stale'; $('connection').className = 'badge warning'; }
      throw e;
    } finally { if (ticket === refreshId) $('refresh').disabled = false; }
  }
  function confirmAction(title, text, accept) {
    $('confirm-title').textContent = title; $('confirm-text').textContent = text; $('confirm-accept').textContent = accept;
    return new Promise(resolve => { $('confirmation').addEventListener('close', () => resolve($('confirmation').returnValue === 'accept'), { once: true }); $('confirmation').showModal(); });
  }
  async function action(name, extra = {}, reset = false) {
    if (pending && name !== 'off') return;
    if (name === 'off' && $('confirmation').open) $('confirmation').close('cancel');
    const myEpoch = ++epoch; ++refreshId; pending = true; updateButtons();
    message(name === 'off' ? 'Sending OFF…' : 'Saving request…');
    try {
      await call('/api/action', { action: name, ...extra });
      if (myEpoch !== epoch) return;
      message(name === 'off' ? 'OFF accepted and DISABLED saved.' : name === 'check_now' ? 'Check requested through the shared scheduler. Refresh to see its result.' : 'Request accepted.');
      await new Promise(resolve => setTimeout(resolve, 150));
      if (myEpoch === epoch) await refresh(reset);
    } catch (e) { if (myEpoch === epoch) message(e.message, true); }
    finally { if (myEpoch === epoch) { pending = false; updateButtons(); } }
  }
  $('login-form').addEventListener('submit', async e => {
    e.preventDefault(); if (loginPending) return;
    loginPending = true; $('sign-in').disabled = true; message('Verifying password…');
    try {
      const info = await call('/api/login-info');
      if (![1, 2].includes(info.version) || !/^[0-9a-f]{32}$/.test(info.salt) || info.iterations !== 100000)
        throw Error('Controller login parameters are invalid.');
      let credential;
      if (info.version === 1) {
        message('Updating password verifier on this first sign-in; allow about 10 seconds.');
        credential = { password: $('password').value };
      } else {
        credential = { material: await loginMaterial($('password').value, info.salt, info.iterations) };
      }
      await call('/api/login', credential, 30000);
      credential = null;
      $('password').value = ''; ++epoch; await refresh(true); message('Signed in. Session lasts 15 minutes.');
    } catch (e) { message(e.message, true); }
    finally { $('password').value = ''; loginPending = false; $('sign-in').disabled = false; }
  });
  $('refresh').onclick = () => refresh().catch(e => message(e.message, true));
  $('off').onclick = () => action('off');
  $('confirm-off').onclick = () => action('off');
  $('auto').onclick = async () => {
    const at = epoch;
    if (await confirmAction('Select AUTO?', 'This permits Tesla API requests and automatic authorization when all commissioning and presence checks pass. Fleet API charges may apply.', 'Select AUTO') && at === epoch) action('auto');
  };
  $('timed-form').onsubmit = async e => {
    e.preventDefault(); if (!$('timed-form').reportValidity()) return;
    const seconds = Math.round(Number($('hours').value) * 3600), at = epoch;
    if (await confirmAction('Bypass Tesla presence?', 'Request ON for ' + duration(seconds) + ', even if the vehicle is away or unreachable. OFF or reboot cancels the override. Commissioning and minimum OFF dwell still apply.', 'Start timed ON') && at === epoch) action('timed_on', { seconds });
  };
  $('check').onclick = () => action('check_now');
  $('settings-form').oninput = () => { dirty = true; $('settings-dirty').hidden = false; for (const [key] of fields) $('setting-' + key).setCustomValidity(''); };
  $('discard').onclick = loadSettings;
  $('settings-form').onsubmit = async e => {
    e.preventDefault(); const settings = {};
    for (const [key, , type] of fields) settings[key] = type === 'number' ? Number($('setting-' + key).value) : $('setting-' + key).value.trim();
    $('setting-disable_m').setCustomValidity(settings.disable_m > settings.enable_m ? '' : 'Disable radius must be larger than enable radius.');
    $('setting-sleep_s').setCustomValidity(settings.sleep_s >= settings.lease_s ? '' : 'Sleeping ceiling must be at least the lease duration.');
    if (!$('settings-form').checkValidity()) {
      if ($('policy-settings').querySelector(':invalid')) $('policy-settings').closest('details').open = true;
      $('settings-form').reportValidity(); return;
    }
    settings.region = region.value; settings.dry_run = $('setting-dry_run').checked;
    if (state.dry_run && !settings.dry_run) { message(errors.usb_commissioning_required, true); return; }
    const at = epoch;
    if (await confirmAction('Save settings?', 'This discards prior AUTO evidence and cancels any timed override. Your current mode is retained. New evidence is required before automatic authorization resumes.', 'Save settings') && at === epoch) action('settings', { settings }, true);
  };
  $('export').onclick = () => {
    if (!state) return;
    // Deliberate allowlist: never spread status/settings into a support report.
    const keys = ['mode', 'commissioned', 'dry_run', 'gpio_command', 'reason', 'auto_home', 'lease_s', 'override_s', 'vehicle',
      'wifi_connected', 'utc_ready', 'uptime_s', 'error', 'reauthorization_needed', 'poll_busy', 'fleet_endpoint', 'fleet_http_status',
      'fleet_detail', 'gps_source_text', 'fleet_txid', 'fleet_date', 'fleet_received_utc_s', 'report_timestamp_text', 'api_version',
      'attempts_this_boot', 'reserved_today', 'reserved_month', 'ready', 'fault', 'fault_source', 'control_max_gap_ms', 'internal_heap_free', 'firmware_version', 'sdk_version'];
    const report = { note: 'Redacted controller snapshot; no VIN, coordinates, passwords, tokens, cookies or client ID.', snapshot_age_s: Math.floor(elapsed()), status: {} };
    for (const key of keys) report.status[key] = state[key];
    report.events = events.map(e => ({ uptime_s: e.uptime_s, reason: e.reason, commanded_on: e.commanded_on }));
    const url = URL.createObjectURL(new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' }));
    const a = document.createElement('a'); a.href = url; a.download = 'smart-contactor-diagnostics.json'; a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  $('logout').onclick = async () => {
    const myEpoch = ++epoch; pending = true; updateButtons();
    try { await call('/api/action', { action: 'logout' }); if (myEpoch === epoch) { clearSession(); message('Signed out.'); } }
    catch (e) { pending = false; updateButtons(); message('Sign-out was not confirmed. ' + e.message, true); }
  };
  // Only repaint local countdowns. No background status polling or Tesla requests.
  setInterval(tick, 1000);
  refresh().catch(() => {}); // Restore an existing HttpOnly session, if present.
})();
