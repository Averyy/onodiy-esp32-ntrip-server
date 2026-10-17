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
    /* Not a state of the station's: what Diagnostics says for no_receiver while the receiver port is closed. */
    uart_closed: 'Receiver port closed',
  },
  caster: {
    disabled: 'Off',
    wait_network: 'Waiting for the network',
    wait_data: 'Waiting for the receiver',
    connecting: 'Connecting',
    streaming: 'Streaming',
    backoff: 'Waiting to retry',
    auth_failed: 'Login refused',
    paused: 'Paused',
    config_error: 'Configuration error',
    banned: 'Banned',
    stopped: 'Stopped: needs you',
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
    login_blocked: 'Onocoy is blocking logins',
    in_use: 'Credential in use elsewhere',
    replaced: 'Taken over by another session',
    local: 'Failed on this station',
  },
  /* Why the Onocoy login guard stopped a slot: [short reason, what to do]. */
  stop: {
    none: '',
    rejected: ['login refused 3 times', 'Onocoy refused the login 3 times. Copy the mountpoint, username and password from the ' +
      'Onocoy console again, then save or press Try again. The station does not retry on its own: repeated failed logins can ' +
      'make Onocoy block logins for a while.'],
    login_blocked: ['logins blocked', 'Onocoy is blocking logins for a while after too many failed ones. Check the credentials, ' +
      'wait, then press Try again.'],
    in_use: ['credential in use', 'Onocoy says another stream already uses this credential. Stop the other device or give this ' +
      'station its own credential, then press Try again.'],
    kicked: ['sessions taken over', 'Onocoy kept ending this station\'s sessions soon after login: another device probably ' +
      'streams with the same credential. Stop it or give this station its own credential, then press Try again.'],
    duplicate: ['duplicate stream', 'Onocoy reports this stream as a duplicate of another one. Check the Onocoy console ' +
      'before you try again.'],
    banned: ['banned', 'Onocoy reports this credential or connection as banned. Check the Onocoy console.'],
  },
  backoff: { fast: 'Fast', standard: 'Standard', polite: 'Polite' },
  trust: { bundle: 'Built-in certificate bundle', custom_ca: 'My own CA certificate', insecure: 'Do not check (insecure)' },
  health: { ok: 'Healthy', degraded: 'Needs attention', recovering: 'Recovering', failed: 'Needs attention' },
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
    power_supply: ['Low voltage', 'Low voltage restarted the station. Check the power adapter and cable.'],
    receiver_power: ['Receiver not answering', 'The receiver does not answer, even after a reset. Unplug the station for a few seconds.'],
    bands: ['Few signal bands', 'Fewer than 4 bands on any constellation; Onocoy\'s quality score needs 4.'],
    profile_drift: ['Output differs from the profile', 'The receiver\'s messages or rates differ from its profile.'],
    heap_fragmented: ['Memory fragmented', 'Restart the station when convenient.'],
    safe_mode: ['Safe mode', 'Receiver and casters are off after repeated crashes.'],
    settings_degraded: ['Settings cannot be saved', 'Changes are not saved this boot. Restart the station; if that does not help, see Diagnostics.'],
    no_upstream: ['No internet', 'Wi-Fi works, but the internet cannot be reached. Check your router.'],
    link_loss: ['Receiver data lost', 'Some data from the receiver was lost or damaged in the last 10 minutes.'],
    power_lost: ['Power was lost', 'The station lost power 3 or more times in 24 hours. Check the power adapter, cable and connections.'],
    receiver_silent: ['Nothing from the receiver', 'The receiver sends nothing and does not answer. Unplug the station for a few seconds; ' +
      'if that does not help, check the receiver\'s power and cable.'],
    /* The Status page's words (alertText adds "The receiver answers." to no_rtcm while receiver.responding says so). */
    no_satellites: ['Antenna sees no satellites', 'Check that the antenna is on the socket marked M and sees the open sky.'],
    no_rtcm: ['Receiver sends no corrections', 'Check that the antenna is on the socket marked M and sees the open sky.'],
    rx_held: ['Receiver not configured yet', 'Confirm its configuration on the Receiver page. Until then it sends no corrections.'],
  },
  /* After a failed receiver configuration: the station's own retry (automatic configuration on), or only the user's (configFailedText). */
  configRetry: {
    auto: 'The station tries again on its own.',
    user: 'Automatic configuration is off, so the station does not retry. Press Apply again in Receiver settings.',
  },
  salert: {
    unsupported_model: ['Receiver model not supported', 'The station streams what this receiver sends but does not configure or update it.'],
    config_failed: ['Receiver configuration failed', ''],   /* the text depends on the retry: configFailedText() */
    position_mismatch: ['Position mismatch', 'The streamed position is more than 5 mm from the locked one.'],
    antenna_moved: ['Antenna moved?', 'The fixed base sends nothing since the receiver restarted. If the antenna moved, survey again.'],
    receiver_attention: ['Receiver needs attention', 'Corrections did not come back after the receiver update.'],
    receiver_in_loader: ['Receiver in its update loader', 'The station only listens until you choose what to do in Settings.'],
    no_receiver: ['No receiver found', ''],   /* the text depends on the alert: noReceiverText() */
    signals_mismatch: ['Signals differ from the signal group', 'The receiver reports other signals than its signal group should give.'],
    fixed_far: ['Fixed position looks wrong', ''],   /* the text says how far: fixedFarText() */
  },
  /* Why the station's own restart (to look for the receiver) is postponed now (health reboot_hold). */
  /* health.reboot_cap_limit: which limit of the restart cap sets its wait */
  capLimit: { hour: 'at most one restart an hour', day: 'at most 3 restarts in 24 hours' },
  hold: {
    setup: 'first setup not finished',
    ap_client: 'someone is on the station\'s Wi-Fi',
    admin: 'you are using this page',
    power_on: 'less than an hour since power-on',
  },
  /* How a start came about (health boot, boot history). */
  resetReason: {
    unknown: 'for an unknown reason', poweron: 'after power-on', ext: 'by its reset pin', sw: 'by software', panic: 'after a crash',
    int_wdt: 'by the interrupt watchdog', task_wdt: 'by the task watchdog', wdt: 'by a watchdog', deepsleep: 'from deep sleep',
    brownout: 'after a power dip', sdio: 'over SDIO', usb: 'over USB', jtag: 'over JTAG', efuse: 'after an eFuse error',
    pwr_glitch: 'after a power glitch', cpu_lockup: 'after a CPU lockup',
  },
  /* A start in a word (Diagnostics' earlier starts): by its reset class; a planned one by its reason (else "Restart"). */
  startKind: { power_on: 'Power-on', crash: 'Crash', brownout: 'Power dip', other: 'Reset' },
  plannedKind: {
    app_update: 'Update', app_revert: 'Rollback', app_rollback: 'Rollback', factory_reset: 'Factory reset', admin_reset: 'Password reset',
    gnss_ladder: 'Recovery', network_ladder: 'Wi-Fi recovery', low_heap: 'Low memory', safe_mode_retry: 'Left safe mode',
    safe_mode_cleared: 'Left safe mode', boot_failure: 'Failed start',
  },
  /* Why a restart was planned, as it follows "Restarted " (startSentence) or "Restart " (the journal). */
  plannedReason: {
    none: '', user: 'by you', factory_reset: 'for a factory reset', admin_reset: 'after a password reset with the BOOT button',
    app_update: 'into an app update', app_revert: 'back to the previous version after a failed app update',
    gnss_ladder: 'to look for the receiver', network_ladder: 'by Wi-Fi recovery', low_heap: 'because memory ran low',
    safe_mode_retry: 'to try normal operation after safe mode', safe_mode_cleared: 'as you left safe mode',
    nvs_slot_switch: 'for a settings storage switch', boot_failure: 'after a failed start', app_rollback: 'as you went back to the previous version',
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
    journal: 'Update record problem', receiver: 'The receiver reported a write error',
  },
  flashOutcome: {
    none: 'None', success: 'Updated', old_firmware: 'The old firmware still runs', unexpected_build: 'An unexpected build runs',
    not_done: 'Not done: the receiver is unchanged', rejected: 'Package refused', not_started: 'Not started',
    needs_user: 'Waiting for your decision', checked: 'Package checked: it passes every rule',
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
    not_available_yet: 'receiver firmware updates are not enabled in this build yet',
    unreadable: 'the update record is damaged: only clearing it works',
    receiver_maintenance: 'receiver maintenance is on',
  },
  job: {
    check: 'Update check', app_update: 'App update', app_upload: 'App upload', gnss_update: 'Receiver package download',
    gnss_upload: 'Receiver package upload', gnss_flash: 'Receiver update', gnss_check: 'Receiver package check',
    rollback: 'Rollback',
  },
  phase: {
    checking: 'Checking', downloading: 'Downloading', rebooting: 'Restarting', receiving: 'Uploading', verifying: 'Verifying',
    inspecting: 'Inspecting the package', entering: 'Putting the receiver into its loader', transferring: 'Transferring',
    committing: 'Committing', finishing: 'Finishing', done: 'Done', failed: 'Failed', needs_user: 'Waiting for your decision',
  },
  result: {
    ok: 'Done',
    not_available_yet: 'Verified; receiver firmware updates are not enabled in this build yet',
    old_firmware: 'The old firmware still runs',
    unexpected_build: 'An unexpected build runs',
    not_done: 'Not done: the receiver is unchanged',
    not_started: 'Not started',
    needs_user: 'Stopped: waiting for your decision',
    refused: 'Refused: the station state changed',
    same_image: 'Not installed: this version is already running',
    rolled_back_ack_required: 'Not installed: this version was rolled back on this station before (installing it needs your confirmation)',
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
  /* If corrections stop (gnss/recovery): the names of Receiver settings' choice (MANAGE), the same on every page. */
  recovery: { full: 'The station', no_reboot: 'The station, but never restart itself', alerts_only: 'Me (alerts only)' },
  trigger: { protected: 'Onocoy caster down', assoc_no_ip: 'connected without an IP address', unreached: 'a caster not reached' },
  liveWhy: {
    none: '',
    busy: 'the station is busy with the receiver',
    not_streaming: 'the receiver is not streaming',
    maintenance: 'receiver maintenance is on',
    autocfg_off: 'automatic configuration is off',
    unsupported: 'the station does not configure this receiver model',
    reconfigured: 'the receiver was configured or restarted; extend to turn them on again',
    rejected: 'the receiver refused the command',
    no_answer: 'the receiver did not answer',
    flash: 'a receiver update runs',
    not_running: 'the receiver path is not running',
  },
  liveEnd: {
    none: '', user: 'you stopped it', expired: 'its time was up', idle: 'no page showed it for a while', flash: 'a receiver update began',
  },
  previewKind: { query: 'reads only', write: 'changes the receiver' },
  previewWhen: { always: 'always', if_differs: 'only if the receiver differs', if_off: 'only if it is off in the receiver' },
  /* Signal-group comparison (GET /api/gnss/signal-test). */
  sgtState: {
    idle: '', switching: 'Switching', settling: 'Settling', measuring: 'Measuring', deciding: 'Waiting for your choice',
    restoring: 'Switching back',
  },
  sgtOutcome: {
    kept: 'You kept a group.',
    timed_out: 'No choice within 24 hours: the group from before the test is back.',
    cancelled: 'Stopped: the group from before the test is back.',
    rejected: 'The receiver refused one of the groups: the group from before the test is back.',
    failed: 'A switch did not work out: the group from before the test is back.',
    interrupted: 'The comparison could not continue after a restart.',
  },
  sgtResult: { pending: 'not measured', measuring: 'measuring', done: 'measured', rejected: 'refused by the receiver' },
};

