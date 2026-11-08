/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

Object.assign(UI.errors, {
  network: 'The station did not answer. Check that this device is still on the same network as the station.',
  timeout: 'The station did not answer in time.',
  bad_json: 'The station could not read the request.',
  invalid_field: 'A value is not accepted',
  confirm_required: 'This action needs a confirmation.',
  bad_request: 'The station refused the request.',
  invalid_image: 'This file is not a valid firmware image for this chip.',
  size_mismatch: 'The upload ended early. Try again.',
  unauthorized: 'Your session ended. Login again.',
  forbidden: 'The station refused the request from this page.',
  wrong_password: 'Wrong password.',
  not_on_setup_ap: 'The first admin password can only be set over the setup hotspot.',
  not_found: 'Not found.',
  nothing_staged: 'No uploaded receiver package is waiting.',
  no_journal: 'There is no receiver update session.',
  no_coredump: 'There is no core dump.',
  nothing_pending: 'No new Wi-Fi settings are waiting (they were confirmed or discarded meanwhile).',
  method_not_allowed: 'The station does not support this action.',
  busy: 'The station is busy with another job. Try again in a moment.',
  journal_open: 'A receiver update session is open. Finish or clear it first.',
  not_waiting: 'The receiver update is still running.',
  app_trial: 'The new app version is still being checked.',
  app_update: 'An app update is running.',
  safe_mode: 'Not possible in safe mode.',
  not_in_safe_mode: 'The station is not in safe mode.',
  receiver_not_streaming: 'The receiver must be streaming first.',
  unsupported_model: 'Not possible with this receiver model.',
  needs_firmware: 'This receiver firmware does not have that command.',
  package_pinned: 'A receiver package is held for an open update session.',
  manifest_changed: 'The offered version changed since this page loaded. Check the new offer and try again.',
  not_offered: 'That version is not offered (any more).',
  rolled_back_ack_required: 'This version was rolled back on this station before.',
  reboot_inhibited: 'A restart is blocked right now',
  settings_unavailable: 'The settings storage is unavailable this boot, so nothing can be saved.',
  no_position_yet: 'There is no surveyed position yet.',
  caster_policy: 'This server setup is not allowed.',
  slot_disabled: 'The slot is not enabled.',
  sha256_mismatch: 'The SHA-256 does not match the uploaded package.',
  package_needed: 'Upload the same receiver package (.pkg) again under Install manually.',
  downgrade: 'That build is older than the receiver\'s current firmware.',
  no_rollback_image: 'There is no previous app version to go back to.',
  admin_already_set: 'An admin password is already set.',
  admin_not_set: 'No admin password is set yet. Set one over the setup hotspot.',
  receiver_maintenance: 'Receiver maintenance is on: nothing is sent to the receiver. End it first.',
  not_in_maintenance: 'Receiver maintenance is not on (it may have ended meanwhile).',
  live_view_closed: 'The sky view is not open (it may have closed meanwhile).',
  length_required: 'The upload needs a known size.',
  too_large: 'Too large for the station.',
  unsupported_media_type: 'The station refused the request type.',
  bad_host: 'Open the station by its IP address or its hostname.',
  not_this_project: 'This image is not firmware for this station.',
  same_image: 'This is the version already running.',
  not_no_receiver: 'The station is not looking for a receiver right now (it found one, or is busy with it).',
  no_receiver: 'No receiver is connected. Connect it, then try again.',
  rate_limited: 'Too many attempts.',
  internal: 'The station hit an internal error',
  not_available_yet: 'Receiver firmware updates are not enabled in this build yet.',
  checks_disabled: 'This build cannot check for updates. Uploading a file still works.',
  too_many_clients: 'Too many pages are open on the station.',
  signal_test_running: 'A signal-group comparison is running. Wait for it to end, or stop it in Receiver settings.',
  signal_test_idle: 'No signal-group comparison is running (it may have ended meanwhile).',
  signal_test_not_deciding: 'The comparison is not waiting for your choice (not yet, or not any more).',
  first_apply_held: 'The receiver is not configured yet. Confirm its configuration on the Receiver page first.',
  autocfg_off: 'The station does not configure the receiver (receiver setup is Manual).',
  identity_unconfirmed: 'The receiver is not identified yet.',
  config_failed: 'The receiver\'s configuration failed. That needs fixing first.',
  position_not_locked: 'Lock the base position first; until then every receiver restart starts a new survey.',
  query_running: 'The receiver is still answering the last question.',
  report_not_failed: 'Refresh is available after a failed Onocoy report check.',
  not_cancellable: 'Too late to cancel: the download has ended, or the station is already installing it.',
});

