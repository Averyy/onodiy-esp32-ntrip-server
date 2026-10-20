/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signals, on the Receiver page: one card with the best constellation and
 * its bands, the Good / Check / Problem hints in one line each (only what
 * that headline does not say), the frequency bands per constellation from
 * the MSM data (with the mean C/N0 from MSM4 to MSM7) and the sky map
 * (66-live.js); in the Receiver card, the RTCM messages seen against
 * the profile. The public page shows the summary and the band table
 * (signalsCard).
 */

const BAND_ORDER = ['l1', 'l2', 'l5', 'l6', 'e5ab'];
const BAND_HEAD = { l1: 'L1', l2: 'L2', l5: 'L5', l6: 'L6', e5ab: 'E5 AltBOC' };
/* What each band bucket holds per constellation. */
const BAND_SIGNAL = {
  gps: { l1: 'L1 C/A, L1C', l2: 'L2', l5: 'L5' },
  glonass: { l1: 'G1', l2: 'G2' },
  galileo: { l1: 'E1', l2: 'E5b', l5: 'E5a', l6: 'E6', e5ab: 'E5 AltBOC' },
  beidou: { l1: 'B1I, B1C', l2: 'B2I, B2b', l5: 'B2a', l6: 'B3I' },
  qzss: { l1: 'L1', l2: 'L2', l5: 'L5', l6: 'L6' },
  navic: { l5: 'L5' },
};

function cnrClass(v) {
  if (v === null || v === undefined) return '';
  return v < 35 ? 'cnr-weak' : v < 42 ? 'cnr-ok' : 'cnr-good';
}

function bandTable(systems) {
  const showE5ab = systems.some(s => (s.bands || []).indexOf('e5ab') >= 0);
  const bands = BAND_ORDER.filter(b => b !== 'e5ab' || showE5ab);
  return scrollBox('Bands per constellation', h('table', { class: 'bands' },
    h('thead', null, h('tr', null, h('th', null, 'Constellation'), h('th', { class: 'num' }, 'Satellites'),
      bands.map(b => h('th', null, BAND_HEAD[b])), h('th', { class: 'num' }, 'Bands'))),
    h('tbody', null, systems.map(s => {
      const have = s.bands || [];
      const cnr = s.cnr_dbhz || {};
      return h('tr', null,
        h('td', null, h('span', { class: 'legend' }, h('span', null, h('i', { class: 'sys-' + s.sys }), lbl(L.sys, s.sys)))),
        h('td', { class: 'num' }, fmt.num(s.satellites)),
        bands.map(b => {
          const on = have.indexOf(b) >= 0;
          const name = (BAND_SIGNAL[s.sys] || {})[b];
          /* On a phone the signal names and the unit hide (the column says the band): a tick and the figure fit. */
          return h('td', { class: on ? 'on' : 'off', title: on ? name || BAND_HEAD[b] : null }, on ? ['✓', h('span', { class: 'wide-only' },
            ' ' + (name || BAND_HEAD[b]))] : DASH,
          on && cnr[b] !== null && cnr[b] !== undefined ? h('small', { class: cnrClass(cnr[b]) }, cnr[b], h('span', { class: 'wide-only' },
            ' dB-Hz')) : null);
        }),
        h('td', { class: 'num' }, h('b', { class: have.length >= 4 ? 't-ok' : null }, String(have.length))));
    }))));
}

/* A seen period in words: whole seconds within 10 % of them ("1 s" for 950 ms: jitter, not another rate). */
function periodText(ms) {
  const s = ms / 1000, r = Math.round(s);
  return r >= 1 && Math.abs(s - r) <= 0.1 * r ? r + 's' : fmt.ms(ms);
}

/* A message name without its MSM type, for the one line of extra messages ("1107 SBAS observations"). */
const rtcmShort = n => (rtcmName(n) || '').replace(/ \(.*\)$/, '');


const neverSeen = m => m.age_ms === null || m.age_ms === undefined;

