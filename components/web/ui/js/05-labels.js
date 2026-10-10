/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Words for the API's enumerations (components/web/API.md, appendix). A value
 * this table does not know is shown verbatim. Entries that are arrays are
 * [short label, explanation].
 */

const L = {
  station: {
    idle: 'Idle',
    detecting: 'Looking for the receiver',
    configuring: 'Configuring the receiver',
    streaming: 'Streaming',
    flashing: 'Updating receiver firmware',
    flash_recovery: 'Recovering a receiver update',
    recovering: 'Recovering',
    no_receiver: 'No receiver found',
  },
  caster: {
    disabled: 'Off',
    wait_network: 'Waiting for the network',
    wait_data: 'Waiting for RTCM',
    connecting: 'Connecting',
    streaming: 'Streaming',
    backoff: 'Waiting to retry',
    auth_failed: 'Login refused',
    paused: 'Paused',
    config_error: 'Configuration error',
    banned: 'Banned',
  },
  fail: {
    none: '',
    dns: 'DNS lookup failed',
    connect: 'Could not connect',
    tls: 'TLS failed',
    timeout: 'Timed out',
    remote_close: 'Closed by the caster',
    write_stall: 'Sending stalled',
    keepalive: 'Connection went silent',
    auth: 'Login refused',
    mount_busy: 'Mountpoint in use',
    config: 'Configuration problem',
    banned: 'Banned by the caster',
  },
  backoff: { fast: 'Fast', standard: 'Standard', polite: 'Polite' },
  trust: { bundle: 'Built-in certificate bundle', custom_ca: 'My own CA certificate', insecure: 'Do not check (insecure)' },
  health: { ok: 'Healthy', degraded: 'Degraded', recovering: 'Recovering', failed: 'Failed' },
  step: {
    none: 'None',
    resync: 'Resync the stream',
    link_restart: 'Restart the receiver link',
    rescan: 'Search the receiver\'s baud rate',
    reconfigure: 'Re-apply the profile',
    receiver_reset: 'Reset the receiver',
    reboot: 'Restart the station',
  },
  halert: {
    power_supply: ['Power supply', 'The station restarted from low voltage since it was powered on. Check the power adapter and cable.'],
    receiver_power: ['Receiver not answering', 'The receiver does not answer commands. Power-cycle the station.'],
    bands: ['Few signal bands', 'Fewer than 4 frequency bands on the best constellation.'],
    profile_drift: ['RTCM differs from the profile', 'The messages or rates in the stream differ from the receiver profile.'],
    heap_fragmented: ['Memory fragmented', 'Free memory is fragmented. A restart clears it.'],
    safe_mode: ['Safe mode', 'Receiver and casters are off after repeated crashes.'],
    settings_degraded: ['Settings unavailable', 'The settings storage is unusable this boot. Nothing is saved.'],
    no_upstream: ['No internet', 'Wi-Fi is connected but the internet cannot be reached.'],
    link_loss: ['Receiver data lost', 'Data from the receiver was lost or corrupted in the last 10 minutes.'],
    power_lost: ['Power was lost', 'The station started from power-off 3 or more times within 24 hours. Check the power adapter, cable and connections.'],
  },
  salert: {
    unsupported_model: ['Receiver model not supported', 'The station forwards what the receiver sends but does not configure or update it.'],
    config_failed: ['Receiver configuration failed', 'The last profile apply failed. The station retries it on its own.'],
    position_mismatch: ['Position mismatch', 'The position in the stream is more than 5 mm away from the locked position.'],
    antenna_moved: ['Antenna moved?', 'The fixed base sends no RTCM after a receiver restart. If the antenna moved, survey again.'],
    receiver_attention: ['Receiver needs attention', 'RTCM did not come back after a receiver update session ended.'],
    receiver_in_loader: ['Receiver in its loader', 'The receiver waits at its loader prompt without an update session. The station only listens until you choose what to do.'],
  },
  upstream: { none: 'Not checked yet', ok: 'Reachable', no_ip: 'No IP address', dns_failed: 'DNS fails', no_upstream: 'Router only, no internet' },
  apReason: { setup: 'first setup', fallback: 'Wi-Fi unreachable', linger: 'closing soon', recovery: 'recovery (BOOT button)' },
  timeSource: { gnss: 'GNSS', sntp: 'internet time', none: 'not set' },
  journal: {
    none: ['No session', ''],
    running: ['Running', 'A receiver update runs. App updates, restarts and receiver changes wait until it ends.'],
    waiting: ['Waiting for you', 'The receiver update stopped and waits for your decision.'],
    unreadable: ['Unreadable', 'The update record is damaged or was written by a newer firmware. Only clearing it is possible.'],
    unknown: ['Unknown', 'The settings could not be read this boot, so the update state is unknown. Nothing is sent to the receiver.'],
  },
  flashPhase: {
    idle: 'Idle', checking: 'Checking the package', entering: 'Putting the receiver into its loader',
    transferring: 'Transferring', committing: 'Committing', finishing: 'Finishing', done: 'Done', failed: 'Failed',
    needs_user: 'Waiting for your decision',
  },
  flashFailure: {
    none: 'None', transport: 'Transfer error', package: 'Package refused', unconfirmed: 'Not confirmed by the receiver',
    no_answer: 'Receiver did not answer', app_not_starting: 'Receiver firmware does not start', entry: 'Could not enter the loader',
    journal: 'Update record problem',
  },
  updState: {
    idle: 'Idle', checking: 'Checking', downloading_app: 'Downloading the app', downloading_gnss: 'Downloading the receiver package',
    reboot_pending: 'Restart pending', failed: 'Failed', uploading_app: 'Receiving an app image',
  },
  trial: {
    none: 'None', confirming: 'Confirming (first minute)', verifying: 'Verifying (first 10 minutes)', watching: 'Watching for crashes',
  },
  gnssBlock: {
    none: '',
    app_trial: 'The new app version is still on trial',
    journal_open: 'A receiver update session is open',
    unsupported_model: 'This receiver model is not supported',
    safe_mode: 'The station is in safe mode',
    app_update: 'An app update is running',
    receiver_not_streaming: 'The receiver is not streaming',
  },
  why: {
    no_journal: 'no session', not_waiting: 'the session still runs', safe_mode: 'not in safe mode',
    settings_unavailable: 'settings unavailable', app_trial: 'app trial running', app_update: 'app update running',
    not_available_yet: 'not available in this firmware yet',
    unreadable: 'the update record is damaged: only clearing it works',
    receiver_maintenance: 'receiver maintenance is on',
  },
  job: {
    check: 'Update check', app_update: 'App update', app_upload: 'App upload', gnss_update: 'Receiver package download',
    gnss_upload: 'Receiver package upload', gnss_flash: 'Receiver update', rollback: 'Rollback',
  },
  phase: {
    checking: 'Checking', downloading: 'Downloading', rebooting: 'Restarting', receiving: 'Uploading', verifying: 'Verifying',
    inspecting: 'Inspecting the package', entering: 'Putting the receiver into its loader', transferring: 'Transferring',
    committing: 'Committing', finishing: 'Finishing', done: 'Done', failed: 'Failed', needs_user: 'Waiting for your decision',
  },
  result: {
    ok: 'Done',
    not_available_yet: 'Verified; installing it is not available yet',
    refused: 'Refused: the station state changed',
    download_failed: 'Download failed',
    checksum_mismatch: 'Checksum mismatch',
    size_mismatch: 'Size mismatch',
    invalid_image: 'Not a valid image',
    signature_failed: 'Signature check failed',
    untrusted_host: 'Refused: GitHub sent the download somewhere this firmware does not trust',
    package_rejected: 'Package refused',
    failed: 'Failed',
  },
  probe: { none: 'Not checked', ok: 'OK', no_ip: 'No IP', dns_failed: 'DNS failed', no_upstream: 'No internet' },
  ladderWhy: {
    none: '', upstream_ok: 'internet reachable', no_upstream: 'no internet', journal_open: 'receiver update open',
    interval: 'too soon', inhibited: 'restart blocked', ap_in_use: 'someone is on the access point',
    no_reboot_trigger: 'no restart for this cause', ap_overridden: 'access point wait over',
  },
  sys: { gps: 'GPS', glonass: 'GLONASS', galileo: 'Galileo', sbas: 'SBAS', qzss: 'QZSS', beidou: 'BeiDou', navic: 'NavIC' },
  linkMode: { off: 'Off', stream: 'Streaming', command: 'Command session', raw: 'Raw (update)' },
  baseMode: { survey_in: 'Survey-in', fixed: 'Fixed position' },
  posSource: { none: 'None', survey: 'Survey-in', averaged: 'Averaged', user: 'Entered by you' },
  setting: { on: 'on', off: 'off', unknown: 'unknown' },
  recovery: {
    full: ['full', 'Every recovery step, up to restarting the station when that can help.'],
    no_reboot: ['no ESP32 reboot', 'Every step that acts on the receiver, but the station itself is never restarted for it.'],
    alerts_only: ['alerts only', 'Nothing is sent to the receiver and nothing restarts: a stall is only shown and recorded in the journal.'],
  },
  trigger: { protected: 'Onocoy caster down', assoc_no_ip: 'connected without an IP address', unreached: 'a caster not reached' },
  liveSky: {
    off: 'off', pending: 'waiting', on: 'on', on_profile: 'on (receiver profile)', blocked: 'not available', stopping: 'turning off',
  },
  liveWhy: {
    none: '',
    busy: 'the station is busy with the receiver; it turns on once that ends',
    not_streaming: 'the receiver is not streaming',
    maintenance: 'receiver maintenance: nothing is sent to the receiver',
    autocfg_off: 'automatic configuration is off, so the station sends the receiver nothing',
    unsupported: 'the station does not configure this receiver model',
    reconfigured: 'the receiver was configured or restarted since; extend the view to turn it on again',
    rejected: 'the receiver refused the command',
    no_answer: 'the receiver did not answer',
    flash: 'a receiver update runs',
    not_running: 'the receiver path is not running',
  },
  liveEnd: {
    none: '', user: 'you stopped it', expired: 'its time was up', idle: 'no page showed it for a while', flash: 'a receiver update began',
  },
};