/* Request fields as the forms label them, for "<label>: <rule>" refusals. */
Object.assign(UI.fieldLabels, {
  name: 'Name', host: 'Host', port: 'Port', mountpoint: 'Mountpoint', username: 'Username', password: 'Password',
  label: 'Dashboard label', trust: 'Certificate check', backoff_profile: 'Retry pace', ntrip_version: 'NTRIP version',
  ssid: 'Network name', ap_password: 'Access point password', hostname: 'Station name', duration_s: 'Duration',
  minutes: 'Duration', pem: 'CA certificate', new: 'New password', current: 'Current password',
  expected_fw: 'Expected firmware', station_id: 'Station ID', com1_baud: 'Receiver link speed',
  wiring: 'Receiver wiring', 'wiring.rx_gpio': 'ESP32 RX pin', 'wiring.tx_gpio': 'ESP32 TX pin', 'wiring.port': 'Receiver port',
  survey_in_s: 'Survey-in time', lat_deg: 'Latitude', lon_deg: 'Longitude', height_m: 'Height',
  signal_group_master: 'Signal group (main antenna)', signal_group_slave: 'Signal group (second antenna)',
  antenna_model: 'Antenna model', antenna_serial: 'Antenna serial number', antenna_setup_id: 'Antenna setup ID',
  elevation_mask_deg: 'Elevation mask', signal_group_um980: 'Signal group (UM980)',
  rtcm: 'RTCM messages', recovery: 'Receiver recovery', step: 'Setup step', tag: 'Tag', level: 'Log level',
  groups: 'Signal groups', phase_s: 'Time per group',
});

const POLICY = {
  second_onocoy: 'Another server here already streams to Onocoy, and Onocoy allows one stream per station.',
  second_rtk2go: 'Another server here already streams to RTK2go. Stream to RTK2go from one server only.',
  dup_stream_ack_required: 'Streaming to Onocoy and to RTK2go or Centipede at the same time needs your OK first.',
};

UI.fieldRule = 'the station did not accept this value';
UI.errorText = (e, t) => {
  if (e.code === 'caster_policy' && POLICY[e.body.reason]) t = POLICY[e.body.reason];
  if (e.code === 'busy' && e.body.reason === 'receiver_session') {
    t = 'The station is working with the receiver right now (configuring or checking it). Offer maintenance again when that ends.';
  }
  if (e.code === 'busy' && e.body.reason === 'wiring_pending') {
    t = 'The receiver wiring changes at the next restart. Restart the station first, then update the receiver.';
  }
  if (e.code === 'busy' && e.body.reason === 'live_view_output') {
    t = 'The sky view has the receiver\'s satellite positions turned on. The station turns them off first (the sky view stays open ' +
      'without them): start maintenance again in a few seconds.';
  }
  if (e.code === 'not_available_yet') t = notEnabled();
  return t;
};

S.rates = {};
UI.sampleStatus = () => sampleRates();
UI.mergeSections.casters = (previous, next) => {
  if (!Array.isArray(previous)) return next;
  const bySlot = {};
  for (const c of previous) bySlot[c.slot] = c;
  for (const c of next) bySlot[c.slot] = c;
  return Object.keys(bySlot).map(Number).sort((a, b) => a - b).map(n => bySlot[n]);
};

