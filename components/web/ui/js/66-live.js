/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Live view, satellites first: a window the user opens on request
 * (POST /api/gnss/live) for a limited time. It can be extended and stopped;
 * the station closes it when its time is up, or when no page asked for it
 * for a while: every GET /api/gnss/live keeps it open, so a page polls only
 * while it is shown (poller() pauses in a hidden tab). While it is open the
 * receiver also sends azimuth and elevation, so the sky plot can place the
 * satellites; everything else comes from the RTCM stream.
 *
 * Order on the page: the window (time left, Extend, Stop), the sky plot
 * coloured by C/N0, the satellite table, satellites per constellation and
 * band, the mean C/N0 per band, the totals as tiles; last and optional, the
 * station map. The setup wizard's reception check reuses the pieces.
 */

const LIVE_POLL_MS = 2000;

/* C/N0 classes for colour (dB-Hz): under 30, 30-34, 35-39, 40-44, 45 or more. */
const CNR_STEPS = [30, 35, 40, 45];
const CNR_STEP_LABEL = ['under 30', '30–34', '35–39', '40–44', '45 or more'];

function cnrStep(v) {
  if (!v) return -1;
  let i = 0;
  while (i < CNR_STEPS.length && v >= CNR_STEPS[i]) i++;
  return i;
}

/* Satellite names as in RINEX files: G12, R05, E11, C23, J02, S20, I03. */
const SYS_LETTER = { gps: 'G', glonass: 'R', galileo: 'E', beidou: 'C', qzss: 'J', sbas: 'S', navic: 'I' };

function satId(s) {
  let n = s.prn;
  if (s.sys === 'qzss' && n > 192) n -= 192;
  else if (s.sys === 'sbas' && n >= 100) n -= 100;
  return (SYS_LETTER[s.sys] || '?') + String(n).padStart(2, '0');
}

/** Best C/N0 of a satellite: its strongest band, else its satellite-in-view report (null = none). */
function satCnr(s) {
  let best = null;
  const c = s.cnr_dbhz || {};
  for (const b of Object.keys(c)) {
    if (c[b] && (best === null || c[b] > best)) best = c[b];
  }
  return best === null && s.sky_cnr_dbhz ? s.sky_cnr_dbhz : best;
}

function hasSkyPos(s) {
  return s.el_deg !== null && s.el_deg !== undefined && s.az_deg !== null && s.az_deg !== undefined && s.el_deg >= 0;
}

/** Bands with tracked satellites, in table order. */
function liveBands(t) {
  return BAND_ORDER.filter(b => t && t.bands && t.bands[b]);
}

/* ---- Sky plot -------------------------------------------------------------- */

function liveSkyPlot(sats) {
  const C = 170, R = 140;
  const placed = sats.filter(hasSkyPos).sort((a, b) => (satCnr(a) || 0) - (satCnr(b) || 0));   /* strongest on top */
  const plot = svg('svg', { class: 'sky live-sky', viewBox: '0 0 340 340', role: 'img',
    'aria-label': 'Sky plot: ' + placed.length + ' satellites placed by azimuth and elevation, coloured by C/N0. The table lists every satellite.' });
  for (const el of [0, 30, 60]) plot.append(svg('circle', { class: 'ring', cx: C, cy: C, r: (R * (90 - el)) / 90 }));
  plot.append(svg('line', { class: 'axis', x1: C, y1: C - R, x2: C, y2: C + R }), svg('line', { class: 'axis', x1: C - R, y1: C, x2: C + R, y2: C }));
  for (const l of [['N', C, C - R - 8], ['S', C, C + R + 16], ['E', C + R + 11, C + 4], ['W', C - R - 11, C + 4]]) {
    plot.append(svg('text', { class: 'lbl', x: l[1], y: l[2], 'text-anchor': 'middle' }, l[0]));
  }
  plot.append(svg('text', { class: 'lbl', x: C + 4, y: C - (R * 60) / 90 + 12 }, '30°'), svg('text', { class: 'lbl', x: C + 4, y: C - (R * 30) / 90 + 12 }, '60°'));
  for (const s of placed) {
    const r = (R * (90 - Math.min(90, s.el_deg))) / 90;
    const a = (s.az_deg * Math.PI) / 180;
    const x = C + r * Math.sin(a), y = C - r * Math.cos(a);
    const cnr = satCnr(s);
    const step = cnrStep(cnr);
    const cls = step < 0 ? 'none' : 'b' + step;
    plot.append(svg('g', null,
      svg('title', null, satId(s) + ' (' + lbl(L.sys, s.sys) + ' ' + s.prn + '): elevation ' + s.el_deg + '°, azimuth ' + s.az_deg + '°' +
        (cnr ? ', ' + cnr + ' dB-Hz' : ', no C/N0')),
      svg('circle', { class: 'sat ' + cls, cx: x.toFixed(1), cy: y.toFixed(1), r: 11 }),
      svg('text', { class: 'sid ' + cls, x: x.toFixed(1), y: (y + 3).toFixed(1), 'text-anchor': 'middle' }, satId(s))));
  }
  return plot;
}

