/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver: identity and link, the settings read back at the last
 * configuration, base position (survey, lock, survey again), what the
 * receiver publishes in RTCM 1033, receiver maintenance (hands off), the
 * automatic recovery mode, the comparison of two signal groups, and the
 * base profile editor (signal groups by name, base mode, station ID, COM1
 * baud, RTCM messages, satellite-in-view output, antenna descriptor). Field
 * rules mirror PUT /api/gnss/profile; the station's own validation has the
 * last word.
 */

const RTCM_NAMES = {
  1005: 'Station position', 1006: 'Station position and height', 1007: 'Antenna name', 1008: 'Antenna name and serial',
  1013: 'System parameters', 1033: 'Receiver and antenna names', 1019: 'GPS ephemeris', 1020: 'GLONASS ephemeris',
  1042: 'BeiDou ephemeris', 1044: 'QZSS ephemeris', 1045: 'Galileo F/NAV ephemeris', 1046: 'Galileo I/NAV ephemeris',
  1074: 'GPS MSM4', 1075: 'GPS MSM5', 1077: 'GPS MSM7', 1084: 'GLONASS MSM4', 1087: 'GLONASS MSM7', 1094: 'Galileo MSM4',
  1097: 'Galileo MSM7', 1114: 'QZSS MSM4', 1117: 'QZSS MSM7', 1124: 'BeiDou MSM4', 1127: 'BeiDou MSM7', 1137: 'NavIC MSM7',
  1230: 'GLONASS code-phase biases',
};
const EPHEMERIS_MSGS = [1019, 1020, 1042, 1045, 1046];
const ONOCOY_RTCM = [1005, 1033, 1077, 1087, 1097, 1117, 1127, 1230];
const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];

function rtcmName(n) {
  if (RTCM_NAMES[n]) return RTCM_NAMES[n];
  if (n >= 1101 && n <= 1107) return 'SBAS MSM (not sent to casters)';
  return '';
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
      body: h('p', null, 'The station switches the receiver to a fixed base at the latest surveyed position and configures it again. ' +
        'The RTCM stream can pause for a few seconds.') });
    if (!ok) return;
    const r = await act(lockBtn, () => api('POST', '/api/gnss/position/lock'), out);
    if (r !== undefined) {
      toast('Locking the position…', 'ok');
      done();
    }
  }, 'primary', { hidden: true });
  const resurveyBtn = btn('Forget it and survey again', async () => {
    const ok = await dialog({ title: 'Survey the position again?', ok: 'Survey again', okKind: 'danger solid',
      body: [h('p', null, 'The locked position is forgotten and the receiver surveys its position again.'),
        msg('warn', 'The reported antenna position moves. Casters that check it, such as Onocoy, may validate the station again.')],
      check: 'I understand that the reported position changes.' });
    if (!ok) return;
    const r = await act(resurveyBtn, () => api('DELETE', '/api/gnss/position/lock'), out);
    if (r !== undefined) {
      toast('Surveying again…', 'ok');
      done();
    }
  }, 'danger', { hidden: true });
  return {
    el: h('div', null, h('div', { class: 'form-actions' }, lockBtn, resurveyBtn), out),
    update(g) {
      const pos = g.position || {};
      const arp = g.arp || {};
      lockBtn.hidden = !(!pos.locked && (arp.valid || (pos.source && pos.source !== 'none')));
      resurveyBtn.hidden = !pos.locked;
    },
  };
}

/* The receiver's settings as read back at the last configuration, and the antenna and B1C/B2a steps of it. */
function readBack(g) {
  const rs = g.rx_settings || {};
  const steps = g.apply_steps || {};
  const a = steps.antenna || {}, b = steps.b1cb2a || {};
  const when = rs.valid ? ' (read ' + (rs.read_unix ? 'at ' + fmt.time(rs.read_unix) : fmt.dur(rs.age_s) + ' ago') + ')' : '';
  const ant = rs.antenna;
  return [
    kv([
      ['BeiDou B1C/B2a in RTCM', lbl(L.setting, rs.b1cb2a || 'unknown') + when],
      ['Multipath mitigation', lbl(L.setting, rs.mmp || 'unknown')],
      ['Elevation mask', rs.mask_deg === null || rs.mask_deg === undefined ? 'unknown' : Number(rs.mask_deg).toFixed(1) + '°'],
      ['Antenna (receiver)', ant ? [ant.model ? h('span', { class: 'ant' }, ant.model) : h('span', { class: 'muted' }, 'none'),
        ant.serial ? ' · serial ' + ant.serial : '', ' · setup ID ' + ant.setup_id] : 'unknown'],
      a.sent ? ['Antenna descriptor sent', a.ok ? h('span', { class: 't-ok' }, 'accepted') :
        h('span', { class: 't-bad' }, 'antenna model rejected: ' + (a.reply || 'no reason given'))] : null,
      b.sent ? ['B1C/B2a turned on', b.ok ? h('span', { class: 't-ok' }, 'accepted') :
        h('span', { class: 't-bad' }, 'rejected: ' + (b.reply || 'no reason given'))] : null,
    ]),
    rs.valid ? null : h('p', { class: 'muted small' }, 'Unknown until the station next configures the receiver: it reads these settings back then ' +
      '(nothing is kept across a restart).'),
  ];
}

/* Constellation names of the signal report (GET /api/gnss rx_bands). */
const BAND_SYS = { gps: 'GPS', glonass: 'GLONASS', galileo: 'Galileo', beidou: 'BeiDou', qzss: 'QZSS' };

/**
 * What the running signal group configures on the main antenna input, from
 * the last configuration (rx_bands), short: bands per constellation, the
 * signals on hover, and what is missing against the profile's group.
 */
