/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Logs (#/logs, a tab) in three parts: Overview (#/logs: health and
 * alerts, this start with its core dump, earlier starts, the chip, the
 * Wi-Fi signal and access point, what the Receiver page leaves out of the
 * receiver, the firmware's raw counters folded), History (#/logs/history:
 * the last 30 days, then one event list read from the journal) and Console
 * (#/logs/console: the log of this start and the previous one, the log
 * level).
 */

const LOGS_TABS = [['', 'Overview'], ['history', 'History'], ['console', 'Console']];

const KEY_WORDS = {
  rx: 'RX', tx: 'TX', rtcm: 'RTCM', crc: 'CRC', hw: 'hardware', fifo: 'FIFO', dns: 'DNS', mmb: 'multiple-message bit',
  ip: 'IP', utc: 'UTC', obs: 'observations', ap: 'AP', gnss: 'receiver', bin: 'binary', ms: '', s: '', unix: '',
};

function words(v) {
  return v === null || v === undefined ? DASH : String(v).replace(/_/g, ' ');
}

/** Label from an API key: "last_rx_age_ms" -> "Last RX age". */
function keyLabel(k) {
  const t = k.split('_').map(w => (KEY_WORDS[w] === undefined ? w : KEY_WORDS[w])).filter(Boolean).join(' ');
  return t.charAt(0).toUpperCase() + t.slice(1);
}

/** Value formatted by the unit in its key (API.md, conventions). */
function keyValue(k, v) {
  if (v === null || v === undefined) return DASH;
  if (typeof v === 'boolean') return v ? 'Yes' : 'No';
  if (Array.isArray(v)) return v.length ? v.join(', ') : DASH;
  if (k === 'last_disconnect_reason') return wifiReason(v);
  if (k === 'baud') return v + ' baud';
  if (k === 'probe') return lbl(L.probe, v);
  if (k === 'mode') return lbl(L.linkMode, v);
  if (k === 'state') return lbl(L.station, v);
  if (typeof v !== 'number') return words(v);
  if (/_ms$/.test(k)) return fmt.ms(v);
  if (/_s$/.test(k)) return fmt.dur(v);
  if (/_unix$/.test(k)) return fmt.time(v);
  if (/bytes|capacity/.test(k)) return fmt.bytes(v);
  if (/_pct$/.test(k)) return v + ' %';
  if (/_permille$/.test(k)) return fmt.permille(v);
  return fmt.num(v);
}

/** Every plain field of an object as a definition list (nested objects skipped). */
function autoKv(obj, skip) {
  return kv(Object.keys(obj || {}).filter(k => (!skip || skip.indexOf(k) < 0) &&
    (obj[k] === null || typeof obj[k] !== 'object' || Array.isArray(obj[k]))).map(k => [keyLabel(k), keyValue(k, obj[k])]));
}

/** Poll `path` every `ms` and render it into box; errors replace a page still loading. */
function pollInto(box, ms, path, render) {
  return poller(ms, async () => {
    let d;
    try {
      d = await api('GET', path);
    } catch (e) {
      if (!box.firstChild || box.querySelector('.loading')) mount(box, msg('bad', errText(e)));
      return;
    }
    render(d);
  });
}

const plural = (n, one, many) => n + ' ' + (n === 1 ? one : many);

/* ---- Overview ------------------------------------------------------------ */

function journalStats(j) {
  if (!j) return h('p', { class: 'muted' }, 'Not available.');
  if (!j.available) return msg('warn', 'The journal partition is missing or unusable: events are not recorded.');
  return kv([
    ['Records kept', fmt.num(j.capacity)],
    ['Oldest / newest', j.newest_seq ? '#' + j.oldest_seq + ' / #' + j.newest_seq : 'empty'],
    ['Written this start', fmt.num(j.written)],
    ['Waiting to be written', fmt.num(j.queued)],
    ['Refused (same kind too soon)', fmt.num(j.refused_kind)],
    ['Refused (hourly budget)', fmt.num(j.refused_budget)],
    j.queue_full ? ['Refused (queue full)', h('span', { class: 't-warn' }, fmt.num(j.queue_full))] : null,
    j.io_errors ? ['Write errors', h('span', { class: 't-bad' }, fmt.num(j.io_errors))] : null,
    ['Erase cycles per sector', fmt.num(j.sector_cycles)],
  ]);
}

function logStats(l) {
  if (!l) return h('p', { class: 'muted' }, 'Not available.');
  return kv([
    ['Lines this start', fmt.num(l.lines)],
    ['Overwritten in memory', fmt.num(l.ring_overwritten)],
    ['Cut to the line limit', fmt.num(l.cut)],
    ['Not sent to the serial console', fmt.num(l.console_dropped) + ' lines (' + fmt.bytes(l.console_dropped_bytes) + ')'],
    l.usb_mirror ? ['Copy on the built-in USB', (l.usb_connected ? 'host attached' : 'no host') + ', '
      + fmt.num(l.usb_dropped) + ' lines not sent (' + fmt.bytes(l.usb_dropped_bytes) + ')'] : null,
    ['Previous start', l.prev_boot_available ? fmt.num(l.prev_boot_lines) + ' lines readable' : 'not readable'],
  ]);
}

/* A start in a word ("Power-on", "Crash", "Update"): its reset class, a planned restart by its reason. */
function startKind(cls, planned) {
  return cls === 'planned' ? L.plannedKind[planned] || 'Restart' : L.startKind[cls] || 'Reset';
}

/*
 * The earlier starts (health boot_history without this one), newest first,
 * one line each: when it started (whenParts; else how long it ran, else a
 * dash), the kind and the version; its sentence on hover. A start ran for
 * the previous run of the start after it when their counts follow on (a
 * power-on between them leaves it unknown); the newest's is This start's
 * "Previous run", so not here. day: today (toDateString).
 */
function startLines(starts, day) {
  const year = Number(day.slice(-4));
  return starts.map((x, i) => {
    const next = starts[i - 1];
    const ran = next && next.boot_no === x.boot_no + 1 && typeof next.prev_uptime_s === 'number' ? next.prev_uptime_s : null;
    return h('tr', { title: startSentence(x.reset_class, x.reset_reason, x.planned_reason) },
      whenCell(x.start_unix ? whenParts(x.start_unix, x.start_unix, false, year) : ['', ran === null ? DASH : 'Ran ' + fmt.dur(ran)]),
      h('td', { class: 'kind' }, startKind(x.reset_class, x.planned_reason)),
      h('td', { class: 'name muted' }, x.version || ''));
  });
}

/* "Receiver setup": the choice, as the Status page names it, or "Paused" while the station holds the recovery. */
function recoveryNow(hl) {
  if (hl.ladder_suspended) return pausedText(hl.station || {});
  return hl.recovery_mode ? lbl(L.manage, hl.recovery_mode) : DASH;
}

/* The recovery waits, and for what (GET /api/health station): it goes on by itself after it. A session of the station's
 * own (an apply, a short exchange under its lease, the seconds after it) is the receiver being configured. */
function pausedText(s) {
  if (s.rx_maintenance) return 'Paused (receiver maintenance)';
  if (s.state === 'flashing' || s.state === 'flash_recovery') return 'Paused during the receiver update';
  if (s.state === 'detecting') return 'Paused while the station looks for the receiver';
  if (s.state === 'configuring' || s.lease_held || (typeof s.since_lease_release_ms === 'number' && s.since_lease_release_ms < 10000)) {
    return 'Paused while the receiver is configured';
  }
  return 'Paused';
}

const toneRank = t => (t in TONE_ORDER ? TONE_ORDER[t] : 9);

/**
 * The Health card's chip, [text, tone]: the worse of the worst alert's
 * colour and the Status page's own (st: stationState), so the two pages
 * never disagree (a caster down or stopped names no alert of its own).
 */
function healthChip(level, alerts, st) {
  const worst = alerts.map(alertTone).sort((a, b) => toneRank(a) - toneRank(b))[0] || '';
  let tone = worst;
  if (st && toneRank(st.tone) < toneRank(tone)) tone = st.tone;
  if (!st && !alerts.length && level !== 'ok') tone = healthTone(level);
  if (toneRank(tone) > 2) return ['Healthy', 'ok'];
  /* Blue from the state alone: something planned or passing (looking for the receiver, maintenance), as on the Status page. */
  if (tone === 'info' && toneRank(worst) > 2) return ['Busy', 'info'];
  return [level === 'ok' ? 'Needs attention' : lbl(L.health, level), tone];
}

/*
 * The Health card's messages: the alerts, and above them the Status page's
 * state (its title and line) while it is at least as bad as every alert (the
 * alerts it says itself then go: its own and those its line covers). A state
 * that only restates an alert of its colour and stands for no page notice
 * (no receiver found) leaves it to the alert, whose words here are the
 * fuller ones; never the recovery's reasons (no corrections, no satellites,
 * the receiver stopped), whose words are the Status page's, nor a line that
 * says another alert instead (covers: the fixed position that stops the
 * corrections). So both pages say it in the same words. A closed port leaves
 * out the no-receiver alert (its cable advice would mislead), as the Status
 * page does.
 */
function healthItems(alerts, st) {
  let list = alerts.filter(a => !(st && st.key === 'uart_closed' && a.id === 'no_receiver'));
  const worst = Math.min(9, ...list.map(a => toneRank(alertTone(a))));
  const own = st && st.alertId ? list.find(a => a.id === st.alertId) : null;
  const restates = !!own && !st.notice && !st.covers.length && LADDER_REASONS.indexOf(own.id) < 0 &&
    toneRank(alertTone(own)) === toneRank(st.tone);
  const state = !!st && toneRank(st.tone) <= 2 && toneRank(st.tone) <= worst && !restates;
  if (state) list = list.filter(a => a.id !== st.alertId && st.covers.indexOf(a.id) < 0);
  return { alerts: list, state: state };
}