function cnrLegend() {
  return h('div', { class: 'legend foot' }, h('span', null, 'C/N0 (dB-Hz):'),
    CNR_STEP_LABEL.map((t, i) => h('span', null, h('i', { class: 'cnr-sw b' + i }), t)),
    h('span', null, h('i', { class: 'cnr-sw none' }), 'none'));
}

const SYS_KEY = 'G GPS · R GLONASS · E Galileo · C BeiDou · J QZSS · S SBAS · I NavIC';

/* ---- Satellite table (sortable) ------------------------------------------------ */

const SAT_SORTS = { sys: 'Constellation', cnr: 'C/N0', el: 'Elevation' };

/**
 * Satellite table with its header kept between refreshes (the sort buttons
 * keep keyboard focus); the rows are patched in place. Returns {el, update(d)}.
 */
function liveSatTable() {
  let sort = store.get('liveSort', 'sys');
  if (!SAT_SORTS[sort]) sort = 'sys';
  let bands = null, last = null;
  const head = h('thead');
  const body = h('tbody');
  const note = h('p', { class: 'muted small foot' });

  const sortHead = (key, label, num) => {
    const on = sort === key;
    return h('th', { class: (num ? 'num ' : '') + 'sortable', 'aria-sort': on ? (key === 'sys' ? 'ascending' : 'descending') : null },
      h('button', { type: 'button', class: 'sort', 'data-sort': key, onclick: () => {
        sort = key;
        store.set('liveSort', key);
        renderHead(true);
        if (last) renderBody(last);
      } }, label, h('span', { class: 'arrow', 'aria-hidden': 'true' }, on ? (key === 'sys' ? ' ▲' : ' ▼') : '')));
  };

  function renderHead(keepFocus) {
    const focused = keepFocus && doc.activeElement && doc.activeElement.getAttribute ? doc.activeElement.getAttribute('data-sort') : null;
    mount(head, h('tr', null,
      h('th', null, 'Satellite'),
      sortHead('sys', SAT_SORTS.sys),
      sortHead('el', SAT_SORTS.el, true),
      h('th', { class: 'num' }, 'Azimuth'),
      sortHead('cnr', 'Best C/N0', true),
      (bands || []).map(b => h('th', { class: 'num' }, BAND_HEAD[b]))));
    if (focused) {
      const again = head.querySelector('[data-sort="' + focused + '"]');
      if (again) again.focus();
    }
  }

  function sorted(sats) {
    const list = sats.slice();
    const nullLast = (a, b) => (a === null || a === undefined ? -1000 : a) - (b === null || b === undefined ? -1000 : b);
    if (sort === 'cnr') list.sort((a, b) => nullLast(satCnr(b), satCnr(a)));
    else if (sort === 'el') list.sort((a, b) => nullLast(hasSkyPos(b) ? b.el_deg : null, hasSkyPos(a) ? a.el_deg : null));
    return list;   /* sys: the station's order (constellation, then number) */
  }

  function renderBody(d) {
    const sats = d.satellites || [];
    patch(body, sorted(sats).map(s => {
      const c = s.cnr_dbhz || {};
      const best = satCnr(s);
      /* data-label: the column's name, shown beside the value on a phone (stacked rows). */
      return h('tr', null,
        h('td', { 'data-label': 'Satellite' }, h('b', null, satId(s))),
        h('td', { 'data-label': SAT_SORTS.sys }, h('span', { class: 'legend' }, h('span', null, h('i', { class: 'sys-' + s.sys }), lbl(L.sys, s.sys) + ' ' + s.prn))),
        h('td', { class: 'num', 'data-label': SAT_SORTS.el }, s.el_deg === null || s.el_deg === undefined ? DASH : s.el_deg + '°'),
        h('td', { class: 'num', 'data-label': 'Azimuth' }, s.az_deg === null || s.az_deg === undefined ? DASH : s.az_deg + '°'),
        h('td', { class: 'num', 'data-label': 'Best C/N0' }, best ? h('span', { class: cnrClass(best) }, best + '\u00a0dB-Hz') : DASH),
        bands.map(b => h('td', { class: 'num', 'data-label': BAND_HEAD[b] }, c[b] ? h('span', { class: cnrClass(c[b]) }, String(c[b])) : b in c ? '✓' : DASH)));
    }));
    const t = d.totals || {};
    const noPos = sats.filter(s => !hasSkyPos(s)).length;
    note.textContent = [
      noPos ? noPos + ' without a position in the sky yet (or below the horizon): listed here, not plotted.' : '',
      t.dropped ? t.dropped + ' more not listed.' : '',
      'C/N0 per band from the RTCM observations; ✓ = signal tracked without a C/N0 value.',
    ].filter(Boolean).join(' ');
  }

  return {
    el: h('div', null, scrollBox('Satellites', h('table', { class: 'sats stack-sm' }, head, body)), note),
    update(d) {
      last = d;
      const nb = liveBands(d.totals);
      for (const s of d.satellites || []) for (const b of Object.keys(s.cnr_dbhz || {})) if (nb.indexOf(b) < 0) nb.push(b);
      nb.sort((a, b) => BAND_ORDER.indexOf(a) - BAND_ORDER.indexOf(b));
      if (!bands || bands.join() !== nb.join()) {
        bands = nb;
        renderHead(true);
      }
      renderBody(d);
    },
  };
}

