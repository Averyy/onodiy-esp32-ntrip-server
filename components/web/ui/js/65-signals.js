/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signals, on the Receiver page: one card with the best constellation, the
 * Good / Check / Problem hints, the frequency bands per constellation from
 * the MSM data (with the mean C/N0 from MSM4 to MSM7), the sky on request
 * (66-live.js) and, behind "Signal details", the epochs and the RTCM
 * messages seen against the profile. The public page shows the summary and
 * the band table (signalsCard).
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
  return r >= 1 && Math.abs(s - r) <= 0.1 * r ? r + NB + 's' : fmt.ms(ms);
}

/* A message name without its MSM type, for the one line of extra messages ("1107 SBAS observations"). */
const rtcmShort = n => (rtcmName(n) || '').replace(/ \(.*\)$/, '');


const neverSeen = m => m.age_ms === null || m.age_ms === undefined;

/** A profile message's row state: 'missing' (red), 'waits' (the position, while surveying) or '' (as it should be). */
function msgCheck(m, colour, surveying) {
  const late = !m.count || (!neverSeen(m) && m.age_ms > m.expected_period_s * 3000 + 2000);
  if (!late) return '';
  if (surveying && (m.msg === 1005 || m.msg === 1006)) return 'waits';
  return colour ? 'missing' : '';
}

