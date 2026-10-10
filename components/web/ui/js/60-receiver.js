/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver: identity and link, base position (survey, lock, survey again),
 * and the base profile editor (signal groups, base mode, station ID, COM1
 * baud, RTCM messages, satellite-in-view output). Field rules mirror
 * PUT /api/gnss/profile; the station's own validation has the last word.
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

function receiverInfo(g) {
  const r = g.receiver || {};
  const link = g.link || {};
  const alerts = g.alerts || [];
  return [
    h('h2', null, 'Receiver', badge(lbl(L.station, g.state), stationTone(g.state))),
    r.valid === false ? h('p', { class: 'muted' }, 'The receiver has not identified itself yet.') : null,
    kv([
      ['Model', r.model],
      ['Firmware', r.firmware ? r.firmware + (r.build ? ' (build ' + r.build + ')' : '') : DASH],
      ['Serial number', r.serial],
      ['Part number', r.pn],
      ['Chip ID', r.efuse_id],
      ['Firmware date', r.compile_date],
      ['Answers commands', fmt.yesno(g.responding)],
      ['Identity confirmed', fmt.yesno(g.identity_confirmed)],
    ]),
    h('h3', { class: 'section-title' }, 'Link and configuration'),
    kv([
      ['Link', lbl(L.linkMode, link.mode) + (link.baud ? ' at ' + link.baud + ' baud' : '')],
      ['Last data', link.rx_age_ms === null || link.rx_age_ms === undefined ? 'never' : fmt.ms(link.rx_age_ms) + ' ago'],
      ['Last RTCM', h('span', { class: 't-' + ageTone(link.rtcm_age_ms) },
        link.rtcm_age_ms === null || link.rtcm_age_ms === undefined ? 'never' : fmt.ms(link.rtcm_age_ms) + ' ago')],
      ['Automatic configuration', g.autocfg ? 'On' : 'Off'],
      ['Profile applied', g.profile_applied ? 'Yes' : h('span', { class: 't-warn' }, 'No')],
      ['Message 1230', g.rtcm1230_supported === null || g.rtcm1230_supported === undefined ? 'Not applied yet' :
        g.rtcm1230_supported ? 'Accepted by the receiver' : h('span', { class: 't-warn' }, 'Refused by the receiver')],
      g.config_failed ? ['Last configuration', h('span', { class: 't-bad' }, 'Failed' + (g.config_retry_in_s ? '; next try in ' + fmt.dur(g.config_retry_in_s) : ''))] : null,
      g.detect_failures ? ['Detection failures', fmt.num(g.detect_failures)] : null,
    ]),
    alerts.length ? h('div', { class: 'stack foot' }, alerts.map(a => {
      const t = L.salert[a] || [a, ''];
      return msg('warn', h('strong', null, t[0]), t[1] ? h('div', { class: 'small' }, t[1]) : null);
    })) : null,
  ];
}

function positionInfo(g) {
  const pos = g.position || {};
  const arp = g.arp || {};
  const sv = g.survey || {};
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

/* ---- Profile editor ----------------------------------------------------- */

function profileEditor(g, done) {
  const p = Object.assign({ signal_group_master: 3, signal_group_slave: 6, base_mode: 'survey_in', survey_in_s: 600, fixed: null,
    station_id: 0, com1_baud: 115200, rtcm: [], sat_view_output: false }, g.profile || {});
  let dirty = false;
  const autocfg = checkbox('Configure the receiver automatically', g.autocfg,
    'The station applies this profile whenever the receiver\'s settings differ from it.');
  const sgm = numInput(p.signal_group_master, { inputmode: 'numeric', maxlength: 3 });
  const sgs = numInput(p.signal_group_slave, { inputmode: 'numeric', maxlength: 3 });
  const mode = radios('Base mode', 'basemode', [['survey_in', 'Survey-in'], ['fixed', 'Fixed position']], p.base_mode);
  const survey = numInput(p.survey_in_s || 600, { inputmode: 'numeric', maxlength: 4 });
  const fx = p.fixed || {};
  const lat = numInput(fx.lat_deg, { inputmode: 'text', placeholder: '45.123456789' });
  const lon = numInput(fx.lon_deg, { inputmode: 'text', placeholder: '-75.123456789' });
  const hgt = numInput(fx.height_m, { inputmode: 'text', placeholder: '85.1234' });
  const sid = numInput(p.station_id, { inputmode: 'numeric', maxlength: 4 });
  const baud = selectInput(BAUDS.map(b => [b, b + ' baud']), p.com1_baud);
  const satView = checkbox('Satellite-in-view output', p.sat_view_output, 'Needed for the sky view on the Signals page.');

  const F = {
    signal_group_master: field('Signal group (main antenna)', sgm),
    signal_group_slave: field('Signal group (second antenna)', sgs, '3 and 6 add Galileo E6 on the UM982. 0 turns the second antenna off.'),
    survey_in_s: field('Survey-in time (seconds)', survey, '1 to 3600. The receiver averages its position this long.'),
    lat_deg: field('Latitude (°)', lat),
    lon_deg: field('Longitude (°)', lon),
    height_m: field('Height above the ellipsoid (m)', hgt),
    station_id: field('Station ID', sid, '0 to 4095; goes into every RTCM message.'),
    com1_baud: field('Receiver link speed (COM1)', baud, 'Ephemeris messages need 460800 baud or more.'),
    rtcm: { el: h('div'), input: null, error: null },
  };
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
    const out = {
      signal_group_master: int(sgm.value, 0, 255),
      signal_group_slave: int(sgs.value, 0, 255),
      base_mode: mode.value(),
      survey_in_s: 0,
      fixed: null,
      station_id: int(sid.value, 0, 4095),
      com1_baud: Number(baud.value),
      rtcm: [],
      sat_view_output: satView.input.checked,
    };
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
  h('div', { class: 'cols c2' }, F.signal_group_master.el, F.signal_group_slave.el),
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
  err,
  h('div', { class: 'form-actions' }, saveBtn, applyBtn));
  return { el: form, dirty: () => dirty };
}

Views.receiver = {
  title: 'Receiver',
  admin: true,
  create(el) {
    let g = null, editor = null, stop = null;
    const info = h('section', { class: 'card' });
    const posActions = positionActions(() => refresh());
    const position = h('section', { class: 'card' }, posActions.el);
    const profile = h('section', { class: 'card wide' });

    const fill = () => {
      mount(info, receiverInfo(g));
      /* Everything in the position card but its actions, which stay put. */
      while (position.firstChild && position.firstChild !== posActions.el) position.firstChild.remove();
      position.insertBefore(add(doc.createDocumentFragment(), positionInfo(g)), posActions.el);
      posActions.update(g);
    };
    const buildEditor = () => {
      editor = profileEditor(g, refresh);
      mount(profile, h('h2', null, 'Receiver profile'),
        h('p', { class: 'muted small' }, 'What the station configures on the receiver. Saving re-applies it when it changed.'),
        editor.el);
    };
    async function refresh() {
      g = await api('GET', '/api/gnss');
      fill();
    }
    async function load() {
      g = await api('GET', '/api/gnss');
      mount(el, h('div', { class: 'grid' }, info, position, profile));
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
      dirty: () => !!(editor && editor.dirty()),
      reload() {
        if (!g) return;
        refresh().then(() => {
          if (!(editor && editor.dirty())) buildEditor();
        }).catch(() => {});
      },
    };
  },
};
