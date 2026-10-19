/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The sky map in the Receiver page's Signals card: the satellites where the
 * receiver profile's satellite positions put them (GET /api/gnss/live, read
 * while the page shows it), the rest from the RTCM stream. The setup
 * wizard's antenna check reuses the plot, the counts and the hints (and,
 * for a profile without the positions, opens the station's live window,
 * POST /api/gnss/live, which turns them on for a while).
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

/* mini: the Status page's preview, without the satellites' names and the elevation labels (each still named in its
 * title): each dot in its constellation's colour, fainter the weaker its signal. */
function liveSkyPlot(sats, mini) {
  const C = 170, R = 140;
  const placed = sats.filter(hasSkyPos).sort((a, b) => (satCnr(a) || 0) - (satCnr(b) || 0));   /* strongest on top */
  const plot = svg('svg', { class: 'sky live-sky' + (mini ? ' mini' : ''), viewBox: '0 0 340 340', role: 'img',
    'aria-label': 'Sky plot: ' + placed.length + ' satellites placed by azimuth and elevation, coloured by C/N0. The list shows every satellite.' });
  for (const el of [0, 30, 60]) plot.append(svg('circle', { class: 'ring', cx: C, cy: C, r: (R * (90 - el)) / 90 }));
  plot.append(svg('line', { class: 'axis', x1: C, y1: C - R, x2: C, y2: C + R }), svg('line', { class: 'axis', x1: C - R, y1: C, x2: C + R, y2: C }));
  for (const l of [['N', C, C - R - 8], ['S', C, C + R + 16], ['E', C + R + 11, C + 4], ['W', C - R - 11, C + 4]]) {
    plot.append(svg('text', { class: 'lbl', x: l[1], y: l[2], 'text-anchor': 'middle' }, l[0]));
  }
  if (!mini) plot.append(svg('text', { class: 'lbl', x: C + 4, y: C - (R * 60) / 90 + 12 }, '30°'), svg('text', { class: 'lbl', x: C + 4, y: C - (R * 30) / 90 + 12 }, '60°'));
  for (const s of placed) {
    const r = (R * (90 - Math.min(90, s.el_deg))) / 90;
    const a = (s.az_deg * Math.PI) / 180;
    const x = C + r * Math.sin(a), y = C - r * Math.cos(a);
    const cnr = satCnr(s);
    const step = cnrStep(cnr);
    const cls = mini ? 'sys-' + s.sys + ' ' + (step < 0 ? 'qn' : 'q' + step) : step < 0 ? 'none' : 'b' + step;
    plot.append(svg('g', null,
      svg('title', null, satId(s) + ' (' + lbl(L.sys, s.sys) + ' ' + s.prn + '): elevation ' + s.el_deg + '°, azimuth ' + s.az_deg + '°' +
        (cnr ? ', ' + cnr + ' dB-Hz' : ', no C/N0')),
      svg('circle', { class: 'sat ' + cls, cx: x.toFixed(1), cy: y.toFixed(1), r: mini ? 13 : 11 }),
      mini ? null : svg('text', { class: 'sid ' + cls, x: x.toFixed(1), y: (y + 3).toFixed(1), 'text-anchor': 'middle' }, satId(s))));
  }
  return plot;
}

function cnrLegend() {
  return h('div', { class: 'legend foot' }, h('span', null, 'C/N0 (dB-Hz):'),
    CNR_STEP_LABEL.map((t, i) => h('span', null, h('i', { class: 'cnr-sw b' + i }), t)),
    h('span', null, h('i', { class: 'cnr-sw none' }), 'none'));
}

const SYS_KEY = 'G GPS · R GLONASS · E Galileo · C BeiDou · J QZSS · S SBAS · I NavIC';

/* ---- Every satellite, one line each (sortable) ------------------------------------------- */

const SAT_SORTS = { sys: 'Satellite', el: 'Elev.', cnr: 'C/N0' };

