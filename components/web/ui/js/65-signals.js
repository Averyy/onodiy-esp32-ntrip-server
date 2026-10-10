/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signals: frequency bands per constellation from the MSM data (with mean
 * CNR from MSM4 to MSM7), the Galileo E6 indicator, the RTCM messages
 * seen against the profile, and the sky view of satellites in view.
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
};

function cnrClass(v) {
  if (v === null || v === undefined) return '';
  return v < 35 ? 'cnr-weak' : v < 42 ? 'cnr-ok' : 'cnr-good';
}

function bandTable(systems) {
  const showE5ab = systems.some(s => (s.bands || []).indexOf('e5ab') >= 0);
  const bands = BAND_ORDER.filter(b => b !== 'e5ab' || showE5ab);
  return h('div', { class: 'scroll' }, h('table', { class: 'bands' },
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
          return h('td', { class: on ? 'on' : 'off' }, on ? '✓ ' + (name || BAND_HEAD[b]) : DASH,
            on && cnr[b] !== null && cnr[b] !== undefined ? h('small', { class: cnrClass(cnr[b]) }, cnr[b] + ' dB-Hz') : null);
        }),
        h('td', { class: 'num' }, h('b', { class: have.length >= 4 ? 't-ok' : null }, String(have.length))));
    }))));
}

function e6View(sig) {
  const gal = (sig.systems || []).find(s => s.sys === 'galileo');
  const e6 = gal && (gal.bands || []).indexOf('l6') >= 0;
  if (sig.e6_lost) {
    return msg('bad', h('strong', null, 'Galileo E6 lost.'), ' It was tracked before with the current signal group.');
  }
  if (e6) return msg('ok', h('strong', null, 'Galileo E6 tracked.'), ' That counts as a fourth band for Galileo.');
  return msg('', h('strong', null, 'Galileo E6 not tracked.'),
    ' Whether the receiver tracks E6 depends on its signal group (3 and 6 add it on the UM982).');
}

function messagesTable(list) {
  return table(['Message', 'Name', 'Every (profile)', 'Every (seen)', 'Count', 'Last seen'], list.map(m => {
    const expected = m.expected_period_s;
    const missing = expected !== null && expected !== undefined &&
      (!m.count || (m.age_ms !== null && m.age_ms !== undefined && m.age_ms > expected * 3000 + 2000));
    return [
      h('b', { class: missing ? 't-bad' : null }, String(m.msg)),
      rtcmName(m.msg) || DASH,
      expected === null || expected === undefined ? h('span', { class: 'muted' }, 'not in profile') : expected + ' s',
      m.period_ms ? fmt.ms(m.period_ms) : DASH,
      fmt.num(m.count),
      m.age_ms === null || m.age_ms === undefined ? 'never' : h('span', { class: missing ? 't-bad' : null }, fmt.ms(m.age_ms) + ' ago'),
    ];
  }), { num: [4] });
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
      return [msg('info', 'The receiver does not send satellite-in-view data: it is off in the receiver profile.'),
        onEnable ? h('div', { class: 'form-actions' }, onEnable) : null];
    }
    return msg('', 'Satellite-in-view output is on, but the receiver has not sent any in the last 30 seconds.');
  }
  const sats = (sky.satellites || []).slice().sort((a, b) => (a.sys < b.sys ? -1 : a.sys > b.sys ? 1 : a.prn - b.prn));
  const systems = [];
  for (const s of sats) if (systems.indexOf(s.sys) < 0) systems.push(s.sys);
  return h('div', { class: 'sky-wrap' },
    h('div', null, skyPlot(sats), h('div', { class: 'legend foot' }, systems.map(s => h('span', null, h('i', { class: 'sys-' + s }), lbl(L.sys, s))))),
    table(['Constellation', 'Satellite', 'Elevation', 'Azimuth', 'CNR'], sats.map(s => [lbl(L.sys, s.sys), String(s.prn),
      s.el_deg + '°', s.az_deg + '°', s.cnr_dbhz ? h('span', { class: cnrClass(s.cnr_dbhz) }, s.cnr_dbhz + ' dB-Hz') : DASH]), { num: [1, 2, 3, 4] }));
}