/* QZSS and NavIC observations: regional, so none arrive away from their area (the station never counts that against the
 * profile either). */
const regionalMsm = n => (n >= 1111 && n <= 1117) || (n >= 1131 && n <= 1137);

/** A profile message's row state: 'missing' (red), 'waits' (the position, while surveying), 'none' (a regional system
 * with nothing arriving) or '' (as it should be). */
function msgCheck(m, colour, surveying) {
  const late = !m.count || (!neverSeen(m) && m.age_ms > m.expected_period_s * 3000 + 2000);
  if (!late) return '';
  if (surveying && (m.msg === 1005 || m.msg === 1006)) return 'waits';
  if (regionalMsm(m.msg)) return 'none';
  return colour ? 'missing' : '';
}

/* The profile's messages, a row each (number and name, wrapping in a narrow card; every; last seen); the others in one line
 * under them. */
function messagesView(list, colour, surveying) {
  const mine = list.filter(m => m.expected_period_s !== null && m.expected_period_s !== undefined);
  const extra = list.filter(m => (m.expected_period_s === null || m.expected_period_s === undefined) && m.count);
  let red = false;
  const rows = mine.map(m => {
    const expected = m.expected_period_s;
    const st = msgCheck(m, colour, surveying);
    const waits = st === 'waits';
    const none = st === 'none';
    const missing = st === 'missing';
    red = red || missing;
    const seen = m.period_ms ? periodText(m.period_ms) : DASH;
    const bad = missing ? 't-bad' : null;
    return h('tr', null,
      h('td', { class: 'name' }, h('b', { class: bad }, String(m.msg)), h('span', { class: bad }, ' ' + (rtcmName(m.msg) || ''))),
      waits || none ? h('td', { class: 'num muted', colspan: 2 }, waits ? 'Waits for the survey' : 'None in view') : [
        h('td', { class: 'num' }, seen, m.period_ms && seen !== expected + 's' ? h('span', { class: 'muted' }, ' (' + expected + 's)') : null),
        h('td', { class: 'num' + (bad ? ' t-bad' : '') }, neverSeen(m) ? 'Never' : fmt.ms(m.age_ms) + NB + 'ago')]);
  });
  return [
    rows.length ? scrollBox('Messages', h('table', { class: 'rows msgs' },
      h('thead', null, h('tr', null, h('th', null, 'Message'), h('th', { class: 'num' }, 'Every'), h('th', { class: 'num' }, 'Last seen'))),
      h('tbody', null, rows))) : h('p', { class: 'muted' }, 'No messages in the profile.'),
    red ? h('p', { class: 'small' }, h('span', { class: 't-bad' }, 'Red:'), ' in the profile but not arriving.') : null,
    extra.length ? h('p', { class: 'muted small' }, 'Also in the stream, not in the profile: ' +
      extra.map(m => m.msg + (rtcmShort(m.msg) ? ' ' + rtcmShort(m.msg) : '')).join(', ') + '.') : null,
  ];
}

/* Under every band table. */
const BANDS_FOOT = 'Last 10 seconds. Figures: mean C/N0 per band, in dB-Hz.';

/* The public page's signals: the best constellation in large type and the band table. */
function signalsCard(sig) {
  return h('section', { class: 'card wide' }, h('h2', null, 'Signals'),
    h('div', { class: 'big' }, lbl(L.sys, sig.best_sys) + ' · ' + sig.best_bands + ' bands'),
    bandTable(sig.systems), h('p', { class: 'muted small foot' }, BANDS_FOOT));
}

/*
 * The Signals card's hints from GET /api/gnss/signals, one line each (the
 * antenna check's, C/N0: band means weighted by satellites), plus E6 lost,
 * no GLONASS, 1230 refused. master: the main antenna's
 * signal group the receiver runs (GET /api/gnss), null while unknown; with
 * Galileo short of E6 it says whether 3 6 would add it or runs already.
 */