/* ---- Counts, band means, tiles ---------------------------------------------------- */

function liveCountsTable(t) {
  const bands = liveBands(t);
  const systems = t.systems || [];
  if (!systems.length) return h('p', { class: 'muted' }, 'No satellites tracked yet.');
  return scrollBox('Satellites per constellation', h('table', { class: 'counts' },
    h('thead', null, h('tr', null, h('th', null, 'Constellation'), h('th', { class: 'num' }, 'Satellites'),
      bands.map(b => h('th', { class: 'num' }, BAND_HEAD[b])), h('th', { class: 'num' }, 'Bands'))),
    h('tbody', null, systems.map(s => {
      const per = s.bands || {};
      const n = Object.keys(per).filter(b => per[b] > 0).length;
      return h('tr', null,
        h('td', null, h('span', { class: 'legend' }, h('span', null, h('i', { class: 'sys-' + s.sys }), lbl(L.sys, s.sys)))),
        h('td', { class: 'num' }, fmt.num(s.satellites)),
        bands.map(b => h('td', { class: 'num' }, per[b] ? String(per[b]) : DASH)),
        h('td', { class: 'num' }, h('b', { class: n >= 4 ? 't-ok' : null }, String(n))));
    })),
    h('tfoot', null, h('tr', null, h('th', null, 'All'), h('td', { class: 'num' }, fmt.num(t.tracked)),
      bands.map(b => h('td', { class: 'num' }, fmt.num(t.bands[b].satellites))), h('td', null, '')))));
}

/* Mean C/N0 per band as bars on a 0 to 55 dB-Hz scale. */
const CNR_SCALE = 55;

function liveBandBars(t) {
  const bands = liveBands(t);
  if (!bands.length) return h('p', { class: 'muted' }, 'No signals yet.');
  return [h('div', { class: 'bandbars' }, bands.map(b => {
    const v = t.bands[b].cnr_dbhz;
    return [h('span', { class: 'band' }, BAND_HEAD[b]),
      h('div', { class: 'bar', role: 'meter', 'aria-label': BAND_HEAD[b] + ' mean C/N0', 'aria-valuemin': 0, 'aria-valuemax': CNR_SCALE,
        'aria-valuenow': v || 0 }, h('span', { style: { width: (Math.min(1, (v || 0) / CNR_SCALE) * 100).toFixed(1) + '%' } })),
      h('span', { class: 'num' }, v ? v + ' dB-Hz' : DASH, h('small', { class: 'muted' }, ' · ' + t.bands[b].satellites + ' sat.'))];
  })), h('p', { class: 'muted small foot' }, 'Mean over the satellites tracked on each band. Scale 0 to ' + CNR_SCALE + ' dB-Hz.')];
}

function tile(label, value, sub) {
  return h('div', { class: 'tile' }, h('div', { class: 'k' }, label), h('div', { class: 'v' }, value), sub ? h('div', { class: 'muted small' }, sub) : null);
}

function liveTiles(t) {
  return h('div', { class: 'tiles' },
    tile('Satellites tracked', fmt.num(t.tracked), t.satellites > t.tracked ? 'of ' + t.satellites + ' listed' : null),
    tile('Signals', fmt.num(t.signals)),
    tile('Mean C/N0', t.cnr_dbhz ? String(t.cnr_dbhz) : DASH, 'dB-Hz'),
    tile('Placed in the sky', fmt.num(t.with_pos), 'azimuth and elevation known'));
}