/* What went wrong since this start, only what happened: "2 receiver stalls, 1 recovery step". */
function sinceStart(hl) {
  const parts = [[hl.gnss_stalls, 'receiver stall', 'receiver stalls'], [hl.recoveries, 'recovery step', 'recovery steps'],
    [hl.caster_restarts, 'server restart', 'server restarts'], [hl.network_recoveries, 'network recovery', 'network recoveries']]
    .filter(p => p[0] > 0).map(p => plural(p[0], p[1], p[2]));
  return parts.length ? capFirst(parts.join(', ')) : 'No stalls or recoveries';
}

/*
 * The Wi-Fi, a row each: the network, then while online the signal (the
 * status's RSSI is the freshest), the access point, its channel and the
 * transmit power, each only while known, and with the access point whether
 * it can ask the station to move (802.11v; "Unknown" until it is read a few
 * seconds into the association; no row in a build that never reads it,
 * where GET /api/wifi leaves ap_steering out). net: the status's network;
 * w: GET /api/wifi; either may be missing.
 */
function wifiRows(net, w) {
  const n = net || w;
  if (!n) return [['Wi-Fi', DASH]];
  if (!n.ssid) return [['Wi-Fi', 'Not set up']];
  if (!n.online) return [['Wi-Fi', n.ssid + ', offline' + (n.offline_s ? ' for ' + fmt.dur(n.offline_s) : '')]];
  const ap = w || {};
  const rssi = rssiKnown(n.rssi) ? n.rssi : ap.rssi;
  return [['Wi-Fi', n.ssid], rssiKnown(rssi) ? ['Signal', rssiView(rssi)] : null, ap.bssid ? ['Access point', ap.bssid] : null,
    ap.channel ? ['Channel', String(ap.channel)] : null,
    typeof ap.tx_power_dbm === 'number' ? ['Transmit power', ap.tx_power_dbm + NB + 'dBm'] : null,
    ap.bssid && 'ap_steering' in ap ? ['Access point can steer', typeof ap.ap_steering === 'boolean' ? fmt.yesno(ap.ap_steering) : 'Unknown'] : null];
}

/* An RSSI reading in dBm: -127 to -1. Anything else means unknown (null from the station; a raw 0, or -128 from earlier builds). */
function rssiKnown(v) {
  return typeof v === 'number' && v >= -127 && v <= -1;
}

/* The tasks (GET /api/diag/tasks), busiest first: one line each. */
function tasksTable(r) {
  const tasks = (r.tasks || []).slice().sort((x, y) => (y.cpu_pct || 0) - (x.cpu_pct || 0) || (x.name < y.name ? -1 : 1));
  return [h('p', { class: 'muted small' }, 'CPU over the last 2 seconds; the least free stack seen.'),
    table(['Task', 'Priority', 'Core', 'CPU', 'Stack left'], tasks.map(t => [t.name, String(t.priority),
      t.core === null || t.core === undefined ? 'any' : String(t.core), fmt.pct(t.cpu_pct),
      h('span', { class: t.stack_free_min < 512 ? 't-bad' : null }, fmt.bytes(t.stack_free_min))]), { num: [1, 2, 3, 4], label: 'Tasks', cls: 'rows' })];
}

/* The Status page's buttons for the state, small, without the one to this page. */
function stateActions(st) {
  const acts = st.actions.filter(a => a.href !== '#/logs')
    .map(a => (a.act ? heroAction(a) : link(a.label, a.href, 'btn small'))).filter(Boolean);
  return acts.length ? h('div', { class: 'form-actions' }, acts) : null;
}

/* The Overview's Health card is on the page: from its first render until the Overview goes (noticeItems). */
let healthCardShown = false;

/* The notice the Health card says itself (the state above its alerts), which Logs' Overview leaves out. */
function healthNotice(s) {
  const st = stationState(s, true);
  return healthItems(Array.isArray(s.alerts) ? s.alerts : [], st).state ? st.notice : null;
}

/*
 * The Health card's line under the state and the alerts: what the receiver
 * recovery does now when the state's line does not say it (st.doing), then
 * the restart plan while no corrections come (as on the Status page); null
 * when there is neither.
 */
function healthPlanText(hl, st) {
  const fixing = !st || !st.delivering;
  return [st && st.doing, fixing && restartPlanText(hl)].filter(Boolean).join(' ') || null;
}

/* The next recovery attempt as a row, or null: only while no corrections come (as on the Status page), and only where the
 * station makes one (alerts only sends nothing). */
function nextAttemptRow(hl, st) {
  const fixing = !st || !st.delivering;
  return fixing && hl.gnss_retry_in_s && hl.recovery_mode !== 'alerts_only' ? ['Next recovery attempt in', fmt.dur(hl.gnss_retry_in_s)] : null;
}

/* The step in progress is said on the card: by the line under it (st.doing), or by the state's own line (the recovery's). */
function stepSaid(hl, st, shown) {
  if (hl.step_phase !== 'running' && hl.step_phase !== 'waiting') return false;
  return !!st && (!!st.doing || (shown && st.key === 'rx_stopped' && st.tone === 'warn' && !!recoveryDoing(hl)));
}

/*
 * The Health card from GET /api/health and the status s (its alerts are
 * the Status page's): the chip, the state with the Status page's button
 * (its notice is left out here) and the alerts (healthItems), the line
 * (healthPlanText), the rows (the last recovery step unless the card says
 * the step in progress: stepSaid). The page handles the data-act buttons
 * (Check again, End now, Leave safe mode).
 */
function healthCard(hl, s) {
  const port = hl.link || {};
  const alerts = s && Array.isArray(s.alerts) ? s.alerts : (hl.alerts || []).map(a => ({ id: a, source: 'health' }));
  const st = s ? stationState(s, true) : null;
  const hi = healthItems(alerts, st);
  const hc = healthChip(hl.level, hi.alerts, st);
  const plan = healthPlanText(hl, st);
  return h('section', { class: 'card' },
    cardHead('Health', badge(hc[0], hc[1])),
    hi.state ? msg(st.tone, h('strong', null, st.head), st.lead ? h('div', { class: 'small' }, stateLead(s, st, st.diagLead)) : null,
      stateActions(st)) : null,
    hi.alerts.length ? alertList(hi.alerts, { detect: true }) : hi.state ? null : h('p', { class: 'muted' }, 'No alerts.'),
    plan ? h('p', { class: 'small' }, plan) : null,
    kv([
      ['Receiver setup', recoveryNow(hl)],
      /* The port's row only while the state above does not say it. */
      port.open === false && port.mode !== 'off' && !(hi.state && st.key === 'uart_closed') ?
        ['Receiver port', h('span', { class: 't-bad' }, 'Closed, reopening')] : null,
      stepSaid(hl, st, hi.state) ? null : ['Last recovery step', lbl(L.step, hl.last_step)],
      ['Since the last RTCM', hl.rtcm_gap_ms === null || hl.rtcm_gap_ms === undefined ? 'never received' : fmt.ms(hl.rtcm_gap_ms)],
      nextAttemptRow(hl, st),
      hl.inhibited_by ? ['Restarts held back by', hl.inhibited_by] : null,
      hl.update_slot && hl.update_slot !== 'none' ? ['Update running', hl.update_slot === 'app' ? 'an app update' : 'a receiver update'] : null,
      hl.factory_reset_refused_age_s !== null && hl.factory_reset_refused_age_s !== undefined ?
        ['BOOT-button factory reset', h('span', { class: 't-warn' }, 'refused ' + fmt.dur(hl.factory_reset_refused_age_s) +
          ' ago (a receiver update was running)')] : null,
      hl.admin_resets ? ['BOOT-button password resets', fmt.num(hl.admin_resets)] : null,
      ['Since this start', sinceStart(hl)],
    ]));
}

/* Chip and Wi-Fi: sys (GET /api/system), the status s, w (GET /api/wifi) and net (health's network), any of them may be missing. */
function chipCard(sys, s, w, net) {
  const chip = (sys && sys.chip) || {};
  net = net || {};
  return h('section', { class: 'card' },
    h('h2', null, 'Chip and Wi-Fi'),
    kv([
      sys ? ['Chip', [[chip.model, chip.revision].filter(Boolean).join(' '), chip.flash_mb ? chip.flash_mb + NB + 'MB flash' : null,
        chip.psram_mb ? chip.psram_mb + NB + 'MB PSRAM' : null].filter(Boolean).join(', ')] : null,
      sys ? ['ESP-IDF', sys.idf] : null,
      ...wifiRows(s && s.network, w),
      sys && sys.mac ? ['MAC address', sys.mac] : null,
      ...linkCounts(net, w),
    ]));
}

/* This start's drops (health's network) and moves to another access point (GET /api/wifi, may be missing), each once it happened. */
function linkCounts(net, w) {
  return [net.reconnects ? ['Wi-Fi drops', fmt.num(net.reconnects) + ' this start' +
    (net.last_disconnect_reason ? ', last: ' + wifiReason(net.last_disconnect_reason) : '')] : null,
  w && w.roams ? ['Moves to another access point', fmt.num(w.roams) + ' this start'] : null];
}

/* ---- Receiver: what the Receiver page leaves out --------------------------- */

const SETTING = { on: 'On', off: 'Off', unknown: 'Unknown' };

/** When the settings were read back: "at 9:06", an older read with its date, or its age. */
function readWhen(rs) {
  if (rs.read_unix) {
    const d = new Date(rs.read_unix * 1000);
    return Date.now() - d.getTime() < 20 * 3600000 ?
      'at ' + d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' }) : 'on ' + fmt.time(rs.read_unix);
  }
  return rs.age_s !== null && rs.age_s !== undefined ? fmt.dur(rs.age_s) + ' ago' : '';
}

/**
 * What the running signal group configures on the main antenna input, from
 * the last configuration (rx_bands), short: bands per constellation, the
 * signals on hover.
 */
function bandsRow(g) {
  const b = g.rx_bands || {};
  const muted = t => ['Bands configured', h('span', { class: 'muted' }, t)];
  if (b.state === 'refused') return muted('Not reported by this firmware');
  if (b.state === 'unread') return muted('Unknown');
  if (b.state !== 'ok') return muted('After the next configuration');
  const list = [];
  (b.systems || []).filter(x => (x.signals || []).length).forEach((x, i) => list.push(i ? ' · ' : '',
    h('span', { title: (BAND_SYS[x.sys] || x.sys) + ': ' + x.signals.join(', ') }, (BAND_SYS[x.sys] || x.sys) + ' ' + x.bands)));
  return ['Bands configured', list];
}