/* Output rate per caster from bytes_sent (device uptime as the clock). */
function sampleRates() {
  const st = S.status;
  if (!st || !Array.isArray(st.casters) || st.uptime_s === undefined) return;
  for (const c of st.casters) {
    if (c.bytes_sent === undefined || c.bytes_sent === null) continue;
    let l = S.rates[c.slot] || (S.rates[c.slot] = []);
    const last = l[l.length - 1];
    if (last && (c.bytes_sent < last[1] || st.uptime_s < last[0])) l = S.rates[c.slot] = [];
    if (!last || st.uptime_s > last[0]) l.push([st.uptime_s, c.bytes_sent]);
    while (l.length > 2 && l[0][0] < st.uptime_s - 61) l.shift();
  }
}

/** Bits per second a slot sent over the last `window` seconds (null = not enough data). */
function slotRate(slot, window) {
  const l = S.rates[slot];
  if (!l || l.length < 2) return null;
  const end = l[l.length - 1];
  let start = l[0];
  for (const p of l) {
    if (p[0] >= end[0] - window) {
      start = p;
      break;
    }
  }
  if (end[0] - start[0] < Math.min(window, 3)) return null;
  return ((end[1] - start[1]) * 8) / (end[0] - start[0]);
}

const NAV = [['', 'Status'], ['ntrip', 'NTRIP'], ['receiver', 'Receiver'], ['logs', 'Logs'], ['settings', 'Settings']];
UI.navigation = NAV;
UI.viewAliases = { '': 'dashboard' };
UI.redirect = stationMovedTo;
UI.classifyStatus = (status, admin) => {
  const state = stationState(status, admin);
  return { key: state.key, tone: state.tone, head: state.head, word: stateWord(state) };
};
UI.notices = stationNoticeItems;
UI.pageIdentity = () => stationPageKey();
UI.brandName = status => status.device && status.device.hostname;
UI.titleName = status => status.device ? status.device.hostname : '';
UI.navDot = status => {
  const u = status.update || {};
  return !!(u.app_available || u.gnss_available || u.app_reverted_in_stage1);
};
UI.navDotPage = 'settings';
UI.tick = () => tickOnocoyReportRetry();
UI.event = message => {
  if (message.event === 'receiver_restarted') toast('The receiver restarted.', 'info');
};
UI.commissioning = claim => stationWizard(claim);
UI.setupHint = 'Join the setup hotspot (its name starts with onodiy-).';
UI.scanWarning = 'Streaming pauses for a few seconds while the station scans.';

function stationMovedTo(name, arg) {
  if (name === 'dashboard') return '';
  if (name === 'updates') return 'settings/' + (arg || 'updates');
  if (name === 'diag') {
    return 'logs' + ({ history: '/history', availability: '/history', journal: '/history', logs: '/console' }[(arg || '').split('/')[0]] || '');
  }
  const to = { signals: 'receiver', live: 'receiver/live', system: 'settings/wifi', casters: 'ntrip' }[name];
  return to ? to + (arg ? '/' + arg : '') : null;
}
function bannerKey(u) {
  return [u.app_available ? u.app_version : '', u.gnss_available ? u.gnss_build : '', u.app_reverted_in_stage1 ? u.app_reverted_version : ''].join('|');
}

let laterKey = null; /* fallback when the browser storage is unavailable */

function bannerDismissed(u) {
  const k = bannerKey(u);
  return store.get('later', laterKey) === k;
}

const upMin = s => fmt.dur(Math.ceil(s / 60) * 60);

function stationPageKey() {
  const c = shell.current;
  if (!c) return '';
  const sub = (c.arg || '').split('/')[0];
  if (c.name === 'receiver') return sub === 'settings' ? 'receiver/settings' : 'receiver';
  if (c.name === 'logs') return sub && LOGS_PAGES[sub] ? 'logs/' + sub : 'logs';
  return c.name;
}