function bandsRow(g) {
  const b = g.rx_bands || {};
  if (b.state === 'refused') return ['Signals configured', h('span', { class: 'muted' }, 'not reported by this receiver firmware')];
  if (b.state === 'unread') return ['Signals configured', h('span', { class: 'muted' }, 'answer not read (unknown)')];
  if (b.state !== 'ok') return ['Signals configured', h('span', { class: 'muted' }, 'read at the next configuration')];
  const parts = (b.systems || []).filter(x => (x.signals || []).length).map(x =>
    h('span', { title: (BAND_SYS[x.sys] || x.sys) + ': ' + x.signals.join(', ') },
      (BAND_SYS[x.sys] || x.sys) + ' ' + x.bands));
  const list = [];
  parts.forEach((p, i) => list.push(i ? ' · ' : '', p));
  return ['Signals configured (bands)', [list, ' (group ' + b.group + ')']];
}

/* The signals the profile's group should give and the report lacks, or null. */
function bandsMissingMsg(g) {
  const b = g.rx_bands || {};
  if (b.state !== 'ok' || b.matches !== false) return null;
  const lack = (b.systems || []).filter(x => (x.missing || []).length)
    .map(x => (BAND_SYS[x.sys] || x.sys) + ' ' + x.missing.join(', '));
  return msg('warn', h('strong', null, 'The receiver reports signal group ' + b.group + ', not what group ' + b.profile_master +
    ' should configure.'), lack.length ? ' Missing: ' + lack.join('; ') + '.' : '');
}

function receiverInfo(g) {
  const r = g.receiver || {};
  const link = g.link || {};
  const alerts = g.alerts || [];
  const maint = (g.maintenance || {}).on;
  return [
    h('h2', null, 'Receiver', maint ? badge('Maintenance', 'warn') : badge(lbl(L.station, g.state), stationTone(g.state))),
    r.valid === false ? h('p', { class: 'muted' }, 'The receiver has not identified itself yet.') : null,
    kv([
      ['Model', r.model],
      ['Firmware', r.firmware ? r.firmware + (r.build ? ' (build ' + r.build + ')' : '') : DASH],
      ['Serial number', r.serial],
      ['Part number', r.pn],
      ['Chip ID', r.efuse_id],
      ['Firmware date', r.compile_date],
      ['Answers commands', g.restarting ? h('span', { class: 't-warn' }, 'Restarting (it said so; it answers again in about 10 s)') :
        fmt.yesno(g.responding)],
      ['Identity confirmed', fmt.yesno(g.identity_confirmed)],
    ]),
    h('h3', { class: 'section-title' }, 'Link and configuration'),
    kv([
      /* Until the receiver streams, the station listens on the link (and searches between): no "Streaming" claim. */
      ['Link', g.state === 'streaming' ? lbl(L.linkMode, link.mode) + (link.baud ? ' at ' + link.baud + '\u00a0baud' : '') :
        'Listening' + (link.baud ? ' at ' + link.baud + '\u00a0baud' : '')],
      ['Last data', link.rx_bytes === 0 ? 'No data from the receiver yet' :
        link.rx_age_ms === null || link.rx_age_ms === undefined ? 'never' : fmt.ms(link.rx_age_ms) + ' ago'],
      ['Last RTCM', h('span', { class: 't-' + ageTone(link.rtcm_age_ms) },
        link.rtcm_age_ms === null || link.rtcm_age_ms === undefined ? 'never' : fmt.ms(link.rtcm_age_ms) + ' ago')],
      link.open === false && link.mode !== 'off' ? ['Receiver UART', h('span', { class: 't-bad' }, 'closed (reopening)' +
        (link.open_failures ? ', ' + link.open_failures + ' failed opens' : ''))] : null,
      link.hands_off ? ['Sending to the receiver', h('span', { class: 't-warn' }, 'off (receiver maintenance)')] : null,
      ['Automatic configuration', g.autocfg ? 'On' : 'Off'],
      ['Profile applied', g.profile_applied ? 'Yes' : h('span', { class: 't-warn' }, 'No')],
      ['Message 1230', g.rtcm1230_supported === null || g.rtcm1230_supported === undefined ? 'Not applied yet' :
        g.rtcm1230_supported ? 'Accepted by the receiver' : h('span', { class: 't-warn' }, 'Refused by the receiver')],
      g.config_failed ? ['Last configuration', h('span', { class: 't-bad' }, 'Failed' + (g.config_auto_retry === false ?
        '; not retried while automatic configuration is off' :
        g.config_retry_in_s ? '; next try in ' + fmt.dur(g.config_retry_in_s) : ''))] : null,
      g.detect_failures ? ['Detection failures', fmt.num(g.detect_failures)] : null,
      ['Signal group (receiver)', g.rx_signal_group ? sgName(g.rx_signal_group.master, g.rx_signal_group.slave) :
        h('span', { class: 'muted' }, 'read at the next configuration')],
      bandsRow(g),
    ]),
    sgRefusedMsg(g),
    bandsMissingMsg(g),
    h('h3', { class: 'section-title' }, 'Settings read back'),
    readBack(g),
    alerts.length ? h('div', { class: 'stack foot' }, alerts.map(a => {
      const t = L.salert[a] || [a, ''];
      const text = a === 'config_failed' ? t[1] + ' ' + (g.config_auto_retry === false ? L.configRetry.user : L.configRetry.auto) : t[1];
      return msg('warn', h('strong', null, t[0]), text ? h('div', { class: 'small' }, text) : null);
    })) : null,
  ];
}