/* ---- Station map (optional, last) ------------------------------------------------------ */

const MAP_ZOOM = 16, MAP_TILE = 256, MAP_W = 512, MAP_H = 320;
const MAP_HOST = 'https://tile.openstreetmap.org/';

/**
 * A small OpenStreetMap tile mosaic centred on the position, no library:
 * the tiles are plain images placed in percent of the frame (so it scales
 * with the page). The browser fetches them only after the user asked for
 * the map; each image sends the page's origin as Referer (the tile
 * server's usage policy), and a tile that fails hides the mosaic (no
 * internet on this device: the coordinates stay).
 */
function tileMosaic(lat, lon, onFail) {
  const n = Math.pow(2, MAP_ZOOM);
  const latR = (Math.max(-85, Math.min(85, lat)) * Math.PI) / 180;
  const x = ((lon + 180) / 360) * n * MAP_TILE;
  const y = ((1 - Math.log(Math.tan(latR) + 1 / Math.cos(latR)) / Math.PI) / 2) * n * MAP_TILE;
  const left = x - MAP_W / 2, top = y - MAP_H / 2;
  /* The tiles and the pin are one image to assistive technology; the
   * attribution (a link the tile policy asks for) sits beside it, so it is
   * not hidden as part of the image. */
  const tiles = h('div', { class: 'map-tiles', role: 'img', 'aria-label': 'Map around the station position' });
  const box = h('div', { class: 'map' }, tiles);
  let failed = false;
  for (let ty = Math.floor(top / MAP_TILE); ty <= Math.floor((top + MAP_H - 1) / MAP_TILE); ty++) {
    if (ty < 0 || ty >= n) continue;
    for (let tx = Math.floor(left / MAP_TILE); tx <= Math.floor((left + MAP_W - 1) / MAP_TILE); tx++) {
      const wx = ((tx % n) + n) % n;
      tiles.append(h('img', { src: MAP_HOST + MAP_ZOOM + '/' + wx + '/' + ty + '.png', alt: '', referrerpolicy: 'origin',
        draggable: 'false', decoding: 'async',
        style: { left: (((tx * MAP_TILE - left) / MAP_W) * 100).toFixed(3) + '%', top: (((ty * MAP_TILE - top) / MAP_H) * 100).toFixed(3) + '%',
          width: ((MAP_TILE / MAP_W) * 100).toFixed(3) + '%', height: ((MAP_TILE / MAP_H) * 100).toFixed(3) + '%' },
        onerror: () => {
          if (!failed) {
            failed = true;
            onFail();
          }
        } }));
    }
  }
  tiles.append(h('span', { class: 'pin', 'aria-hidden': 'true' }));
  box.append(h('span', { class: 'map-attr' }, '© ', h('a', { href: 'https://www.openstreetmap.org/copyright', target: '_blank', rel: 'noopener noreferrer' },
      'OpenStreetMap'), ' contributors'));
  return box;
}

/** The station-map card: coordinates always, the map on request. Returns {el, update(position)}. */
function mapPanel() {
  const el = h('section', { class: 'card wide' });
  let key = null;
  function render(pos) {
    const have = pos && pos.source && pos.source !== 'none' && pos.lat_deg !== null && pos.lat_deg !== undefined;
    if (!have) {
      mount(el, h('h2', null, 'Station map'), h('p', { class: 'muted' }, 'No surveyed or locked position yet.'));
      return;
    }
    const lat = pos.lat_deg, lon = pos.lon_deg;
    const ext = 'https://www.openstreetmap.org/?mlat=' + lat.toFixed(7) + '&mlon=' + lon.toFixed(7) + '#map=17/' + lat.toFixed(6) + '/' + lon.toFixed(6);
    const mapBox = h('div');
    const offer = () => mount(mapBox, h('div', { class: 'stack' }, h('p', { class: 'muted small' },
      'The map is loaded from OpenStreetMap over the internet by this browser (the tiles show the area around the station).'),
    h('div', null, btn('Show the map', show, 'small'))));
    function show() {
      store.set('map', true);
      mount(mapBox, tileMosaic(lat, lon, () => mount(mapBox, msg('', 'The map tiles could not be loaded: this device may have no internet. ' +
        'The coordinates above are what the map would show.'))),
      h('div', { class: 'row foot' }, btn('Hide the map', () => {
        store.set('map', false);
        offer();
      }, 'small ghost')));
    }
    mount(el,
      cardHead('Station map', badge(pos.locked ? 'Locked position' : 'Last surveyed', pos.locked ? 'ok' : '')),
      kv([
        ['Latitude', fmt.deg(lat)],
        ['Longitude', fmt.deg(lon)],
        ['Height (ellipsoid)', fmt.m(pos.height_m)],
        ['Source', lbl(L.posSource, pos.source)],
      ]),
      mapBox,
      h('div', { class: 'row foot' }, h('a', { href: ext, target: '_blank', rel: 'noopener noreferrer' }, 'Open this position in OpenStreetMap')));
    if (store.get('map', false)) show();
    else offer();
  }
  return {
    el: el,
    update(pos) {
      const k = JSON.stringify(pos || null);
      if (k !== key) {
        key = k;
        render(pos);
      }
    },
  };
}