function signalChecks(sig, master) {
  const systems = sig.systems || [];
  let n = 0, sum = 0, w = 0, sum1 = 0, w1 = 0;
  const sys = systems.map(s => {
    const bands = {};
    for (const b of s.bands || []) bands[b] = s.satellites;
    for (const b of Object.keys(s.cnr_dbhz || {})) {
      if (s.cnr_dbhz[b]) {
        sum += s.cnr_dbhz[b] * s.satellites;
        w += s.satellites;
        if (b === 'l1') {
          sum1 += s.cnr_dbhz[b] * s.satellites;
          w1 += s.satellites;
        }
      }
    }
    n += s.satellites;
    return { sys: s.sys, bands: bands };
  });
  const r = receptionChecks({ totals: { tracked: n, cnr_dbhz: w ? Math.round(sum / w) : null, msm_age_ms: sig.epoch_age_ms, systems: sys,
    bands: w1 ? { l1: { cnr_dbhz: Math.round(sum1 / w1) } } : {} } });
  if (r.waiting) return r;
  const gal = systems.find(s => s.sys === 'galileo');
  const bands = r.items.find(i => i[4] === 'bands');
  const known = master !== null && master !== undefined;
  if (bands[0] !== 'ok' && known && !sig.e6_lost && gal && (gal.bands || []).indexOf('l6') < 0) {
    bands[3] = master === 3 ? 'Onocoy needs 4 bands: Galileo E6 is not tracked, check the antenna covers L6' :
      'Onocoy needs 4 bands: signal group 3 6 adds Galileo E6';
  }
  const add = (tone, line) => r.items.push([tone, null, null, line, 'more']);
  if (sig.e6_lost) add('bad', 'Galileo E6 lost: it was tracked before with this signal group');
  if (!systems.some(s => s.sys === 'glonass')) add('warn', 'No GLONASS: Onocoy\'s quality score needs it');
  else if (sig.rtcm1230_supported === false) add('warn', 'Message 1230 refused: GLONASS does not count for Onocoy');
  r.ok = r.items.every(i => i[0] === 'ok');
  return r;
}

/** The big line: the constellations with the most bands while observations arrive, else how long they have been missing. */
function signalsHeadline(sig) {
  const age = sig.epoch_age_ms;
  if (age === null || age === undefined) return 'No signals yet';
  if (age >= 5000) return 'No signals for ' + fmt.dur(age / 1000);
  if (!sig.best_sys) return 'No signals yet';
  const most = mostBands((sig.systems || []).map(s => ({ sys: s.sys, n: (s.bands || []).length })));
  return most.n ? most.n + ' bands on ' + most.names : lbl(L.sys, sig.best_sys) + ' · ' + sig.best_bands + ' bands';
}

/* Observations arrive: the headline names the best constellation (else a banner says none do). */
const hasSignals = sig => typeof sig.epoch_age_ms === 'number' && sig.epoch_age_ms < 5000 && !!sig.best_sys;

/* The card's head: while corrections come (live), the hints in one line each (master: signalChecks), else the headline.
 * Without signals one banner says so, with why the sky cannot show under it (why: nodes, '' when it can). */
function signalsHead(sig, live, master, why) {
  if (!hasSignals(sig)) {
    return msg(typeof sig.epoch_age_ms === 'number' ? 'warn' : 'info', h('strong', null, signalsHeadline(sig)),
      why ? h('div', null, why) : null);
  }
  const checks = live ? signalChecks(sig, master) : null;
  return [checks && !checks.waiting ? h('div', { class: 'check-banner' }, receptionList(checks, true)) :
    h('div', { class: 'big' }, signalsHeadline(sig)),
    why ? msg('info', why) : null];
}

/**
 * The Receiver page's Signals card in el: the best constellation, the hints
 * (while the receiver delivers), the band table with the sky map beside it
 * while the profile sends satellite positions (every satellite under the
 * table; without them a line says where to turn them on). Hidden without a
 * receiver (the Receiver card says so). msgs: the Receiver card's box for
 * the messages the receiver sends, shown once any arrived (until then the
 * card's line says that none do) and not while messagesOff(true). Returns
 * {update(status), gnss(g), messagesOff(off), destroy()}.
 */