/** A signal group the receiver refused, in plain words (GET /api/gnss signal_group_refused), or null. */
function sgRefusedMsg(g) {
  const r = g.signal_group_refused;
  if (!r) return null;
  const why = r.reply && r.reply !== 'signal group rejected' ? ' (it answered "' + r.reply + '")' : '';
  return msg('bad', h('strong', null, 'This receiver firmware does not accept signal group ' + r.master + ' ' + r.slave + why + '.'),
    ' Build 25164 accepts only ' + SG_KNOWN_PAIRS + '.',
    r.back_to ? ' It went back to ' + sgName(r.back_to.master, r.back_to.slave) + ' and was configured again. ' +
      'Saving the profile clears this note.' :
      ' The group it ran before is not known, so the configuration counts as failed.');
}

/* What the receiver publishes in its antenna and receiver descriptor (RTCM 1033), as received. */
function rtcm1033Info(g) {
  const d = g.rtcm1033 || {};
  return [
    h('h2', null, 'Published in RTCM 1033', d.valid ? badge('Received', 'ok') : badge('Not yet', '')),
    d.valid ? kv([
      ['Station ID', String(d.station_id)],
      ['Antenna', d.antenna ? h('span', { class: 'ant' }, d.antenna) : h('span', { class: 'muted' }, 'empty')],
      ['Antenna setup ID', String(d.setup_id)],
      ['Antenna serial', d.antenna_serial || DASH],
      ['Receiver type', d.receiver_type || DASH],
      ['Receiver firmware', d.receiver_fw || DASH],
      ['Receiver serial', d.receiver_serial || DASH],
      ['Received', d.age_ms === null || d.age_ms === undefined ? DASH : fmt.ms(d.age_ms) + ' ago'],
    ]) : h('p', { class: 'muted' }, 'No antenna and receiver descriptor (message 1033) received yet.'),
    d.bad_frames ? h('p', { class: 'small t-warn' }, fmt.num(d.bad_frames) + ' message' + (d.bad_frames === 1 ? '' : 's') +
      ' 1033 could not be decoded since the start.') : null,
    h('p', { class: 'muted small foot' }, 'What casters and rovers read about the antenna and the receiver. The antenna fields of the profile below change it.'),
  ];
}

/**
 * Receiver maintenance: start (with its duration), extend, end now. Built
 * once per view so a pending request keeps its button; update(g) shows the
 * state. done() refreshes the page.
 */