/* Every satellite in one line (id, elevation, best C/N0, C/N0 per band); the sort buttons keep the focus. {el, update(d)}. */
function satRows() {
  let sort = store.get('liveSort', 'sys');
  if (!SAT_SORTS[sort]) sort = 'sys';
  let bands = null, last = null;
  const head = h('thead');
  const body = h('tbody');
  const note = h('p', { class: 'muted small foot' });

  const sortHead = (key, num) => {
    const on = sort === key;
    return h('th', { class: (num ? 'num ' : '') + 'sortable', 'aria-sort': on ? (key === 'sys' ? 'ascending' : 'descending') : null },
      h('button', { type: 'button', class: 'sort', 'data-sort': key, onclick: () => {
        sort = key;
        store.set('liveSort', key);
        renderHead(true);
        if (last) renderBody(last);
      } }, SAT_SORTS[key], h('span', { class: 'arrow', 'aria-hidden': 'true' }, on ? (key === 'sys' ? ' ▲' : ' ▼') : '')));
  };

  function renderHead(keepFocus) {
    const focused = keepFocus && doc.activeElement && doc.activeElement.getAttribute ? doc.activeElement.getAttribute('data-sort') : null;
    mount(head, h('tr', null, sortHead('sys'), sortHead('el', true), sortHead('cnr', true),
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
      return h('tr', null,
        h('td', { title: lbl(L.sys, s.sys) + ' ' + s.prn }, h('i', { class: 'dot sys-' + s.sys }), h('b', null, satId(s))),
        h('td', { class: 'num' }, s.el_deg === null || s.el_deg === undefined ? DASH : s.el_deg + '°'),
        h('td', { class: 'num' }, best ? h('b', { class: cnrClass(best) }, String(best)) : DASH),
        bands.map(b => h('td', { class: 'num' }, c[b] ? h('span', { class: cnrClass(c[b]) }, String(c[b])) : b in c ? '✓' : DASH)));
    }));
    const t = d.totals || {};
    /* The band table's note above says the unit. */
    note.textContent = (t.dropped ? t.dropped + ' more not listed. ' : '') + '✓ = tracked, no C/N0 value.';
  }

  return {
    el: h('div', null, scrollBox('Satellites', h('table', { class: 'rows' }, head, body)), note),
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

/* ---- Counts and tiles (the setup wizard's reception check) ---------------------------- */

function liveCountsTable(t) {
  const bands = liveBands(t);
  const systems = t.systems || [];
  if (!systems.length) return h('p', { class: 'muted' }, 'No satellites tracked yet.');
  /* Short headings on a phone: every column fits. */
  const wide = (text, short) => [h('span', { class: 'wide-only' }, text), h('span', { class: 'phone-only' }, short)];
  return scrollBox('Satellites per constellation', h('table', { class: 'counts' },
    h('thead', null, h('tr', null, h('th', null, wide('Constellation')), h('th', { class: 'num' }, wide('Satellites', 'Sats')),
      bands.map(b => h('th', { class: 'num' }, b === 'e5ab' ? wide(BAND_HEAD[b], 'E5') : BAND_HEAD[b])), h('th', { class: 'num' }, 'Bands'))),
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

function tile(label, value, sub) {
  return h('div', { class: 'tile' }, h('div', { class: 'k' }, label), h('div', { class: 'v' }, value), sub ? h('div', { class: 'muted small' }, sub) : null);
}

/* The totals as tiles (the setup wizard's reception check). */
function liveTiles(t) {
  return h('div', { class: 'tiles' },
    tile('Satellites tracked', fmt.num(t.tracked), t.satellites > t.tracked ? 'of ' + t.satellites + ' listed' : null),
    tile('Signals', fmt.num(t.signals)),
    tile('Mean C/N0', t.cnr_dbhz ? String(t.cnr_dbhz) : DASH, 'dB-Hz'),
    tile('In the sky plot', fmt.num(t.with_pos)));
}

/* ---- Station map (on request, in the Receiver page's Position card) ---------------------- */

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

/* The station map: tiles only after "Show the map" (remembered here), and a link to OpenStreetMap. {el, update(position)}. */
function mapBox() {
  const el = h('div', { class: 'stack map-box' });
  let key = null;
  function render(pos) {
    const have = pos && pos.source && pos.source !== 'none' && pos.lat_deg !== null && pos.lat_deg !== undefined;
    el.hidden = !have;
    if (!have) return clear(el);
    const lat = pos.lat_deg, lon = pos.lon_deg;
    const ext = 'https://www.openstreetmap.org/?mlat=' + lat.toFixed(7) + '&mlon=' + lon.toFixed(7) + '#map=17/' + lat.toFixed(6) + '/' + lon.toFixed(6);
    const box = h('div');
    const offer = () => mount(box, h('div', { class: 'row' }, btn('Show the map', show, 'small'),
      h('span', { class: 'muted small' }, 'Loads from OpenStreetMap over the internet.')));
    function show() {
      store.set('map', true);
      mount(box, tileMosaic(lat, lon, () => mount(box, msg('', 'The map did not load. This device may have no internet.'))),
        h('div', { class: 'row foot' }, btn('Hide the map', () => {
          store.set('map', false);
          offer();
        }, 'small ghost')));
    }
    mount(el, box, h('div', { class: 'foot' }, extLink('Open in OpenStreetMap', ext)));
    if (store.get('map', false)) show();
    else offer();
  }
  return {
    el: el,
    update(pos) {
      const k = JSON.stringify(pos ? [pos.source, pos.lat_deg, pos.lon_deg] : null);
      if (k !== key) {
        key = k;
        render(pos);
      }
    },
  };
}

/* ---- The sky map (the profile's satellite positions) -------------------------------------- */

/**
 * The sky in the Receiver page's Signals card while the receiver profile
 * sends satellite positions (Sky map in Receiver settings): plot (beside the
 * band table) and list (every satellite, under that table), read from GET
 * /api/gnss/live every LIVE_POLL_MS. Nothing to open: the station never
 * toggles the output the profile sends. Both stay hidden while no satellite
 * is tracked (the card's head says why). Returns {plot, list, set(on,
 * manual), watch(f), destroy()}: set(true) reads, set(false) hides and stops
 * (manual: receiver setup is manual, so only an apply sends the positions).
 */
function skyMap() {
  let stopPoll = null, gone = false, on = false, manual = false, onChange = null;
  const plotBox = h('div');
  const note = h('p', { class: 'muted small' });
  const plot = h('div', { class: 'sky-pane', hidden: true }, plotBox, cnrLegend(), h('p', { class: 'muted small' }, SYS_KEY), note);
  const rows = satRows();
  const list = h('div', { class: 'sat-list', hidden: true }, rows.el);
  const show = vis => {
    if (plot.hidden !== vis) return;
    plot.hidden = list.hidden = !vis;
    if (onChange) onChange();
  };
  const endPoll = () => {
    if (stopPoll) stopPoll();
    stopPoll = null;
  };

  function render(d) {
    if (gone || !on) return;
    const t = d.totals;
    const sats = t ? d.satellites || [] : [];
    show(sats.length > 0);
    if (!sats.length) return;
    patch(plotBox, liveSkyPlot(sats));
    const placed = sats.filter(hasSkyPos).length;
    /* None this boot with manual setup: the profile may never have reached the receiver. */
    note.textContent = placed ? placed + ' of ' + sats.length + ' satellites on the map.' :
      manual && (t.sky_age_ms === null || t.sky_age_ms === undefined) ?
        'No satellite positions yet. With manual setup, Apply again in Receiver settings sends them.' :
        'Waiting for the first satellite positions from the receiver.';
    rows.update(d);
  }

  return {
    plot: plot,
    list: list,
    set(want, man) {
      manual = !!man;
      if (!!want === on) return;
      on = !!want;
      if (on) {
        stopPoll = poller(LIVE_POLL_MS, async () => render(await api('GET', '/api/gnss/live')));
      } else {
        endPoll();
        show(false);
      }
    },
    /* f(): called when the sky shows or hides (the card hides what has nothing to show). */
    watch(f) {
      onChange = f;
    },
    destroy() {
      gone = true;
      endPoll();
    },
  };
}

/* ---- No receiver ------------------------------------------------------------------------- */

/**
 * What a page shows instead of satellites while no receiver is found:
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
    /* A closed port (the station keeps reopening it): no search to ask for, no cable advice. */
    const shut = !!r.uart_closed;
    const searching = !shut && (r.state === 'detecting' || r.state === 'idle' ||
      (r.state === 'no_receiver' && (a.next_detect_in_s === 0 || Date.now() - askedAt < 4000)));
    if (searching) {
      if (!searchSince) searchSince = Date.now();
      el.className = 'msg info';
      title.textContent = 'Looking for the receiver… (' + fmt.dur((Date.now() - searchSince) / 1000) + ')';
      text.textContent = 'The station tries every link speed the receiver may use.';
      actions.hidden = true;
    } else {
      searchSince = 0;
      el.className = 'msg bad';
      title.textContent = shut ? 'Receiver port closed' : 'No receiver found';
      text.textContent = shut ? UART_CLOSED_TEXT : noReceiverText(a);
      actions.hidden = shut;
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

/* ---- Reception hints (the setup wizard and the Signals card) ------------------------------- */

/*
 * Guide values for a first look at the antenna's sky view. PROVISIONAL, to
 * be confirmed on hardware: what a clear sky gives a multi-band antenna,
 * not Onocoy's own scoring.
 */
const RX_CHECK = { satsGood: 20, satsFair: 10, cnrGood: 40, cnrFair: 35, bands: 4 };
/* The constellations Onocoy rewards: QZSS and NavIC never count as the best (as GET /api/gnss/signals best_sys). */
const REWARDED = ['gps', 'glonass', 'galileo', 'beidou'];

/**
 * Pass/warn hints from the live data: {waiting} or {items: [[tone, title,
 * text, line, id]], ok}. title and text: the setup's two lines; line: the
 * same in one line (the Signals card); id: 'sats', 'cnr' or 'bands'.
 */
function receptionChecks(d) {
  const t = d && d.totals;
  if (!t || (!t.tracked && (t.msm_age_ms === null || t.msm_age_ms === undefined))) return { waiting: true };
  const items = [];
  const sats = t.tracked + ' satellites tracked';
  if (t.tracked >= RX_CHECK.satsGood) {
    items.push(['ok', sats, 'A good view of the sky.', sats, 'sats']);
  } else if (t.tracked >= RX_CHECK.satsFair) {
    items.push(['warn', sats, 'Fewer than an open sky gives. Trees, walls or a roof edge may hide part of the sky.',
      sats + ': check that nothing hides the sky', 'sats']);
  } else {
    items.push(['bad', sats, 'Very few. Check that the antenna sees the open sky and that its cable is connected.',
      sats + ': check the antenna\'s view and cable', 'sats']);
  }
  /* Judged on L1, the band every satellite has: L2, L5 and L6 run a few dB lower on any antenna, so a mean over every
   * band reads "fair" for a good one. The mean over every band only without L1's. */
  const l1 = t.bands && t.bands.l1 && t.bands.l1.cnr_dbhz;
  const c = l1 || t.cnr_dbhz;
  const on = l1 ? ' on L1' : '';
  const db = c ? ' (' + c + NB + 'dB\u2011Hz' + on + ')' : '';
  const title = 'Signal strength' + on + ' ' + (c ? c + NB + 'dB\u2011Hz' : 'unknown');
  if (c && c >= RX_CHECK.cnrGood) {
    items.push(['ok', title, 'Strong signals.', 'Strong signals' + db, 'cnr']);
  } else if (c && c >= RX_CHECK.cnrFair) {
    items.push(['warn', title, 'Fair. A partly hidden sky, or a long or damaged cable, lowers it.',
      'Fair signals' + db + ': check the antenna\'s view and cable', 'cnr']);
  } else {
    items.push(['bad', title, 'Weak. Check the antenna, its cable and its connectors.',
      (c ? 'Weak signals' + db : 'Signal strength unknown') + ': check the antenna and its cable', 'cnr']);
  }
  const most = mostBands((t.systems || []).map(s => ({ sys: s.sys, n: Object.keys(s.bands || {}).filter(b => s.bands[b] > 0).length })));
  const bestN = most.n;
  if (bestN >= RX_CHECK.bands) {
    items.push(['ok', most.names + ': ' + bestN + ' bands', '4 bands on one constellation: the most this receiver can reach.',
      bestN + ' bands on ' + most.names, 'bands']);
  } else {
    items.push(['warn', bestN ? 'Best: ' + most.names + ' with ' + bestN + (bestN === 1 ? ' band' : ' bands') : 'No bands yet',
      'Onocoy\'s quality score needs 4 bands on one constellation. The signal group in Receiver settings sets them; ' +
      'right after a start this can take a minute.', 'Onocoy needs 4 bands: change the signal group in Receiver settings', 'bands']);
  }
  return { items: items, ok: items.every(i => i[0] === 'ok') };
}

/* The constellations Onocoy rewards that have the most bands (list: [{sys, n}]): {n, names: "Galileo and BeiDou"}. */
function mostBands(list) {
  const rewarded = list.filter(x => REWARDED.indexOf(x.sys) >= 0 && x.n > 0);
  const n = rewarded.reduce((m, x) => Math.max(m, x.n), 0);
  const names = rewarded.filter(x => x.n === n).map(x => lbl(L.sys, x.sys));
  return { n: n, names: names.length > 1 ? names.slice(0, -1).join(', ') + ' and ' + names[names.length - 1] : names[0] || '' };
}

const CHECK_ICON = { ok: '✓', warn: '!', bad: '✕' };
const CHECK_WORD = { ok: 'Good', warn: 'Check', bad: 'Problem' };

/* The hints as a list; oneLine: each in one line (the item's line; its tone word only for assistive technology). */
function receptionList(r, oneLine) {
  return h('ul', { class: 'checks' + (oneLine ? ' lines' : '') }, r.items.map(i => h('li', { class: i[0] },
    h('span', { class: 'ic', 'aria-hidden': 'true' }, CHECK_ICON[i[0]]),
    oneLine ? h('span', null, h('span', { class: 'sr-only' }, CHECK_WORD[i[0]] + ': '), i[3]) :
      h('span', null, h('strong', null, CHECK_WORD[i[0]] + ': ' + i[1]), h('div', { class: 'muted small' }, i[2])))));
}