function signalsSection(el, msgs) {
  const sky = skyMap();
  const head = h('div', { class: 'stack' }, h('p', { class: 'loading' }, 'Loading signals…'));
  const bands = h('div');
  const wrap = h('div', { class: 'sky-wrap' }, sky.plot, h('div', { class: 'stack' }, bands, sky.list));
  const card = h('section', { class: 'card wide' }, h('h2', null, 'Signals'), head, wrap);
  /* No band table and no sky: no empty box under the head. The sky beside the table: its signal names give way. */
  const wrapSync = () => {
    wrap.hidden = !bands.firstChild && sky.plot.hidden;
    card.classList.toggle('sky-open', !sky.plot.hidden);
  };
  sky.watch(wrapSync);
  el.append(card);
  let collapsed = false, g = null, msgsOff = false, why = '', lastSig = null, skyOn = false;
  const msgsSync = () => {
    msgs.hidden = collapsed || msgsOff || !msgs.firstChild;
  };
  /* The sky map is read while it can show: the profile sends the positions and the card shows. */
  const skySync = () => sky.set(skyOn && !collapsed, !!g && !g.autocfg);
  /* Verdicts (hints, red messages) only while corrections come from the receiver. */
  const delivering = () => !!(S.status && stationState(S.status, true).delivering);
  /* No receiver, or the station looking for it: the card hides. */
  function receiverState(st) {
    const r = (st && st.receiver) || {};
    const c = !r.rx_maintenance && (r.state === 'no_receiver' || r.state === 'detecting' || r.state === 'idle');
    if (c === collapsed) return;
    collapsed = c;
    card.hidden = c;
    msgsSync();
    skySync();
  }
  receiverState(S.status);

  /* The head from the last signals read: also when only GET /api/gnss changed (why the sky cannot show). */
  function drawHead() {
    /* The group the receiver runs, else the profile's (the hint on Galileo E6 depends on it). */
    const sg = g && (g.rx_signal_group ? g.rx_signal_group.master : (g.profile || {}).signal_group_master);
    patch(head, signalsHead(lastSig, delivering(), sg === undefined ? null : sg, why));
  }

  async function loadSignals() {
    let sig;
    try {
      sig = await api('GET', '/api/gnss/signals');
    } catch (e) {
      patch(head, msg('bad', errText(e)));
      return;
    }
    if (collapsed) return;
    const systems = sig.systems || [];
    lastSig = sig;
    drawHead();
    /* No observations: the headline says so ("No signals for 1 min"), the table goes. */
    patch(bands, systems.length ? [bandTable(systems), h('p', { class: 'muted small foot' }, BANDS_FOOT)] : null);
    wrapSync();
    /* The epoch counters are on Logs (Firmware internals, Observations). */
    const seen = (sig.messages || []).some(m => m.count);
    patch(msgs, seen ? [h('h3', { class: 'sub' }, 'Messages'), messagesView(sig.messages, delivering(), !!(g && (g.survey || {}).active))] : null);
    msgsSync();
  }

  const stop = poller(2000, loadSignals);
  return {
    update: receiverState,
    /* GET /api/gnss: the survey (1005 waits for it) and whether the sky map can show satellites: the profile's satellite
     * positions (Sky map in Receiver settings) give them once applied (a held first configuration sent nothing). */
    gnss(gg) {
      g = gg;
      const settings = h('a', { href: '#/receiver/settings' }, 'Receiver settings');
      skyOn = !g.first_apply_held && !!(g.profile || {}).sat_view_output;
      why = g.first_apply_held ? 'To see the sky map, configure the receiver first.' :
        skyOn ? '' : ['Turn on Sky map in ', settings, ' to see the satellites on a map.'];
      skySync();
      if (lastSig && !collapsed) drawHead();
    },
    messagesOff(off) {
      msgsOff = off;
      msgsSync();
    },
    destroy() {
      stop();
      sky.destroy();
    },
  };
}
