/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver page: the receiver (identity, link, alerts; the settings read
 * back, what it publishes in RTCM 1033 behind Details), the base position
 * (survey, lock, survey again), the receiver profile in four groups (signals,
 * position, corrections, advanced), what the station does if corrections
 * stop, receiver maintenance (hands off), and last the comparison of two
 * signal groups. Field rules mirror PUT /api/gnss/profile; the station's own
 * validation has the last word.
 */

const RTCM_NAMES = {
  1005: 'Station position', 1006: 'Station position and height', 1007: 'Antenna name', 1008: 'Antenna name and serial',
  1013: 'System parameters', 1033: 'Receiver and antenna names', 1019: 'GPS ephemeris', 1020: 'GLONASS ephemeris',
  1042: 'BeiDou ephemeris', 1044: 'QZSS ephemeris', 1045: 'Galileo F/NAV ephemeris', 1046: 'Galileo I/NAV ephemeris',
  1074: 'GPS observations (MSM4)', 1075: 'GPS observations (MSM5)', 1077: 'GPS observations (MSM7)',
  1084: 'GLONASS observations (MSM4)', 1087: 'GLONASS observations (MSM7)', 1094: 'Galileo observations (MSM4)',
  1097: 'Galileo observations (MSM7)', 1114: 'QZSS observations (MSM4)', 1117: 'QZSS observations (MSM7)',
  1124: 'BeiDou observations (MSM4)', 1127: 'BeiDou observations (MSM7)', 1137: 'NavIC observations (MSM7)',
  1230: 'GLONASS biases',
};
const EPHEMERIS_MSGS = [1019, 1020, 1042, 1045, 1046];
const ONOCOY_RTCM = [1005, 1033, 1077, 1087, 1097, 1117, 1127, 1230];
const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const IGS_ANTENNAS = 'https://files.igs.org/pub/station/general/rcvr_ant.tab';
const ONOCOY_INSTALL = 'https://docs.onocoy.com/documentation/3.-become-a-miner/2.-install-your-station';

function rtcmName(n) {
  if (RTCM_NAMES[n]) return RTCM_NAMES[n];
  if (n >= 1101 && n <= 1107) return 'SBAS observations (not sent to casters)';
  return '';
}

/** A link that opens outside the station's page. */
function extLink(label, href) {
  return h('a', { href: href, target: '_blank', rel: 'noopener noreferrer', class: 'ext' }, label);
}

/** A distance in words: mm under 1 cm, cm under 1 m, else m or km. */
function fmtDist(m) {
  if (m === null || m === undefined || isNaN(m)) return DASH;
  const a = Math.abs(m);
  if (a < 0.01) return Math.round(a * 1000) + NB + 'mm';
  if (a < 1) return (a * 100).toFixed(1) + NB + 'cm';
  if (a < 1000) return a.toFixed(a < 10 ? 2 : 1) + NB + 'm';
  return (a / 1000).toFixed(1) + NB + 'km';
}

/** Whether a disclosure in el is open (a rebuild keeps what the user left it at). */
function isOpen(el, cls) {
  return !!el.querySelector('details.' + cls + '[open]');
}

/** A disclosure with a class of its own (isOpen() finds it again). */
function moreAs(cls, summary, open, ...kids) {
  const d = disclosure(summary, open, kids);
  d.classList.add(cls);
  return d;
}

/**
 * Lock / survey-again buttons with their warnings and their error box. Built
 * once per view: the 5 s refresh rebuilds the position card around them, and
 * a request that is still pending (or its confirmation dialog) must keep the
 * button it disabled and the box it reports into. update(g) shows the buttons
 * that fit the latest state.
 */
function positionActions(done) {
  const out = errBox();
  const lockBtn = btn('Lock this position', async () => {
    const ok = await dialog({ title: 'Lock the surveyed position?', ok: 'Lock position',
      body: h('p', null, 'The receiver becomes a fixed base at this position, the same after every restart. ' +
        'Corrections pause for a few seconds.') });
    if (!ok) return;
    const r = await act(lockBtn, () => api('POST', '/api/gnss/position/lock'), out);
    if (r !== undefined) {
      toast('Locking the position…', 'ok');
      done();
    }
  }, 'primary', { hidden: true });
  const resurveyBtn = btn('Survey again', async () => {
    const ok = await dialog({ title: 'Survey the position again?', ok: 'Survey again', okKind: 'danger solid',
      body: [h('p', null, 'The station forgets the locked position and the receiver surveys again.'),
        msg('warn', 'The position sent to casters changes. Onocoy may validate the station again.')],
      check: 'I understand that the position changes.' });
    if (!ok) return;
    const r = await act(resurveyBtn, () => api('DELETE', '/api/gnss/position/lock'), out);
    if (r !== undefined) {
      toast('Surveying again…', 'ok');
      done();
    }
  }, 'danger', { hidden: true });
  return {
    el: h('div', null, out, h('div', { class: 'form-actions' }, resurveyBtn, lockBtn)),
    update(g) {
      const pos = g.position || {};
      const arp = g.arp || {};
      lockBtn.hidden = !(!pos.locked && (arp.valid || (pos.source && pos.source !== 'none')));
      resurveyBtn.hidden = !pos.locked;
    },
  };
}

/* ---- Receiver card ------------------------------------------------------ */

/** When the settings were read back: "at 9:06", an older read with its date, or its age. */
function readWhen(rs) {
  if (rs.read_unix) {
    const d = new Date(rs.read_unix * 1000);
    return Date.now() - d.getTime() < 20 * 3600000 ?
      'at ' + d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' }) : 'on ' + fmt.time(rs.read_unix);
  }
  return rs.age_s !== null && rs.age_s !== undefined ? fmt.dur(rs.age_s) + ' ago' : '';
}

const SETTING = { on: 'On', off: 'Off', unknown: 'Unknown' };

/* The receiver's settings as read back at the last configuration, and the antenna and B1C/B2a steps of it. */
function readBack(g) {
  const rs = g.rx_settings || {};
  const steps = g.apply_steps || {};
  const a = steps.antenna || {}, b = steps.b1cb2a || {};
  const ant = rs.antenna;
  const sent = [
    a.sent ? ['Antenna name sent', a.ok ? h('span', { class: 't-ok' }, 'accepted') :
      h('span', { class: 't-bad' }, 'antenna model rejected: ' + (a.reply || 'no reason given'))] : null,
    b.sent ? ['B1C/B2a turned on', b.ok ? h('span', { class: 't-ok' }, 'accepted') :
      h('span', { class: 't-bad' }, 'rejected: ' + (b.reply || 'no reason given'))] : null,
  ];
  return [
    h('h3', { class: 'sub' }, 'Read from the receiver' + (rs.valid && readWhen(rs) ? ' ' + readWhen(rs) : '')),
    rs.valid ? kv([
      ['BeiDou B1C/B2a output', lbl(SETTING, rs.b1cb2a || 'unknown')],
      ['Multipath mitigation', lbl(SETTING, rs.mmp || 'unknown')],
      ['Elevation mask', rs.mask_deg === null || rs.mask_deg === undefined ? 'Unknown' : Number(rs.mask_deg).toFixed(1) + '°'],
      ['Antenna model in the receiver', ant ? [ant.model ? h('span', { class: 'ant' }, ant.model) : h('span', { class: 'muted' }, 'none'),
        ant.serial ? ' · serial ' + ant.serial : '', ' · setup ID ' + ant.setup_id] : 'Unknown'],
      ...sent,
    ]) : [h('p', { class: 'muted small' }, 'Read at each configuration; unknown until the next one.'), sent.some(Boolean) ? kv(sent) : null],
  ];
}