/* ---- The window: time left, Extend, Stop --------------------------------------------- */

function skyOutputText(w) {
  const why = w.sky_why && w.sky_why !== 'none' ? lbl(L.liveWhy, w.sky_why) : '';
  switch (w.sky) {
    case 'on':
      return 'Satellite positions: on, every ' + w.sky_period_s + ' s (not saved in the receiver).';
    case 'on_profile':
      return 'Satellite positions: on (the receiver profile sends them all the time).';
    case 'pending':
      return 'Satellite positions: waiting' + (why ? ': ' + why : '') + '.';
    case 'blocked':
      if (w.sky_why === 'maintenance') {
        return 'Satellite positions: turned off for receiver maintenance. Extend the view to turn them on again (after maintenance). ' +
          'The table still lists every satellite.';
      }
      if (w.sky_why === 'no_answer') {
        return 'Satellite positions: the receiver did not answer the command' + (w.sky_reply ? ' (' + w.sky_reply + ')' : '') +
          '. They may be on all the same; they are turned off when the view closes. Extend the view to try again.';
      }
      return 'Satellite positions: not available in this window' + (why ? ': ' + why : '') +
        (w.sky_reply ? ' (' + w.sky_reply + ')' : '') + '. The table still lists every satellite.';
    case 'stopping':
      return 'Satellite positions: turning off.';
    default:
      return '';
  }
}

/**
 * The window card. opts: onStart(button, errBox), onExtend(...), onStop(...),
 * each returning a promise. Returns {el, update(window), connLost(text),
 * connOk(), stop(), leftMs()}.
 */
function liveWindowCard(opts) {
  const badgeEl = badge('', '');
  const leftText = h('span', { class: 'countdown' });
  const state = h('p', { class: 'state-line' });
  const announce = h('span', { class: 'sr-only', role: 'status' });   /* opened / closed, said once */
  const sky = h('p', { class: 'small' });
  const err = errBox();
  const conn = h('p', { class: 'msg warn', role: 'status', hidden: true });   /* polling failed: once, until it works again */
  let deadline = 0, open = false, seen = false;
  /* Start hides itself, Stop hides itself and Extend: after the action the
   * keyboard focus goes to the button that is there now (Start <-> Extend). */
  const run = (fn, b) => async () => {
    try {
      await fn(b, err);
    } finally {
      setTimeout(() => {
        const a = doc.activeElement;
        if (a && a !== doc.body && !a.hidden && el.contains(a)) return;
        if (a && a !== doc.body && a !== b && !el.contains(a)) return;   /* the user went elsewhere meanwhile */
        const t = !b.hidden ? b : open ? extendBtn : startBtn;
        if (!t.hidden) t.focus();
      }, 0);
    }
  };
  const startBtn = btn('Start', () => run(opts.onStart, startBtn)(), 'primary');
  const extendBtn = btn('Extend', () => run(opts.onExtend, extendBtn)(), 'primary');
  const stopBtn = btn('Stop', () => run(opts.onStop, stopBtn)());
  const el = h('section', { class: 'card wide' },
    cardHead('Live view', badgeEl),
    announce, state, sky, conn, err,
    h('div', { class: 'form-actions' }, startBtn, extendBtn, stopBtn),
    h('p', { class: 'muted small foot' }, 'While the view is open the receiver also reports where each satellite is in the sky. ' +
      'Switching that output on and off pauses the casters for a moment. The view closes by itself when its time is up, ' +
      'or about two minutes after no page shows it.'));
  const tick = () => {
    if (!open) return;
    leftText.textContent = fmt.dur(Math.max(0, Math.round((deadline - Date.now()) / 1000)));
  };
  const timer = setInterval(tick, 1000);
  return {
    el: el,
    update(w) {
      const was = open;
      open = !!w.open;
      if (seen && was !== open) announce.textContent = open ? 'Live view open.' : 'Live view closed.';
      seen = true;
      badgeEl.className = 'badge ' + (open ? 'ok' : '');
      badgeEl.textContent = open ? 'Open' : 'Closed';
      startBtn.hidden = open;
      extendBtn.hidden = stopBtn.hidden = !open;
      startBtn.textContent = 'Start (' + w.default_min + ' min)';
      const again = w.sky === 'blocked' && ['reconfigured', 'maintenance', 'no_answer'].indexOf(w.sky_why) >= 0;
      extendBtn.textContent = again ? 'Extend and turn positions on again' : 'Extend by ' + w.default_min + ' min';
      if (open) {
        deadline = Date.now() + w.left_s * 1000;
        mount(state, 'Open: ', leftText, ' left (at most ' + w.max_min + ' min from now with extensions).');
        tick();
      } else {
        const end = w.last_end && w.last_end !== 'none' ? ' It closed because ' + lbl(L.liveEnd, w.last_end) + '.' : '';
        mount(state, 'The live view is closed.' + end);
      }
      sky.textContent = open ? skyOutputText(w) : '';
      sky.hidden = !sky.textContent;
    },
    connLost(text) {
      const t = 'Refreshing failed: ' + text + ' The figures below are from the last answer; the page keeps asking.';
      if (conn.hidden || conn.textContent !== t) {
        conn.textContent = t;
        conn.hidden = false;
      }
    },
    connOk() {
      conn.hidden = true;
    },
    stop() {
      clearInterval(timer);
    },
    leftMs() {
      return open ? deadline - Date.now() : 0;
    },
  };
}