/*
 * Signal groups offered by name (the receiver's CONFIG SIGNALGROUP pairs,
 * main and second antenna input): exactly the pairs UM982 build 25164
 * accepts, with what each gives on the main input (as that build reported
 * it): [pair, name, one line, short name]. Other pairs are entered as
 * numbers, for another firmware.
 */
const SG_CHOICES = [
  ['3 6', 'Galileo E6', 'Four Galileo bands; no BeiDou B2a.', 'Galileo E6'],
  ['7 0', 'BeiDou B2a', 'Four BeiDou bands; no Galileo E6; second antenna off.', 'BeiDou B2a'],
  ['4 5', 'Factory setting', 'At most three bands on any constellation.', 'factory'],
  ['5 0', 'No L5', 'Low power, no L5 signals; second antenna off.', 'no L5'],
];
const SG_RECOMMENDED = '3 6';

/** "3 6 (Galileo E6)" for a named pair, else "M S". */
function sgName(m, s) {
  const key = m + ' ' + s;
  const c = SG_CHOICES.find(x => x[0] === key);
  return c ? key + ' (' + c[3] + ')' : key;
}

/** A receiver's firmware string, with " (build N)" only when the string does not hold the build already. */
function fwText(fw, build) {
  if (!fw) return DASH;
  return build && String(fw).indexOf(String(build)) < 0 ? fw + ' (build ' + build + ')' : fw;
}

