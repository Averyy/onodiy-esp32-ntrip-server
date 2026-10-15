/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signals: the summary (best constellation, Galileo E6, the entry to the
 * live view, #/signals/live in 66-live.js), frequency bands per
 * constellation from the MSM data (with the mean C/N0 from MSM4 to MSM7),
 * the RTCM messages seen against the profile, and the sky view of
 * satellites in view. Without a receiver (none found, or the station still
 * looking for it) the page is one card that says so: empty tables and red
 * "missing" rows would only mislead.
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

/* Galileo E6: tracked, or lost; "not tracked" only while no constellation has 4 bands (then it would help). */
function e6View(sig) {
  const gal = (sig.systems || []).find(s => s.sys === 'galileo');
  const e6 = gal && (gal.bands || []).indexOf('l6') >= 0;
  if (sig.e6_lost) return msg('bad', h('strong', null, 'Galileo E6 lost.'), ' It was tracked before with this signal group.');
  if (e6) return msg('ok', h('strong', null, 'Galileo E6 tracked:'), ' a fourth Galileo band.');
  if (sig.best_bands >= 4) return null;
  return msg('', h('strong', null, 'Galileo E6 not tracked.'), ' Signal group 3 6 adds it.');
}

/* A seen period in words: whole seconds when it is close to them ("1 s" for 996 ms). */
function periodText(ms) {
  const s = ms / 1000, r = Math.round(s);
  return r >= 1 && Math.abs(s - r) < 0.05 * r ? r + NB + 's' : fmt.ms(ms);
}

/* A message name without its MSM type, for the one line of extra messages ("1107 SBAS observations"). */
const rtcmShort = n => (rtcmName(n) || '').replace(/ \(.*\)$/, '');

/*
 * The profile's messages: number and name, how often each arrives (the
 * profile's period beside it when it differs), when it was last seen. The
 * messages outside the profile are one line under the table. colour: mark
 * messages the receiver should send but does not (only while it streams).
 */
function messagesView(list, colour) {
  const mine = list.filter(m => m.expected_period_s !== null && m.expected_period_s !== undefined);
  const extra = list.filter(m => (m.expected_period_s === null || m.expected_period_s === undefined) && m.count);
  let red = false;
  const rows = mine.map(m => {
    const expected = m.expected_period_s;
    const missing = colour && (!m.count || (m.age_ms !== null && m.age_ms !== undefined && m.age_ms > expected * 3000 + 2000));
    red = red || missing;
    const seen = m.period_ms ? periodText(m.period_ms) : DASH;
    return [
      h('span', { class: missing ? 't-bad' : null }, h('b', null, String(m.msg)), ' ' + (rtcmName(m.msg) || '')),
      [seen, m.period_ms && seen !== expected + NB + 's' ? h('span', { class: 'muted' }, ' (profile: ' + expected + NB + 's)') : null],
      m.age_ms === null || m.age_ms === undefined ? 'Never' : h('span', { class: missing ? 't-bad' : null }, fmt.ms(m.age_ms) + NB + 'ago'),
    ];
  });
  return [
    rows.length ? table(['Message', 'Every', 'Last seen'], rows, { num: [1, 2] }) : h('p', { class: 'muted' }, 'No messages in the profile.'),
    red ? h('p', { class: 'small' }, h('span', { class: 't-bad' }, 'Red:'), ' in the profile but not arriving.') : null,
    extra.length ? h('p', { class: 'muted small' }, 'Also in the stream, not in the profile: ' +
      extra.map(m => m.msg + (rtcmShort(m.msg) ? ' ' + rtcmShort(m.msg) : '')).join(', ') + '.') : null,
  ];
}

function skyPlot(sats) {
  const C = 170, R = 145;
  const plot = svg('svg', { class: 'sky', viewBox: '0 0 340 340', role: 'img', 'aria-label': 'Sky view: ' + sats.length + ' satellites' });
  for (const el of [0, 30, 60]) plot.append(svg('circle', { class: 'ring', cx: C, cy: C, r: (R * (90 - el)) / 90 }));
  plot.append(svg('line', { class: 'axis', x1: C, y1: C - R, x2: C, y2: C + R }), svg('line', { class: 'axis', x1: C - R, y1: C, x2: C + R, y2: C }));
  const labels = [['N', C, C - R - 8], ['S', C, C + R + 16], ['E', C + R + 10, C + 4], ['W', C - R - 10, C + 4]];
  for (const l of labels) plot.append(svg('text', { class: 'lbl', x: l[1], y: l[2], 'text-anchor': 'middle' }, l[0]));
  plot.append(svg('text', { class: 'lbl', x: C + 4, y: C - (R * 60) / 90 + 12 }, '30°'), svg('text', { class: 'lbl', x: C + 4, y: C - (R * 30) / 90 + 12 }, '60°'));
  for (const s of sats) {
    if (s.el_deg === null || s.el_deg === undefined || s.az_deg === null || s.az_deg === undefined) continue;
    const r = (R * (90 - Math.max(0, Math.min(90, s.el_deg)))) / 90;
    const a = (s.az_deg * Math.PI) / 180;
    const x = C + r * Math.sin(a), y = C - r * Math.cos(a);
    plot.append(svg('g', null,
      svg('title', null, lbl(L.sys, s.sys) + ' ' + s.prn + ': elevation ' + s.el_deg + '°, azimuth ' + s.az_deg + '°' +
        (s.cnr_dbhz ? ', ' + s.cnr_dbhz + ' dB-Hz' : '')),
      svg('circle', { class: 'sat sys-' + s.sys, cx: x.toFixed(1), cy: y.toFixed(1), r: 10 }),
      svg('text', { class: 'prn', x: x.toFixed(1), y: (y + 3).toFixed(1), 'text-anchor': 'middle' }, String(s.prn))));
  }
  return plot;
}

function skyView(sky, onEnable) {
  if (!sky.available) {
    if (sky.reason === 'sat_view_off') {
      return [h('p', null, 'Satellite positions are off in the receiver profile. The live view turns them on while it is open.'),
        onEnable ? h('div', { class: 'form-actions' }, onEnable) : null];
    }
    return h('p', { class: 'muted' }, 'No satellite positions from the receiver in the last 30 seconds.');
  }
  const sats = (sky.satellites || []).slice().sort((a, b) => (a.sys < b.sys ? -1 : a.sys > b.sys ? 1 : a.prn - b.prn));
  const systems = [];
  for (const s of sats) if (systems.indexOf(s.sys) < 0) systems.push(s.sys);
  return h('div', { class: 'sky-wrap' },
    h('div', null, skyPlot(sats), h('div', { class: 'legend foot' }, systems.map(s => h('span', null, h('i', { class: 'sys-' + s }), lbl(L.sys, s))))),
    table(['Constellation', 'Satellite', 'Elevation', 'Azimuth', 'C/N0'], sats.map(s => [lbl(L.sys, s.sys), String(s.prn),
      s.el_deg + '°', s.az_deg + '°', s.cnr_dbhz ? h('span', { class: cnrClass(s.cnr_dbhz) }, s.cnr_dbhz + '\u00a0dB-Hz') : DASH]), { num: [1, 2, 3, 4], stack: true }));
}

Views.signals = {
  title: 'Signals',
  create(el, arg) {
    if (arg === 'live' || arg === 'live/start') return livePage(el, arg === 'live/start');
    const summary = h('section', { class: 'card wide' });
    const bandsCard = h('section', { class: 'card wide' });
    const msgsCard = h('section', { class: 'card wide' });
    const skyCard = h('section', { class: 'card wide' });
    /* The way into the live view: in the summary card, built once (a refresh moves it, never rebuilds it). */
    const liveRow = isAdmin() ? h('div', { class: 'form-actions' },
      h('span', { class: 'note muted small' }, 'Every satellite in view and how strong its signals are, live for a few minutes.'),
      btn('Open the live view', openLiveView, 'primary')) : null;
    /* No receiver: the page is this one card. */
    const nr = isAdmin() ? noReceiverPanel() : null;
    const nrPub = h('div');
    const nrCard = h('section', { class: 'card wide', hidden: true }, h('h2', null, 'Signals'), nr ? nr.el : nrPub);
    mount(el, h('div', { class: 'grid' }, nrCard, summary, bandsCard, msgsCard, skyCard));
    mount(summary, h('p', { class: 'loading' }, 'Loading signals…'));
    let collapsed = false;
    const streaming = () => !!(S.status && S.status.receiver && S.status.receiver.state === 'streaming');
    function receiverState(st) {
      const r = (st && st.receiver) || {};
      const c = !r.rx_maintenance && (r.state === 'no_receiver' || r.state === 'detecting' || r.state === 'idle');
      if (c && nr) nr.update(st);
      if (c && !nr) mount(nrPub, msg(r.state === 'no_receiver' ? 'bad' : 'info', r.state === 'no_receiver' ? 'No receiver found.' : 'Looking for the receiver.'));
      if (c === collapsed) return;
      collapsed = c;
      nrCard.hidden = !c;
      for (const x of [summary, bandsCard, msgsCard, skyCard]) x.hidden = c;
    }
    receiverState(S.status);

    const enableBtn = isAdmin() ? btn('Turn on', async () => {
      const ok = await dialog({ title: 'Turn on satellite positions?', ok: 'Turn on',
        body: h('p', null, 'The station saves this in the receiver profile and configures the receiver. Corrections pause for a few seconds.') });
      if (!ok) return;
      await act(enableBtn, async () => {
        const g = await api('GET', '/api/gnss');
        const profile = Object.assign({}, g.profile, { sat_view_output: true });
        await api('PUT', '/api/gnss/profile', { profile: profile });
        toast('Saved. The Sky card fills once the receiver sends the positions.', 'ok');
      });
    }) : null;

    async function loadSignals() {
      let sig;
      try {
        sig = await api('GET', '/api/gnss/signals');
      } catch (e) {
        mount(summary, msg('bad', errText(e)), liveRow);
        return;
      }
      if (collapsed) return;
      const systems = sig.systems || [];
      const live = streaming();   /* verdicts (bands, E6, refused messages) only while the receiver streams */
      /* A rebuild waits while the user reads or selects in the card; the Details stay as the user left them. */
      if (!interacting(summary)) {
        mount(summary,
          cardHead('Signals', live ? badge(sig.best_bands >= 4 ? 'Enough bands' : 'Few bands', sig.best_bands >= 4 ? 'ok' : 'warn') : null),
          h('div', { class: 'big' }, sig.best_sys ? lbl(L.sys, sig.best_sys) + ' · ' + sig.best_bands + ' bands' : 'No signals yet'),
          h('p', { class: 'muted small' }, 'Best constellation. Onocoy\'s quality score needs 4 bands on one constellation.'),
          live ? e6View(sig) : null,
          kv([['Last epoch', sig.epoch_age_ms === null || sig.epoch_age_ms === undefined ? 'Never' : fmt.ms(sig.epoch_age_ms) + ' ago']]),
          moreAs('sig-details', 'Details', isOpen(summary, 'sig-details'), kv([
            ['Epochs', fmt.num(sig.epochs)],
            ['Missing epochs', sig.epoch_gaps === null || sig.epoch_gaps === undefined ? 'Not counted (no observation messages in the profile)' :
              fmt.num(sig.epoch_gaps)],
            ['Outages or time jumps', fmt.num(sig.epoch_jumps || 0)],
            live && sig.rtcm1230_supported === false ? ['GLONASS biases (1230)', h('span', { class: 't-warn' }, 'Refused by the receiver')] : null,
          ])),
          liveRow);
      }
      mount(bandsCard, h('h2', null, 'Bands per constellation'),
        systems.length ? bandTable(systems) : h('p', { class: 'muted' }, 'No observations yet.'),
        h('p', { class: 'muted small foot' }, 'Last 10 seconds. Figures: mean C/N0 per band, in dB-Hz.'));
      mount(msgsCard, h('h2', null, 'RTCM messages'),
        sig.messages ? messagesView(sig.messages, live) : h('p', { class: 'muted' }, 'No message statistics.'));
    }

    let lastSky = '';
    async function loadSky() {
      let sky;
      try {
        sky = await api('GET', '/api/sky');
      } catch (e) {
        lastSky = '';
        mount(skyCard, h('h2', null, 'Sky'), msg('bad', errText(e)));
        return;
      }
      const live = streaming();
      const key = JSON.stringify([sky, live]);
      if (key === lastSky) return;
      lastSky = key;
      /* "Turn on" saves the profile and reconfigures the receiver: only with a receiver that streams. */
      mount(skyCard, cardHead('Sky', sky.available ? badge((sky.satellites || []).length + ' satellites', 'plain') : null),
        skyView(sky, live ? enableBtn : null));
    }

    const stopA = poller(2000, loadSignals);
    const stopB = poller(5000, () => (collapsed ? Promise.resolve() : loadSky()));
    return {
      update(st) {
        receiverState(st);
      },
      destroy() {
        stopA();
        stopB();
      },
    };
  },
};