/* ---- No receiver ------------------------------------------------------------------------- */

/** The receiver is not found, or the station is looking for it (a station status's receiver.state). */
function receiverMissing(st) {
  const r = st && st.receiver;
  return !!r && !r.rx_maintenance && (r.state === 'no_receiver' || r.state === 'detecting');
}

/**
 * What the page shows instead of satellites while no receiver is found:
 * what to check, when the station looks again, and "Check again" (a
 * search now). While a search runs: "Looking for the receiver…" with the
 * time it has taken. Returns {el, update(status)}: a GET /api/status
 * object (the wizard polls it, the app has it from the WebSocket).
 */
function noReceiverPanel() {
  const title = h('strong');
  const text = h('div', { class: 'small' });
  const err = errBox();
  let askedAt = 0, searchSince = 0, last = null;
  const check = btn('Check again', () => act(check, async () => {
    await api('POST', '/api/gnss/detect', {});
    askedAt = Date.now();
    render();
  }, err), 'small');
  const actions = h('div', { class: 'form-actions' }, check);
  const el = h('div', { class: 'msg bad', role: 'status' }, title, text, actions, err);
  function render() {
    const r = (last && last.receiver) || {};
    const a = ((last && last.alerts) || []).find(x => x.source === 'station' && x.id === 'no_receiver') ||
      { saw_bytes: false, next_detect_in_s: r.next_detect_in_s };
    const searching = r.state === 'detecting' || r.state === 'idle' ||
      (r.state === 'no_receiver' && (a.next_detect_in_s === 0 || Date.now() - askedAt < 4000));
    if (searching) {
      if (!searchSince) searchSince = Date.now();
      el.className = 'msg info';
      title.textContent = 'Looking for the receiver… (' + fmt.dur((Date.now() - searchSince) / 1000) + ')';
      text.textContent = 'The station tries every link speed the receiver may use.';
      actions.hidden = true;
    } else {
      searchSince = 0;
      el.className = 'msg bad';
      title.textContent = 'No receiver found';
      text.textContent = noReceiverText(a);
      actions.hidden = false;
    }
  }
  return {
    el: el,
    update(st) {
      last = st;
      render();
    },
  };
}

/* ---- The page ---------------------------------------------------------------------------- */

/* Set by the entry button (Signals page): the page starts the window when it opens. */
let liveAutoStart = false;

/* The plain-link entry (the dashboard, rebuilt every second, carries no buttons): opens the page and starts the window. */
const LIVE_START_HASH = '#/signals/live/start';

/** Open the live view page and start the window there. */
function openLiveView() {
  liveAutoStart = true;
  if (location.hash === '#/signals/live') openView('signals', 'live');
  else location.hash = '#/signals/live';
}