/* MSM signal IDs by name (public RTCM signal tables), per constellation. */
const SIG_NAME = {
  gps: { 2: 'L1 C/A', 3: 'L1P', 4: 'L1W', 8: 'L2C', 9: 'L2P', 10: 'L2W', 15: 'L2C(M)', 16: 'L2C(L)', 17: 'L2C', 22: 'L5I',
    23: 'L5Q', 24: 'L5', 30: 'L1C(D)', 31: 'L1C(P)', 32: 'L1C' },
  glonass: { 2: 'G1 C/A', 3: 'G1P', 8: 'G2 C/A', 9: 'G2P' },
  galileo: { 2: 'E1C', 3: 'E1A', 4: 'E1B', 5: 'E1', 6: 'E1Z', 8: 'E6C', 9: 'E6A', 10: 'E6B', 11: 'E6', 12: 'E6Z', 14: 'E5bI',
    15: 'E5bQ', 16: 'E5b', 18: 'E5 AltBOC I', 19: 'E5 AltBOC Q', 20: 'E5 AltBOC', 22: 'E5aI', 23: 'E5aQ', 24: 'E5a' },
  qzss: { 2: 'L1 C/A', 9: 'L6D', 10: 'L6P', 11: 'L6', 12: 'L6E', 13: 'L6Z', 15: 'L2C(M)', 16: 'L2C(L)', 17: 'L2C', 22: 'L5I',
    23: 'L5Q', 24: 'L5', 30: 'L1C(D)', 31: 'L1C(P)', 32: 'L1C' },
  beidou: { 2: 'B1I', 3: 'B1Q', 4: 'B1', 8: 'B3I', 9: 'B3Q', 10: 'B3', 14: 'B2I', 15: 'B2Q', 16: 'B2', 22: 'B2a data',
    23: 'B2a pilot', 24: 'B2a', 25: 'B2b', 30: 'B1C data', 31: 'B1C pilot', 32: 'B1C' },
  navic: { 22: 'L5 SPS' },
};