/* Two names as the station compares them: runs of spaces as one, the ends trimmed. */
const sameText = (a, b) => String(a || '').replace(/\s+/g, ' ').trim() === String(b || '').replace(/\s+/g, ' ').trim();

/**
 * What the descriptor (RTCM 1033, d) says of the receiver and its antenna,
 * against the identity (r) and the antenna read back (ant, null: not read)
 * above it: {rows, matches}. Where every field that can be compared is the
 * same, only those that cannot be are rows and matches is true; once one
 * differs, every field is a row, those that differ in amber.
 */
function descriptorRows(d, r, ant) {
  const known = r.valid !== false;
  /* The type names the model, maybe with its maker ("UNICORE UM982"). */
  const typeHas = m => String(d.receiver_type || '').toUpperCase().indexOf(String(m).toUpperCase()) >= 0;
  /* [label, value, null: cannot be compared / true: the same / false: differs] */
  const f = [
    ['Receiver type', d.receiver_type, known && r.model ? typeHas(r.model) : null],
    ['Receiver firmware', d.receiver_fw, known && r.firmware ? sameText(d.receiver_fw, r.firmware) : null],
    ['Receiver serial', d.receiver_serial, known && r.serial ? sameText(d.receiver_serial, r.serial) : null],
    ['Antenna', d.antenna, ant ? sameText(d.antenna, ant.model) : null],
    ['Antenna serial', d.antenna_serial, ant ? sameText(d.antenna_serial, ant.serial) : null],
    ['Antenna setup ID', String(d.setup_id), ant ? Number(d.setup_id) === Number(ant.setup_id) : null],
  ];
  const differs = f.some(x => x[2] === false);
  /* The antenna's name as the receiver keeps it (spaces shown); no name is "none", any other empty field a dash. */
  const value = x => (x[0] === 'Antenna' ? (x[1] ? h('span', { class: 'ant' }, x[1]) : h('span', { class: 'muted' }, 'none')) : x[1] || DASH);
  return {
    matches: !differs && f.some(x => x[2] === true),
    rows: f.filter(x => differs || x[2] === null).map(x => [x[0], x[2] === false ? h('span', { class: 't-warn' }, value(x), ' (differs)') :
      value(x)]),
  };
}

/**
 * The Receiver card (GET /api/gnss, may be missing): who the receiver is and
 * whether it answers, what it read back at the last configuration with the
 * steps it took or refused, and what servers get from it (the station's own
 * messages: its position and its descriptor, RTCM 1033, whose receiver and
 * antenna fields show only where they cannot be checked against the parts
 * before or differ: descriptorRows). While the first configuration is held,
 * the commands it would send (preview: GET /api/gnss/apply/preview).
 */
function diagReceiverCard(g, preview) {
  if (!g) return null;
  const r = g.receiver || {};
  const rs = g.rx_settings || {};
  const steps = g.apply_steps || {};
  const ant = rs.antenna;
  const d = g.rtcm1033 || {};
  const arp = g.arp || {};
  const ago = v => (v === null || v === undefined ? DASH : fmt.ms(v) + ' ago');
  const step = (label, x) => (x && x.sent ? [label, x.ok ? 'Accepted' :
    h('span', { class: 't-warn' }, 'Rejected: ' + (x.reply || 'no reason given'))] : null);
  const part = (title, ...kids) => h('div', null, h('h3', { class: 'sub' }, title), kids);
  const when = rs.valid ? readWhen(rs) : '';
  const desc = d.valid ? descriptorRows(d, r, rs.valid ? ant : null) : { rows: [], matches: false };
  return h('section', { class: 'card wide tight-kv' }, h('h2', null, 'Receiver'), h('div', { class: 'parts' },
    part('Identity', kv([
      ['Model', r.valid === false ? h('span', { class: 'muted' }, 'Not identified yet') : r.model || DASH],
      r.valid === false ? null : ['Firmware', [fwText(r.firmware, r.build), r.compile_date ? h('span', { class: 'muted' }, ' · ' + r.compile_date) : null]],
      ['Serial number', r.serial],
      ['Part number', r.pn],
      ['Chip ID', r.efuse_id],
      /* responding: it streams, or it answered since its corrections stopped (a reply, its NMEA). */
      ['Answers', g.restarting ? h('span', { class: 't-warn' }, 'Restarting, back in about 10s') : fmt.yesno(g.responding)],
      /* Not checked during maintenance: no "No" that reads as a failed check. */
      ['Identity confirmed', gnssUnchecked(g) ? DASH : fmt.yesno(g.identity_confirmed)],
    ])),
    part('Read from the receiver' + (when ? ' ' + when : ''), kv([
      rs.valid ? null : ['Settings', h('span', { class: 'muted' }, 'Read at the next configuration')],
      rs.valid ? ['B1C/B2a output', lbl(SETTING, rs.b1cb2a || 'unknown')] : null,
      rs.valid ? ['Multipath mitigation', lbl(SETTING, rs.mmp || 'unknown')] : null,
      rs.valid ? ['Elevation mask', rs.mask_deg === null || rs.mask_deg === undefined ? 'Unknown' : Number(rs.mask_deg).toFixed(1) + '°'] : null,
      rs.valid ? ['Antenna in the receiver', ant ? [ant.model ? h('span', { class: 'ant' }, ant.model) : h('span', { class: 'muted' }, 'none'),
        ant.serial ? ' · serial ' + ant.serial : '', ' · setup ID ' + ant.setup_id] : 'Unknown'] : null,
      step('Antenna name sent', steps.antenna),
      step('B1C/B2a turned on', steps.b1cb2a),
      ['GLONASS biases (1230)', g.rtcm1230_supported === null || g.rtcm1230_supported === undefined ? 'Not configured yet' :
        g.rtcm1230_supported ? 'Accepted' : h('span', { class: 't-warn' }, 'Refused by the receiver')],
      bandsRow(g),
    ])),
    part('What servers get', kv([
      ['Station ID', d.valid ? String(d.station_id) : arp.valid ? String(arp.station_id) : DASH],
      ['Position (1005)', arp.valid ? 'Received ' + ago(arp.age_ms) : h('span', { class: 'muted' }, 'Not received yet')],
      ['Descriptor (1033)', d.valid ? 'Received ' + ago(d.age_ms) + (desc.matches ? ', matches the receiver' : '') :
        h('span', { class: 'muted' }, 'Not received yet')],
      ...desc.rows,
      d.bad_frames ? ['Unreadable 1033 messages', h('span', { class: 't-warn' }, fmt.num(d.bad_frames))] : null,
    ]))),
    g.first_apply_held ? h('div', null, h('h3', { class: 'sub' }, 'Commands the first configuration sends'),
      preview ? previewList(preview) : h('p', { class: 'muted small' }, 'Reading the commands…')) : null);
}

/*
 * Patched in place with each GET /api/health (every 5 s) and each status
 * (patchDetect; the buttons act through data-act), so the focus, a
 * selection and the open sections survive. The tasks are read every 2 s
 * while their section is open.
 */