function maintenanceCtl(done) {
  const out = errBox();
  const badgeEl = badge('', '');
  const status = h('p');
  const dur = selectInput(MAINT_CHOICES, 1800, { 'aria-label': 'Duration' });
  const durField = field('Duration', dur);
  const label = () => lbl(Object.fromEntries(MAINT_CHOICES), dur.value);
  const startBtn = btn('Start maintenance', async () => {
    const ok = await dialog({ title: 'Start receiver maintenance?', ok: 'Start',
      body: [h('p', null, 'For ' + label() + ' the station sends nothing to the receiver: no configuration, no checks, no recovery steps, ' +
        'no updates. The receiver\'s RTCM still reaches the casters.'),
      h('p', { class: 'muted' }, 'It ends by itself, with End now, or with a power cut; then the station checks the receiver as at start-up. ' +
        'The time counts as planned in the availability record.'),
      msg('warn', MAINT_SAVE_NOTE)] });
    if (!ok) return;
    const r = await act(startBtn, () => api('POST', '/api/gnss/maintenance', { duration_s: Number(dur.value) }), out);
    if (r !== undefined) {
      toast('Receiver maintenance on: nothing is sent to the receiver.', 'ok');
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
  });
  return {
    el: h('div', null,
      h('h2', null, 'Receiver maintenance', badgeEl),
      h('p', { class: 'muted small' }, 'Hands off while you work on the receiver through its other port, for example with a PC tool ' +
        'or a firmware update there: the station then sends it nothing, and the casters still get its RTCM. ' + MAINT_SAVE_NOTE),
      status, durField.el, h('div', { class: 'form-actions' }, startBtn, extendBtn, endBtn), out),
    update(g) {
      const m = g.maintenance || {};
      badgeEl.className = 'badge ' + (m.on ? 'warn' : '');
      badgeEl.textContent = m.on ? 'On' : 'Off';
      startBtn.hidden = !!m.on;
      extendBtn.hidden = endBtn.hidden = !m.on;
      status.textContent = m.on ? 'On: about ' + fmt.dur(Math.ceil((m.left_s || 0) / 60) * 60) + ' left (' + fmt.dur(m.chosen_s) +
        ' in all). Nothing is sent to the receiver.' + (m.max_left_s ? ' Adding time keeps at most ' + fmt.dur(m.max_left_s) + ' left.' : '') : '';
      status.hidden = !m.on;
    },
  };
}

/** Automatic receiver recovery (gnss/recovery). Returns {el, update(g), dirty()}. */
function recoveryCtl() {
  const out = errBox();
  let dirty = false, current = 'full';
  const opts = ['full', 'no_reboot', 'alerts_only'].map(k => [k, h('span', { class: 'opt' }, h('span', null, L.recovery[k][0]),
    h('small', { class: 'hint' }, L.recovery[k][1]))]);
  const choice = radios('When the receiver stalls', 'recovery', opts, current);
  choice.el.querySelector('.choices').classList.add('stacked');
  choice.onchange(() => {
    dirty = choice.value() !== current;
  });
  const save = btn('Save', async () => {
    const v = choice.value();
    const r = await act(save, () => api('PUT', '/api/gnss/profile', { recovery: v }), out);
    if (r === undefined) return;
    current = v;
    dirty = false;
    toast('Automatic receiver recovery: ' + L.recovery[v][0] + '.', 'ok');
  }, 'primary');
  return {
    el: h('div', null,
      h('h2', null, 'Automatic receiver recovery'),
      h('p', { class: 'muted small' }, 'What the station does on its own when the receiver stops sending RTCM. A step already running completes.'),
      choice.el, out, h('div', { class: 'form-actions' }, save)),
    update(g) {
      if (!dirty && g.recovery) {
        current = g.recovery;
        choice.set(current);
      }
    },
    dirty: () => dirty,
  };
}

function positionInfo(g) {
  const pos = g.position || {};
  const arp = g.arp || {};
  const sv = g.survey || {};
  const prof = g.profile || {};
  /* A fixed position saved in the profile but not applied yet (no receiver, or not configured since): show it as the profile's. */
  if (prof.base_mode === 'fixed' && prof.fixed && !(pos.locked && pos.source && pos.source !== 'none')) {
    const f = prof.fixed;
    return [
      h('h2', null, 'Base position', badge('From the profile', 'info')),
      msg('info', 'Fixed position from the profile: ' + fmt.deg(f.lat_deg) + ', ' + fmt.deg(f.lon_deg) + ', ' + fmt.m(f.height_m) +
        ', sent to the receiver when it is configured.'),
      kv([
        ['Latitude', fmt.deg(f.lat_deg)],
        ['Longitude', fmt.deg(f.lon_deg)],
        ['Height (ellipsoid)', fmt.m(f.height_m)],
        ['Source', 'Entered by you (receiver profile)'],
      ]),
      h('h3', { class: 'section-title' }, 'Position in the RTCM stream'),
      arp.valid ? kv([
        ['Latitude', fmt.deg(arp.lat_deg)],
        ['Longitude', fmt.deg(arp.lon_deg)],
        ['Height (ellipsoid)', fmt.m(arp.height_m)],
        ['Received', fmt.ms(arp.age_ms) + ' ago'],
      ]) : h('p', { class: 'muted' }, 'No position message (1005/1006) received yet.'),
    ];
  }
  let state, tone;
  if (pos.locked) {
    state = 'Locked';
    tone = 'ok';
  } else if (sv.active) {
    state = 'Surveying';
    tone = 'warn';
  } else {
    state = 'Not locked';
    tone = '';
  }
  const offTone = arp.offset_m === null || arp.offset_m === undefined ? '' : arp.offset_m > 0.005 ? 't-warn' : 't-ok';
  return [
    h('h2', null, 'Base position', badge(state, tone)),
    sv.active ? msg('info', 'The receiver is surveying its position' + (sv.window_left_s ? ': about ' + fmt.dur(sv.window_left_s) + ' left.' : '.')) : null,
    pos.source && pos.source !== 'none' ? h('h3', { class: 'section-title' }, pos.locked ? 'Locked position' : 'Last surveyed position') : null,
    pos.source && pos.source !== 'none' ? kv([
      ['Latitude', fmt.deg(pos.lat_deg)],
      ['Longitude', fmt.deg(pos.lon_deg)],
      ['Height (ellipsoid)', fmt.m(pos.height_m)],
      ['Source', lbl(L.posSource, pos.source)],
      pos.sigma_m ? ['Accuracy (σ)', fmt.m(pos.sigma_m)] : null,
      pos.locked_unix ? ['Locked on', fmt.time(pos.locked_unix)] : null,
    ]) : h('p', { class: 'muted' }, 'No surveyed position yet.'),
    h('h3', { class: 'section-title' }, 'Position in the RTCM stream'),
    arp.valid ? kv([
      ['Latitude', fmt.deg(arp.lat_deg)],
      ['Longitude', fmt.deg(arp.lon_deg)],
      ['Height (ellipsoid)', fmt.m(arp.height_m)],
      ['Station ID', arp.station_id],
      ['Received', fmt.ms(arp.age_ms) + ' ago'],
      arp.offset_m === null || arp.offset_m === undefined ? null : ['Distance to locked', h('span', { class: offTone }, fmt.m(arp.offset_m))],
    ]) : h('p', { class: 'muted' }, 'No position message (1005/1006) received yet.'),
  ];
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
 * build that holds it): what would be sent, the choice of automatic
 * configuration afterwards, "Configure the receiver now" and "Not now".
 * opts.onLater: "Not now"; opts.onApplied: the apply was accepted. Returns
 * {el, load()}.
 */
function heldPanel(opts) {
  const err = errBox();
  const status = h('div', { hidden: true });
  const list = h('div', null, h('p', { class: 'muted small' }, 'Reading what the station would send…'));
  const auto = checkbox('Then keep it configured and recover it automatically (recommended)', true,
    'Turns automatic configuration and full receiver recovery on once the first configuration has worked.');
  let preview = null;
  let watching = false;
  const load = async () => {
    try {
      preview = await api('GET', '/api/gnss/apply/preview');
      mount(list, previewList(preview));
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
        show('bad', 'The configuration failed and stays held: the station does not try again on its own. The receiver ' +
          'page shows why; press Configure the receiver now to try again.');
        break;
      }
      show('info', g.state === 'streaming' || g.state === 'configuring' ? 'Configuring the receiver…' :
        'Queued: the station configures the receiver as soon as it streams (now: ' + lbl(L.station, g.state).toLowerCase() + ').');
    }
    watching = false;
  };
  const confirmBtn = btn('Configure the receiver now', async () => {
    if (!preview) await load();
    const ok = await dialog({ title: 'Configure the receiver now?', ok: 'Configure',
      body: [h('p', null, 'The station sends these commands to the receiver, the conditional ones only where it differs, and ends with ' +
        'SAVECONFIG, which saves the receiver\'s whole running configuration. The RTCM stream pauses for a few seconds.'),
      previewList(preview),
      auto.input.checked ? h('p', { class: 'muted small' }, 'Once it has worked, automatic configuration and full recovery are turned on.') :
        h('p', { class: 'muted small' }, 'Automatic configuration stays off: the station changes nothing on the receiver by itself afterwards.')] });
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
      h('h2', null, 'Receiver not configured yet', badge('Held', 'info')),
      h('p', null, 'This build holds the receiver\'s first configuration until you confirm it. So far the station has only asked the ' +
        'receiver for its version and settings; it has sent nothing that changes it. When you confirm, it sends the commands below ' +
        '(the conditional ones only where the receiver differs) and ends with SAVECONFIG, which saves the receiver\'s whole running ' +
        'configuration.'),
      list, auto.el, status, err,
      h('div', { class: 'form-actions' }, confirmBtn, laterBtn)),
    load: load,
  };
}