/* Receiver maintenance durations (s) as the station offers them. */
const MAINT_CHOICES = [[900, '15 minutes'], [1800, '30 minutes'], [3600, '1 hour'], [7200, '2 hours'], [14400, '4 hours']];

/* Journal enumerations by C value (API.md appendix: position in C order). */
const J = {
  kind: { boot: 'Start', session: 'Previous session', reboot: 'Restart', ladder_step: 'Recovery step', app_update: 'App update',
    app_revert: 'App reverted', gnss_flash: 'Receiver update', safe_mode: 'Safe mode', wifi_disconnect: 'Wi-Fi disconnect',
    data_loss: 'Data loss', caster_down: 'Caster session ended', caster_up: 'Caster back', wifi_up: 'Wi-Fi back',
    rx_maintenance: 'Receiver maintenance', live_view: 'Live view' },
  resetClass: ['power on', 'planned', 'crash', 'brownout', 'other'],
  rebootReason: ['none', 'user', 'factory reset', 'admin reset', 'app update', 'app revert', 'receiver recovery',
    'network recovery', 'low memory', 'safe-mode retry', 'safe mode cleared', 'settings slot switch', 'start-up failure',
    'app rollback'],
  resetReason: ['unknown', 'power on', 'reset pin', 'software', 'panic', 'interrupt watchdog', 'task watchdog', 'watchdog',
    'deep sleep', 'brownout', 'SDIO', 'USB', 'JTAG', 'eFuse', 'power glitch', 'CPU lockup'],
  trial: ['none', 'confirming', 'verifying', 'watching'],
  ladder: ['receiver', 'network', 'caster'],
  outcome: ['started', 'healthy again', 'failed or refused', 'abandoned', 'not run (recovery mode)'],
  gnssRung: [null, 'resync', 'restart link', 'search baud rate', 're-apply profile', 'reset receiver', 'restart station',
    'power-cycle alert', 'gave up (retry pending)', 'restart blocked by the cap'],
  netRung: [null, 'reconnect', 'renew DHCP and fallback DNS', 'restart Wi-Fi', 'restart station'],
  netWhy: ['none', 'internet reachable', 'no internet', 'receiver update open', 'too soon', 'restart blocked', 'someone on the access point',
    'no restart for this cause', 'access point wait over'],
  netTrigger: ['protected caster down', 'connected without an IP address', 'a caster not reached'],
  fail: ['none', 'dns', 'connect', 'tls', 'timeout', 'remote_close', 'write_stall', 'keepalive', 'auth', 'mount_busy', 'config', 'banned'],
  liveEv: [null, 'Opened', 'Extended', 'Closed', 'Satellite output on', 'Satellite output off'],
  liveEnd: ['', 'stopped by you', 'time up', 'nobody watching', 'receiver update'],
  liveCmd: ['accepted', 'rejected', 'no answer', 'not sent'],
  loss: [null, 'UART FIFO overflows', 'UART buffer full', 'line errors', 'RTCM CRC errors', 'framer discarded bytes',
    'hub publish failures', 'baud-switch bytes', 'UART events dropped'],
};