function sigNames(sys, ids) {
  const t = SIG_NAME[sys] || {};
  return (ids || []).map(id => t[id] || 'signal ' + id).join(', ');
}

/* Receiver maintenance durations (s) as the station offers them. */
const MAINT_CHOICES = [[900, '15 minutes'], [1800, '30 minutes'], [3600, '1 hour'], [7200, '2 hours'], [14400, '4 hours']];

/* Journal enumerations by C value (API.md appendix: position in C order). */
const J = {
  /* The event list's names (a connection or a recovery step is named by connRow, by its recovery). */
  kind: { boot: 'Start', session: 'Previous run', reboot: 'Restart', app_update: 'App update', app_revert: 'App rolled back',
    gnss_flash: 'Receiver update', safe_mode: 'Safe mode', data_loss: 'Data lost', rx_maintenance: 'Receiver maintenance',
    live_view: 'Sky view', sg_test: 'Signal groups', rx_boot: 'Receiver at the start', fixed_check: 'Fixed position',
    wifi_roam: 'Wi-Fi moved' },
  /* By C value, as the API names them (lifecycle_reset_class_t, lifecycle_reboot_reason_t). */
  resetClassKey: ['power_on', 'planned', 'crash', 'brownout', 'other'],
  rebootKey: ['none', 'user', 'factory_reset', 'admin_reset', 'app_update', 'app_revert', 'gnss_ladder', 'network_ladder',
    'low_heap', 'safe_mode_retry', 'safe_mode_cleared', 'nvs_slot_switch', 'boot_failure', 'app_rollback'],
  rebootReason: ['none', 'user', 'factory reset', 'admin reset', 'app update', 'app revert', 'receiver recovery',
    'network recovery', 'low memory', 'safe-mode retry', 'safe mode cleared', 'settings slot switch', 'start-up failure',
    'app rollback'],
  resetReason: ['unknown', 'power on', 'reset pin', 'software', 'panic', 'interrupt watchdog', 'task watchdog', 'watchdog',
    'deep sleep', 'brownout', 'SDIO', 'USB', 'JTAG', 'eFuse', 'power glitch', 'CPU lockup'],
  ladder: ['receiver', 'network', 'caster'],
  /* A recovery step's outcome after its name (0: started, said by the name alone) */
  outcome: ['', 'healthy again', 'failed', 'given up', 'skipped (your recovery choice)', 'postponed'],
  /* A receiver recovery step in the words the Status page uses while it runs (RECOVERY_DOING). */
  gnssRung: [null, 'resyncing the receiver\'s data', 'reopening the link to the receiver', 'searching for the receiver',
    're-sending the receiver\'s settings', 'restarting the receiver', 'restarted the station to look for the receiver',
    'asked for a receiver power cycle', 'waiting for the next search', 'restart postponed'],
  /* Why a restart was postponed (ladder_step code 9, outcome 0: bits 16-23 of b; 0 = not recorded). */
  capWhy: ['', 'at most one an hour', 'at most 3 in 24 hours', 'another job holds restarts'],
  /* hold_mask bits of a postponed restart (ladder_step code 9, outcome 5). */
  holdBits: ['setup', 'ap_client', 'admin', 'power_on'],
  netRung: [null, 'reconnecting', 'renewing the address', 'restarting the Wi-Fi', 'restarting the station'],
  netWhy: ['none', 'internet reachable', 'no internet', 'receiver update open', 'too soon', 'restart blocked', 'someone on the access point',
    'no restart for this cause', 'access point wait over'],
  netTrigger: ['Onocoy down', 'no IP address', 'a caster not reached'],
  fail: ['none', 'dns', 'connect', 'tls', 'timeout', 'remote_close', 'write_stall', 'keepalive', 'auth', 'mount_busy', 'config', 'banned',
    'login_blocked', 'in_use', 'replaced', 'local'],
  /* ntrip_stop_t (a caster_stop record's bits 8-15) */
  stop: ['none', 'rejected', 'login_blocked', 'in_use', 'kicked', 'duplicate', 'banned'],
  liveEv: [null, 'Opened', 'Extended', 'Closed', 'Satellite output on', 'Satellite output off'],
  liveEnd: ['', 'closed by you', 'time up', 'nobody watching', 'receiver update'],
  /* A satellite output command's answer (0: accepted, said by nothing) */
  liveCmd: ['', 'refused by the receiver', 'no answer from the receiver', 'not sent'],
  /* sg_test: the outcome in b of an end (station_sgt_outcome_t) */
  sgtOut: ['', 'kept', 'timed out', 'cancelled', 'rejected', 'failed', 'interrupted'],
  /* station_flash_outcome_t */
  flashOut: ['none', 'updated', 'the old firmware still runs', 'an unexpected build runs', 'not done (receiver unchanged)',
    'package refused', 'not started', 'waiting for a decision', 'package checked'],
  /* data_loss by counter id: [event, unit of the count] */
  loss: [null, ['Receiver data lost', 'overflow', 'overflows'], ['Receiver data lost', 'full buffer', 'full buffers'],
    ['Receiver link errors', 'error', 'errors'], ['Damaged corrections', 'message', 'messages'], ['Receiver data lost', 'byte', 'bytes'],
    ['Corrections not passed on', 'message', 'messages'], ['Bytes dropped at a speed change', 'byte', 'bytes'],
    ['Receiver data lost', 'missed event', 'missed events']],
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

/** A Wi-Fi reason in words; noCode: without the number (the setup's connect view). */
function wifiReason(code, noCode) {
  if (code === null || code === undefined || code === 0) return DASH;
  if (!WIFI_REASON[code]) return 'reason ' + code;
  return noCode ? WIFI_REASON[code] : WIFI_REASON[code] + ' (' + code + ')';
}

/**
 * "Good", "Fair", "Weak" plus bars for an RSSI in dBm; noDbm: the word only
 * (the dashboard, the network list). A reading is -127 to -1 dBm: null and 0
 * are unknown and show a dash.
 */
function rssiView(rssi, noDbm) {
  if (rssi === null || rssi === undefined || rssi >= 0) return DASH;
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  const word = ['', 'Weak', 'Fair', 'Good', 'Excellent'][n];
  return h('span', { class: 'nowrap' }, h('span', { class: 'sig s' + n, 'aria-hidden': 'true' }, h('i'), h('i'), h('i'), h('i')),
    ' ' + word + (noDbm ? '' : ' (' + rssi + ' dBm)'));
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
  if (state === 'auth_failed' || state === 'config_error' || state === 'banned' || state === 'stopped') return 'bad';
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

/* Minutes for a sentence ("4 min"; under a minute in seconds). */
function inMinutes(s) {
  if (s === null || s === undefined) return DASH;
  return s < 60 ? fmt.dur(s) : fmt.dur(Math.ceil(s / 60) * 60);
}

/** The no-receiver alert in words: what to check, and when the station looks again. */
function noReceiverText(a) {
  const next = a.next_detect_in_s === null || a.next_detect_in_s === undefined ? '' :
    a.next_detect_in_s <= 0 ? ' Searching now.' : ' Next search in ' + inMinutes(a.next_detect_in_s) + '.';
  if (a.saw_bytes) return 'Data arrives, but the station cannot read it. Check the wiring and the receiver\'s port speed.' + next;
  return 'Nothing arrives from the receiver. Check its power and the cable from its COM1 to the station ' +
    '(receiver TX to GPIO1, RX to GPIO2).' + next;
}

/* The Status page's no-receiver line (the pins are in the Receiver page's panel). */
function noReceiverLead(a) {
  if (a.saw_bytes) return 'Data arrives, but the station cannot read it. Check the wiring and the receiver\'s port speed.';
  return 'Nothing arrives from the receiver. Check its power and the COM1 cable.';
}

/* "next search in 23 s", or null. */
function nextSearchText(a) {
  const n = a.next_detect_in_s;
  if (n === null || n === undefined) return null;
  return n <= 0 ? 'searching now' : 'next search in ' + inMinutes(n);
}

/* Metres in words for a warning: whole metres, km from 1 km on. */
function metresText(m) {
  const a = Math.abs(m || 0);
  return a < 1000 ? Math.round(a) + NB + 'm' : (a / 1000).toFixed(1) + NB + 'km';
}

/*
 * Where the fixed position is against the receiver's own, in one sentence
 * (the fixed_far alert, or GET /api/gnss fixed_check: horizontal_m,
 * vertical_m (fixed minus the receiver's), distance_m, height_only,
 * sea_level: the fixed height is the receiver's height above sea level).
 */
function fixedFarSentence(a) {
  if (a.sea_level) return 'The fixed height looks like a height above sea level.';
  if (a.height_only) {
    return 'The fixed height is ' + metresText(a.vertical_m) + (a.vertical_m > 0 ? ' above' : ' below') + ' where the receiver is.';
  }
  return 'The fixed position is ' + metresText(a.distance_m) + ' from where the receiver is.';
}

/* The fixed_far warning: how far, and what to do (where; a sea-level height: the height the field wants). */
function fixedFarText(a) {
  return fixedFarSentence(a) + (a.sea_level ? ' Enter the height above the ellipsoid in Receiver settings.' :
    ' Check it in Receiver settings.');
}

/* The same when it is why no corrections come (the receiver stops them this far off): the cause in the sentence. */
function fixedStopsText(a) {
  return fixedFarSentence(a).replace(/\.$/, ', which stops them.') + ' Check it in Receiver settings.';
}

/* During receiver maintenance the station sends the receiver nothing, so what it has not checked stays unchecked until the end
 * (a maintenance that went on across a station restart with nothing kept of what the station knew: no identity this boot). */
const NOT_CHECKED = 'Not checked during maintenance';
const NOT_CHECKED_CHIP = 'Not checked';   /* a chip, on a page whose notice says maintenance */

/** The station has not checked the receiver and will not before maintenance ends (maint: it is on; confirmed: identity_confirmed). */
function rxUnchecked(maint, confirmed) {
  return !!maint && !confirmed;
}

/** The failed-configuration alert's text: when the station tries again, or that only the user does (alert or GET /api/gnss). */
function configFailedText(autoRetry, retryInS) {
  if (autoRetry === false) return L.configRetry.user;
  return retryInS ? 'The station tries again in ' + inMinutes(retryInS) + '.' : L.configRetry.auto;
}

/**
 * The station's own restart to look for the receiver (health:
 * recovery_mode, reboot_hold, reboot_hold_left_s, reboot_cap_wait_s,
 * reboot_cap_limit, reboot_recheck) as {k, t, why, limit}: k 'cap' (next
 * possible in t), 'latest' (in t at the latest), 'later' (a hold with no
 * end: the setup's), 'recheck'; null when nothing is pending (reported only
 * while the receiver recovery runs) and in every mode but full recovery,
 * where the station never restarts itself for the receiver. With a hold and
 * the cap, the restart waits for the later of the two.
 */
function restartPlan(hl) {
  if (!hl || hl.recovery_mode !== 'full') return null;
  const hold = hl.reboot_hold || [];
  const cap = hl.reboot_cap_wait_s;
  const hasCap = cap !== null && cap !== undefined;
  const left = hold.indexOf('setup') >= 0 ? null : hl.reboot_hold_left_s;
  const hasLeft = left !== null && left !== undefined;
  if (!hold.length && !hasCap && !hl.reboot_recheck) return null;
  const why = hold.map(r => L.hold[r] || String(r).replace(/_/g, ' ')).join(', ');
  if (hasCap && (!hold.length || !hasLeft || cap > left)) return { k: 'cap', t: inMinutes(cap), why: why, limit: hl.reboot_cap_limit };
  if (hold.length) return { k: hasLeft ? 'latest' : 'later', t: hasLeft ? inMinutes(left) : '', why: why };
  return { k: 'recheck', why: '' };
}

/** restartPlan() as a sentence (Diagnostics). */
function restartPlanText(hl) {
  const p = restartPlan(hl);
  if (!p) return null;
  const why = ' to get the receiver back.' + (p.why ? ' Postponed: ' + p.why + '.' : '');
  if (p.k === 'cap') return 'The station restarts itself' + why + ' Next possible in ' + p.t + ' (' + (L.capLimit[p.limit] || 'the restart limit') + ').';
  if (p.k === 'latest') return 'The station restarts itself in ' + p.t + ' at the latest' + why;
  if (p.k === 'later') return 'The station restarts itself later' + why;
  return 'The station decides after the next search whether to restart itself.';
}

/** restartPlan() in a few words, lower case, to follow the next search on one muted line (Status, Receiver). */
function restartPlanShort(hl) {
  const p = restartPlan(hl);
  if (!p) return null;
  return p.k === 'cap' ? 'station restart possible in ' + p.t : p.k === 'latest' ? 'station restart in ' + p.t + ' at the latest' :
    p.k === 'later' ? 'station restart postponed (' + p.why + ')' : 'station may restart after the next search';
}

/** After a power-on start or a power dip (cls: boot.reset_class): whether the receiver kept running (health station.rx_boot). */
function rxBootSentence(v, cls) {
  switch (v) {
    case 'kept_running':
      return 'The receiver kept running';
    case 'restarted':
      return cls === 'brownout' ? 'The receiver restarted too' : 'The power was lost';
    default:   /* still judged, or not known: nothing to say */
      return '';
  }
}

/** How a start came about, as a sentence (reset class, reset reason, planned reason). */
function startSentence(cls, reason, planned) {
  const why = planned && planned !== 'none' ? lbl(L.plannedReason, planned) : '';
  switch (cls) {
    case 'power_on':
      return 'Started after power-on or a reset by its pin';
    case 'planned':
      return 'Restarted ' + (why || 'on purpose');
    case 'crash':
      return 'Restarted after a crash' + (reason && reason !== 'panic' ? ' (' + lbl(L.resetReason, reason).replace(/^(after|by|for|from|over) /, '') + ')' : '');
    case 'brownout':
      return 'Restarted after a power dip (low supply voltage)';
    default:
      return 'Restarted' + (reason ? ' ' + lbl(L.resetReason, reason) : '') + (why ? ', ' + why : '');
  }
}