/* Constellation names of the signal report (GET /api/gnss rx_bands). */
const BAND_SYS = { gps: 'GPS', glonass: 'GLONASS', galileo: 'Galileo', beidou: 'BeiDou', qzss: 'QZSS' };

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
  if (b.state !== 'ok') return muted('Known after the next configuration');
  const list = [];
  (b.systems || []).filter(x => (x.signals || []).length).forEach((x, i) => list.push(i ? ' · ' : '',
    h('span', { title: (BAND_SYS[x.sys] || x.sys) + ': ' + x.signals.join(', ') }, (BAND_SYS[x.sys] || x.sys) + ' ' + x.bands)));
  return ['Bands configured', list];
}

/* The signals the profile's group should give and the report lacks, or null. */
function bandsMissingMsg(g) {
  const b = g.rx_bands || {};
  if (b.state !== 'ok' || b.matches !== false) return null;
  const lack = (b.systems || []).filter(x => (x.missing || []).length)
    .map(x => (BAND_SYS[x.sys] || x.sys) + ' ' + x.missing.join(', '));
  return msg('warn', h('strong', null, 'The receiver lacks signals its signal group should give.'),
    lack.length ? h('div', { class: 'small' }, 'Missing: ' + lack.join('; ') + '.') : null);
}

/** A signal group the receiver refused, in plain words (GET /api/gnss signal_group_refused), or null. */
function sgRefusedMsg(g) {
  const r = g.signal_group_refused;
  if (!r) return null;
  return msg(r.back_to ? 'warn' : 'bad', h('strong', null, 'The receiver refused signal group ' + r.master + ' ' + r.slave + '.'),
    h('div', { class: 'small' }, (r.back_to ? 'It is back on ' + sgName(r.back_to.master, r.back_to.slave) + ' and configured again.' :
      'Its previous group is unknown, so the configuration failed.') + ' Choose a group from the list and save.'),
    r.reply && r.reply !== 'signal group rejected' ? h('div', { class: 'small muted' }, 'Receiver reply: ' + r.reply) : null);
}

/* What the receiver publishes in its antenna and receiver descriptor (RTCM 1033), as received. */
function rtcm1033Rows(g) {
  const d = g.rtcm1033 || {};
  return [
    h('h3', { class: 'sub' }, 'What casters see (RTCM 1033)'),
    d.valid ? kv([
      ['Station ID', String(d.station_id)],
      ['Antenna', d.antenna ? h('span', { class: 'ant' }, d.antenna) : h('span', { class: 'muted' }, 'none')],
      ['Antenna setup ID', String(d.setup_id)],
      ['Antenna serial', d.antenna_serial || DASH],
      ['Receiver type', d.receiver_type || DASH],
      ['Receiver firmware', d.receiver_fw || DASH],
      ['Receiver serial', d.receiver_serial || DASH],
      ['Received', d.age_ms === null || d.age_ms === undefined ? DASH : fmt.ms(d.age_ms) + ' ago'],
      d.bad_frames ? ['Unreadable messages', h('span', { class: 't-warn' }, fmt.num(d.bad_frames))] : null,
    ]) : h('p', { class: 'muted small' }, 'Not received yet.' + (d.bad_frames ? ' Unreadable messages: ' + fmt.num(d.bad_frames) + '.' : '')),
  ];
}

/* Alerts that belong to the base position card (they are about the position). */
const POSITION_ALERTS = ['position_mismatch', 'antenna_moved'];

/** Station alerts as messages (bold title, one line); only: the ids to show. */
function alertMsgs(g, only) {
  return (g.alerts || []).filter(only).map(a => {
    const t = L.salert[a] || [a, ''];
    const text = a === 'config_failed' ? configFailedText(g.config_auto_retry, g.config_retry_in_s) : t[1];
    return msg('warn', h('strong', null, t[0]), text ? h('div', { class: 'small' }, text) : null);
  });
}

/** The Receiver card while a receiver is there (open: the Details as the user left them). */
function receiverInfo(g, open) {
  const r = g.receiver || {};
  const link = g.link || {};
  const maint = (g.maintenance || {}).on;
  const missing = bandsMissingMsg(g);
  const age = v => (v === null || v === undefined ? 'Never' : fmt.ms(v) + ' ago');
  return [
    cardHead('Receiver', maint ? badge('Maintenance', 'warn') : badge(lbl(L.station, g.state), stationTone(g.state))),
    r.valid === false ? h('p', { class: 'muted' }, 'Not identified yet.') : null,
    sgRefusedMsg(g),
    missing,
    alertMsgs(g, a => a !== 'no_receiver' && POSITION_ALERTS.indexOf(a) < 0 && !(a === 'signals_mismatch' && missing)),
    kv([
      ['Model', r.model],
      ['Firmware', fwText(r.firmware, r.build)],
      /* Until the receiver streams, the station listens on the link (and searches between): no "Streaming" claim. */
      ['Link', g.state === 'streaming' ? lbl(L.linkMode, link.mode) + (link.baud ? ' at ' + link.baud + NB + 'baud' : '') :
        'Listening' + (link.baud ? ' at ' + link.baud + NB + 'baud' : '')],
      ['Last corrections', h('span', { class: 't-' + ageTone(link.rtcm_age_ms) }, age(link.rtcm_age_ms))],
      ['Signal group', g.rx_signal_group ? sgName(g.rx_signal_group.master, g.rx_signal_group.slave) :
        h('span', { class: 'muted' }, 'Known after the next configuration')],
    ]),
    moreAs('rx-details', 'Details', open,
      kv([
        ['Serial number', r.serial],
        ['Part number', r.pn],
        ['Chip ID', r.efuse_id],
        ['Firmware date', r.compile_date],
        ['Answers commands', g.restarting ? h('span', { class: 't-warn' }, 'Restarting, back in about 10 s') : fmt.yesno(g.responding)],
        ['Identity confirmed', fmt.yesno(g.identity_confirmed)],
        ['Last data', link.rx_bytes === 0 ? 'Nothing yet' : age(link.rx_age_ms)],
        link.open === false && link.mode !== 'off' ? ['Receiver port', h('span', { class: 't-bad' }, 'Closed, reopening' +
          (link.open_failures ? ' (' + link.open_failures + ' failed tries)' : ''))] : null,
        g.detect_failures ? ['Detection failures', fmt.num(g.detect_failures)] : null,
        ['GLONASS biases (1230)', g.rtcm1230_supported === null || g.rtcm1230_supported === undefined ? 'Not configured yet' :
          g.rtcm1230_supported ? 'Accepted' : h('span', { class: 't-warn' }, 'Refused by the receiver')],
        bandsRow(g),
      ]),
      readBack(g),
      rtcm1033Rows(g)),
  ];
}

/* ---- Base position card ---------------------------------------------------- */