function overviewTab(box) {
  let lastHl = null, sys = null, wifi = null, gnss = null, preview = null, tasks = null, stopTasks = null, gone = false;
  const render = hl => {
    if (gone) return;   /* an answer that came after the page was left */
    lastHl = hl;
    const b = hl.boot || {};
    const heap = hl.heap || {};
    const rxLink = hl.link || {};
    const net = hl.network || {};
    const cd = hl.coredump || {};
    const dev = (S.status || {}).device || {};
    /* A closed port is why the station finds no receiver: its state row says the port, not "No receiver found". */
    const station = rxLink.open === false && rxLink.mode !== 'off' && (hl.station || {}).state === 'no_receiver' ?
      Object.assign({}, hl.station, { state: 'uart_closed' }) : hl.station;
    const opened = new Set(Array.from(box.querySelectorAll('details[data-k]')).filter(d => d.open).map(d => d.getAttribute('data-k')));
    const sect = (k, title, ...kids) => h('details', { class: 'more', 'data-k': k, open: opened.has(k) }, h('summary', null, title), kids);
    const rxBoot = rxBootSentence((hl.station || {}).rx_boot, b.reset_class);
    /* The starts before this one: the history's first is this start (unless it could not be stored), which "This start" has. */
    const bh = hl.boot_history || [];
    const starts = bh[0] && bh[0].boot_no === b.boots_since_power_on && bh[0].reset_class === b.reset_class ? bh.slice(1) : bh;
    const lines = startLines(starts, new Date().toDateString());
    patchDetect(box, h('div', { class: 'grid' },
      healthCard(hl, S.status),
      h('section', { class: 'card' },
        /* Safe mode, and when it tries normal operation again, are the Health card's (the Status page's state). */
        cardHead('This start', b.stable && !b.safe_mode ? badge('Stable', 'ok') : null),
        h('p', null, startSentence(b.reset_class, b.reset_reason, b.prev_reboot_reason) + '.'),
        rxBoot ? h('p', { class: 'muted small' }, rxBoot + '.') : null,
        kv([
          ['Running for', fmt.dur(hl.uptime_s)],
          ['Time', dev.time_source && dev.time_source !== 'none' && dev.time_unix ?
            fmt.time(dev.time_unix) : h('span', { class: 't-warn' }, 'Not known yet')],
          ['Starts since power-on', fmt.num(b.boots_since_power_on)],
          ['Crashes counted', fmt.num(b.crashes)],
          ['Brownouts since power-on', fmt.num(b.brownouts_since_power_on)],
          ['Previous run', fmt.dur(b.prev_session_uptime_s)],
        ]),
        /* A crash's copy of the memory: it can hold passwords, so the download asks first. */
        cd.present ? msg('warn', h('strong', null, 'Core dump stored'),
          h('div', { class: 'small' }, 'A copy of the memory from the last crash, ' + fmt.bytes(cd.size) + '.'),
          kv([['Crashed task', cd.task || DASH], ['Program counter', cd.pc ? h('code', null, cd.pc) : DASH],
            ['Backtrace', (cd.backtrace || []).length ? h('code', null, cd.backtrace.join(' ')) : DASH]]),
          h('div', { class: 'form-actions' }, h('button', { type: 'button', class: 'btn danger', 'data-act': 'cd-erase' }, 'Erase'),
            h('button', { type: 'button', class: 'btn primary', 'data-act': 'cd-dl' }, 'Download'))) : null),
      chipCard(sys, S.status, wifi, net),
      h('section', { class: 'card' },
        h('h2', null, 'Earlier starts'),
        lines.length ? scrollBox('Earlier starts', h('table', { class: 'rows starts fixed' },
          h('tbody', null, foldRows(lines, 5, 'Older starts', 3, opened.has('starts'), 'starts')))) : h('p', { class: 'muted' }, 'None recorded.')),
      diagReceiverCard(gnss, preview),
      h('section', { class: 'card wide tight-kv' },
        h('details', { class: 'fold', 'data-k': 'internals', open: opened.has('internals') },
          h('summary', null, h('h2', null, 'Firmware internals')),
          sect('mem', 'Memory', kv([
            ['Internal free', fmt.bytes(heap.internal_free)],
            ['Internal lowest', fmt.bytes(heap.internal_min)],
            ['Largest internal block', fmt.bytes(heap.internal_largest)],
            ['PSRAM free', fmt.bytes(heap.psram_free)],
            heap.psram_min !== undefined ? ['PSRAM lowest', fmt.bytes(heap.psram_min)] : null,
            heap.psram_largest !== undefined ? ['Largest PSRAM block', fmt.bytes(heap.psram_largest)] : null,
          ])),
          sect('link', 'Receiver link', autoKv(rxLink)),
          sect('framing', 'Framing', autoKv(rxLink.framing)),
          sect('hub', 'RTCM hub', autoKv(hl.hub)),
          sect('station', 'Station', autoKv(station)),
          sect('obs', 'Observations', autoKv(hl.obs)),
          sect('net', 'Network', autoKv(net)),
          net.ladder ? sect('ladder', 'Network recovery', kv([
            ['Active', fmt.yesno(net.ladder.active)],
            ['Step', String(net.ladder.rung)],
            ['Held back because', net.ladder.withheld === 'none' ? DASH : lbl(L.ladderWhy, net.ladder.withheld)],
            ['Triggers now', (net.ladder.triggers || []).length ? net.ladder.triggers.map(t => lbl(L.trigger, t)).join(', ') : 'none'],
            ['Actions', fmt.num(net.ladder.actions)],
          ])) : null,
          sect('journal', 'Event journal', journalStats(hl.journal)),
          sect('log', 'Log', logStats(hl.log)),
          sect('tasks', 'Tasks', tasks ? tasksTable(tasks) : h('p', { class: 'muted' }, 'Loading…'))))));
    if (!healthCardShown) {
      healthCardShown = true;
      renderNotices();   /* the notice the card says now goes */
    }
  };
  /* Also on every status (Views.logs.update): the Health card follows it as the notices do. */
  const again = () => {
    if (lastHl) render(lastHl);
  };
  detectClicks(box, again);
  endMaintClicks(box, again);
  leaveSafeClicks(box, again);
  /* The tasks only while both folds are open. */
  const tasksOn = () => {
    const f = box.querySelector('details[data-k="internals"]'), t = box.querySelector('details[data-k="tasks"]');
    const want = !!(f && f.open && t && t.open);
    if (want && !stopTasks) {
      stopTasks = poller(2000, async () => {
        tasks = await api('GET', '/api/diag/tasks');
        again();
      });
    } else if (!want && stopTasks) {
      stopTasks();
      stopTasks = null;
    }
  };
  box.addEventListener('toggle', tasksOn, true);
  box.addEventListener('click', async e => {
    const b = e.target && e.target.closest ? e.target.closest('[data-act^="cd-"]') : null;
    if (!b || !box.contains(b)) return;
    if (b.getAttribute('data-act') === 'cd-dl') {
      const ok = await dialog({ title: 'Download the core dump?', ok: 'Download',
        body: [h('p', null, 'It is a copy of the station\'s memory at the crash and can contain your Wi-Fi and server passwords. ' +
          'Share it only with people you trust.'),
        h('p', { class: 'muted' }, 'Decode it with idf.py coredump-info and the ELF file of the same version.')],
        check: 'I understand it may contain passwords.' });
      if (!ok) return;
      const a = h('a', { href: '/api/coredump', download: 'coredump.elf', hidden: true });
      doc.body.append(a);
      a.click();
      a.remove();
      return;
    }
    const ok = await dialog({ title: 'Erase the core dump?', ok: 'Erase', okKind: 'danger solid',
      body: h('p', null, 'It cannot be downloaded afterwards.') });
    if (!ok) return;
    if (await act(b, () => api('DELETE', '/api/coredump')) === undefined) return;
    toast('Core dump erased.', 'ok');
    api('GET', '/api/health').then(render, () => {});
  });
  /* The chip and the build, once (GET /api/system); the access point, its channel and the transmit power every 15 s. */
  api('GET', '/api/system').then(v => {
    sys = v;
    again();
  }, () => {});
  const stopWifi = poller(15000, async () => {
    wifi = await api('GET', '/api/wifi');
    again();
  });
  /* The receiver (its Receiver card) every 10 s; while its first configuration is held, also what that would send. */
  const stopGnss = poller(10000, async () => {
    gnss = await api('GET', '/api/gnss');
    preview = gnss && gnss.first_apply_held ? await api('GET', '/api/gnss/apply/preview').catch(() => null) : null;
    again();
  });
  const stopHl = pollInto(box, 5000, '/api/health', render);
  return {
    update: again,
    stop() {
      gone = true;
      healthCardShown = false;
      stopHl();
      stopWifi();
      stopGnss();
      if (stopTasks) stopTasks();
    },
  };
}

/* ---- History: the last 30 days and the event list --------------------------- */

/*
 * A ledger day of slot i in words, only what happened: ["10 min offline",
 * "1 min of it planned", "4 reconnects", "38 epochs lost", ...] (the lost
 * epochs are counted for today only).
 */
function dayParts(d, i) {
  const n = k => (d[k] || [])[i] || 0;
  return [n('down_s') ? fmt.dur(n('down_s')) + ' offline' : null, n('planned_s') ? fmt.dur(n('planned_s')) + ' of it planned' : null,
    n('reconnects') ? plural(n('reconnects'), 'reconnect', 'reconnects') : null, d.lost_epochs ? plural(d.lost_epochs, 'epoch', 'epochs') + ' lost' : null,
    d.power_off_s ? fmt.dur(d.power_off_s) + ' powered off' : null,
    d.rx_overflows ? plural(d.rx_overflows, 'receiver data overflow', 'receiver data overflows') : null].filter(Boolean);
}

/* The last 30 days of the Onocoy slot (else the first), one line a day, today on top (GET /api/availability). */
function monthCard(card, av) {
  const slots = av.slots || [];
  /* The slot the station counts (the status says which one is on). */
  const ono = S.status ? onocoySlot(S.status) : null;
  const prot = (ono && slots.find(x => x.slot === ono.slot)) || slots.find(x => x.protected);
  const i = prot ? slots.indexOf(prot) : 0;
  const p = av.protected || {};
  const counting = !!prot && p.onocoy_ready !== false && p.enabled !== false;
  const day = av.today;
  const row = (d, today) => {
    const parts = dayParts(d, i);
    /* Red: the streak is lost that day (today by the Status page's own rule; earlier days: over the full day's budget). */
    const down = (d.down_s || [])[i] || 0;
    const over = counting && down > 0 && (today ? p.streak_ok === false : down > 473);
    return h('tr', null, h('td', { class: 'when' }, today ? 'Today' : d.date ?
      new Date(d.date + 'T00:00:00Z').toLocaleDateString(undefined, { month: 'short', day: 'numeric', timeZone: 'UTC' }) : String(d.utc_day)),
    h('td', null, parts.length ? parts.map((t, k) => [k ? ', ' : '', !k && over ? h('span', { class: 't-bad' }, t) : t]) :
      h('span', { class: 'muted' }, today ? 'No downtime so far' : 'No downtime')));
  };
  const rows = (day ? [row(day, true)] : []).concat((av.history || []).map(d => row(d, false)));
  /* Today and the week before; the rest folded (it stays open across the refresh). */
  const was = card.querySelector('details');
  mount(card, h('h2', null, 'Last 30 days'),
    av.clock === 'none' ? msg('warn', 'No trusted clock yet, so the station cannot count days.') : null,
    rows.length ? scrollBox('Last 30 days', h('table', { class: 'fixed days' }, h('tbody', null, foldRows(rows, 8, 'Earlier days', 2, was && was.open)))) :
      h('p', { class: 'muted' }, 'Nothing recorded yet.'),
    h('p', { class: 'muted small foot' }, (prot ? 'Onocoy' : 'Server 1') + ', per UTC day. Planned: receiver setup, updates and restarts you asked for.' +
      (counting ? ' Red: more offline time than the streak allows.' : '')));
}

/* Rows an event list shows first, and how many more each "Show older" adds. */
const EV_PAGE = 20;

/* "9:41 AM" */
function clockTime(unix) {
  return new Date(unix * 1000).toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
}

/* "9:40–9:42 AM" from "9:40 AM" and "9:42 AM" (an ending they share once). */
function timeRange(a, b) {
  const m = /\s\D+$/.exec(b);
  if (m && a.slice(-m[0].length) === m[0]) a = a.slice(0, -m[0].length);
  return a + '–' + b;
}

/*
 * When a record happened (t0 = t1), or a run of them from t0 to t1 (unix
 * seconds), as every When cell says it: [date, time], "Oct 15" and "9:41
 * AM", a run "9:40–9:42 AM", one over midnight "Oct 14–15" and "11:58 PM–
 * 12:03 AM"; the year when it is not `year` ("Oct 20, 2025"). est: a time
 * worked out from the uptime ("about 9:41 AM").
 */