function stationNoticeItems(st, page) {
  const items = [];
  const add = (key, tone, title, text, actions, urgent, trial) => items.push({ key: key, tone: tone, title: title, text: text || '',
    actions: actions || null, urgent: !!urgent, trial: !!trial });
  const go = (label, href) => link(label, href, 'btn small');
  const dev = st.device || {};
  const sm = st.safe_mode || {};
  const rcv = st.receiver || {};
  const u = st.update || {};
  const net = st.network || {};

  if (dev.settings_available === false) add('settings', 'bad', 'Settings cannot be saved', 'Restart the station.', go('Restart', '#/settings/restart'), true);
  if (st.health && st.health.factory_reset_refused) {
    add('factory_reset', 'warn', 'Factory reset refused', 'Hold BOOT again after the receiver update.', go('Details', '#/logs'), true);
  }
  /* Settings has the safe-mode box itself, with "Leave safe mode". */
  if (sm.active && page !== 'settings') add('safe_mode', 'bad', 'Safe mode', 'The receiver and NTRIP servers are off.', go('Details', '#/settings/restart'), true);
  /* Settings has maintenance next to the receiver update, with End now. */
  if (rcv.rx_maintenance && page !== 'settings') {
    add('maintenance', 'info', 'Receiver maintenance', rcv.maintenance_left_s ? 'Ends in about ' + upMin(rcv.maintenance_left_s) + '.' : '',
      btn('End now', endMaintenance, 'small'), true);
  }
  if (rcv.first_apply_held && page !== 'receiver') add('held', 'info', 'Receiver not configured yet', '', go('Review and configure', '#/receiver'));
  /* The Receiver page's own panel says it there. */
  if (rcv.uart_closed && !sm.active && page !== 'receiver') {
    add('uart_closed', 'bad', 'Receiver port closed', 'The station keeps trying.', go('Details', '#/logs'), true);
  }
  /* Settings shows a Wi-Fi trial and new settings waiting in its Wi-Fi card, with the buttons. */
  const pend = net.pending_ssid || 'the new network';
  if (page !== 'settings' && net.cred_trial) {
    /* No button: the trial ends by itself, either way. The countdown ticks without a rebuild. */
    add('wifi', 'info', 'Trying ' + pend, '', null, false, true);
  } else if (page !== 'settings' && net.cred_pending) {
    add('wifi', 'warn', net.cred_reverted_why === 'auth_failed' && net.cred_reverted ? pend + ' refused the new password' :
      net.cred_reverted ? 'No address on ' + pend : 'New Wi-Fi settings for ' + pend + ' not confirmed yet', '', go('Wi-Fi', '#/settings/wifi'));
  }
  /* A signal-group comparison running or finished: [tone, title, text, button]; Receiver settings has its card. */
  const sgn = page === 'receiver/settings' ? null : sgTestNotice(st);
  if (sgn) add('signal_test', sgn[0], sgn[1], sgn[2], sgn[3]);
  const j = rcv.journal;
  const inLoader = (st.alerts || []).some(a => a.id === 'receiver_in_loader');
  const session = j === 'waiting' || j === 'unreadable' || j === 'unknown';
  /* Settings shows a waiting receiver update in its session card. */
  if ((session && page !== 'settings') || (inLoader && !session)) {
    add('journal', 'warn', 'Receiver update needs you', !session ? 'The receiver waits in its update loader.' :
      j === 'unreadable' ? 'Its record is damaged.' : 'Keep the station powered.', go('Open', '#/settings/session'), true);
  }
  /* A slot the Onocoy login guard stopped tries nothing until the user acts; the NTRIP page shows it on its card. */
  for (const c of page === 'ntrip' ? [] : st.casters || []) {
    if (c.state === 'stopped') {
      add('onocoy_stopped:' + c.slot, 'bad', (c.name || 'Onocoy') + ' stopped: ' + lbl(L.stop, c.stop_reason || 'none'), '',
        go('What to do', '#/ntrip'), true);
    }
  }
  if (page !== 'settings' && (u.app_available || u.gnss_available || u.app_reverted_in_stage1) && !bannerDismissed(u)) {
    const parts = [];
    if (u.app_available) parts.push('app ' + u.app_version);
    if (u.gnss_available) parts.push('receiver build ' + u.gnss_build);
    add('update', 'info', 'Update available', capFirst(parts.join(' and ')) + (parts.length ? '.' : '') +
      (u.app_reverted_in_stage1 ? (parts.length ? ' ' : '') + u.app_reverted_version + ' was rolled back in its first minute.' : ''), [
      btn('Later', () => {
        laterKey = bannerKey(u);
        store.set('later', laterKey);
        shell.noticeKey = '';
        renderNotices();
      }, 'small ghost'),
      go('Settings page', '#/settings/updates')]);   /* it opens the Updates card; the update starts there */
  }
  const covered = page === '' ? stationState(st, true).notice : page === 'logs' && healthCardShown ? healthNotice(st) : null;
  /* The Status page says a stopped Onocoy slot in its top card; an update shows as the banner there too (its Station
   * card only names the version, easy to miss). */
  const statusSays = i => page === '' && i.key.indexOf('onocoy_stopped:') === 0;
  return items.filter(i => i.key !== covered && !statusSays(i)).sort((a, b) => TONE_ORDER[a.tone] - TONE_ORDER[b.tone]);
}