/** Everything in the base position card but its buttons (open: the Details as the user left them). */
function positionInfo(g, open) {
  const pos = g.position || {};
  const arp = g.arp || {};
  const sv = g.survey || {};
  const prof = g.profile || {};
  const have = pos.source && pos.source !== 'none';
  const streamKv = () => kv([
    ['Latitude', fmt.deg(arp.lat_deg)],
    ['Longitude', fmt.deg(arp.lon_deg)],
    ['Height (ellipsoid)', fmt.m(arp.height_m)],
    ['Station ID', arp.station_id],
    ['Received', fmt.ms(arp.age_ms) + ' ago'],
    arp.offset_m === null || arp.offset_m === undefined ? null : ['Distance to the locked position', fmtDist(arp.offset_m)],
  ]);
  const alerts = alertMsgs(g, a => POSITION_ALERTS.indexOf(a) >= 0);
  /* A fixed position saved in the profile but not applied yet (no receiver, or not configured since): the one typed in. */
  if (prof.base_mode === 'fixed' && prof.fixed && !(pos.locked && have)) {
    const f = prof.fixed;
    return [
      cardHead('Base position', badge('Fixed (typed in)', 'info')),
      h('p', { class: 'small' }, 'The position you entered; sent to the receiver at its next configuration.'),
      alerts,
      kv([
        ['Latitude', fmt.deg(f.lat_deg)],
        ['Longitude', fmt.deg(f.lon_deg)],
        ['Height (ellipsoid)', fmt.m(f.height_m)],
        ['Sent to casters', 'Not yet'],
      ]),
      arp.valid ? moreAs('pos-details', 'Details', open, h('h3', { class: 'sub' }, 'Position in the stream now'), streamKv()) : null,
    ];
  }
  let state, tone, line;
  if (pos.locked) {
    state = 'Locked';
    tone = 'ok';
    line = 'The same position after every restart. Never move the antenna; if it moves, survey again.';
  } else if (sv.active) {
    state = 'Surveying';
    tone = 'warn';
    line = sv.window_left_s ? 'About ' + inMinutes(sv.window_left_s) + ' left. Then lock the result.' : 'Lock the result when it ends.';
  } else {
    state = 'Not locked';
    tone = '';
    line = have ? 'Lock it so the position stays the same after restarts.' : 'No position yet.';
  }
  let sent = 'Not yet';
  if (arp.valid && pos.locked && arp.offset_m !== null && arp.offset_m !== undefined) {
    sent = arp.offset_m > 0.005 ? h('span', { class: 't-warn' }, fmtDist(arp.offset_m) + ' from the locked position') :
      h('span', { class: 't-ok' }, 'Matches' + (arp.offset_m >= 0.0005 ? ' (' + fmtDist(arp.offset_m) + ' apart)' : ''));
  } else if (arp.valid) {
    sent = 'Yes';
  }
  return [
    cardHead('Base position', badge(state, tone)),
    h('p', { class: 'small' }, line),
    alerts,
    have ? kv([
      ['Latitude', fmt.deg(pos.lat_deg)],
      ['Longitude', fmt.deg(pos.lon_deg)],
      ['Height (ellipsoid)', fmt.m(pos.height_m)],
      pos.sigma_m ? ['Accuracy (σ)', fmtDist(pos.sigma_m)] : null,
      pos.locked_unix ? ['Locked on', fmt.time(pos.locked_unix)] : null,
      ['Sent to casters', sent],
    ]) : null,
    have || arp.valid ? moreAs('pos-details', 'Details', open,
      have ? kv([['Source', lbl(L.posSource, pos.source)]]) : null,
      h('h3', { class: 'sub' }, 'Position in the stream'),
      arp.valid ? streamKv() : h('p', { class: 'muted small' }, 'Not received yet.')) : null,
  ];
}

/* ---- Receiver maintenance, if corrections stop ------------------------------- */

/**
 * Receiver maintenance: start (with its duration), extend, end now. Built
 * once per view so a pending request keeps its button; update(g) shows the
 * state. done() refreshes the page. nodes: the card's children.
 */
function maintenanceCtl(done) {
  const out = errBox();
  const badgeEl = badge('', '');
  const dur = selectInput(MAINT_CHOICES, 1800);
  const durField = field('Duration', dur);
  const label = () => lbl(Object.fromEntries(MAINT_CHOICES), dur.value);
  const startBtn = btn('Start maintenance', async () => {
    const ok = await dialog({ title: 'Start receiver maintenance?', ok: 'Start',
      body: [h('p', null, 'For ' + label() + ' the station sends the receiver nothing. Corrections keep streaming.'),
        h('p', { class: 'muted' }, 'It ends by itself, with End now, or at a power cut. Then the station checks the receiver as at start-up.'),
        msg('warn', MAINT_SAVE_NOTE + ' Undo any you do not want to keep before you end.')] });
    if (!ok) return;
    const r = await act(startBtn, () => api('POST', '/api/gnss/maintenance', { duration_s: Number(dur.value) }), out);
    if (r !== undefined) {
      toast('Receiver maintenance on.', 'ok');
      done();
    }
  }, 'primary');
  const extendBtn = btn('Add time', async () => {
    const r = await act(extendBtn, () => api('POST', '/api/gnss/maintenance', { duration_s: Number(dur.value) }), out);
    if (r !== undefined) {
      toast('Receiver maintenance extended: ' + fmt.dur(r.left_s) + ' left.', 'ok');
      done();
    }
  });
  const endBtn = btn('End now', async e => {
    if (await endMaintenance(e)) done();
  }, 'primary');
  return {
    nodes: [
      cardHead('Receiver maintenance', badgeEl),
      h('p', null, MAINT_WHAT),
      h('p', { class: 'muted small' }, MAINT_SAVE_NOTE),
      out,
      h('div', { class: 'form-actions card-actions' }, durField.el, startBtn, extendBtn, endBtn)],
    update(g) {
      const m = g.maintenance || {};
      badgeEl.className = 'badge ' + (m.on ? 'warn' : '');
      badgeEl.textContent = m.on ? 'On · ' + inMinutes(m.left_s || 0) + ' left' : 'Off';
      startBtn.hidden = !!m.on;
      extendBtn.hidden = endBtn.hidden = !m.on;
    },
  };
}

/** If corrections stop (gnss/recovery). Returns {nodes, update(g), dirty()}. */
function recoveryCtl() {
  const out = errBox();
  let dirty = false, current = 'full', autocfg = true;
  const opts = ['full', 'no_reboot', 'alerts_only'].map(k => [k, h('span', { class: 'opt' },
    h('span', null, L.recovery[k][0] + (k === 'full' ? ' (recommended)' : '')), h('small', { class: 'hint' }, L.recovery[k][1]))]);
  const choice = radios('If corrections stop', 'recovery', opts, current);
  choice.el.querySelector('.choices').classList.add('stacked');
  choice.el.querySelector('legend').classList.add('sr-only');
  /* Recovery's re-apply step is refused while automatic configuration is off. */
  const offNote = h('p', { class: 'muted small', hidden: true }, 'Automatic configuration is off, so recovery skips reconfiguring the receiver.');
  const syncNote = () => {
    offNote.hidden = autocfg || choice.value() === 'alerts_only';
  };
  choice.onchange(() => {
    dirty = choice.value() !== current;
    syncNote();
  });
  const save = btn('Save', async () => {
    const v = choice.value();
    const r = await act(save, () => api('PUT', '/api/gnss/profile', { recovery: v }), out);
    if (r === undefined) return;
    current = v;
    dirty = false;
    toast('Saved: ' + L.recovery[v][0] + '.', 'ok');
  }, 'primary');
  return {
    nodes: [
      cardHead('If corrections stop'),
      h('p', { class: 'muted small' }, 'What the station does when the receiver stops sending corrections.'),
      choice.el, offNote, out, h('div', { class: 'form-actions card-actions' }, save)],
    update(g) {
      autocfg = !!g.autocfg;
      if (!dirty && g.recovery) {
        current = g.recovery;
        choice.set(current);
      }
      syncNote();
    },
    dirty: () => dirty,
  };
}

/* ---- The held first configuration --------------------------------------- */

/* The user's "keep it configured" choice, kept until the first configuration has worked (this browser). */
const HELD_AUTO_KEY = 'heldAuto';

/** What the station would send, from GET /api/gnss/apply/preview, as a numbered list. */
function previewList(d) {
  const steps = (d && d.steps) || [];
  if (!steps.length) return h('p', { class: 'muted' }, 'Nothing to list (no valid profile).');
  return h('ol', { class: 'stack small' }, steps.map(s => h('li', null, h('code', null, s.text), ' ',
    h('span', { class: 'muted' }, [lbl(L.previewKind, s.kind), lbl(L.previewWhen, s.when),
      s.saved ? 'saved in the receiver' : null].filter(Boolean).join(' · ')))));
}

/**
 * The first configuration worked: turn automatic configuration and full
 * recovery on when the user ticked it with the confirmation. g: GET
 * /api/gnss. Resolves true when it did.
 */