/* The profile's messages in one line each (number and name, every, last seen); the others in one line under them. */
function messagesView(list, colour, surveying) {
  const mine = list.filter(m => m.expected_period_s !== null && m.expected_period_s !== undefined);
  const extra = list.filter(m => (m.expected_period_s === null || m.expected_period_s === undefined) && m.count);
  let red = false;
  const rows = mine.map(m => {
    const expected = m.expected_period_s;
    const st = msgCheck(m, colour, surveying);
    const waits = st === 'waits';
    const missing = st === 'missing';
    red = red || missing;
    const seen = m.period_ms ? periodText(m.period_ms) : DASH;
    const bad = missing ? 't-bad' : null;
    return h('tr', null,
      h('td', { class: 'name', title: rtcmName(m.msg) || null }, h('b', { class: bad }, String(m.msg)), h('span', { class: bad }, ' ' + (rtcmName(m.msg) || ''))),
      waits ? h('td', { class: 'num muted', colspan: 2 }, 'Waits for the survey') : [
        h('td', { class: 'num' }, seen, m.period_ms && seen !== expected + NB + 's' ? h('span', { class: 'muted' }, ' (' + expected + NB + 's)') : null),
        h('td', { class: 'num' + (bad ? ' t-bad' : '') }, neverSeen(m) ? 'Never' : fmt.ms(m.age_ms) + NB + 'ago')]);
  });
  return [
    rows.length ? scrollBox('Messages', h('table', { class: 'rows' },
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

/* The antenna check's hints from GET /api/gnss/signals (C/N0: band means weighted by satellites), plus E6 lost, no GLONASS, 1230 refused. */
function signalChecks(sig) {
  const systems = sig.systems || [];
  let n = 0, sum = 0, w = 0;
  const sys = systems.map(s => {
    const bands = {};
    for (const b of s.bands || []) bands[b] = s.satellites;
    for (const b of Object.keys(s.cnr_dbhz || {})) {
      if (s.cnr_dbhz[b]) {
        sum += s.cnr_dbhz[b] * s.satellites;
        w += s.satellites;
      }
    }
    n += s.satellites;
    return { sys: s.sys, bands: bands };
  });
  const r = receptionChecks({ totals: { tracked: n, cnr_dbhz: w ? Math.round(sum / w) : null, msm_age_ms: sig.epoch_age_ms, systems: sys } });
  if (r.waiting) return r;
  const gal = systems.find(s => s.sys === 'galileo');
  if (sig.e6_lost) r.items.push(['bad', 'Galileo E6 lost', 'It was tracked before with this signal group.']);
  else if (gal && sig.best_bands < 4 && (gal.bands || []).indexOf('l6') < 0) r.items[2][2] += ' Signal group 3 6 adds Galileo E6.';
  if (!systems.some(s => s.sys === 'glonass')) r.items.push(['warn', 'No GLONASS', 'Onocoy\'s quality score needs it.']);
  else if (sig.rtcm1230_supported === false) {
    r.items.push(['warn', 'GLONASS biases refused', 'The receiver refused message 1230, so GLONASS does not count for Onocoy.']);
  }
  r.ok = r.items.every(i => i[0] === 'ok');
  return r;
}

/** The big line: the best constellation while observations arrive, else how long they have been missing. */
function signalsHeadline(sig) {
  const age = sig.epoch_age_ms;
  if (age === null || age === undefined) return 'No signals yet';
  if (age >= 5000) return 'No signals for ' + fmt.dur(age / 1000);
  return sig.best_sys ? lbl(L.sys, sig.best_sys) + ' · ' + sig.best_bands + ' bands' : 'No signals yet';
}

/**
 * The Receiver page's Signals card in el: the best constellation, the hints
 * (while the receiver delivers), the band table with the sky beside it once
 * shown, "Signal details". Hidden without a receiver (the Receiver card says
 * so). Returns {update(status), gnss(g), destroy(), sky}.
 */
function signalsSection(el) {
  const sky = skySection();
  const head = h('div', { class: 'stack' }, h('p', { class: 'loading' }, 'Loading signals…'));
  const bands = h('div');
  const body = h('div');
  const details = moreAs('sig-details', 'Signal details', false, body);
  const card = h('section', { class: 'card wide' }, h('h2', null, 'Signals'), head, sky.head,
    h('div', { class: 'sky-wrap' }, sky.plot, bands),
    h('div', { class: 'form-actions' }, sky.note, sky.button), sky.el, details);
  el.append(card);
  let collapsed = false, g = null;
  /* Verdicts (hints, red messages) only while corrections come from the receiver. */
  const delivering = () => !!(S.status && stationState(S.status, true).delivering);
  /* No receiver, or the station looking for it: the card hides. */
  function receiverState(st) {
    const r = (st && st.receiver) || {};
    const c = !r.rx_maintenance && (r.state === 'no_receiver' || r.state === 'detecting' || r.state === 'idle');
    card.classList.toggle('sky-open', !sky.plot.hidden);
    if (c === collapsed) return;
    collapsed = c;
    card.hidden = c;
    if (c) sky.hide();
  }
  receiverState(S.status);

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
    const live = delivering();
    const checks = live && sig.epoch_age_ms < 5000 ? signalChecks(sig) : null;
    patch(head, h('div', { class: 'big' }, signalsHeadline(sig)), checks && !checks.waiting ? receptionList(checks) : null);
    patch(bands, systems.length ? [bandTable(systems), h('p', { class: 'muted small foot' }, BANDS_FOOT)] : h('p', { class: 'muted' },
      sig.epoch_age_ms === null || sig.epoch_age_ms === undefined ? 'No observations yet.' : 'No observations in the last 10 seconds.'));
    if (interacting(details)) return;
    patch(body, kv([
      ['Last epoch', sig.epoch_age_ms === null || sig.epoch_age_ms === undefined ? 'Never' : fmt.ms(sig.epoch_age_ms) + ' ago'],
      ['Epochs', fmt.num(sig.epochs)],
      ['Missing epochs', sig.epoch_gaps === null || sig.epoch_gaps === undefined ? 'Not counted (no observation messages in the profile)' :
        fmt.num(sig.epoch_gaps)],
      ['Outages or time jumps', fmt.num(sig.epoch_jumps || 0)],
    ]), h('h3', { class: 'sub' }, 'Messages'),
    sig.messages ? messagesView(sig.messages, live, !!(g && (g.survey || {}).active)) : h('p', { class: 'muted' }, 'No message statistics.'));
  }

  const stop = poller(2000, loadSignals);
  return {
    sky: sky,
    update: receiverState,
    /* GET /api/gnss: the survey (1005 waits for it) and whether the sky can open (it needs automatic configuration). */
    gnss(gg) {
      g = gg;
      sky.block(g.autocfg ? '' : 'Not available while you manage the receiver yourself (Receiver settings).');
    },
    destroy() {
      stop();
      sky.destroy();
    },
  };
}