const ESP_ERR = {
  0: 'ESP_OK', '-1': 'ESP_FAIL', 257: 'ESP_ERR_NO_MEM', 258: 'ESP_ERR_INVALID_ARG', 259: 'ESP_ERR_INVALID_STATE',
  260: 'ESP_ERR_INVALID_SIZE', 261: 'ESP_ERR_NOT_FOUND', 262: 'ESP_ERR_NOT_SUPPORTED', 263: 'ESP_ERR_TIMEOUT',
  264: 'ESP_ERR_INVALID_RESPONSE', 265: 'ESP_ERR_INVALID_CRC', 266: 'ESP_ERR_INVALID_VERSION', 267: 'ESP_ERR_INVALID_MAC',
  268: 'ESP_ERR_NOT_FINISHED', 269: 'ESP_ERR_NOT_ALLOWED',
};

function espErr(v) {
  if (v === null || v === undefined) return DASH;
  return ESP_ERR[v] || ('0x' + (v >>> 0).toString(16));
}

/* Wi-Fi disconnect reasons worth explaining (wifi_err_reason_t). */
const WIFI_REASON = {
  2: 'authentication expired', 8: 'left the network', 15: 'handshake timed out (wrong password?)',
  200: 'lost the access point', 201: 'network not found', 202: 'authentication failed (wrong password?)',
  203: 'association failed', 204: 'handshake timed out (wrong password?)', 205: 'connection failed',
  210: 'network not found with a supported security mode', 211: 'network security too weak', 212: 'signal too weak',
};