async function finishHeld(g) {
  if (!store.get(HELD_AUTO_KEY, null) || !g || g.first_apply_held || !g.profile_applied || g.autocfg) return false;
  store.set(HELD_AUTO_KEY, null);
  try {
    await api('PUT', '/api/gnss/profile', { autocfg: true, recovery: 'full' });
  } catch (e) {
    store.set(HELD_AUTO_KEY, true);   /* tried again on the next look */
    throw e;
  }
  toast('The receiver is configured. Automatic configuration and recovery are on.', 'ok');
  return true;
}

/**
 * The receiver's first configuration, held until the user confirms it (a
 * build that holds it): the choice of automatic configuration afterwards,
 * what would be sent (behind a disclosure), "Configure the receiver now" and
 * "Not now". The Receiver page and the setup wizard show it alike, so its
 * words name no place on either. opts.onLater: "Not now"; opts.onApplied:
 * the apply was accepted. Returns {el, load()}.
 */
function heldPanel(opts) {
  const err = errBox();
  const status = h('div', { hidden: true });
  const listSummary = h('summary', null, 'Commands it will send');
  const list = h('div', null, h('p', { class: 'muted small' }, 'Reading the commands…'));
  const auto = checkbox('Afterwards, keep it configured and fix problems automatically (recommended)', true,
    'Turns on automatic configuration and automatic recovery once this has worked.');
  let preview = null;
  let watching = false;
  const load = async () => {
    try {
      preview = await api('GET', '/api/gnss/apply/preview');
      mount(list, previewList(preview));
      listSummary.textContent = 'Commands it will send (' + ((preview && preview.steps) || []).length + ')';
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      mount(list, msg('warn', 'The list could not be read: ' + errText(e)));
    }
  };
  const show = (tone, text) => {
    mount(status, msg(tone, text));
    status.hidden = false;
  };
  /* Until the receiver has taken it (or the attempt failed): then the user's choice is carried out. Runs on
   * when the panel leaves the page (the wizard goes on to the live view meanwhile). After an earlier failed
   * attempt (failedBefore) its alert stays raised until an apply works, so this attempt's failure is only
   * its own once the station was seen configuring and has left that state. */
  const watch = async failedBefore => {
    if (watching) return;
    watching = true;
    let seen = false;
    const until = Date.now() + 15 * 60000;
    while (Date.now() < until) {
      await sleep(3000);
      let g;
      try {
        g = await api('GET', '/api/gnss');
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) break;
        continue;
      }
      if (g.profile_applied && !g.first_apply_held) {
        show('ok', 'The receiver is configured.' + (store.get(HELD_AUTO_KEY, null) ? ' Turning automatic configuration on…' : ''));
        try {
          await finishHeld(g);
        } catch (e) {
          show('warn', 'The receiver is configured, but automatic configuration was not turned on: ' + errText(e));
        }
        break;
      }
      if (g.state === 'configuring') seen = true;
      if (g.config_failed && (!failedBefore || (seen && g.state !== 'configuring'))) {
        /* Held: automatic configuration is off, so nothing retries it but this button. */
        show('bad', 'The configuration failed and the station does not retry on its own. The Receiver page says why; ' +
          'then press Configure the receiver now.');
        break;
      }
      show('info', g.state === 'streaming' || g.state === 'configuring' ? 'Configuring the receiver…' :
        'Queued until the receiver streams (now: ' + lbl(L.station, g.state).toLowerCase() + ').');
    }
    watching = false;
  };
  const confirmBtn = btn('Configure the receiver now', async () => {
    if (!preview) await load();
    const ok = await dialog({ title: 'Configure the receiver now?', ok: 'Configure',
      body: [h('p', null, 'The station configures the receiver and saves the settings in it. Corrections pause for a few seconds.'),
        disclosure('Show the commands', false, previewList(preview)),
        h('p', { class: 'muted small' }, auto.input.checked ? 'Afterwards, automatic configuration and recovery turn on.' :
          'Afterwards, the station changes nothing on the receiver by itself.')] });
    if (!ok) return;
    let failedBefore = false;
    try {
      failedBefore = !!(await api('GET', '/api/gnss')).config_failed;
    } catch (e) {
      /* unknown: the watch takes a failure it sees as this attempt's */
    }
    const r = await act(confirmBtn, () => api('POST', '/api/gnss/apply'), err);
    if (r === undefined) return;
    store.set(HELD_AUTO_KEY, auto.input.checked ? true : null);
    toast('Configuring the receiver…', 'ok');
    show('info', 'Configuring the receiver…');
    watch(failedBefore);
    if (opts && opts.onApplied) opts.onApplied();
  }, 'primary');
  const laterBtn = btn('Not now', () => {
    if (opts && opts.onLater) opts.onLater();
  });
  return {
    el: h('div', { class: 'stack' },
      cardHead('Receiver not configured yet', badge('Waiting for you', 'info')),
      h('p', null, 'So far the station has only read the receiver. Confirm to configure it with the receiver profile.'),
      auto.el,
      h('details', { class: 'more' }, listSummary, list),
      status, err,
      h('div', { class: 'form-actions' }, laterBtn, confirmBtn)),
    load: load,
  };
}

/* ---- Profile editor ----------------------------------------------------- */

/** The profile's state for its chip: [text, tone]. */
function profileChip(g) {
  if (g.state === 'configuring') return ['Applying', 'info'];
  if (g.config_failed) return ['Failed', 'bad'];
  if (g.profile_applied && !g.first_apply_held) return ['Applied', 'ok'];
  return ['Not applied', 'warn'];
}

/** "Casters see now: ..." under the antenna fields (GET /api/gnss rtcm1033), or ''. */
function castersSee(g) {
  const d = g.rtcm1033 || {};
  if (!d.valid) return '';
  return d.antenna ? ['Casters see now: ', h('span', { class: 'ant' }, d.antenna)] : 'Casters see now: no antenna name.';
}

/** A group of the profile form: its title, then kids. */
function pgroup(title, ...kids) {
  return h('section', { class: 'pgroup stack' }, h('h3', null, title), kids);
}

