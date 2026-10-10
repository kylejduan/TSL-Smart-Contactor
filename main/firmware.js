'use strict';
// The dashboard supplies its in-memory session and command-order controls.
window.setupFirmware = ui => {
  const $ = id => document.getElementById(id);
  let available = false, uploading = false;
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  function buttons() {
    const s = ui.state();
    $('firmware-install').disabled = !available || uploading || ui.busy() || !s || s.fault || !s.ready || s.mode === 'TIMED_ON';
  }
  function render(s) {
    available = s.available && !s.busy;
    $('firmware-status').textContent = 'Version ' + s.version + ' · ' + s.slot + ' · ' + s.boot_check.replaceAll('_', ' ');
    if (s.build_sha256) $('firmware-status').textContent += ' · Build ' + s.build_sha256.slice(0, 12);
    if (s.busy) $('firmware-status').textContent += ' · Update in progress; output inhibited';
    if (!s.available) $('firmware-status').textContent += ' · Signed USB migration required';
    buttons();
  }
  async function waitFor(id, offset, token, deadline) {
    while (true) {
      if (!ui.current(token)) throw Error('Update canceled by a newer command.');
      if (performance.now() >= deadline) throw Error('Update timed out. Refresh to check the outcome.');
      const s = await ui.call('/api/firmware');
      if (s.state === 'failed') throw Error('Update stopped: ' + s.error.replaceAll('_', ' ') + '.');
      if (s.upload_id !== id) throw Error('Upload session changed. Refresh to check the outcome.');
      if (s.state === 'receiving' && s.received === offset) return s;
      if (s.state === 'rebooting') return s;
      await sleep(75);
    }
  }
  $('firmware-form').onsubmit = async event => {
    event.preventDefault(); if (uploading || ui.busy()) return;
    const file = $('firmware-file').files[0];
    if (!file || file.size < 8192 || file.size > 0x300000 || file.size % 4096) {
      ui.message('Choose a signed application .bin, up to 3 MiB, with a complete signature sector.', true); return;
    }
    const at = ui.epoch();
    if (!await ui.confirm('Install firmware?', 'This temporarily turns the outlet OFF and restarts the controller. AUTO/settings are preserved; timed ON is canceled by reboot. Use a signed application image made with your installation key. Failed uploads leave the current firmware running. A failed startup rolls back. USB recovery may still be necessary.', 'Install and restart') || at !== ui.epoch()) return;
    const generation = ui.state().generation, token = ui.start();
    uploading = true; buttons(); let id = '', committed = false;
    const deadline = performance.now() + 180000;
    try {
      ui.message('Preparing firmware update. The relay is being commanded OFF.');
      await ui.call('/api/firmware', { action: 'begin', bytes: file.size, generation });
      const initial = await ui.call('/api/firmware'); id = initial.upload_id;
      if (!/^[0-9a-f]{32}$/.test(id)) throw Error('No upload session; refresh to check controller state.');
      await waitFor(id, 0, token, deadline);
      for (let offset = 0; offset < file.size; offset += 4096) {
        if (!ui.current(token)) throw Error('Update canceled by a newer command.');
        const chunk = file.slice(offset, Math.min(offset + 4096, file.size));
        const abort = new AbortController(), timer = setTimeout(() => abort.abort(), 10000);
        try {
          const r = await fetch('/api/firmware/chunk', { method: 'POST', credentials: 'same-origin', cache: 'no-store',
            headers: { 'Content-Type': 'application/octet-stream', 'X-CSRF-Token': ui.csrf(), 'X-Upload-ID': id, 'X-Upload-Offset': String(offset) },
            body: chunk, signal: abort.signal });
          if (!r.ok) throw Error('Firmware chunk rejected. Refresh or sign in again.');
        } finally { clearTimeout(timer); }
        await waitFor(id, offset + chunk.size, token, deadline);
        $('firmware-progress').value = (offset + chunk.size) / file.size;
      }
      ui.message('Checking the complete image and signature…');
      await ui.call('/api/firmware', { action: 'finish', upload_id: id });
      await waitFor(id, file.size, token, deadline); committed = true;
      ui.message('Signed image accepted. Controller is restarting. Wait about 15 seconds, reload, sign in and check the version and startup result.');
    } catch (error) {
      if (ui.current(token)) ui.message(error.message + ' No automatic resend. Refresh to check the outcome.', true);
    } finally {
      if (id && !committed) {
        try { await ui.call('/api/firmware', { action: 'abort', upload_id: id }); } catch (_) { /* OFF/timeout also cancels server-side. */ }
      }
      uploading = false; $('firmware-file').value = ''; ui.end(token); buttons();
    }
  };
  return { render, buttons, unavailable() {
    available = false; $('firmware-status').textContent = 'Updater status unavailable. Refresh or use USB recovery.'; buttons();
  } };
};