/* ---- Profile editor ----------------------------------------------------- */

function profileEditor(g, done) {
  const p = Object.assign({ signal_group_master: 3, signal_group_slave: 6, base_mode: 'survey_in', survey_in_s: 600, fixed: null,
    station_id: 0, com1_baud: 115200, rtcm: [], sat_view_output: false }, g.profile || {});
  let dirty = false;
  const autocfg = checkbox('Configure the receiver automatically', g.autocfg,
    'The station applies this profile whenever the receiver\'s settings differ from it.');
  const sgm = numInput(p.signal_group_master, { inputmode: 'numeric', maxlength: 3 });
  const sgs = numInput(p.signal_group_slave, { inputmode: 'numeric', maxlength: 3 });
  /* Named choices; other pairs (a newer receiver firmware) as numbers under "Advanced". */
  const sgKey = p.signal_group_master + ' ' + p.signal_group_slave;
  const named = SG_CHOICES.some(c => c[0] === sgKey);
  const sgChoice = radios('Signal group', 'sgroup', SG_CHOICES.map(c => [c[0], h('span', { class: 'opt' }, h('span', null, c[1]),
    h('small', { class: 'hint' }, c[2]))]), named ? sgKey : null);
  sgChoice.el.querySelector('.choices').classList.add('stacked');
  const sgCustom = checkbox('Advanced: custom numbers', !named, 'Only for a receiver firmware that offers other groups. Build 25164 ' +
    'accepts exactly ' + SG_KNOWN_PAIRS + ' and refuses any other pair (the station then goes back to the group it ran).');
  const mode = radios('Base mode', 'basemode', [['survey_in', 'Survey-in'], ['fixed', 'Fixed position']], p.base_mode);
  const survey = numInput(p.survey_in_s || 600, { inputmode: 'numeric', maxlength: 4 });
  const fx = p.fixed || {};
  const lat = numInput(fx.lat_deg, { inputmode: 'text', placeholder: '45.123456789' });
  const lon = numInput(fx.lon_deg, { inputmode: 'text', placeholder: '-75.123456789' });
  const hgt = numInput(fx.height_m, { inputmode: 'text', placeholder: '85.1234' });
  const sid = numInput(p.station_id, { inputmode: 'numeric', maxlength: 4 });
  const baud = selectInput(BAUDS.map(b => [b, b + ' baud']), p.com1_baud);
  const satView = checkbox('Satellite-in-view output', p.sat_view_output, 'Needed for the sky view on the Signals page.');
  const antModel = textInput(p.antenna_model || '', { maxlength: 64, class: 'ant' });
  const antSerial = textInput(p.antenna_serial || '', { maxlength: 64 });
  const antSetup = numInput(p.antenna_setup_id || 0, { inputmode: 'numeric', maxlength: 3 });

  const F = {
    signal_group_master: field('Signal group (main antenna)', sgm, 'Which satellite signals the receiver tracks on its main antenna input ' +
      '(a number from the receiver\'s manual). Build 25164 takes the pairs ' + SG_KNOWN_PAIRS + ' only.'),
    signal_group_slave: field('Signal group (second antenna)', sgs, 'The same for the second antenna input; 0 turns it off. The two ' +
      'numbers only work as one of the receiver\'s pairs.'),
    signal_group: { el: sgChoice.el, input: null, error() {} },
    survey_in_s: field('Survey-in time (seconds)', survey, '1 to 3600. The receiver averages its position this long.'),
    lat_deg: field('Latitude (°)', lat),
    lon_deg: field('Longitude (°)', lon),
    height_m: field('Height above the ellipsoid (m)', hgt),
    station_id: field('Station ID', sid, '0 to 4095; goes into every RTCM message.'),
    com1_baud: field('Receiver link speed (COM1)', baud, 'Ephemeris messages need 460800 baud or more.'),
    rtcm: { el: h('div'), input: null, error: null },
    antenna_model: field('Antenna model (IGS name and radome)', antModel, 'Only if the antenna has a published calibration: its IGS name ' +
      'with the radome, exactly as listed (the name padded with spaces to 16 characters, then the 4-character radome code, ' +
      'such as NONE). Leave it empty to keep the receiver\'s own value. To clear a model the receiver already holds, enter its ' +
      'null-antenna name (ADVNULLANTENNA).'),
    antenna_serial: field('Antenna serial number', antSerial, 'Optional; sent only with a model.'),
    antenna_setup_id: field('Antenna setup ID', antSetup, '0 to 255; sent only with a model.'),
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
      sgChoice.set(SG_CHOICES.some(c => c[0] === key) ? key : '3 6');
      const v = (sgChoice.value() || '3 6').split(' ');
      sgm.value = v[0];
      sgs.value = v[1];
    }
    sgSync();
  });
  sgSync();
  const rows = h('div', { class: 'msgrows' });
  const rtcmErr = h('small', { class: 'err', hidden: true });
  F.rtcm.error = t => {
    rtcmErr.textContent = t || '';
    rtcmErr.hidden = !t;
  };

  function addRow(msgNo, period) {
    const m = numInput(msgNo, { inputmode: 'numeric', maxlength: 4, 'aria-label': 'Message number' });
    const per = numInput(period || 1, { inputmode: 'numeric', maxlength: 4, 'aria-label': 'Every (seconds)' });
    const name = h('span', { class: 'muted' }, rtcmName(Number(msgNo)));
    const row = h('div', { class: 'msgrow' }, m, per, name, h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Remove message',
      onclick: () => {
        row.remove();
        dirty = true;
      } }, '✕'));
    m.addEventListener('input', () => {
      name.textContent = rtcmName(Number(m.value));
    });
    rows.append(row);
    return m;
  }
  for (const r of p.rtcm || []) addRow(r.msg, r.period_s);

  const fixedBox = h('div', { class: 'stack' }, h('div', { class: 'cols c3' }, F.lat_deg.el, F.lon_deg.el, F.height_m.el),
    g.arp && g.arp.valid ? h('div', null, btn('Use the position from the stream', () => {
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
      if (bad.focus) bad.focus();
      return null;
    }
    return out;
  }

  const err = errBox();
  const saveBtn = h('button', { type: 'submit', class: 'btn primary' }, 'Save profile');
  const applyBtn = btn('Apply again', async () => {
    const ok = await dialog({ title: 'Apply the profile again?', ok: 'Apply',
      body: h('p', null, 'The station configures the receiver again. The RTCM stream can pause for a few seconds.') });
    if (!ok) return;
    const r = await act(applyBtn, () => api('POST', '/api/gnss/apply'), err);
    if (r !== undefined) toast('Applying the profile…', 'ok');
  });
  const APPLY = {
    queued: 'Saved. The station configures the receiver now.',
    unchanged: 'Saved. Nothing changes on the receiver.',
    safe_mode: 'Saved. It is applied after safe mode (with automatic configuration on).',
    maintenance: 'Saved. Receiver maintenance is on: the station checks the profile when it ends (and applies it with automatic configuration on).',
    unsupported_model: 'Saved, but the station does not configure this receiver model.',
    not_running: 'Saved. The receiver path did not start this boot; the station applies the profile once it runs.',
  };
  const form = h('form', { class: 'stack', oninput: () => {
    dirty = true;
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
  sgChoice.el,
  sgCustom.el,
  sgNumbers,
  mode.el,
  F.survey_in_s.el,
  fixedBox,
  h('div', { class: 'cols c2' }, F.station_id.el, F.com1_baud.el),
  h('div', { class: 'field' }, h('span', { class: 'lbl' }, 'RTCM messages'),
    h('div', { class: 'msgrow muted small' }, h('span', null, 'Message'), h('span', null, 'Every (s)'), h('span', null, ''), h('span', null, '')),
    rows, rtcmErr,
    h('div', { class: 'row foot' },
      btn('Add message', () => {
        addRow('', 1).focus();
        dirty = true;
      }, 'small'),
      btn('Onocoy defaults', () => {
        clear(rows);
        for (const m of ONOCOY_RTCM) addRow(m, 1);
        dirty = true;
      }, 'small'))),
  satView.el,
  h('fieldset', { class: 'fieldset group' }, h('legend', null, 'Antenna (published in RTCM 1033)'),
    F.antenna_model.el, h('div', { class: 'cols c2' }, F.antenna_serial.el, F.antenna_setup_id.el)),
  err,
  h('div', { class: 'form-actions' }, saveBtn, applyBtn));
  return { el: form, dirty: () => dirty };
}

/* ---- Signal-group comparison --------------------------------------------- */

const SGT_MINUTES = [[15, '15 minutes'], [30, '30 minutes'], [60, '1 hour'], [120, '2 hours']];
const SGT_RUNNING = ['switching', 'settling', 'measuring', 'deciding', 'restoring'];

/**
 * The comparison for the shell's notices (S.status, receiver.signal_test):
 * null, or [tone, title, text, actions], the shape renderNotices pushes.
 * A running test shows everywhere but the receiver page (its card is there);
 * a finished one only when the station ended it without the user's choice.
 */
function sgTestNotice(st) {
  const t = st && st.receiver ? st.receiver.signal_test : null;
  if (!t) return null;
  if (typeof shell !== 'undefined' && shell.current && shell.current.name === 'receiver') return null;
  const details = [link('Details', '#/receiver', 'btn small')];
  const grp = t.group ? sgName(t.group.master, t.group.slave) : '';
  if (t.state === 'deciding') {
    return ['warn', 'Signal-group comparison: your choice', 'Both groups are measured. Keep one on the receiver page' +
      (t.decide_left_s ? '; without a choice the station goes back to the group from before in about ' +
        fmt.dur(Math.ceil(t.decide_left_s / 3600) * 3600) : '') + '.', [link('Compare and choose', '#/receiver', 'btn small')]];
  }
  if (SGT_RUNNING.indexOf(t.state) >= 0) {
    const what = t.state === 'restoring' ? 'switching the receiver to ' + grp :
      lbl(L.sgtState, t.state).toLowerCase() + (grp ? ' ' + grp : '') + (t.phase !== null && t.phase !== undefined ?
        ' (group ' + (t.phase + 1) + ' of 2' + (t.left_s ? ', about ' + fmt.dur(Math.ceil(t.left_s / 60) * 60) + ' left' : '') + ')' : '');
    return ['info', 'Comparing signal groups: ' + what, 'Each switch restarts the receiver; the casters pause briefly for it ' +
      '(planned downtime). Other receiver changes wait until the comparison ends.', details];
  }
  if (t.state === 'idle' && t.outcome && ['timed_out', 'rejected', 'failed', 'interrupted'].indexOf(t.outcome) >= 0) {
    return ['warn', 'Signal-group comparison ended', L.sgtOutcome[t.outcome] || t.outcome, details];
  }
  return null;
}

function sgPairLabel(g) {
  return g ? sgName(g.master, g.slave) : DASH;
}

/* One group's results side by side with the other's: per constellation, then per band. */
function sgtResults(t) {
  const res = t.results || [];
  if (res.length !== 2) return null;
  const sysOrder = ['gps', 'glonass', 'galileo', 'qzss', 'beidou', 'navic'];
  const bandOrder = ['l1', 'l2', 'l5', 'l6', 'e5ab'];
  const sysOf = (r, sys) => (r.systems || []).find(x => x.sys === sys);
  const present = sysOrder.filter(sys => res.some(r => sysOf(r, sys)));
  const head = ['', sgPairLabel(res[0].group), sgPairLabel(res[1].group)];
  const rows = [];
  rows.push(['Measured', ...res.map(r => r.status === 'rejected' ? h('span', { class: 't-bad' }, lbl(L.sgtResult, r.status)) :
    fmt.dur(r.measured_s) + ' (' + fmt.num(r.samples) + ' samples' + (r.epoch_gaps ? ', ' + fmt.num(r.epoch_gaps) + ' epochs missing' : '') + ')' +
    (r.status === 'measuring' ? ', measuring' : ''))]);
  rows.push(['Best rewarded constellation', ...res.map(r => r.best_sys ?
    h('b', { class: r.best_bands >= 4 ? 't-ok' : 't-warn' }, lbl(L.sys, r.best_sys) + ': ' + r.best_bands + ' bands (' +
      ((sysOf(r, r.best_sys) || {}).bands || []).map(b => BAND_HEAD[b] || b).join(', ') + ')') : DASH)]);
  for (const sys of present) {
    rows.push([h('b', null, lbl(L.sys, sys)), ...res.map(r => {
      const x = sysOf(r, sys);
      return x ? h('b', null, x.satellites.toFixed(1) + ' satellites · ' + (x.bands || []).length + ' band' +
        ((x.bands || []).length === 1 ? '' : 's')) : DASH;
    })]);
    for (const band of bandOrder) {
      if (!res.some(r => (sysOf(r, sys) || {}).per_band && sysOf(r, sys).per_band.some(p => p.band === band))) continue;
      rows.push([h('span', { class: 'sgt-band' }, BAND_HEAD[band] || band), ...res.map(r => {
        const pb = ((sysOf(r, sys) || {}).per_band || []).find(p => p.band === band);
        if (!pb) return DASH;
        return [pb.satellites.toFixed(1) + ' sats', pb.cnr_dbhz === null || pb.cnr_dbhz === undefined ? '' :
          h('span', { class: cnrClass(pb.cnr_dbhz) }, ' · ' + pb.cnr_dbhz.toFixed(1) + ' dB-Hz'),
        h('small', { class: 'hint' }, sigNames(sys, pb.signal_ids))];
      })]);
    }
  }
  return h('div', { class: 'sgt-results' }, table(head, rows, { label: 'Signal-group comparison results' }));
}

/**
 * "Compare two signal groups": the start form (two groups, minutes per
 * group, a confirmation), the progress, the results side by side, keep and
 * stop. Built once per view; update(t, g) shows GET /api/gnss/signal-test.
 */
function sgTestCtl(done) {
  const out = errBox();
  const status = h('div');
  const results = h('div');
  const opts = SG_CHOICES.map(c => [c[0], c[1]]);
  const selA = selectInput(opts, '3 6', { 'aria-label': 'First group' });
  const selB = selectInput(opts, '7 0', { 'aria-label': 'Second group' });
  const minutes = selectInput(SGT_MINUTES, 30, { 'aria-label': 'Minutes per group' });
  let defaultsSet = false;
  const pair = v => {
    const p = String(v).split(' ');
    return { master: Number(p[0]), slave: Number(p[1]) };
  };
  const startBtn = btn('Compare', async () => {
    if (selA.value === selB.value) {
      say(out, 'Choose two different signal groups.');
      return;
    }
    const mins = Number(minutes.value);
    const ok = await dialog({ title: 'Compare two signal groups?', ok: 'Start the comparison',
      body: [h('p', null, 'The station measures ' + sgName(...selA.value.split(' ')) + ' and ' + sgName(...selB.value.split(' ')) +
        ' for ' + lbl(Object.fromEntries(SGT_MINUTES), mins) + ' each (after a minute of settling), then shows the results side by side ' +
        'and waits up to 24 hours for you to keep one.'),
      msg('warn', 'Each switch restarts the receiver; the casters pause for up to about a minute each time (counted as planned ' +
        'downtime); up to three switches.'),
      h('p', { class: 'muted small' }, 'Without a choice within 24 hours the station goes back to the group in use now. ' +
        'Other receiver changes wait until the comparison ends.')] });
    if (!ok) return;
    const r = await act(startBtn, () => api('POST', '/api/gnss/signal-test',
      { groups: [pair(selA.value), pair(selB.value)], phase_s: mins * 60 }), out);
    if (r !== undefined) {
      toast('Comparison started.', 'ok');
      done();
    }
  }, 'primary');
  /* No receiver connected (GET /api/gnss state no_receiver): nothing to compare, so Compare waits for one.
   * The station refuses a start then anyway (409 no_receiver). */
  const noRxNote = h('span', { class: 'note muted small', id: 'sgt-no-rx', hidden: true }, 'Connect a receiver to compare signal groups.');
  const form = h('div', { class: 'stack' },
    h('div', { class: 'cols c3' }, field('First group (A)', selA).el, field('Second group (B)', selB).el,
      field('Measuring time per group', minutes).el),
    h('div', { class: 'form-actions' }, startBtn, noRxNote));
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
    el: h('div', { class: 'stack' },
      h('h2', null, 'Compare two signal groups', badgeEl),
      h('p', { class: 'muted small' }, 'Measures two signal groups on this antenna, one after the other, and shows satellites, ' +
        'signals, bands and signal strength side by side. You then keep the one you prefer.'),
      status, results, form, h('div', { class: 'form-actions' }, keepA, keepB, stopBtn), out),
    update(t, g) {
      if (!t) return;
      const running = SGT_RUNNING.indexOf(t.state) >= 0;
      badgeEl.className = 'badge ' + (t.state === 'deciding' ? 'warn' : running ? 'info' : '');
      badgeEl.textContent = lbl(L.sgtState, t.state);
      form.hidden = running;
      const noRx = !running && !!g && g.state === 'no_receiver';
      /* A start in flight keeps its own disabled state (busy() gives it back when the answer is in). */
      if (!startBtn.classList.contains('busy')) startBtn.disabled = noRx;
      noRxNote.hidden = !noRx;
      if (noRx) startBtn.setAttribute('aria-describedby', noRxNote.id);
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
      const grp = sgPairLabel(t.running_group);
      let text = null;
      if (t.state === 'switching') text = msg('info', 'Switching the receiver to ' + grp + '. The casters pause for it.');
      else if (t.state === 'settling') text = msg('info', 'Group ' + (t.phase + 1) + ' of 2: ' + grp + ' settles for a minute; ' +
        'about ' + fmt.dur(Math.ceil((t.left_s || 0) / 60) * 60) + ' left in this phase.');
      else if (t.state === 'measuring') text = msg('info', 'Group ' + (t.phase + 1) + ' of 2: measuring ' + grp + ', about ' +
        fmt.dur(Math.ceil((t.left_s || 0) / 60) * 60) + ' left (only time with a streaming receiver counts).');
      else if (t.state === 'deciding') text = msg('warn', h('strong', null, 'Both groups are measured: keep one.'), ' The receiver ' +
        'stays on ' + grp + ' meanwhile. Without a choice within ' + fmt.dur(Math.ceil((t.decide_left_s || 0) / 3600) * 3600) +
        ' the station goes back to ' + sgPairLabel(t.before) + ', the group from before the comparison.');
      else if (t.state === 'restoring') text = msg('info', 'Switching the receiver to ' + grp + '.' +
        (t.outcome ? ' ' + (L.sgtOutcome[t.outcome] || '') : ''));
      else if (t.outcome) text = msg(t.outcome === 'kept' || t.outcome === 'cancelled' ? 'ok' : 'warn',
        h('strong', null, t.outcome === 'kept' && t.kept ? 'You kept ' + sgPairLabel(t.kept) + '.' : (L.sgtOutcome[t.outcome] || t.outcome)),
        t.reason && t.outcome !== 'kept' ? ' ' + t.reason : '', t.ended_unix ? h('span', { class: 'muted' }, ' (' + fmt.time(t.ended_unix) + ')') : null);
      mount(status, text);
      mount(results, t.results ? sgtResults(t) : null);
    },
  };
}

Views.receiver = {
  title: 'Receiver',
  admin: true,
  create(el) {
    let g = null, editor = null, stop = null, held = null, heldLater = false;
    const heldCard = h('section', { class: 'card wide', hidden: true });
    const info = h('section', { class: 'card' });
    const posActions = positionActions(() => refresh());
    const position = h('section', { class: 'card' }, posActions.el);
    const desc = h('section', { class: 'card' });
    const maint = maintenanceCtl(() => refresh().catch(() => {}));
    const maintCard = h('section', { class: 'card' }, maint.el);
    const recovery = recoveryCtl();
    const recoveryCard = h('section', { class: 'card' }, recovery.el);
    const sgt = sgTestCtl(() => refresh().catch(() => {}));
    const sgtCard = h('section', { class: 'card wide' }, sgt.el);
    let t = null;
    const profile = h('section', { class: 'card wide' });

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
      mount(info, receiverInfo(g));
      /* Everything in the position card but its actions, which stay put. */
      while (position.firstChild && position.firstChild !== posActions.el) position.firstChild.remove();
      position.insertBefore(add(doc.createDocumentFragment(), positionInfo(g)), posActions.el);
      posActions.update(g);
      mount(desc, rtcm1033Info(g));
      maint.update(g);
      recovery.update(g);
      sgt.update(t, g);
    };
    const buildEditor = () => {
      editor = profileEditor(g, refresh);
      mount(profile, h('h2', null, 'Receiver profile'),
        h('p', { class: 'muted small' }, 'What the station configures on the receiver. Saving re-applies it when it changed.'),
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
      mount(el, h('div', { class: 'grid' }, heldCard, info, position, desc, maintCard, recoveryCard, sgtCard, profile));
      fill();
      buildEditor();
    }
    mount(el, h('p', { class: 'loading' }, 'Loading the receiver…'));
    load().then(() => {
      stop = poller(5000, refresh, true);
    }, e => mount(el, msg('bad', errText(e)), btn('Try again', () => openView('receiver', ''), 'primary')));
    return {
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