function livePage(el, startArg) {
  if (!isAdmin()) {
    mount(el, card('Live view', h('p', null, 'Log in to open the live view.'),
      h('div', { class: 'form-actions' }, btn('Log in', () => showLogin(), 'primary'))));
    return {};
  }
  let stopPoll = null, gone = false;
  /* A 200 to our own start means open: a closed window read within 5 s of it is a stale answer, ignored once. */
  let postAt = 0, closeIgnored = false;
  const posted = () => {
    postAt = Date.now();
    closeIgnored = false;
  };
  const startPoll = () => {
    if (!stopPoll) stopPoll = poller(LIVE_POLL_MS, fetchLive, true);
  };
  const endPoll = () => {
    if (stopPoll) stopPoll();
    stopPoll = null;
  };

  const win = liveWindowCard({
    onStart: (b, out) => act(b, async () => {
      posted();
      show(await api('POST', '/api/gnss/live', {}));
    }, out),
    onExtend: (b, out) => act(b, async () => {
      posted();
      show(await api('POST', '/api/gnss/live', {}));
    }, out),
    onStop: (b, out) => act(b, async () => {
      try {
        await api('DELETE', '/api/gnss/live');
      } catch (e) {
        if (!(e instanceof ApiError && e.code === 'live_view_closed')) throw e;
      }
      show(await api('GET', '/api/gnss/live'));
    }, out),
  });
  const skyBox = h('div');
  const skyNote = h('p', { class: 'muted small' });
  const table = liveSatTable();
  const countsBox = h('div');
  const barsBox = h('div');
  const tilesBox = h('div');
  const map = mapPanel();
  const dataCards = [
    h('section', { class: 'card wide' }, h('h2', null, 'Sky'), h('div', { class: 'sky-wrap' }, skyBox,
      h('div', null, cnrLegend(), h('p', { class: 'muted small' }, SYS_KEY), skyNote))),
    h('section', { class: 'card wide' }, h('h2', null, 'Satellites'), table.el),
    h('section', { class: 'card' }, h('h2', null, 'Per constellation and band'), countsBox,
      h('p', { class: 'muted small foot' }, 'Satellites tracked on each band. Onocoy\'s top band reward needs 4 bands on one constellation.')),
    h('section', { class: 'card' }, h('h2', null, 'Mean C/N0 per band'), barsBox),
    h('section', { class: 'card wide' }, h('h2', null, 'Totals'), tilesBox),
  ];
  const closedNote = h('section', { class: 'card wide' }, h('p', { class: 'muted' },
    'Start the live view to see every satellite, where it is in the sky and how strong its signals are.'));
  /* No receiver: the same card as the setup wizard's, instead of an empty window. */
  const nr = noReceiverPanel();
  const nrCard = h('section', { class: 'card wide', hidden: true }, h('h2', null, 'Live view'), nr.el,
    h('p', { class: 'muted small' }, 'The live view opens once the station finds the receiver. ', link('Receiver page', '#/receiver')));
  mount(el, h('div', { class: 'grid' }, nrCard, win.el, closedNote, dataCards, map.el));
  map.update(null);

  let missing = false, pendingStart = false;
  /* Status from the WebSocket (every second): no receiver shows the card; the receiver back starts what was asked. */
  function receiverState(st) {
    const m = receiverMissing(st);
    if (m) nr.update(st);
    if (m === missing) return;
    missing = m;
    nrCard.hidden = !m;
    win.el.hidden = m;
    map.el.hidden = m;
    if (m) {
      closedNote.hidden = true;
      for (const c of dataCards) c.hidden = true;
      endPoll();
      return;
    }
    if (pendingStart) {
      pendingStart = false;
      start();
    } else {
      fetchLive();
    }
  }

  function start() {
    posted();
    act(null, async () => {
      show(await api('POST', '/api/gnss/live', {}));
      return true;
    }).then(r => {
      if (r === undefined) fetchLive();   /* refused: show the window as it is */
    });
  }

  function show(d) {
    if (gone || !d || missing) return;
    const w = d.window || {};
    if (w.open === false && Date.now() - postAt < 5000 && !closeIgnored) {
      closeIgnored = true;
      startPoll();   /* keep asking: the next answer tells */
      return;
    }
    win.connOk();
    win.update(w);
    map.update(d.position);
    closedNote.hidden = !!w.open;
    for (const c of dataCards) c.hidden = !w.open;
    if (!w.open) {
      endPoll();
      return;
    }
    startPoll();
    const t = d.totals;
    if (!t) {
      patch(skyBox, msg('warn', 'The station cannot read the observations right now.'));
      return;
    }
    const sats = d.satellites || [];
    patch(skyBox, liveSkyPlot(sats));
    const placed = sats.filter(hasSkyPos).length;
    skyNote.textContent = placed ? placed + ' of ' + sats.length + ' satellites placed.' :
      w.sky === 'on' || w.sky === 'on_profile' ? 'Waiting for the first satellite positions from the receiver.' :
        'No satellite positions in this window: see the line under Live view.';
    table.update(d);
    patch(countsBox, liveCountsTable(t));
    patch(barsBox, liveBandBars(t));
    patch(tilesBox, liveTiles(t));
  }

  async function fetchLive() {
    let d;
    try {
      d = await api('GET', '/api/gnss/live');
    } catch (e) {
      /* Once in the card, not a toast every 2 s (a phone off Wi-Fi, a restarting station). */
      if (!gone && !(e instanceof ApiError && e.status === 401)) win.connLost(errText(e));
      return;
    }
    show(d);
  }

  if (startArg) {
    /* Started once: a reload or the back button must not start it again. */
    history.replaceState(null, '', '#/signals/live');
    if (shell.current) shell.current.arg = 'live';
  }
  const wantStart = liveAutoStart || startArg;
  liveAutoStart = false;
  if (receiverMissing(S.status)) {
    /* Nothing to open: the card says why and when the station looks again. */
    pendingStart = wantStart;
    receiverState(S.status);
  } else if (wantStart) {
    start();
  } else {
    fetchLive();
  }
  return {
    update(st) {
      receiverState(st);
    },
    destroy() {
      gone = true;
      endPoll();
      win.stop();
    },
  };
}