function whenParts(t0, t1, est, year) {
  const a = new Date(t0 * 1000), b = new Date(t1 * 1000);
  const o = { month: 'short', day: 'numeric' };
  if (a.getFullYear() !== year || b.getFullYear() !== year) o.year = 'numeric';
  const f = new Intl.DateTimeFormat(undefined, o);
  /* Two days as one range ("Oct 14–15", "Sep 30–Oct 1"), the dash without the spaces some browsers put around it. */
  const date = a.toDateString() === b.toDateString() ? f.format(b) : f.formatRange ?
    f.formatRange(a, b).replace(/\s*–\s*/, '–') : f.format(a) + '–' + f.format(b);
  const x = clockTime(t0), y = clockTime(t1);
  return [date, (est ? 'about ' : '') + (x === y ? y : timeRange(x, y))];
}

/* A range's two ends, each kept whole (the first with its dash), so a narrow column wraps it only between them. */
function rangeEnds(t) {
  const i = t.indexOf('–');
  return (i > 0 && i < t.length - 1 ? [t.slice(0, i + 1), t.slice(i + 1)] : [t]).map(x => h('span', { class: 'nw' }, x));
}

/*
 * A When cell (td, or th with tag): the date, then the time (whenParts; an
 * empty date leaves the time alone), in a column of one width (app.css), on
 * a phone the date over the time; "about" and a range's ends wrap apart.
 */
function whenCell(w, tag) {
  const est = w[1].indexOf('about ') === 0;
  return h(tag || 'td', { class: 'when' }, w[0] ? [h('span', { class: 'd' }, rangeEnds(w[0])), h('span', { class: 'j' }, ', ')] : null,
    h('span', { class: 'tm' }, est ? [h('span', { class: 'nw' }, 'about'), ' '] : null, rangeEnds(est ? w[1].slice(6) : w[1])));
}

/*
 * Consecutive rows with the same key k become one: the newest, its `all`
 * holding every one (newest first) and its time (t0 to t1) the span they
 * cover, estimated when any of theirs is.
 */
function collapse(rows) {
  const out = [];
  for (const r of rows) {
    const last = out[out.length - 1];
    if (last && !last.head && !r.head && last.k === r.k) {
      if (!last.all) last.all = [Object.assign({}, last)];
      last.all.push(r);
      last.id = 'r' + r.seq;
      last.t0 = r.t0;
      last.est = last.est || r.est;
    } else {
      out.push(Object.assign({}, r));
    }
  }
  return out;
}

/*
 * Newest first: heading rows {head: [when or null, text]} and event rows
 * {when, ev, det, tone, seq, all (a collapsed repeat: its "×N" toggle), id}
 * (when: whenParts), one line each, also on a phone: the time, then the
 * event and its details, which start at the same place in every row (the
 * When column has one width). open: ids of the repeats shown in full (the
 * toggles carry data-rep; the page handles their clicks).
 */
function eventTable(label, rows, open) {
  const tr = [];
  const row = (r, sub) => h('tr', { class: sub ? 'rep-row' : null, title: r.seq ? '#' + r.seq : null },
    whenCell(r.when),
    h('td', null, h('strong', { class: r.tone ? 't-' + r.tone : null }, r.ev), !sub && r.all ? h('button', { type: 'button', class: 'rep',
      'data-rep': r.id, 'aria-expanded': open.has(r.id) ? 'true' : 'false', 'aria-label': (open.has(r.id) ? 'Hide' : 'Show') + ' all ' + r.all.length },
    '×' + r.all.length) : null, ' ', r.det));
  for (const r of rows) {
    if (r.head) {
      tr.push(h('tr', { class: 'sep' }, r.head[0] ? whenCell(r.head[0], 'th') : h('th', { class: 'when' }),
        h('th', { scope: 'rowgroup' }, r.head[1])));
      continue;
    }
    tr.push(row(r));
    if (r.all && open.has(r.id)) for (const x of r.all) tr.push(row(x, true));
  }
  return scrollBox(label, h('table', { class: 'events fixed' },
    h('thead', null, h('tr', null, h('th', { class: 'when' }, 'When'), h('th', null, 'Event'))),
    h('tbody', null, tr)));
}

/* Repeat toggles in root: flip the id in `open`, then redraw. */
function repeatClicks(root, open, redraw) {
  root.addEventListener('click', e => {
    const b = e.target && e.target.closest ? e.target.closest('[data-rep]') : null;
    if (!b || !root.contains(b)) return;
    const id = b.getAttribute('data-rep');
    if (open.has(id)) open.delete(id);
    else open.add(id);
    redraw();
    const again = root.querySelector('[data-rep="' + id + '"]');
    if (again) again.focus();
  });
}

/* A server slot by its name now (the status), else its number. */
function slotName(i, name) {
  const c = ((S.status && S.status.casters) || []).find(x => x.slot === i);
  return name || (c && c.name) || 'Server ' + (i + 1);
}

/* ", signal −67 dBm", or nothing when the RSSI is unknown. */
function rssiSuffix(v) {
  return rssiKnown(v) ? ', signal ' + dbmText(v) : '';
}

/*
 * One connection record in words (the GET /api/connections shape, connOf),
 * or a drop together with the return that ended it (back: the caster_up or
 * wifi_up after it): [event, details, tone].
 */
function connRow(e, back) {
  const name = e.slot === null || e.slot === undefined ? '' : slotName(e.slot, e.slot_name);
  const tries = c => (c ? ', ' + plural(c, 'failed attempt', 'failed attempts') : '');
  switch (e.kind) {
    case 'caster_down': {
      const why = e.class && e.class !== 'none' ? ', ' + lbl(L.fail, e.class).toLowerCase() : '';
      return [name + ' disconnected', (back ? 'back after ' + fmt.dur(back.duration_s) : 'after ' + fmt.dur(e.duration_s) + ' online') + why +
        (back ? tries(back.count) : '') + (e.count ? ', ' + plural(e.count, 'shorter session', 'shorter sessions') + ' not listed' : ''), 'warn'];
    }
    case 'caster_up':
      return [name + ' back', 'after ' + fmt.dur(e.duration_s) + tries(e.count), 'ok'];
    case 'caster_stop':
      return [name + ' stopped', lbl(L.stop, e.stop_reason) + ', no new attempt until you act', 'bad'];
    case 'wifi_up':
      return ['Wi-Fi back', 'after ' + fmt.dur(e.duration_s) + (e.count ? ', ' + plural(e.count, 'connect attempt', 'connect attempts') : ''), 'ok'];
    case 'wifi_disconnect':
      return ['Wi-Fi dropped', (back ? 'back after ' + fmt.dur(back.duration_s) + ', ' : '') + wifiReason(e.reason) + rssiSuffix(e.rssi), 'warn'];
    default:
      return [words(e.kind), name, ''];
  }
}

/* A rover's address from its rover_connect record: IPv4 from b; IPv6 (a bit 6) as the last 4 bytes, which b holds. */
function roverAddr(e) {
  const b = e.b >>> 0;
  if ((e.a >>> 6) & 1) return 'IPv6 …:' + (b >>> 16).toString(16) + ':' + (b & 0xffff).toString(16);
  return [b >>> 24, (b >>> 16) & 0xff, (b >>> 8) & 0xff, b & 0xff].join('.');
}

/*
 * A rover's session in words: [event, details, tone]. Its start (journalled
 * once it streamed 10 s): the address, the NTRIP version and the shorter
 * sessions not listed before it. Its end: the address of its start (from:
 * null when not in this start's records), how long it lasted, why (neutral
 * when the owner's own settings or a restart ended it).
 */
function roverEvent(e, from) {
  const a = e.a >>> 0, why = (a >>> 8) & 0xff, n = (a >>> 16) & 0x7fff;
  if (e.kind === 'rover_connect') {
    return [J.kind.rover_connect, roverAddr(e) + ', NTRIP ' + ((a >>> 4) & 3) + '.0' +
      (n ? ', ' + plural(n, 'shorter session', 'shorter sessions') + ' not listed' : ''), 'ok'];
  }
  /* The rover closing it (1), new settings (4) and a restart (6) are normal ends: neutral. */
  return [J.kind.rover_disconnect, (from ? roverAddr(from) : 'a rover') + ', after ' + fmt.dur(e.b) +
    (J.roverEnd[why] ? ': ' + J.roverEnd[why] : ''), why === 1 || why === 4 || why === 6 ? '' : 'warn'];
}

/* The journal's connection records (casters and Wi-Fi drops and returns; a move between access points is a row of its own); key: the caster's slot, or the Wi-Fi. */
const isConn = e => /^(caster|wifi)_/.test(e.kind) && e.kind !== 'wifi_roam';
const connKey = e => (e.kind.indexOf('wifi') === 0 ? 'w' : 'c' + (e.a & 0xff));

/* A connection record of the journal as GET /api/connections has it (what connRow reads). */
function connOf(e) {
  const a = e.a >>> 0, x = (a >>> 8) & 0xff, wifi = e.kind.indexOf('wifi') === 0;
  return { kind: e.kind, slot: wifi ? null : a & 0xff, duration_s: e.b, reason: a, rssi: e.b,
    count: wifi ? (e.kind === 'wifi_up' ? a : null) : (a >>> 16) & 0x7fff,
    class: e.kind === 'caster_down' ? J.fail[x] || String(x) : null, stop_reason: e.kind === 'caster_stop' ? J.stop[x] || String(x) : null };
}

/* ---- Console: the log ------------------------------------------------------ */

const LOG_KEEP = 200000; /* characters kept in the view */
const LOG_LEVELS = ['none', 'error', 'warn', 'info', 'debug', 'verbose'];

/*
 * A log line's parts for its one-line row: [time since the start
 * ("0:02:02"), level letter, text], or null for a line not in the
 * "I (122407) tag: text" form (shown as it is).
 */