/* Receiver maintenance in the owner's words (its start dialog in Settings). */
const MAINT_WHAT = 'The station stops sending commands to the receiver while you use a PC tool on its USB port. Streaming continues.';
/* What a configuration by the station saves along with its own settings (the start and end dialogs). */
const MAINT_SAVE_NOTE = 'Changes you make from the PC are saved the next time the station configures the receiver.';
/* The first configuration waits for the user; the receiver port does not open (the Status page's top card). */
const HELD_TEXT = 'The station waits for your OK before it configures the receiver.';
const UART_CLOSED_TEXT = 'The station cannot open its port to the receiver and keeps trying.';
/* A receiver update that needs the user (receiver.journal). */
const JOURNAL_LINE = {
  waiting: 'A receiver update stopped and needs your decision.',
  unreadable: 'A receiver update left a damaged record. Clear it to go on.',
};

async function endMaintenance(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'End receiver maintenance?', ok: 'End now',
    body: [h('p', null, 'The station starts sending commands to the receiver again. Close the PC tool first.'),
      h('p', { class: 'muted small' }, MAINT_SAVE_NOTE)] });
  if (!ok) return false;
  const r = await act(b, () => api('DELETE', '/api/gnss/maintenance'));
  if (r !== undefined) toast('Maintenance ended. The station checks the receiver now.', 'ok');
  return r !== undefined;
}

/** "Leave safe mode" (the Status page's top card, Settings): asks, then restarts normally. */
async function leaveSafeMode(b, err) {
  const ok = await dialog({ title: 'Leave safe mode?', ok: 'Restart normally',
    body: h('p', null, 'The station restarts normally. If the problem is still there, it can crash again and go back to safe mode.') });
  if (!ok) return false;
  const r = await act(b, () => api('POST', '/api/system/safe-mode/clear'), err);
  if (r !== undefined) showRestarting();
  return r !== undefined;
}


UI.forgetSecrets = () => forgetSecrets();
UI.progress = (message, fraction) => stationProgress(message, fraction);
UI.updateState = value => lbl(L.updState, value);
UI.updateResultText = message => resultText(message);
UI.receiverUpdateActive = update => update.gnss_job_active;
UI.appDowntime = () => h('p', { class: 'muted' }, 'The servers reconnect after the restart, which usually takes less than a minute of today\'s Onocoy budget.');
UI.finishChecksText = 'Do this once the station works as before. The new version is kept, receiver updates are ' +
  'possible at once, and the crash watch goes on: if it crashes, the station still goes back by itself.';