function profileEditor(g, done) {
  const p = Object.assign({ signal_group_master: 3, signal_group_slave: 6, base_mode: 'survey_in', survey_in_s: 600, fixed: null,
    station_id: 0, com1_baud: 115200, rtcm: [], sat_view_output: false }, g.profile || {});
  let dirty = false;
  const autocfg = checkbox('Configure the receiver automatically', g.autocfg,
    'Re-applies this profile whenever the receiver differs from it.');
  const sgm = numInput(p.signal_group_master, { inputmode: 'numeric', maxlength: 3 });
  const sgs = numInput(p.signal_group_slave, { inputmode: 'numeric', maxlength: 3 });
  /* Named choices; other pairs (a newer receiver firmware) as numbers under "Other group numbers". */
  const sgKey = p.signal_group_master + ' ' + p.signal_group_slave;
  const named = SG_CHOICES.some(c => c[0] === sgKey);
  const sgChoice = radios('Signal group', 'sgroup', SG_CHOICES.map(c => [c[0], h('span', { class: 'opt' },
    h('span', null, c[0] + ' ' + c[1] + (c[0] === SG_RECOMMENDED ? ' (recommended)' : '')), h('small', { class: 'hint' }, c[2]))]),
  named ? sgKey : null);
  sgChoice.el.querySelector('.choices').classList.add('stacked', 'grid2');
  sgChoice.el.querySelector('legend').classList.add('sr-only');
  const sgCustom = checkbox('Use other numbers', !named, 'For receiver firmware with more groups. A pair the receiver refuses is undone.');
  const mode = radios('Base mode', 'basemode', [['survey_in', 'Survey-in'], ['fixed', 'Fixed position']], p.base_mode);
  mode.el.querySelector('legend').classList.add('sr-only');
  const survey = numInput(p.survey_in_s || 600, { inputmode: 'numeric', maxlength: 4 });
  const fx = p.fixed || {};
  const lat = numInput(fx.lat_deg, { inputmode: 'text', placeholder: '45.123456789' });
  const lon = numInput(fx.lon_deg, { inputmode: 'text', placeholder: '-75.123456789' });
  const hgt = numInput(fx.height_m, { inputmode: 'text', placeholder: '85.1234' });
  const sid = numInput(p.station_id, { inputmode: 'numeric', maxlength: 4 });
  const baud = selectInput(BAUDS.map(b => [b, b + ' baud']), p.com1_baud);
  const satView = checkbox('Satellite positions output', p.sat_view_output,
    'For the Sky card on the Signals page. The live view does not need it.');
  const antModel = textInput(p.antenna_model || '', { maxlength: 64, class: 'ant' });
  const antSerial = textInput(p.antenna_serial || '', { maxlength: 64 });
  const antSetup = numInput(p.antenna_setup_id || 0, { inputmode: 'numeric', maxlength: 3 });
  const seeLine = h('p', { class: 'muted small' });

  const F = {
    signal_group_master: field('Main antenna', sgm),
    signal_group_slave: field('Second antenna', sgs, '0 turns it off.'),
    signal_group: { el: sgChoice.el, input: null, error() {} },
    survey_in_s: field('Survey-in time (seconds)', survey),
    lat_deg: field('Latitude (°)', lat),
    lon_deg: field('Longitude (°)', lon),
    height_m: field('Height above the ellipsoid (m)', hgt),
    station_id: field('Station ID', sid, 'Goes into every message.'),
    com1_baud: field('Receiver link speed (COM1)', baud, 'Ephemeris messages need 460800 baud or more.'),
    rtcm: { el: h('div'), input: null, error: null },
    antenna_model: field('Antenna model (IGS name)', antModel, ['Only for an antenna in the IGS list: name and radome as listed. ' +
      'Empty keeps the receiver\'s value; ADVNULLANTENNA clears it. ', extLink('IGS antenna list', IGS_ANTENNAS)]),
    antenna_serial: field('Antenna serial number', antSerial),
    antenna_setup_id: field('Antenna setup ID', antSetup),
  };
  const sgNumbers = h('div', { class: 'cols c2 sg-custom' }, F.signal_group_master.el, F.signal_group_slave.el);
  const sgSync = () => {
    sgNumbers.hidden = !sgCustom.input.checked;
    for (const r of sgChoice.el.querySelectorAll('input')) r.disabled = sgCustom.input.checked;
  };
  sgChoice.onchange(() => {
    const v = (sgChoice.value() || '').split(' ');
    if (v.length === 2) {
      sgm.value = v[0];
      sgs.value = v[1];
    }
  });
  sgCustom.input.addEventListener('change', () => {
    if (!sgCustom.input.checked) {
      const key = sgm.value.trim() + ' ' + sgs.value.trim();
      sgChoice.set(SG_CHOICES.some(c => c[0] === key) ? key : SG_RECOMMENDED);
      const v = (sgChoice.value() || SG_RECOMMENDED).split(' ');
      sgm.value = v[0];
      sgs.value = v[1];
    }
    sgSync();
  });
  sgSync();
  const rows = h('div', { class: 'msgrows' });
  const rtcmErr = h('small', { class: 'err', hidden: true });
  const summary = h('p', { class: 'small' });
  F.rtcm.error = t => {
    rtcmErr.textContent = t || '';
    rtcmErr.hidden = !t;
  };
  /* The messages in one line, kept up to date while rows change. */
  const sumUp = () => {
    const list = Array.from(rows.children, r => Array.from(r.querySelectorAll('input'), i => Number(i.value.trim())));
    const n = list.length;
    const each = list.every(x => x[1] === 1);
    if (!n) summary.textContent = 'No messages: the receiver sends no corrections.';
    else if (n === ONOCOY_RTCM.length && each && ONOCOY_RTCM.every(m => list.some(x => x[0] === m))) {
      summary.textContent = 'Onocoy defaults: ' + n + ' messages, each every second.';
    } else summary.textContent = n + (n === 1 ? ' message' : ' messages') + (each ? ', each every second' : '') + ' (custom).';
  };

  function addRow(msgNo, period) {
    const m = numInput(msgNo, { inputmode: 'numeric', maxlength: 4, 'aria-label': 'Message number' });
    const per = numInput(period || 1, { inputmode: 'numeric', maxlength: 4, 'aria-label': 'Every (seconds)' });
    const name = h('span', { class: 'muted' }, rtcmName(Number(msgNo)));
    const row = h('div', { class: 'msgrow' }, m, per, name, h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Remove message',
      onclick: () => {
        row.remove();
        dirty = true;
        sumUp();
      } }, '✕'));
    m.addEventListener('input', () => {
      name.textContent = rtcmName(Number(m.value));
    });
    rows.append(row);
    return m;
  }
  for (const r of p.rtcm || []) addRow(r.msg, r.period_s);
  sumUp();

  const fixedBox = h('div', { class: 'stack' }, h('div', { class: 'cols c3' }, F.lat_deg.el, F.lon_deg.el, F.height_m.el),
    h('p', { class: 'hint' }, 'ITRF coordinates of the antenna\'s exact spot, for example from a PPP service.'),
    g.arp && g.arp.valid ? h('div', null, btn('Use the current position', () => {
      lat.value = g.arp.lat_deg.toFixed(9);
      lon.value = g.arp.lon_deg.toFixed(9);
      hgt.value = g.arp.height_m.toFixed(4);
      dirty = true;
    }, 'small')) : null);
  const sync = () => {
    const fixed = mode.value() === 'fixed';
    fixedBox.hidden = !fixed;
    F.survey_in_s.el.hidden = fixed;
  };
  mode.onchange(sync);
  sync();

  function collect() {
    for (const k of Object.keys(F)) F[k].error(null);
    let bad = null;
    const fail = (k, t) => {
      F[k].error(t);
      if (!bad) bad = F[k].input || rows;
    };
    const int = (s, lo, hi) => {
      const n = Number(String(s).trim());
      return String(s).trim() !== '' && Number.isInteger(n) && n >= lo && n <= hi ? n : null;
    };
    const num = s => {
      const n = Number(String(s).trim());
      return String(s).trim() !== '' && isFinite(n) ? n : null;
    };
    const pick = sgCustom.input.checked ? null : (sgChoice.value() || '').split(' ');
    const out = {
      signal_group_master: pick && pick.length === 2 ? Number(pick[0]) : int(sgm.value, 0, 255),
      signal_group_slave: pick && pick.length === 2 ? Number(pick[1]) : int(sgs.value, 0, 255),
      base_mode: mode.value(),
      survey_in_s: 0,
      fixed: null,
      station_id: int(sid.value, 0, 4095),
      com1_baud: Number(baud.value),
      rtcm: [],
      sat_view_output: satView.input.checked,
      antenna_model: '',
      antenna_serial: '',
      antenna_setup_id: int(antSetup.value, 0, 255),
    };
    /* Trailing spaces are dropped (the stored form); inner spaces stay. */
    const antenna = (input, k) => {
      const v = input.value.replace(/ +$/, '');
      if (v.length > 31 || /[^\x20-\x7e]/.test(v) || v.indexOf('"') >= 0) {
        fail(k, 'At most 31 plain characters (letters, digits, spaces and punctuation, no ").');
        return '';
      }
      return v;
    };
    out.antenna_model = antenna(antModel, 'antenna_model');
    out.antenna_serial = antenna(antSerial, 'antenna_serial');
    if (out.antenna_setup_id === null) fail('antenna_setup_id', 'A number from 0 to 255.');
    if (out.signal_group_master === null) fail('signal_group_master', 'A number from 0 to 255.');
    if (out.signal_group_slave === null) fail('signal_group_slave', 'A number from 0 to 255.');
    if (out.station_id === null) fail('station_id', 'A number from 0 to 4095.');
    if (out.base_mode === 'fixed') {
      const la = num(lat.value), lo = num(lon.value), he = num(hgt.value);
      if (la === null || la < -90 || la > 90) fail('lat_deg', 'Latitude from -90 to 90.');
      if (lo === null || lo < -180 || lo > 180) fail('lon_deg', 'Longitude from -180 to 180.');
      if (he === null || he < -1000 || he > 10000) fail('height_m', 'Height from -1000 to 10000 m.');
      out.fixed = { lat_deg: la, lon_deg: lo, height_m: he };
    } else {
      out.survey_in_s = int(survey.value, 1, 3600);
      if (out.survey_in_s === null) fail('survey_in_s', 'From 1 to 3600 seconds.');
    }
    const seen = {};
    for (const row of rows.children) {
      const inputs = row.querySelectorAll('input');
      const m = int(inputs[0].value, 1001, 4095);
      const per = int(inputs[1].value, 1, 65535);
      if (m === null || per === null) {
        fail('rtcm', 'Message numbers 1001 to 4095, each with a period of at least 1 second.');
        break;
      }
      if (m >= 1101 && m <= 1107) {
        fail('rtcm', 'Messages 1101 to 1107 never reach a caster; remove them.');
        break;
      }
      if (seen[m]) {
        fail('rtcm', 'Message ' + m + ' is listed twice.');
        break;
      }
      seen[m] = true;
      out.rtcm.push({ msg: m, period_s: per });
    }
    if (out.rtcm.length > 16) fail('rtcm', 'At most 16 messages.');
    if (out.rtcm.some(r => EPHEMERIS_MSGS.indexOf(r.msg) >= 0) && out.com1_baud < 460800) {
      fail('com1_baud', 'Ephemeris messages (1019, 1020, 1042, 1045, 1046) need 460800 baud or more.');
    }
    if (bad) {
      /* A field in a closed disclosure (Other group numbers, Edit messages, Advanced): open it first. */
      for (let d = bad.closest ? bad.closest('details') : null; d; d = d.parentElement && d.parentElement.closest('details')) d.open = true;
      if (bad.focus) bad.focus();
      return null;
    }
    return out;
  }

  const err = errBox();
  const saveBtn = h('button', { type: 'submit', class: 'btn primary' }, 'Save profile');
  const applyBtn = btn('Apply again', async () => {
    const ok = await dialog({ title: 'Apply the profile again?', ok: 'Apply',
      body: h('p', null, 'The station configures the receiver again. Corrections pause for a few seconds.') });
    if (!ok) return;
    const r = await act(applyBtn, () => api('POST', '/api/gnss/apply'), err);
    if (r !== undefined) toast('Applying the profile…', 'ok');
  });
  const APPLY = {
    queued: 'Saved. The station configures the receiver now.',
    unchanged: 'Saved. Nothing changes on the receiver.',
    safe_mode: 'Saved. Applied after safe mode ends (with automatic configuration on).',
    maintenance: 'Saved. Applied after receiver maintenance ends (with automatic configuration on).',
    unsupported_model: 'Saved, but the station does not configure this receiver model.',
    not_running: 'Saved. Applied once the station can talk to the receiver.',
  };
  const form = h('form', { class: 'stack', oninput: () => {
    dirty = true;
    sumUp();
  }, onsubmit: async e => {
    e.preventDefault();
    const profile = collect();
    if (!profile) return;
    const r = await act(saveBtn, () => api('PUT', '/api/gnss/profile', { profile: profile, autocfg: autocfg.input.checked }), err);
    if (r === undefined) return;
    dirty = false;
    toast(APPLY[r.apply] || 'Saved.', r.apply === 'unsupported_model' ? 'warn' : 'ok');
    done();
  } },
  autocfg.el,
  pgroup('Signals', sgChoice.el,
    disclosure('Other group numbers', !named, h('div', { class: 'stack' }, sgCustom.el, sgNumbers))),
  pgroup('Position', mode.el,
    h('p', { class: 'muted small' }, 'For Onocoy: survey-in, then lock the result so the position stays the same after restarts. ' +
      'Onocoy finds the exact spot itself; a precise fixed position only matters if you use this base directly. ',
    extLink('Onocoy install guide', ONOCOY_INSTALL)),
    F.survey_in_s.el, fixedBox),
  pgroup('Corrections (RTCM messages)', summary,
    disclosure('Edit messages', false, h('div', { class: 'stack' },
      h('div', { class: 'msgrow muted small' }, h('span', null, 'Message'), h('span', null, 'Every (s)'), h('span', null, ''), h('span', null, '')),
      rows, rtcmErr,
      h('div', { class: 'row' },
        btn('Add message', () => {
          addRow('', 1).focus();
          dirty = true;
          sumUp();
        }, 'small'),
        btn('Use Onocoy defaults', () => {
          clear(rows);
          for (const m of ONOCOY_RTCM) addRow(m, 1);
          dirty = true;
          sumUp();
        }, 'small'))))),
  h('section', { class: 'pgroup' }, disclosure('Advanced', false, h('div', { class: 'stack' },
    h('div', { class: 'cols c2' }, F.station_id.el, F.com1_baud.el),
    satView.el,
    h('fieldset', { class: 'fieldset stack' }, h('legend', null, 'Antenna name for casters'),
      F.antenna_model.el, h('div', { class: 'cols c2' }, F.antenna_serial.el, F.antenna_setup_id.el),
      h('p', { class: 'hint' }, 'Serial and setup ID are only sent with a model.'), seeLine)))),
  err,
  h('div', { class: 'form-actions' }, h('span', { class: 'note muted small' }, 'Saving applies your changes. ' +
    'Apply again sends the saved profile once more.'), applyBtn, saveBtn));
  const update = gg => {
    patch(seeLine, castersSee(gg));
    seeLine.hidden = !seeLine.textContent;
  };
  update(g);
  return { el: form, dirty: () => dirty, update: update };
}