/* ---- Reception check (setup wizard) --------------------------------------------------------- */

/*
 * Guide values for a first look at the antenna's sky view. PROVISIONAL, to
 * be confirmed on hardware: what a clear sky gives a multi-band antenna,
 * not Onocoy's own scoring.
 */
const RX_CHECK = { satsGood: 20, satsFair: 10, cnrGood: 40, cnrFair: 35, bands: 4 };

/** Pass/warn hints from the live data: {waiting} or {items: [[tone, title, text]], ok}. */
function receptionChecks(d) {
  const t = d && d.totals;
  if (!t || (!t.tracked && (t.msm_age_ms === null || t.msm_age_ms === undefined))) return { waiting: true };
  const items = [];
  if (t.tracked >= RX_CHECK.satsGood) {
    items.push(['ok', t.tracked + ' satellites tracked', 'A good view of the sky.']);
  } else if (t.tracked >= RX_CHECK.satsFair) {
    items.push(['warn', t.tracked + ' satellites tracked', 'Fewer than an open sky gives. Trees, walls or a roof edge may hide part of the sky.']);
  } else {
    items.push(['bad', t.tracked + ' satellites tracked', 'Very few. Check that the antenna sees the open sky and that its cable is connected.']);
  }
  const c = t.cnr_dbhz;
  if (c && c >= RX_CHECK.cnrGood) {
    items.push(['ok', 'Mean signal strength ' + c + ' dB-Hz', 'Strong signals.']);
  } else if (c && c >= RX_CHECK.cnrFair) {
    items.push(['warn', 'Mean signal strength ' + c + ' dB-Hz', 'Fair. Check the antenna cable and its connectors, and keep metal and walls away from the antenna.']);
  } else {
    items.push(['bad', 'Mean signal strength ' + (c ? c + ' dB-Hz' : 'unknown'), 'Weak. Check the antenna, its cable and its connectors.']);
  }
  let best = null, bestN = 0;
  for (const s of t.systems || []) {
    const n = Object.keys(s.bands || {}).filter(b => s.bands[b] > 0).length;
    if (n > bestN) {
      best = s.sys;
      bestN = n;
    }
  }
  if (bestN >= RX_CHECK.bands) {
    items.push(['ok', lbl(L.sys, best) + ': ' + bestN + ' bands', 'Four bands on one constellation: the condition for Onocoy\'s top band reward.']);
  } else {
    items.push(['warn', best ? 'Best: ' + lbl(L.sys, best) + ' with ' + bestN + (bestN === 1 ? ' band' : ' bands') : 'No bands yet',
      'Onocoy\'s top band reward needs 4 bands on one constellation. The receiver profile chooses the signals; right after a start it can also take a minute.']);
  }
  return { items: items, ok: items.every(i => i[0] === 'ok') };
}

const CHECK_ICON = { ok: '✓', warn: '!', bad: '✕' };
const CHECK_WORD = { ok: 'Good', warn: 'Check', bad: 'Problem' };

function receptionList(r) {
  return h('ul', { class: 'checks' }, r.items.map(i => h('li', { class: i[0] },
    h('span', { class: 'ic', 'aria-hidden': 'true' }, CHECK_ICON[i[0]]),
    h('span', null, h('strong', null, CHECK_WORD[i[0]] + ': ' + i[1]), h('span', { class: 'muted small' }, ' ' + i[2])))));
}