Views.signals = {
  title: 'Signals',
  create(el) {
    const summary = h('section', { class: 'card wide' });
    const bandsCard = h('section', { class: 'card wide' });
    const msgsCard = h('section', { class: 'card wide' });
    const skyCard = h('section', { class: 'card wide' });
    mount(el, h('div', { class: 'grid' }, summary, bandsCard, msgsCard, skyCard));
    mount(summary, h('p', { class: 'loading' }, 'Loading signals…'));

    const enableBtn = isAdmin() ? btn('Turn it on', async () => {
      const ok = await dialog({ title: 'Turn on satellite-in-view output?', ok: 'Turn on',
        body: h('p', null, 'The station saves the receiver profile with this output on and configures the receiver again. ' +
          'The RTCM stream can pause for a few seconds.') });
      if (!ok) return;
      await act(enableBtn, async () => {
        const g = await api('GET', '/api/gnss');
        const profile = Object.assign({}, g.profile, { sat_view_output: true });
        await api('PUT', '/api/gnss/profile', { profile: profile });
        toast('Saved. The sky view appears once the receiver sends the data.', 'ok');
      });
    }, 'primary') : null;

    async function loadSignals() {
      let sig;
      try {
        sig = await api('GET', '/api/gnss/signals');
      } catch (e) {
        mount(summary, msg('bad', errText(e)));
        return;
      }
      const systems = sig.systems || [];
      mount(summary,
        h('h2', null, 'Signals', badge(sig.best_bands >= 4 ? 'Enough bands' : 'Few bands', sig.best_bands >= 4 ? 'ok' : 'warn')),
        h('div', { class: 'big' }, sig.best_sys ? lbl(L.sys, sig.best_sys) + ' · ' + sig.best_bands + ' bands' : 'No signals yet'),
        h('p', { class: 'muted small' }, 'Best constellation by distinct frequency bands; Onocoy rewards four or more.'),
        e6View(sig),
        kv([
          ['Last epoch', sig.epoch_age_ms === null || sig.epoch_age_ms === undefined ? 'never' : fmt.ms(sig.epoch_age_ms) + ' ago'],
          ['Epochs', fmt.num(sig.epochs)],
          ['Epoch gaps', sig.epoch_gaps === null || sig.epoch_gaps === undefined ? 'not counted (no MSM message in the profile)' :
            fmt.num(sig.epoch_gaps) + ' missing at ' + fmt.ms(sig.epoch_period_ms)],
          sig.epoch_jumps ? ['Outages or time jumps', fmt.num(sig.epoch_jumps)] : null,
          sig.rtcm1230_supported === false ? ['Message 1230', h('span', { class: 't-warn' }, 'Refused by the receiver')] : null,
        ]));
      mount(bandsCard, h('h2', null, 'Bands per constellation'),
        systems.length ? bandTable(systems) : h('p', { class: 'muted' }, 'No MSM observations yet.'),
        h('p', { class: 'muted small foot' }, 'Seen in the last 10 seconds. CNR is the mean per band (MSM4 to MSM7).'));
      mount(msgsCard, h('h2', null, 'RTCM messages'),
        sig.messages ? messagesTable(sig.messages) : h('p', { class: 'muted' }, 'No message statistics.'));
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
      const key = JSON.stringify(sky);
      if (key === lastSky) return;
      lastSky = key;
      mount(skyCard, h('h2', null, 'Sky', sky.available ? badge((sky.satellites || []).length + ' satellites', 'plain') : null), skyView(sky, enableBtn));
    }

    const stopA = poller(2000, loadSignals);
    const stopB = poller(5000, loadSky);
    return {
      destroy() {
        stopA();
        stopB();
      },
    };
  },
};