/* ---- Signal-group comparison --------------------------------------------- */

const SGT_MINUTES = [[15, '15 minutes'], [30, '30 minutes'], [60, '1 hour'], [120, '2 hours']];
const SGT_RUNNING = ['switching', 'settling', 'measuring', 'deciding', 'restoring'];

/* Hours left to choose, rounded up ("24 h"). */
const sgtHours = s => Math.max(1, Math.ceil((s || 0) / 3600)) + NB + 'h';

/** What a running comparison does now, in one line (null when idle or deciding). */
function sgtLine(t) {
  const grp = sgPairLabel(t.running_group);
  const left = 'about ' + inMinutes(Math.ceil((t.left_s || 0) / 60) * 60);
  const of = (t.phase !== null && t.phase !== undefined ? 'Group ' + (t.phase + 1) + ' of 2, ' : '') + grp;
  if (t.state === 'switching') return 'Switching the receiver to ' + grp + '. Corrections pause briefly.';
  if (t.state === 'settling') return of + ': settling, ' + left + '.';
  if (t.state === 'measuring') return of + ': measuring, ' + left + ' left.';
  if (t.state === 'restoring') return 'Switching back to ' + grp + '.' + (t.outcome ? ' ' + (L.sgtOutcome[t.outcome] || '') : '');
  return null;
}

/**
 * The comparison for the shell's notices (S.status, receiver.signal_test):
 * null, or [tone, title, text, actions], as noticeItems (20-shell.js) takes it.
 * A running test shows everywhere but the receiver page (its card is there);
 * a finished one only when the station ended it without the user's choice.
 */
function sgTestNotice(st) {
  const t = st && st.receiver ? st.receiver.signal_test : null;
  if (!t) return null;
  if (typeof shell !== 'undefined' && shell.current && shell.current.name === 'receiver') return null;
  const details = [link('Details', '#/receiver', 'btn small')];
  if (t.state === 'deciding') {
    return ['warn', 'Signal-group comparison: choose a group', 'Both groups are measured. Keep one on the Receiver page' +
      (t.decide_left_s ? '; with no choice the old group comes back in about ' + sgtHours(t.decide_left_s) : '') + '.',
    [link('Compare and choose', '#/receiver', 'btn small')]];
  }
  if (SGT_RUNNING.indexOf(t.state) >= 0) {
    /* The status names the group being measured "group". */
    return ['info', 'Comparing signal groups', sgtLine(Object.assign({}, t, { running_group: t.group })) +
      ' Other receiver changes wait until it ends.', details];
  }
  if (t.state === 'idle' && t.outcome && ['timed_out', 'rejected', 'failed', 'interrupted'].indexOf(t.outcome) >= 0) {
    return ['warn', 'Signal-group comparison ended', L.sgtOutcome[t.outcome] || t.outcome, details];
  }
  return null;
}