function wifiReason(code) {
  if (code === null || code === undefined || code === 0) return DASH;
  return WIFI_REASON[code] ? WIFI_REASON[code] + ' (' + code + ')' : 'reason ' + code;
}

/** "Good", "Fair", "Weak" plus bars for an RSSI in dBm. */
function rssiView(rssi) {
  if (rssi === null || rssi === undefined) return DASH;
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  const word = ['', 'Weak', 'Fair', 'Good', 'Excellent'][n];
  return h('span', { class: 'nowrap' }, h('span', { class: 'sig s' + n, 'aria-hidden': 'true' }, h('i'), h('i'), h('i'), h('i')),
    ' ' + word + ' (' + rssi + ' dBm)');
}

/** Tone for a station state. */
function stationTone(state) {
  if (state === 'streaming') return 'ok';
  if (state === 'no_receiver' || state === 'flash_recovery') return 'bad';
  if (state === 'idle') return '';
  return 'warn';
}

function casterTone(state) {
  if (state === 'streaming') return 'ok';
  if (state === 'disabled' || state === 'paused') return '';
  if (state === 'auth_failed' || state === 'config_error' || state === 'banned') return 'bad';
  return 'warn';
}

function healthTone(level) {
  return { ok: 'ok', degraded: 'warn', recovering: 'warn', failed: 'bad' }[level] || '';
}

/** Tone for an RTCM age in ms (Onocoy wants each epoch within a second). */
function ageTone(ms) {
  if (ms === null || ms === undefined) return 'bad';
  return ms < 1500 ? 'ok' : ms < 5000 ? 'warn' : 'bad';
}