function logParts(line) {
  const m = /^([EWIDV]) \((\d+)\) (.*)$/.exec(line);
  if (!m) return null;
  const s = Math.floor(Number(m[2]) / 1000);
  return [Math.floor(s / 3600) + ':' + String(Math.floor(s / 60) % 60).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0'), m[1], m[3]];
}

/* One row per line: the time, then the text (errors and warnings keep their letter and colour); a tap shows a long line whole. */
function logRow(line) {
  const p = logParts(line);
  if (!p) return h('span', { class: 'ln' }, line);
  const lv = p[1] === 'E' || p[1] === 'W' ? p[1] : '';
  return h('span', { class: 'ln' + (lv ? ' ' + lv.toLowerCase() : '') }, h('span', { class: 't' }, p[0] + ' '), lv ? lv + ' ' : null, p[2]);
}

/*
 * The log reads over HTTP up to the newest line, then follows the WebSocket
 * log from that cursor (each message says where it starts, "from"); a
 * message that does not continue where the view stands makes it read the
 * difference over HTTP and follow again from there.
 */
function consoleTab(box) {
  let cursor = 0, prev = false, paused = false, loading = false, again = false, kept = 0;
  const pre = h('pre', { class: 'log', tabindex: '0', 'aria-label': 'Log' });
  const follow = checkbox('Follow new lines', true);
  const err = errBox();

  function append(text, gap) {
    const atBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 40;
    if (gap) pre.append(h('span', { class: 'ln gap' }, '… some lines were overwritten before they could be read …'));
    for (const line of String(text || '').split('\n')) {
      if (!line) continue;
      pre.append(logRow(line));
      kept += line.length;
    }
    while (kept > LOG_KEEP && pre.firstChild) {
      kept -= pre.firstChild.textContent.length;
      pre.firstChild.remove();
    }
    if (follow.input.checked && atBottom !== false) pre.scrollTop = pre.scrollHeight;
  }
  pre.addEventListener('click', e => {
    const r = e.target && e.target.closest ? e.target.closest('.ln') : null;
    const sel = window.getSelection ? window.getSelection() : null;
    if (r && (!sel || sel.isCollapsed)) r.classList.toggle('open');
  });

  async function fetchMore() {
    if (loading) return;
    loading = true;
    try {
      for (let i = 0; i < 64; i++) {
        const r = await api('GET', '/api/logs?cursor=' + cursor + (prev ? '&boot=prev' : ''));
        if (r.level_max) offerLevels(r.level_max);
        if (r.text) append(r.text, r.gap);
        cursor = r.cursor;
        if (!r.more) break;
      }
      err.hidden = true;
    } catch (e) {
      showErr(e, err);
    } finally {
      loading = false;
    }
  }

  const following = () => !prev && !paused;

  function subscribeLog(on) {
    if (on) live.subscribe(['status', 'progress', 'log'], () => ({ log_cursor: cursor }));
    else live.subscribe(['status', 'progress']);
  }

  /* Read over HTTP up to the newest line, then let the socket go on from there. */
  async function catchUp() {
    if (loading) {
      again = true;
      return;
    }
    subscribeLog(false);
    do {
      again = false;
      await fetchMore();
    } while (again);
    if (following()) subscribeLog(true);
  }

  function restart() {
    clear(pre);
    cursor = 0;
    kept = 0;
    catchUp().then(() => {
      if (!pre.firstChild) pre.append(h('span', { class: 'ln gap' }, prev ? 'No lines from the previous start (they do not survive a power cut or an app change).' : 'No lines yet.'));
    });
    mount(dl, 'Download ' + (prev ? 'previous start' : 'log'));
    dl.href = '/api/logs?format=text' + (prev ? '&boot=prev' : '');
  }

  const offLog = bus.on('log', m => {
    if (!following() || loading) return;
    if (m.from !== undefined && m.from !== cursor) {
      if (m.cursor !== undefined && m.cursor <= cursor) return;   /* sent before this view's cursor took effect */
      catchUp();
      return;
    }
    append(m.text, m.gap);
    if (m.cursor !== undefined) cursor = m.cursor;
  });
  /* Without the WebSocket, poll for new lines. */
  const stopPoll = poller(2000, () => (following() && live.state !== 'ws' ? fetchMore() : Promise.resolve()), true);

  const which = radios('Which start', 'logboot', [['now', 'This start'], ['prev', 'Previous start']], 'now');
  which.el.querySelector('legend').className = 'sr-only';
  which.onchange(() => {
    prev = which.value() === 'prev';
    restart();
  });
  const pauseBtn = btn('Pause', () => {
    paused = !paused;
    pauseBtn.textContent = paused ? 'Resume' : 'Pause';
    if (paused) subscribeLog(false);
    else catchUp();
  }, 'small');
  const dl = h('a', { class: 'btn small', href: '/api/logs?format=text', download: '' }, 'Download log');

  const tag = textInput('*', { maxlength: 15 });
  const level = selectInput(LOG_LEVELS.map(l => [l, l]), 'info');
  const levelNote = h('p', { class: 'muted small', hidden: true });
  let levelMax = null;
  /* Only the levels this build can show at all (level_max of GET /api/logs, the build's maximum): the station refuses higher ones. */
  function offerLevels(max) {
    const top = LOG_LEVELS.indexOf(max);
    if (top < 0 || max === levelMax) return;
    levelMax = max;
    const allowed = LOG_LEVELS.slice(0, top + 1);
    const pick = allowed.indexOf(level.value) >= 0 ? level.value : allowed.indexOf('info') >= 0 ? 'info' : max;
    clear(level);
    for (const l of allowed) level.append(h('option', { value: l, selected: l === pick }, l));
    const debug = LOG_LEVELS.indexOf('debug');
    levelNote.textContent = top < debug ? 'Debug and verbose need a debug build.' : top === debug ? 'Verbose needs a build with verbose logging.' : '';
    levelNote.hidden = !levelNote.textContent;
  }
  const lvErr = errBox();
  const setBtn = btn('Set', async () => {
    const t = tag.value.trim();
    if (!t || (t !== '*' && t.length > 15)) {
      say(lvErr, 'Enter a component tag (at most 15 characters), or * for all.');
      return;
    }
    const r = await act(setBtn, () => api('PUT', '/api/diag/log-level', { tag: t, level: level.value }), lvErr);
    if (r !== undefined) toast('Log level of ' + t + ' set to ' + level.value + ' until the next restart.', 'ok');
  });

  /* Receiver byte capture: only bench builds record it (byte_capture_built). */
  const capCard = h('section', { class: 'card', hidden: true },
    h('h2', null, 'Receiver byte capture'),
    h('p', { class: 'muted small' }, 'The last 256 KB the station exchanged with the receiver in a receiver update or a bench test.'),
    h('div', { class: 'form-actions' }, h('a', { class: 'btn', href: '/api/diag/flash-capture', download: '' }, 'Download receiver bytes')));
  api('GET', '/api/health').then(hl => {
    capCard.hidden = !(hl && hl.byte_capture_built);
  }).catch(() => {});
  mount(box,
    h('section', { class: 'card' },
      h('div', { class: 'card-head fit' }, h('h2', null, 'Log'), h('div', { class: 'tools' }, pauseBtn, dl)),
      which.el,
      pre,
      follow.el,
      err),
    h('section', { class: 'card' },
      h('h2', null, 'Log level'),
      h('p', { class: 'muted small' }, 'Per component tag (* for all), until the next restart.'),
      h('div', { class: 'cols c3' }, field('Tag', tag).el, field('Level', level).el, h('div', { class: 'act' }, setBtn)),
      levelNote,
      lvErr),
    capCard);
  restart();

  return {
    stop() {
      offLog();
      stopPoll();
      live.subscribe(['status', 'progress']);
    },
  };
}

/* ---- Event journal -------------------------------------------------------- */

/* A record's details in words (the connection records go through connRow). */
function journalDetail(e) {
  const a = e.a, b = e.b;
  switch (e.kind) {
    case 'boot':
      return startSentence(J.resetClassKey[a], null, J.rebootKey[b]);
    case 'session':
      return 'The previous run ' + (a < 0 ? 'lasted an unknown time' : 'lasted ' + fmt.dur(a)) + '; it ended ' +
        (J.resetReason[b] ? 'with a ' + J.resetReason[b] + ' reset' : 'with reset ' + b);
    case 'reboot': {
      const why = lbl(L.plannedReason, J.rebootKey[a]) || J.rebootReason[a] || String(a);
      return why.charAt(0).toUpperCase() + why.slice(1);
    }
    case 'ladder_step': {
      const outcome = (b >> 8) & 0xff, code = b & 0xff, why = (b >> 16) & 0xff;
      if (a === 1 && code === 0) {
        const trig = J.netTrigger.filter((t, i) => why & (1 << i));
        return 'started' + (trig.length ? ': ' + trig.join(', ') : '');
      }
      /* The receiver ladder's own restart, in words: done, waiting, postponed (and why). */
      if (a === 0 && code === 9) {
        if (outcome === 5) {
          const mask = (b >>> 16) & 0xff;
          const why9 = J.holdBits.filter((k, i) => mask & (1 << i)).map(k => L.hold[k]);
          return 'restart postponed: ' + (why9.length ? why9.join(', ') : 'held back');
        }
        if (outcome === 4) return 'restart ' + J.outcome[4];
        /* outcome 0: what blocked it (records from older builds carry none) */
        const capWhy = J.capWhy[(b >>> 16) & 0xff];
        return J.gnssRung[9] + (capWhy ? ': ' + capWhy : '');
      }
      if (outcome === 1) return J.outcome[1];
      const step = (a === 0 ? J.gnssRung[code] : a === 1 ? J.netRung[code] : null) || 'step ' + code;
      const out = J.outcome[outcome] === undefined ? outcome : J.outcome[outcome];
      return step + (out !== '' ? ': ' + out : '') + (a === 1 && why ? ' (' + (J.netWhy[why] || why) + ')' : '');
    }
    case 'app_update':
      return a === 0 ? 'Installed; the station restarted into it' : 'Not installed: ' + espErr(a);
    case 'app_revert':
      /* a: the trial stage that gave the image up, 0 = the user (b = 1 since this build). */
      if (a === 0) return 'Rolled back by you';
      if (a === 1) return 'Rolled back during its first minute (a crash or a reset)' + (b === 2 ? ', the second time' : '');
      if (a === 2) return 'Rolled back: it did not reach the station\'s state from before the update';
      if (a === 3) return 'Rolled back: it kept crashing';
      return 'Rolled back';
    case 'gnss_flash':
      return 'Outcome: ' + (J.flashOut[a] || a) + ', transfers ' + b;
    case 'safe_mode':
      return 'Episode ' + a;
    case 'data_loss':
      return fmt.num(b) + (J.loss[a] ? ' ' + J.loss[a][b === 1 ? 1 : 2] : '');
    case 'rx_maintenance':
      return a ? 'Started for ' + fmt.dur(b) : 'Ended after ' + fmt.dur(b);
    case 'rx_boot':
      return a === 1 ? 'Kept running' : a === 2 ? 'Restarted too: the power was lost' : 'Not known';
    case 'wifi_roam': {
      /* a: channels before and after, who moved it, stayed on the same one, kept the address; b: signals before and
       * after, seconds offline (with the address kept: without a link, not said). */
      const ch0 = a & 0xff, ch1 = (a >>> 8) & 0xff, r0 = (b << 24) >> 24, r1 = (b << 16) >> 24, by = (a >>> 16) & 0xff;
      /* What the owner looks for first (a phone cuts the rest off): whether the connections were kept. */
      const off = (a >>> 25) & 1 ? 'connections kept' : fmt.dur((b >>> 16) & 0x7fff) + ' offline';
      if ((a >>> 24) & 1) return off + (by === 3 ? ', on request, the same access point' : ', no stronger access point after all');
      return [off, by === 1 ? "at the access point's request" : by === 3 ? 'on request' : 'to a stronger access point',
        ch0 && ch1 ? 'channel ' + (ch0 === ch1 ? ch1 : ch0 + ' to ' + ch1) : null,
        rssiKnown(r0) && rssiKnown(r1) ? 'signal ' + String(r0).replace('-', '\u2212') + ' to ' + dbmText(r1) : null].filter(Boolean).join(', ');
    }
    case 'sg_test': {
      const ev = a & 0xff, grp = ((a >>> 8) & 0xff) + ' ' + ((a >>> 16) & 0xff);
      if (ev === 1) return 'Comparison started with ' + grp + ', ' + fmt.dur(b) + ' per group';
      if (ev === 2) return 'Switched the receiver to ' + grp + ' (group ' + (b + 1) + ' of 2)';
      if (ev === 3) return 'The receiver refused signal group ' + grp + (b >= 0 ? '; back to ' + ((b >>> 8) & 0xff) + ' ' + (b & 0xff) : '');
      if (ev === 4) return 'Comparison ended (' + (J.sgtOut[b] || b) + ') on ' + grp;
      return 'event ' + ev + ', ' + grp;
    }
    case 'fixed_check': {
      /* a: the event, then the fixed height minus the receiver's (m); b: the horizontal distance (m) */
      const ev = a & 0xff, up = a >> 8;
      if (ev === 1) return (b <= 50 ? 'Height ' + metresText(up) : metresText(Math.hypot(b, up))) + ' off';
      if (ev === 2) return 'Looks right again';
      if (ev === 3) return 'Changed';
      if (ev === 4) return 'Sea level, ' + metresText(up) + ' off';
      return 'event ' + ev;
    }
    case 'live_view': {
      /* A record of a session not loaded whole (journalItems makes a whole session one row) */
      const ev = a & 0xff, detail = (a >> 8) & 0xff;
      if (ev === 1) return 'opened for ' + fmt.dur(b);
      if (ev === 2) return 'extended, ' + fmt.dur(b) + ' left';
      if (ev === 3) return 'closed after ' + fmt.dur(b) + (J.liveEnd[detail] ? ', ' + J.liveEnd[detail] : '');
      return (J.liveEv[ev] || 'event ' + ev).toLowerCase() + (J.liveCmd[detail] ? ': ' + J.liveCmd[detail] : '');
    }
    default:
      return 'a = ' + a + ', b = ' + b;
  }
}

/**
 * Journal entries (oldest first) grouped by start: a new group begins at a
 * "boot" record or where the uptime goes back. Records written before the
 * clock was trusted get the wall time the group's first dated record
 * implies (its time minus its uptime, plus theirs), marked as estimated.
 */
function journalByBoot(entries) {
  const groups = [];
  let g = null, prevUp = -1;
  for (const e of entries) {
    if (!g || e.kind === 'boot' || e.uptime_s < prevUp) {
      g = { entries: [], offset: null };
      groups.push(g);
    }
    g.entries.push(e);
    if (g.offset === null && e.time_unix) g.offset = e.time_unix - e.uptime_s;
    prevUp = e.uptime_s;
  }
  return groups;
}

/* The filter of the event list: [value, label, kinds (null: all)]. */
const EV_FILTER = [
  ['all', 'All events', null],
  ['starts', 'Starts and restarts', ['boot', 'session', 'reboot', 'safe_mode', 'rx_boot']],
  ['recovery', 'Recovery', ['ladder_step']],
  ['casters', 'NTRIP', ['caster_down', 'caster_up', 'caster_stop', 'rover_connect', 'rover_disconnect']],
  ['wifi', 'Wi-Fi', ['wifi_disconnect', 'wifi_up', 'wifi_roam']],
  ['updates', 'Updates', ['app_update', 'app_revert', 'gnss_flash']],
  ['receiver', 'Receiver', ['rx_maintenance', 'live_view', 'sg_test', 'fixed_check']],
  ['loss', 'Data loss', ['data_loss']],
];

/* esp_reset_reason_t by C value, as L.resetReason names them. */
const RESET_KEYS = Object.keys(L.resetReason);

/*
 * The event list's rows, newest first (eventTable): per start a heading
 * that says when and how it started (its boot and session records, and the
 * restart record that announced it), then its other records newest first:
 * a caster's or the Wi-Fi's drop and the return after it as one row (the
 * connection history), repeats collapsed. entries: oldest first; atStart:
 * they reach back to the journal's oldest record (else the oldest start may
 * just not be loaded yet); kinds: the filter (null: everything).
 */
function journalItems(entries, atStart, kinds) {
  const groups = journalByBoot(entries);
  const items = [];
  const year = new Date().getFullYear();
  for (let gi = groups.length - 1; gi >= 0; gi--) {
    const g = groups[gi], es = g.entries;
    const boot = es[0].kind === 'boot' ? es[0] : null;
    const sess = boot && es[1] && es[1].kind === 'session' ? es[1] : null;
    /* The restart this start was announced by: folded into the next start's heading when that names the same reason. */
    const nb = groups[gi + 1] && groups[gi + 1].entries[0];
    const reboots = es.filter(e => e.kind === 'reboot');
    const told = nb && nb.kind === 'boot' && J.resetClassKey[nb.a] === 'planned' && reboots.length && reboots[reboots.length - 1].a === nb.b ?
      reboots[reboots.length - 1] : null;
    /* Each drop's return: the next record of the same caster (or of the Wi-Fi) in this start, when it is a return. */
    const backOf = new Map(), used = new Set(), down = {};
    for (const e of es.filter(isConn)) {
      const key = connKey(e);
      if (e.kind === 'caster_down' || e.kind === 'wifi_disconnect') {
        down[key] = e;
      } else if ((e.kind === 'caster_up' || e.kind === 'wifi_up') && down[key]) {
        backOf.set(down[key], e);
        used.add(e);
        down[key] = null;
      } else {
        down[key] = null;
      }
    }
    /* A rover's end: the start of the same connection (a bits 0-3) before it in this start, used once. */
    const roverOf = new Map(), roverUp = {};
    for (const e of es) {
      if (e.kind === 'rover_connect') roverUp[e.a & 0xf] = e;
      else if (e.kind === 'rover_disconnect') {
        roverOf.set(e, roverUp[e.a & 0xf] || null);
        roverUp[e.a & 0xf] = null;
      }
    }
    /* A sky session (opened, extended, satellite output on, closed, output off) is one row, at its newest record. */
    const skyOf = new Map();
    let ses = null;
    for (const e of es.filter(x => x.kind === 'live_view')) {
      const ev = e.a & 0xff;
      if (ev === 1) ses = [];
      else if (!ses || (ses.closed && ev !== 5)) {
        ses = null;
        continue;
      }
      ses.push(e);
      skyOf.set(e, ses);
      if (ev === 3) ses.closed = true;
      else if (ses.closed) ses = null;
    }
    /* A record's time: its own, else the one the start's first dated record implies (estimated), else none (null). */
    const timeOf = e => (e.time_unix ? e.time_unix : g.offset !== null ? g.offset + e.uptime_s : null);
    const at = e => ({ t0: timeOf(e), t1: timeOf(e), est: !e.time_unix && g.offset !== null, up: e.uptime_s });
    const rows = [];
    for (let i = es.length - 1; i >= 0; i--) {
      const e = es[i];
      if (e === boot || e === sess || e === told || used.has(e) || (kinds && kinds.indexOf(e.kind) < 0)) continue;
      const when = at(e);
      const sky = skyOf.get(e);
      if (sky) {
        if (e !== sky[sky.length - 1]) continue;
        /* The time it was open, and what the receiver refused (an accepted command says nothing). */
        const shut = sky.find(x => (x.a & 0xff) === 3);
        const det = [shut ? fmt.dur(shut.b) : 'not closed'].concat(sky.filter(x => (x.a & 0xff) > 3 && J.liveCmd[(x.a >> 8) & 0xff])
          .map(x => J.liveEv[x.a & 0xff].toLowerCase() + ': ' + J.liveCmd[(x.a >> 8) & 0xff])).join(', ');
        const from = at(sky[0]);
        rows.push(Object.assign(when, { k: 'sky ' + det.replace(/^[^,]*/, ''), seq: e.seq, ev: J.kind.live_view, det: det,
          t0: from.t0, est: when.est || from.est }));
        continue;
      }
      if (isConn(e)) {
        const back = backOf.get(e);
        const c = connRow(connOf(e), back && connOf(back));
        rows.push(Object.assign(when, { k: [e.kind, connKey(e), !!back].join('|'), seq: e.seq, ev: c[0], det: c[1], tone: c[2] }));
      } else if (e.kind === 'rover_connect' || e.kind === 'rover_disconnect') {
        const c = roverEvent(e, roverOf.get(e));
        rows.push(Object.assign(when, { k: e.kind + '|' + c[1], seq: e.seq, ev: c[0], det: c[1], tone: c[2] }));
      } else {
        /* A recovery step under its recovery's name ("Receiver recovery"); a start and an end (maintenance, the sky) never fold together. */
        const sub = e.kind === 'ladder_step' || e.kind === 'rx_maintenance' || e.kind === 'data_loss' ? e.a :
          e.kind === 'live_view' || e.kind === 'sg_test' || e.kind === 'fixed_check' ? e.a & 0xff :
          e.kind === 'wifi_roam' ? e.a >>> 16 : '';
        const loss = e.kind === 'data_loss' ? (J.loss[e.a] || [e.a >= 16 ? slotName(e.a - 16) + ' data lost' : null])[0] : null;
        /* A move whose reconnect found no stronger access point stayed: no move; a reconnect, unless the connections
         * were kept (nothing dropped). */
        const stayed = e.kind === 'wifi_roam' && (e.a >>> 24) & 1 ? ((e.a >>> 25) & 1 ? 'Wi-Fi stayed' : 'Wi-Fi reconnected') : null;
        rows.push(Object.assign(when, { k: e.kind + sub, seq: e.seq, ev: e.kind === 'ladder_step' ? capFirst(J.ladder[e.a] || 'ladder ' + e.a) +
          ' recovery' : loss || stayed || J.kind[e.kind] || words(e.kind), det: journalDetail(e) }));
      }
    }
    if (!kinds || kinds.indexOf('boot') >= 0 || rows.length) {
      let text = boot ? startSentence(J.resetClassKey[boot.a], sess ? RESET_KEYS[sess.b] : null, J.rebootKey[boot.b]) :
        gi === 0 && !atStart ? 'Start' : 'Start (its first records are gone)';
      if (sess && sess.a >= 0) text += ' (previous run ' + fmt.dur(sess.a) + ')';
      /* When it started: its boot record's time, else (its first records gone) the start the others imply. */
      const t = boot ? timeOf(boot) : g.offset;
      items.push({ head: [t !== null ? whenParts(t, t, !(boot && boot.time_unix), year) : null,
        g.offset !== null ? text : text + ', time not known yet'] });
    }
    for (const r of collapse(rows)) {
      items.push(r);
      for (const x of [r].concat(r.all || [])) {
        x.when = x.t1 !== null ? whenParts(x.t0, x.t1, x.est, year) : ['', fmt.dur(x.up) + ' after the start'];
      }
    }
  }
  return items;
}

/*
 * The event list (the journal), newest first. It reads the newest 64
 * records (from health.journal.newest_seq), then 64 at a time backwards
 * until it has a page of rows (or the journal's start); "Show older" adds a
 * page the same way, Refresh reads what came since (all of it again after a
 * clear). No record is read twice.
 */
function eventCard() {
  const recs = new Map();   /* seq -> record */
  const open = new Set();
  let avail = true, oldest = 0, atStart = false, shown = EV_PAGE, kinds = null;
  const out = h('div', null, h('p', { class: 'loading' }, 'Loading…'));
  const err = errBox();
  const keys = () => Array.from(recs.keys());
  const items = () => journalItems(Array.from(recs.values()).sort((x, y) => x.seq - y.seq), atStart, kinds);
  async function read(cursor) {
    const r = await api('GET', '/api/journal?cursor=' + cursor);
    let added = 0;
    for (const e of r.entries || []) {
      if (!recs.has(e.seq)) {
        recs.set(e.seq, e);
        added++;
      }
    }
    return { r: r, added: added };
  }
  /* Older records until a page of rows shows (or the journal's start); none while the first read still runs (start() pages on from it). */
  async function fill() {
    for (let i = 0; i < 64 && recs.size && !atStart && items().length < shown; i++) {
      const lo = Math.min(...keys());
      const c = await read(Math.max(0, lo - 65));
      if (!c.added || Math.min(...keys()) <= oldest) atStart = true;
    }
  }
  async function start() {
    const hl = await api('GET', '/api/health');
    const j = hl.journal || {};
    avail = !!j.available;
    oldest = j.oldest_seq || 0;
    recs.clear();
    atStart = !j.newest_seq;
    if (j.newest_seq) {
      await read(Math.max(0, j.newest_seq - 64));
      if (!recs.size || Math.min(...keys()) <= oldest) atStart = true;
    }
    await fill();
    render();
  }
  async function refresh() {
    if (!recs.size) return start();
    /* Cleared meanwhile (here or on another page): every record shown is gone, so read it from the start. */
    const j = (await api('GET', '/api/health')).journal || {};
    if (!j.available || !j.newest_seq || j.oldest_seq > Math.max(...keys())) return start();
    let cursor = Math.max(...keys());
    for (let i = 0; i < 64; i++) {
      const c = await read(cursor);
      cursor = c.r.cursor;
      if (!c.r.more) break;
    }
    return render();
  }
  function render() {
    const all = items();
    mount(out, !avail ? msg('warn', 'The journal partition is missing or unusable: events are not recorded.') :
      !recs.size ? h('p', { class: 'muted' }, 'No events recorded.') :
        all.length ? eventTable('Events', all.slice(0, shown), open) :
          h('p', { class: 'muted' }, 'No events of this kind.'),
    avail && recs.size && atStart && all.length <= shown ? h('p', { class: 'muted small' }, 'No older events.') : null);
    older.hidden = all.length <= shown && atStart;
    wipe.hidden = !avail || !recs.size;
  }
  const older = btn('Show older', () => act(older, async () => {
    shown += EV_PAGE;
    await fill();
    render();
  }, err));
  const wipe = btn('Clear journal…', async () => {
    const ok = await dialog({ title: 'Clear the event journal?', ok: 'Clear journal', okKind: 'danger solid',
      body: [h('p', null, 'Every recorded event is deleted. The start history, the last 30 days and the logs are kept.'),
        h('p', { class: 'muted' }, 'The power-loss alert forgets earlier power cuts.')] });
    if (!ok) return;
    err.hidden = true;
    let done = 'Event journal cleared.', tone = 'ok';
    try {
      /* Every sector is erased: seconds on a slow flash. */
      await busy(wipe, () => api('DELETE', '/api/journal', undefined, { timeout: 60000 }));
    } catch (e) {
      const ae = e instanceof ApiError ? e : null;
      /* 500: ESP_FAIL did not clear (a full journal may lose its oldest sector, as when it moves on); ESP_ERR_NOT_FINISHED
       * cleared, but a sector not erased returns at a restart. */
      const m = ae && ae.status === 500 ? e.message : '';
      if (m === 'ESP_FAIL') return say(err, 'Not cleared: the journal flash could not be written.');
      /* 404: no usable journal; reading it again shows that. 409: a clear waits or runs, or an upload holds the station's worker. */
      if (ae && ae.status === 404) return act(null, start, err);
      if (ae && ae.code === 'busy') return say(err, 'Another clear or an upload is running. Try again when it ends.');
      /* No answer in time: the clear runs on in the station (Refresh reads the journal again once it is cleared). */
      if (ae && ae.code === 'timeout') {
        return say(err, 'No answer within a minute. Press Refresh in a moment to see whether it cleared.', 'warn');
      }
      if (m !== 'ESP_ERR_NOT_FINISHED') return showErr(e, err);
      done = 'Event journal cleared, but a flash error may bring some events back after a restart.';
      tone = 'warn';
    }
    toast(done, tone);
    shown = EV_PAGE;
    open.clear();
    act(null, start, err);
  }, 'danger', { hidden: true });
  const refreshBtn = btn('Refresh', () => act(refreshBtn, refresh, err));
  const sel = selectInput(EV_FILTER.map(f => [f[0], f[1]]), 'all', { 'aria-label': 'Show' });
  sel.addEventListener('change', () => act(null, async () => {
    kinds = EV_FILTER.find(f => f[0] === sel.value)[2];
    shown = EV_PAGE;
    await fill();
    render();
  }, err));
  repeatClicks(out, open, render);
  /* On a phone a row is one line: a tap shows it whole. */
  out.addEventListener('click', e => {
    const tr = e.target && e.target.closest && !e.target.closest('button') ? e.target.closest('tbody tr:not(.sep)') : null;
    const sel = window.getSelection ? window.getSelection() : null;
    if (tr && (!sel || sel.isCollapsed)) tr.classList.toggle('open');
  });
  act(null, start, err);
  return { el: h('section', { class: 'card wide' },
    cardHead('Events', h('div', { class: 'tools grow' }, sel, refreshBtn)),
    err, out,
    h('div', { class: 'form-actions' }, wipe, older)) };
}

function historyTab(box) {
  const month = h('section', { class: 'card wide' }, h('h2', null, 'Last 30 days'), h('p', { class: 'loading' }, 'Loading…'));
  mount(box, h('div', { class: 'grid' }, month, eventCard().el));
  return { stop: pollInto(month, 60000, '/api/availability', av => monthCard(month, av)) };
}

const LOGS_PAGES = { '': overviewTab, history: historyTab, console: consoleTab };

Views.logs = {
  title: 'Logs',
  showTitle: true,
  admin: true,
  create(el, arg) {
    const tab = LOGS_PAGES[arg] ? arg : '';
    const box = h('div', { class: 'stack' }, h('p', { class: 'loading' }, 'Loading…'));
    mount(el, h('nav', { class: 'seg', 'aria-label': 'Logs sections' },
      LOGS_TABS.map(t => h('a', { href: '#/logs' + (t[0] ? '/' + t[0] : ''), class: t[0] === tab ? 'active' : null,
        'aria-current': t[0] === tab ? 'page' : null }, t[1]))), box);
    const sub = LOGS_PAGES[tab](box);
    return {
      update() {
        if (sub && sub.update) sub.update();
      },
      destroy() {
        if (sub && sub.stop) sub.stop();
      },
    };
  },
};