function sgPairLabel(g) {
  return g ? sgName(g.master, g.slave) : DASH;
}

/* One group's results side by side with the other's: per constellation; per band behind a disclosure. */
function sgtResults(t, open) {
  const res = t.results || [];
  if (res.length !== 2) return null;
  const sysOrder = ['gps', 'glonass', 'galileo', 'qzss', 'beidou', 'navic'];
  const bandOrder = ['l1', 'l2', 'l5', 'l6', 'e5ab'];
  const sysOf = (r, sys) => (r.systems || []).find(x => x.sys === sys);
  const present = sysOrder.filter(sys => res.some(r => sysOf(r, sys)));
  const head = ['', sgPairLabel(res[0].group), sgPairLabel(res[1].group)];
  const rows = [];
  const bandRows = [];
  rows.push(['Measured', ...res.map(r => r.status === 'rejected' ? h('span', { class: 't-bad' }, lbl(L.sgtResult, r.status)) :
    fmt.dur(r.measured_s) + ' (' + fmt.num(r.samples) + ' samples' + (r.epoch_gaps ? ', ' + fmt.num(r.epoch_gaps) + ' epochs missing' : '') + ')' +
    (r.status === 'measuring' ? ', measuring' : ''))]);
  rows.push(['Best constellation', ...res.map(r => r.best_sys ?
    h('b', { class: r.best_bands >= 4 ? 't-ok' : 't-warn' }, lbl(L.sys, r.best_sys) + ': ' + r.best_bands + ' bands (' +
      ((sysOf(r, r.best_sys) || {}).bands || []).map(b => BAND_HEAD[b] || b).join(', ') + ')') : DASH)]);
  for (const sys of present) {
    rows.push([h('b', null, lbl(L.sys, sys)), ...res.map(r => {
      const x = sysOf(r, sys);
      return x ? x.satellites.toFixed(1) + ' satellites · ' + (x.bands || []).length + ' band' + ((x.bands || []).length === 1 ? '' : 's') : DASH;
    })]);
    for (const band of bandOrder) {
      if (!res.some(r => (sysOf(r, sys) || {}).per_band && sysOf(r, sys).per_band.some(p => p.band === band))) continue;
      bandRows.push([lbl(L.sys, sys) + ' ' + (BAND_HEAD[band] || band), ...res.map(r => {
        const pb = ((sysOf(r, sys) || {}).per_band || []).find(p => p.band === band);
        if (!pb) return DASH;
        return [pb.satellites.toFixed(1) + ' sats', pb.cnr_dbhz === null || pb.cnr_dbhz === undefined ? '' :
          h('span', { class: cnrClass(pb.cnr_dbhz) }, ' · ' + pb.cnr_dbhz.toFixed(1) + ' dB-Hz'),
        h('small', { class: 'hint' }, sigNames(sys, pb.signal_ids))];
      })]);
    }
  }
  return h('div', { class: 'stack' }, table(head, rows, { label: 'Signal-group comparison results' }),
    bandRows.length ? moreAs('sgt-bands', 'Per band', open, table(head, bandRows, { label: 'Results per band' })) : null);
}

/** Why the station would refuse a start now (its own order), or '' (GET /api/gnss). The station still decides. */
function sgtBlock(g) {
  if (!g) return '';
  if (g.first_apply_held) return 'Configure the receiver first.';
  if (!g.autocfg) return 'Needs automatic configuration on.';
  if ((g.maintenance || {}).on) return 'Not during receiver maintenance.';
  if (g.state === 'no_receiver' || g.state === 'detecting') return 'Needs a connected receiver.';
  if (!(g.position || {}).locked) return 'Lock the base position first.';
  return '';
}

/**
 * "Compare two signal groups": the start form (two groups, time per group,
 * a confirmation), the progress, the results side by side, keep and stop.
 * Built once per view; update(t, g) shows GET /api/gnss/signal-test.
 * nodes: the card's children.
 */
function sgTestCtl(done) {
  const out = errBox();
  const status = h('div');
  const results = h('div');
  let resultsKey = '';
  const opts = SG_CHOICES.map(c => [c[0], sgName(...c[0].split(' '))]);
  const selA = selectInput(opts, '3 6');
  const selB = selectInput(opts, '7 0');
  const minutes = selectInput(SGT_MINUTES, 30);
  let defaultsSet = false;
  const pair = v => {
    const p = String(v).split(' ');
    return { master: Number(p[0]), slave: Number(p[1]) };
  };
  const startBtn = btn('Start comparison', async () => {
    if (selA.value === selB.value) {
      say(out, 'Choose two different signal groups.');
      return;
    }
    const mins = Number(minutes.value);
    const ok = await dialog({ title: 'Compare two signal groups?', ok: 'Start',
      body: [h('p', null, 'The station measures ' + sgName(...selA.value.split(' ')) + ' and ' + sgName(...selB.value.split(' ')) +
        ' for ' + lbl(Object.fromEntries(SGT_MINUTES), mins) + ' each, then waits up to 24 hours for your choice.'),
      msg('warn', 'Each switch restarts the receiver: corrections pause up to a minute, up to three times.'),
      h('p', { class: 'muted small' }, 'With no choice, the group in use now comes back. Other receiver changes wait until it ends.')] });
    if (!ok) return;
    const r = await act(startBtn, () => api('POST', '/api/gnss/signal-test',
      { groups: [pair(selA.value), pair(selB.value)], phase_s: mins * 60 }), out);
    if (r !== undefined) {
      toast('Comparison started.', 'ok');
      done();
    }
  }, 'primary');
  /* Why a start would be refused now (no receiver, held, automatic configuration off, not locked): Start waits. */
  const why = h('span', { class: 'note muted small', id: 'sgt-why', hidden: true });
  const form = h('div', { class: 'stack' },
    h('div', { class: 'cols c3' }, field('Group A', selA).el, field('Group B', selB).el, field('Time per group', minutes).el),
    h('div', { class: 'form-actions' }, why, startBtn));
  const keepA = btn('Keep A', async () => keep(0, keepA), 'primary');
  const keepB = btn('Keep B', async () => keep(1, keepB), 'primary');
  async function keep(i, b) {
    const r = await act(b, () => api('POST', '/api/gnss/signal-test/keep', { group: i }), out);
    if (r !== undefined) {
      toast('Keeping the group.', 'ok');
      done();
    }
  }
  const stopBtn = btn('Stop', async () => {
    const ok = await dialog({ title: 'Stop the comparison?', ok: 'Stop', okKind: 'danger solid',
      body: h('p', null, 'The station goes back to the signal group from before the comparison.') });
    if (!ok) return;
    const r = await act(stopBtn, () => api('DELETE', '/api/gnss/signal-test'), out);
    if (r !== undefined) {
      toast('Comparison stopped.', 'ok');
      done();
    }
  }, 'danger');
  const badgeEl = badge('', '');
  return {
    nodes: [
      cardHead('Compare two signal groups', badgeEl),
      h('p', { class: 'muted small' }, 'Tries two signal groups one after the other on this antenna, so you can keep the better one.'),
      status, results, form, out, h('div', { class: 'form-actions' }, stopBtn, keepA, keepB)],
    update(t, g) {
      if (!t) return;
      const running = SGT_RUNNING.indexOf(t.state) >= 0;
      badgeEl.className = 'badge ' + (t.state === 'deciding' ? 'warn' : 'info');
      badgeEl.textContent = lbl(L.sgtState, t.state);
      badgeEl.hidden = !running;
      form.hidden = running;
      const block = running ? '' : sgtBlock(g);
      /* A start in flight keeps its own disabled state (busy() gives it back when the answer is in). */
      if (!startBtn.classList.contains('busy')) startBtn.disabled = !!block;
      why.textContent = block;
      why.hidden = !block;
      if (block) startBtn.setAttribute('aria-describedby', why.id);
      else startBtn.removeAttribute('aria-describedby');
      if (!running && !defaultsSet && g && g.profile) {
        /* Default: the group running now, and 7 0 (or 3 6 when 7 0 runs). */
        const now = g.profile.signal_group_master + ' ' + g.profile.signal_group_slave;
        if (SG_CHOICES.some(c => c[0] === now)) selA.value = now;
        selB.value = now === '7 0' ? '3 6' : '7 0';
        defaultsSet = true;
      }
      const groups = t.groups || [];
      keepA.hidden = keepB.hidden = t.state !== 'deciding';
      if (groups.length === 2) {
        keepA.textContent = 'Keep ' + sgName(groups[0].master, groups[0].slave);
        keepB.textContent = 'Keep ' + sgName(groups[1].master, groups[1].slave);
      }
      stopBtn.hidden = !running || t.state === 'restoring';
      let text = null;
      const line = sgtLine(t);
      if (line) text = msg('info', line);
      else if (t.state === 'deciding') text = msg('warn', h('strong', null, 'Both groups measured: keep one.'), ' Until then the receiver ' +
        'stays on ' + sgPairLabel(t.running_group) + '. With no choice within ' + sgtHours(t.decide_left_s) + ', ' +
        sgPairLabel(t.before) + ' comes back.');
      else if (t.outcome) text = msg(t.outcome === 'kept' || t.outcome === 'cancelled' ? 'ok' : 'warn',
        h('strong', null, t.outcome === 'kept' && t.kept ? 'You kept ' + sgPairLabel(t.kept) + '.' : (L.sgtOutcome[t.outcome] || t.outcome)),
        t.reason && t.outcome !== 'kept' ? ' ' + t.reason : '', t.ended_unix ? h('span', { class: 'muted' }, ' (' + fmt.time(t.ended_unix) + ')') : null);
      mount(status, text);
      const rk = JSON.stringify(t.results || null);
      if (rk !== resultsKey) {
        resultsKey = rk;
        mount(results, t.results ? sgtResults(t, isOpen(results, 'sgt-bands')) : null);
      }
    },
  };
}

/** The receiver page's own note while a comparison runs or waits for a choice: [title, line] or null. */
function sgtPageNote(t) {
  if (!t || SGT_RUNNING.indexOf(t.state) < 0) return null;
  if (t.state === 'deciding') return ['Signal-group comparison: choose a group', 'Both groups are measured.'];
  return ['Comparing signal groups', sgtLine(t)];
}

Views.receiver = {
  title: 'Receiver',
  admin: true,
  create(el) {
    let g = null, t = null, editor = null, stop = null, held = null, heldLater = false, infoMode = '';
    const heldCard = h('section', { class: 'card wide', hidden: true });
    const info = h('section', { class: 'card' });
    /* No receiver, or the station looking for it: the same panel as on Signals and in the setup. */
    const nr = noReceiverPanel();
    const posActions = positionActions(() => refresh());
    const position = h('section', { class: 'card' }, posActions.el);
    const profChip = badge('', '');
    const profile = h('section', { class: 'card wide' });
    const recovery = recoveryCtl();
    const recoveryCard = h('section', { class: 'card has-actions' }, recovery.nodes);
    const maint = maintenanceCtl(() => refresh().catch(() => {}));
    const maintCard = h('section', { class: 'card has-actions' }, maint.nodes);
    const sgt = sgTestCtl(() => refresh().catch(() => {}));
    const sgtCard = h('section', { class: 'card wide', id: 'compare', tabindex: '-1' }, sgt.nodes);
    const noteTitle = h('strong'), noteLine = h('span');
    const sgtNote = h('div', { class: 'notice info wide', hidden: true },
      h('div', { class: 'text' }, noteTitle, noteLine),
      h('div', { class: 'form-actions' }, btn('Go to the comparison', () => {
        sgtCard.scrollIntoView({ behavior: 'smooth', block: 'start' });
        sgtCard.focus({ preventScroll: true });
      }, 'small')));

    const fill = () => {
      /* The held first configuration first, until it is made (or put off for this visit). */
      if (g.first_apply_held && !heldLater) {
        if (!held) {
          held = heldPanel({
            onLater: () => {
              heldLater = true;
              heldCard.hidden = true;
            },
            onApplied: () => refresh().catch(() => {}),
          });
          mount(heldCard, held.el);
          held.load();
        }
        heldCard.hidden = false;
      } else {
        heldCard.hidden = true;
        if (!g.first_apply_held) finishHeld(g).then(done => done && refresh(), () => {});
      }
      const note = sgtPageNote(t);
      sgtNote.hidden = !note;
      if (note) {
        sgtNote.className = 'notice wide ' + (t.state === 'deciding' ? 'warn' : 'info');
        noteTitle.textContent = note[0];
        noteLine.textContent = note[1];
      }
      if ((g.state === 'no_receiver' || g.state === 'detecting') && !(g.maintenance || {}).on) {
        if (infoMode !== 'nr') mount(info, h('h2', null, 'Receiver'), nr.el);
        infoMode = 'nr';
        nr.update(S.status || { receiver: { state: g.state } });
      } else if (infoMode !== 'rx' || !interacting(info)) {
        /* A rebuild waits while the user reads or selects in the card; the Details stay as the user left them. */
        mount(info, receiverInfo(g, isOpen(info, 'rx-details')));
        infoMode = 'rx';
      }
      /* Everything in the position card but its actions, which stay put. */
      if (!interacting(position)) {
        const open = isOpen(position, 'pos-details');
        while (position.firstChild && position.firstChild !== posActions.el) position.firstChild.remove();
        position.insertBefore(add(doc.createDocumentFragment(), positionInfo(g, open)), posActions.el);
      }
      posActions.update(g);
      const c = profileChip(g);
      profChip.className = 'badge ' + c[1];
      profChip.textContent = c[0];
      if (editor) editor.update(g);
      maint.update(g);
      recovery.update(g);
      sgt.update(t, g);
    };
    const buildEditor = () => {
      editor = profileEditor(g, refresh);
      mount(profile, cardHead('Receiver profile', profChip),
        h('p', { class: 'muted small' }, 'What the station sets on the receiver.'),
        editor.el);
    };
    /* The comparison's state; a failed read only leaves its card as it was. */
    const readTest = () => api('GET', '/api/gnss/signal-test').then(v => {
      t = v;
    }, e => {
      if (e instanceof ApiError && e.status === 401) throw e;
    });
    async function refresh() {
      g = await api('GET', '/api/gnss');
      await readTest();
      fill();
    }
    async function load() {
      g = await api('GET', '/api/gnss');
      await readTest();
      mount(el, h('div', { class: 'grid' }, heldCard, sgtNote, info, position, profile, recoveryCard, maintCard, sgtCard));
      buildEditor();
      fill();
    }
    mount(el, h('p', { class: 'loading' }, 'Loading the receiver…'));
    load().then(() => {
      stop = poller(5000, refresh, true);
    }, e => mount(el, msg('bad', errText(e)), btn('Try again', () => openView('receiver', ''), 'primary')));
    return {
      update(st) {
        if (infoMode === 'nr') nr.update(st);
      },
      destroy() {
        if (stop) stop();
      },
      dirty: () => !!(editor && editor.dirty()) || recovery.dirty(),
      reload() {
        if (!g) return;
        refresh().then(() => {
          if (!(editor && editor.dirty())) buildEditor();
        }).catch(() => {});
      },
    };
  },
};
