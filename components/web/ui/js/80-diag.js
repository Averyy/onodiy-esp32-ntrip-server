/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics (#/diag, not a tab: reached from Settings): health and loss
 * counters, per-day availability with the connection history next to it,
 * the log (live, previous start, download, levels), the event journal, task
 * statistics and the core dump.
 */

const DIAG_TABS = [
  ['', 'Health'],
  ['availability', 'Availability'],
  ['logs', 'Logs'],
  ['journal', 'Events'],
  ['tasks', 'Tasks'],
  ['coredump', 'Core dump'],
];

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

/* ---- Health ------------------------------------------------------------- */

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

/* "Restart 3 after power-on", or "Power-on" for the first start after one. */
function bootWhen(x) {
  if (x.reset_class === 'power_on' || x.boot_no <= 1) return 'Power-on';
  return 'Restart ' + (x.boot_no - 1) + ' after power-on';
}

/* "If corrections stop": the choice's name, or "Paused" while the station holds the recovery. */
function recoveryNow(hl) {
  if (hl.ladder_suspended) return 'Paused' + ((hl.station || {}).rx_maintenance ? ' (receiver maintenance)' : '');
  return hl.recovery_mode ? lbl(L.recovery, hl.recovery_mode) : DASH;
}

/*
 * Patched in place, not rebuilt (patchDetect: no form controls and no
 * element with a listener of its own here), so the keyboard focus, on "Check
 * again" or a table's scroll box, a text selection and the counters' open
 * sections survive the 5 s poll.
 */
function healthTab(box) {
  let lastHl = null;
  const render = hl => {
    lastHl = hl;
    const b = hl.boot || {};
    const heap = hl.heap || {};
    const link = hl.link || {};
    const net = hl.network || {};
    /* The same alerts as the dashboard (the station's and health's), under the chip. */
    const st = S.status || {};
    const alerts = Array.isArray(st.alerts) ? st.alerts : (hl.alerts || []).map(a => ({ id: a, source: 'health' }));
    const plan = restartPlanText(hl);
    const tone = healthTone(hl.level);
    /* The counters' sections stay as the user left them. */
    const opened = new Set(Array.from(box.querySelectorAll('details[data-k]')).filter(d => d.open).map(d => d.getAttribute('data-k')));
    const sect = (k, title, ...kids) => h('details', { class: 'more', 'data-k': k, open: opened.has(k) }, h('summary', null, title), kids);
    const rxBoot = rxBootSentence((hl.station || {}).rx_boot);
    patchDetect(box, h('div', { class: 'grid' },
      h('section', { class: 'card' },
        cardHead('Health', badge(lbl(L.health, hl.level), tone)),
        alerts.length ? alertList(alerts, { detect: true }) :
          h('p', { class: 'muted' }, tone === 'ok' || tone === '' ? 'No alerts.' : 'No alert names the cause: see the receiver and caster pages.'),
        plan ? h('p', { class: 'small' }, plan) : null,
        kv([
          ['If corrections stop', recoveryNow(hl)],
          link.open === false && link.mode !== 'off' ? ['Receiver link', h('span', { class: 't-bad' }, 'Closed (reopening)')] : null,
          ['Last recovery step', lbl(L.step, hl.last_step)],
          ['Running for', fmt.dur(hl.uptime_s)],
          ['Since the last RTCM', hl.rtcm_gap_ms === null || hl.rtcm_gap_ms === undefined ? 'never received' : fmt.ms(hl.rtcm_gap_ms)],
          hl.gnss_retry_in_s ? ['Next recovery attempt in', fmt.dur(hl.gnss_retry_in_s)] : null,
          hl.inhibited_by ? ['Restarts held back by', hl.inhibited_by] : null,
          hl.update_slot && hl.update_slot !== 'none' ? ['Update running', hl.update_slot === 'app' ? 'an app update' : 'a receiver update'] : null,
          hl.factory_reset_refused_age_s !== null && hl.factory_reset_refused_age_s !== undefined ?
            ['BOOT-button factory reset', h('span', { class: 't-warn' }, 'refused ' + fmt.dur(hl.factory_reset_refused_age_s) +
              ' ago (a receiver update was running)')] : null,
          hl.admin_resets ? ['BOOT-button password resets', fmt.num(hl.admin_resets)] : null,
        ]),
        h('h3', { class: 'sub' }, 'Since this start'),
        kv([
          ['Receiver stalls', fmt.num(hl.gnss_stalls)],
          ['Recovery steps', fmt.num(hl.recoveries)],
          ['Caster restarts', fmt.num(hl.caster_restarts)],
          ['Network recoveries', fmt.num(hl.network_recoveries)],
        ])),
      h('section', { class: 'card' },
        cardHead('This start', b.safe_mode ? badge('Safe mode', 'bad') : b.stable ? badge('Stable', 'ok') : null),
        h('p', null, startSentence(b.reset_class, b.reset_reason, b.prev_reboot_reason) + '.'),
        rxBoot ? h('p', null, rxBoot + '.') : null,
        kv([
          ['Starts since power-on', fmt.num(b.boots_since_power_on)],
          ['Crashes counted', fmt.num(b.crashes)],
          ['Brownouts since power-on', fmt.num(b.brownouts_since_power_on)],
          ['Previous run', fmt.dur(b.prev_session_uptime_s)],
          b.safe_mode ? ['Normal retry in', fmt.dur(b.safe_mode_retry_in_s)] : null,
        ])),
      h('section', { class: 'card' },
        h('h2', null, 'Recent starts'),
        (hl.boot_history || []).length ? table(['When', 'How', 'Version', 'Previous run'],
          hl.boot_history.map(x => [h('span', { class: 'nowrap' }, bootWhen(x)), startSentence(x.reset_class, x.reset_reason, x.planned_reason),
            x.version || DASH, x.prev_uptime_s === null || x.prev_uptime_s === undefined ? 'unknown' : fmt.dur(x.prev_uptime_s)]), { stack: true }) :
          h('p', { class: 'muted' }, 'No history yet.')),
      h('section', { class: 'card' },
        h('h2', null, 'Memory'),
        kv([
          ['Internal free', fmt.bytes(heap.internal_free)],
          ['Internal lowest', fmt.bytes(heap.internal_min)],
          ['Largest internal block', fmt.bytes(heap.internal_largest)],
          ['PSRAM free', fmt.bytes(heap.psram_free)],
          heap.psram_min !== undefined ? ['PSRAM lowest', fmt.bytes(heap.psram_min)] : null,
          heap.psram_largest !== undefined ? ['Largest PSRAM block', fmt.bytes(heap.psram_largest)] : null,
        ])),
      h('section', { class: 'card wide tight-kv' },
        h('h2', null, 'Counters'),
        h('p', { class: 'muted small' }, 'Raw figures for troubleshooting, since this start.'),
        sect('link', 'Receiver link', autoKv(link)),
        sect('framing', 'Framing', autoKv(link.framing)),
        sect('hub', 'RTCM hub', autoKv(hl.hub)),
        sect('station', 'Station', autoKv(hl.station)),
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
        sect('log', 'Log', logStats(hl.log)))));
  };
  detectClicks(box, () => {
    if (lastHl) render(lastHl);
  });
  return { stop: pollInto(box, 5000, '/api/health', render) };
}

/* ---- Event lists (the connection history, the journal) -------------------- */

/* Rows an event list shows first, and how many more each "Show older" adds. */
/* Onocoy's streak needs at least 99.8 % graced uptime per UTC day. */
const STREAK_PERMILLE = 998;

/**
 * "Graced uptime so far" in words and tone: green when it meets 99.8 %, red
 * when it misses it and the streak is lost; below 99.8 % while the streak
 * still holds (early in the day, the budget not used up) neither: not red,
 * since the streak is the verdict, and not green, since it misses the
 * threshold it is measured against.
 */
function soFarView(sofar, streakOk) {
  if (sofar === null || sofar === undefined) return DASH;
  const meets = sofar >= STREAK_PERMILLE;
  const tone = meets ? 't-ok' : streakOk ? null : 't-bad';
  const note = meets ? (streakOk === false ? '' : ' (streak on track)') : streakOk ? ' (below 99.8 %; the streak still holds)' : ' (below 99.8 %)';
  return h('span', { class: tone }, fmt.permille(sofar) + note);
}

const EV_PAGE = 20;

/* "9:41 AM" */
function clockTime(unix) {
  return new Date(unix * 1000).toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
}

/* A record's time: the clock time on `day` (a Date's toDateString), else date and time. */
function dayTime(unix, day) {
  return new Date(unix * 1000).toDateString() === day ? clockTime(unix) : fmt.time(unix);
}

/*
 * Consecutive rows with the same key k become one: the newest, its `all`
 * holding every one (newest first) and its time the span they cover.
 */
function collapse(rows) {
  const out = [];
  for (const r of rows) {
    const last = out[out.length - 1];
    if (last && !last.head && !r.head && last.k === r.k) {
      if (!last.all) {
        last.all = [Object.assign({}, last)];
        last.newest = last.when;
      }
      last.all.push(r);
      last.id = 'r' + r.seq;
      last.when = r.when !== last.newest && r.when.length + last.newest.length < 28 ? r.when + ' to ' + last.newest : last.newest;
    } else {
      out.push(Object.assign({}, r));
    }
  }
  return out;
}

/*
 * Newest first: heading rows {head: [time, text]} and event rows {when, ev,
 * det, tone, seq, all (a collapsed repeat: its "×N" toggle), id}. open: ids
 * of the repeats shown in full (the toggles carry data-rep; the page
 * handles their clicks).
 */
function eventTable(label, rows, open) {
  const tr = [];
  const row = (r, sub) => h('tr', { class: sub ? 'rep-row' : null, title: r.seq ? '#' + r.seq : null },
    h('td', null, r.when),
    h('td', { class: 'ev' + (r.tone ? ' t-' + r.tone : '') }, r.ev, !sub && r.all ? h('button', { type: 'button', class: 'rep', 'data-rep': r.id,
      'aria-expanded': open.has(r.id) ? 'true' : 'false', 'aria-label': (open.has(r.id) ? 'Hide' : 'Show') + ' all ' + r.all.length },
    '×' + r.all.length) : null),
    h('td', null, r.det));
  for (const r of rows) {
    if (r.head) {
      tr.push(h('tr', { class: 'sep' }, h('th', { colspan: '3', scope: 'rowgroup' }, r.head[0] ? h('span', { class: 't' }, r.head[0]) : null, r.head[1])));
      continue;
    }
    tr.push(row(r));
    if (r.all && open.has(r.id)) for (const x of r.all) tr.push(row(x, true));
  }
  return scrollBox(label, h('table', { class: 'events' },
    h('thead', null, h('tr', null, h('th', null, 'When'), h('th', null, 'Event'), h('th', null, 'Details'))),
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

/* A filter select for a card's title row: [[value, label], ...]. */
function filterSelect(choices, onchange) {
  const s = selectInput(choices.map(c => [c[0], c[1]]), choices[0][0], { 'aria-label': 'Show' });
  s.addEventListener('change', () => onchange(s.value));
  return s;
}

/* ---- Availability -------------------------------------------------------- */

/* An RSSI reading in dBm: -127 to -1. Anything else means unknown (null from the station; a raw 0, or -128 from earlier builds). */
function rssiKnown(v) {
  return typeof v === 'number' && v >= -127 && v <= -1;
}

/* ", signal -67 dBm", or nothing when the RSSI is unknown. */
function rssiSuffix(v) {
  return rssiKnown(v) ? ', signal ' + v + ' dBm' : '';
}

/* A caster slot by its name now (the status), else its number. */
function slotName(i, name) {
  const c = ((S.status && S.status.casters) || []).find(x => x.slot === i);
  return name || (c && c.name) || 'Caster ' + (i + 1);
}

const plural = (n, one, many) => n + ' ' + (n === 1 ? one : many);

/*
 * One connection-history record in words, or a drop together with the
 * return that ended it (back: the caster_up or wifi_up after it):
 * [event, details, tone].
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

/*
 * The connection history (GET /api/connections, newest first) as event
 * rows: a drop and the return after it (same caster, or Wi-Fi) make one
 * row, repeats of the same kind of row one row with a "×N" toggle.
 */
function connItems(list) {
  const drop = {}, backOf = new Map(), used = new Set();
  for (const e of list.slice().reverse()) {
    const key = e.kind.indexOf('wifi') === 0 ? 'w' : 'c' + e.slot;
    if (e.kind === 'caster_down' || e.kind === 'wifi_disconnect') {
      drop[key] = e;
    } else if ((e.kind === 'caster_up' || e.kind === 'wifi_up') && drop[key]) {
      backOf.set(drop[key], e);
      used.add(e);
      drop[key] = null;
    } else {
      drop[key] = null;
    }
  }
  const today = new Date().toDateString();
  return collapse(list.filter(e => !used.has(e)).map(e => {
    const c = connRow(e, backOf.get(e));
    return { k: [e.kind, e.slot, e.class || e.reason || e.stop_reason, backOf.has(e)].join('|'), seq: e.seq, ev: c[0], det: c[1], tone: c[2],
      when: e.time_unix ? dayTime(e.time_unix, today) : fmt.dur(e.uptime_s) + ' after a start' };
  }));
}

const CONN_FILTER = [['all', 'All'], ['caster', 'Casters'], ['wifi', 'Wi-Fi']];

/** The connection-history card (GET /api/connections), refreshed every minute and on demand. */
function connHistory() {
  let list = [], complete = true, shown = EV_PAGE, filter = 'all';
  const open = new Set();
  const out = h('div');
  const err = errBox();
  const older = btn('Show older', () => {
    shown += EV_PAGE;
    render();
  });
  function render() {
    const items = connItems(filter === 'all' ? list : list.filter(e => e.kind.indexOf(filter) === 0));
    mount(out, items.length ? eventTable('Connection history', items.slice(0, shown), open) :
      h('p', { class: 'muted' }, 'No dropped connections recorded.'),
    !complete && items.length <= shown ? h('p', { class: 'muted small' }, 'Older records are on the Events tab.') : null);
    older.hidden = items.length <= shown;
  }
  const load = async () => {
    const r = await api('GET', '/api/connections');
    list = r.entries || [];
    complete = r.complete !== false;
    render();
  };
  const refresh = btn('Refresh', () => act(refresh, load, err));
  const sel = filterSelect(CONN_FILTER, v => {
    filter = v;
    shown = EV_PAGE;
    render();
  });
  const el = h('section', { class: 'card wide' },
    cardHead('Connection history', h('div', { class: 'tools grow' }, sel, refresh)),
    h('p', { class: 'muted small' }, 'Caster and Wi-Fi drops, newest first.'),
    err, out, h('div', { class: 'form-actions' }, older));
  repeatClicks(out, open, render);
  const stop = poller(60000, () => load().catch(e => showErr(e, err)));
  return { el: el, stop: stop };
}

function availabilityTab(box) {
  const hist = connHistory();
  const today = h('section', { class: 'card' });
  const casters = h('section', { class: 'card' });
  const month = h('section', { class: 'card wide' });
  /* Built once: the 10 s refresh redraws the three cards, never the history (its filter keeps the focus). */
  let built = false;
  const render = av => {
    if (!built) {
      built = true;
      mount(box, h('div', { class: 'grid' }, today, casters, month, hist.el));
    }
    const slots = av.slots || [];
    const prot = slots.find(s => s.protected);
    const pi = prot ? slots.indexOf(prot) : -1;
    const p = av.protected || {};
    const day = av.today || {};
    const days = av.history || [];
    const sod = av.utc_second_of_day;
    /* An empty slot (no caster in it) has a row only for what it did earlier today, under its number. */
    const figs = ['up_s', 'down_s', 'planned_s', 'reconnects'];
    const dayRows = slots.map((s, i) => [s, i])
      .filter(x => !x[0].empty || figs.some(k => (day[k] || [])[x[1]]))
      .map(x => [x[0].empty ? 'Caster ' + (x[0].slot + 1) : x[0].name || 'Caster ' + (x[0].slot + 1), fmt.dur((day.up_s || [])[x[1]]),
        fmt.dur((day.down_s || [])[x[1]]), fmt.dur((day.planned_s || [])[x[1]]), fmt.num((day.reconnects || [])[x[1]])]);
    /* Onocoy's figures only while its slot is set up and on: numbers for a slot that cannot stream mislead. */
    const counting = prot && p.onocoy_ready !== false && p.enabled !== false;
    const sofar = p.graced_uptime_so_far_permille;
    const notReady = p.onocoy_ready === false;
    mount(today,
      cardHead('Today', day.date ? badge(day.date + ' UTC', 'plain') : null),
      av.clock === 'none' ? msg('warn', 'No trusted clock yet (GNSS or internet time), so seconds cannot be assigned to a day.') : null,
      kv([
        ['Clock', lbl(L.timeSource, av.clock)],
        ['UTC time', sod === null || sod === undefined ? DASH :
          String(Math.floor(sod / 3600)).padStart(2, '0') + ':' + String(Math.floor((sod % 3600) / 60)).padStart(2, '0')],
        prot && !counting ? ['Onocoy', [notReady ? 'Not set up. ' : 'Off. ', link(notReady ? 'Set it up' : 'Turn it on', '#/casters/onocoy')]] : null,
        counting ? ['Onocoy offline today', fmt.dur(p.down_today_s)] : null,
        counting && p.budget_s !== null && p.budget_s !== undefined ? ['Offline budget today', 'about ' + fmt.dur(p.budget_s)] : null,
        counting ? ['Offline budget left', fmt.dur(p.budget_left_s)] : null,
        counting ? ['Data rate since the start', p.data_rate_permille === null || p.data_rate_permille === undefined ? 'nothing sent yet today' :
          fmt.permille(p.data_rate_permille)] : null,
        /* What the budget is figured from: every epoch Onocoy missed today while connected, across restarts. */
        counting && day.lost_epochs !== undefined ? ['Epochs lost today', fmt.num(day.lost_epochs)] : null,
        counting ? ['Graced uptime so far', soFarView(sofar, p.streak_ok)] : null,
        /* (online today + the 300 s grant) / 24 h: what the whole day reaches if nothing more is sent. */
        counting && p.graced_uptime_permille !== null && p.graced_uptime_permille !== undefined ?
          ['Day covered so far (online + 5 min grant)', fmt.permille(p.graced_uptime_permille)] : null,
        ['Powered off', fmt.dur(day.power_off_s)],
        ['UART overflows', fmt.num(day.rx_overflows)],
      ]));
    mount(casters, h('h2', null, 'Casters today'),
      dayRows.length ? [table(['Caster', 'Online', 'Offline', 'Planned', 'Reconnects'], dayRows, { num: [1, 2, 3, 4], stack: true, label: 'Casters today' }),
        h('p', { class: 'muted small foot' }, 'Offline: should have streamed but did not. Planned: receiver setup, updates and restarts you asked for.')] :
        h('p', { class: 'muted' }, 'No casters.'));
    mount(month, h('h2', null, 'Last 30 days'),
      days.length ? table(['Day (UTC)', prot ? 'Onocoy offline' : 'Offline (caster 1)', 'Planned', 'Reconnects', 'Powered off'],
        days.map(d => {
          const i = pi >= 0 ? pi : 0;
          const down = (d.down_s || [])[i];
          const over = counting && down > 473;
          return [d.date || String(d.utc_day), h('span', { class: over ? 't-bad' : null }, fmt.dur(down)), fmt.dur((d.planned_s || [])[i]),
            fmt.num((d.reconnects || [])[i]), fmt.dur(d.power_off_s)];
        }), { num: [1, 2, 3, 4] }) : h('p', { class: 'muted' }, 'No earlier days recorded yet.'),
      counting && days.length ? h('p', { class: 'muted small foot' }, 'Red: more than 473 s offline, about the most Onocoy\'s streak allows in a UTC day.') : null);
  };
  const stopAv = pollInto(box, 10000, '/api/availability', render);
  return {
    stop() {
      stopAv();
      hist.stop();
    },
  };
}

/* ---- Logs ------------------------------------------------------------------ */

const LOG_KEEP = 200000; /* characters kept in the view */
const LOG_LEVELS = ['none', 'error', 'warn', 'info', 'debug', 'verbose'];

/*
 * The log view reads over HTTP up to the newest line, then subscribes to the
 * WebSocket log with that cursor: the station sends each client the lines
 * after its own cursor, and every message says where it starts ("from"), so
 * nothing is shown twice or skipped when the view switches between the two.
 * A message that does not continue where the view stands (one sent before
 * the subscription took effect, or after lines were missed) makes the view
 * read the difference over HTTP and subscribe again from there.
 */
function logsTab(box) {
  let cursor = 0, prev = false, paused = false, loading = false, again = false;
  const pre = h('pre', { class: 'log', tabindex: '0', 'aria-label': 'Log' });
  const follow = checkbox('Follow new lines', true);
  const err = errBox();

  function append(text, gap) {
    const atBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 40;
    if (gap) pre.append(h('span', { class: 'gap' }, '… some lines were overwritten before they could be read …\n'));
    const lines = String(text || '').split('\n');
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i] + (i < lines.length - 1 ? '\n' : '');
      if (!line) continue;
      const c = line.charAt(0);
      pre.append(c === 'E' && line.charAt(1) === ' ' ? h('span', { class: 'e' }, line) :
        c === 'W' && line.charAt(1) === ' ' ? h('span', { class: 'w' }, line) : line);
    }
    while (pre.textContent.length > LOG_KEEP && pre.firstChild) pre.firstChild.remove();
    if (follow.input.checked && atBottom !== false) pre.scrollTop = pre.scrollHeight;
  }

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
    catchUp().then(() => {
      if (!pre.firstChild) pre.append(h('span', { class: 'gap' }, prev ? 'No lines from the previous start (they do not survive a power cut or an app change).' : 'No lines yet.'));
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

  /* Receiver byte capture: only bench builds record it (404 elsewhere). */
  const capErr = errBox();
  const capBtn = btn('Download receiver bytes', () => act(capBtn, async () => {
    let res;
    try {
      res = await fetch('/api/diag/flash-capture', { credentials: 'same-origin', cache: 'no-store' });
    } catch (e) {
      throw new ApiError(0, { code: 'network' });
    }
    if (res.status === 404) {
      say(capErr, 'This build does not record the receiver\'s bytes; bench builds do.', 'info');
      return;
    }
    if (!res.ok) {
      let body = null;
      try {
        body = await res.json();
      } catch (e) {
        body = null;
      }
      if (res.status === 401) bus.emit('unauthorized');
      throw new ApiError(res.status, body && body.error);
    }
    const name = /filename="?([^";]+)"?/.exec(res.headers.get('Content-Disposition') || '');
    const url = URL.createObjectURL(await res.blob());
    const a = h('a', { href: url, download: name ? name[1] : 'flash-capture.txt', hidden: true });
    doc.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
  }, capErr));

  const capCard = h('section', { class: 'card', hidden: true },
    h('h2', null, 'Receiver byte capture'),
    h('p', { class: 'muted small' }, 'The last 256 KB the station exchanged with the receiver in a receiver update or a bench test.'),
    capErr,
    h('div', { class: 'form-actions' }, capBtn));
  api('GET', '/api/health').then(hl => {
    capCard.hidden = !(hl && hl.byte_capture_built);
  }).catch(() => {});
  mount(box,
    h('section', { class: 'card' },
      cardHead('Log', h('div', { class: 'tools' }, pauseBtn, dl)),
      which.el,
      pre,
      follow.el,
      err),
    h('section', { class: 'card' },
      h('h2', null, 'Log level'),
      h('p', { class: 'muted small' }, 'Per component tag (* for all), until the next restart. Set it back when done: debug output costs memory.'),
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
        return 'Network recovery: pass started' + (trig.length ? ' (' + trig.join(', ') + ')' : '');
      }
      /* The receiver ladder's own restart, in words: done, waiting, postponed (and why). */
      if (a === 0 && code === 9) {
        if (outcome === 5) {
          const mask = (b >>> 16) & 0xff;
          const why9 = J.holdBits.filter((k, i) => mask & (1 << i)).map(k => L.hold[k]);
          return 'Receiver recovery: restart postponed: ' + (why9.length ? why9.join(', ') : 'held back');
        }
        if (outcome === 4) return 'Receiver recovery: restart not taken (' + J.outcome[4] + ')';
        /* outcome 0: what blocked it (records from older builds carry none) */
        const capWhy = J.capWhy[(b >>> 16) & 0xff];
        return 'Receiver recovery: ' + J.gnssRung[9] + (capWhy ? ': ' + capWhy : '');
      }
      if (a === 0 && (code === 6 || code === 8) && outcome === 0) return 'Receiver recovery: ' + J.gnssRung[code];
      let step = 'step ' + code;
      if (a === 0 && J.gnssRung[code]) step = J.gnssRung[code];
      if (a === 1 && J.netRung[code]) step = J.netRung[code];
      return (J.ladder[a] || 'ladder ' + a) + ' recovery: ' + step + ', ' + (J.outcome[outcome] || outcome) +
        (a === 1 && why ? ' (' + (J.netWhy[why] || why) + ')' : '');
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
    case 'wifi_disconnect':
      return wifiReason(a) + rssiSuffix(b);
    case 'data_loss':
      return (a >= 16 ? slotName(a - 16) : J.loss[a] || 'counter ' + a) + ': +' + fmt.num(b);
    case 'caster_down': {
      const cls = (a >> 8) & 0xff, skipped = a >>> 16;
      return slotName(a & 0xff) + ', after ' + fmt.dur(b) + ' online' +
        (cls ? ': ' + lbl(L.fail, J.fail[cls] || cls).toLowerCase() : '') +
        (skipped ? '; ' + skipped + ' short session' + (skipped === 1 ? '' : 's') + ' not listed' : '');
    }
    case 'caster_up': {
      const tries = a >>> 16;
      return slotName(a & 0xff) + ', after ' + fmt.dur(b) +
        (tries ? ', ' + tries + ' failed attempt' + (tries === 1 ? '' : 's') : '');
    }
    case 'caster_stop': {
      const why = (a >> 8) & 0xff;
      return slotName(a & 0xff) + ': ' + lbl(L.stop, J.stop[why] || why) + ', no new attempt until you act';
    }
    case 'wifi_up':
      return 'After ' + fmt.dur(b) + ', ' + plural(a, 'connect attempt', 'connect attempts');
    case 'rx_maintenance':
      return a ? 'Started for ' + fmt.dur(b) : 'Ended after ' + fmt.dur(b);
    case 'rx_boot': {
      const how = ['', ' (its restart banner)', ' (its own time since its restart)', ' (data at once)'][b] || '';
      if (a === 1) return 'The receiver kept running: only the ESP32 was reset' + how;
      if (a === 2) return 'The receiver restarted too: the power was cut' + how;
      return 'Not known whether the receiver kept running';
    }
    case 'sg_test': {
      const ev = a & 0xff, grp = ((a >>> 8) & 0xff) + ' ' + ((a >>> 16) & 0xff);
      if (ev === 1) return 'Comparison started with ' + grp + ', ' + fmt.dur(b) + ' per group';
      if (ev === 2) return 'Switched the receiver to ' + grp + ' (group ' + (b + 1) + ' of 2)';
      if (ev === 3) return 'The receiver refused signal group ' + grp + (b >= 0 ? '; back to ' + ((b >>> 8) & 0xff) + ' ' + (b & 0xff) : '');
      if (ev === 4) return 'Comparison ended (' + (J.sgtOut[b] || b) + ') on ' + grp;
      return 'event ' + ev + ', ' + grp;
    }
    case 'live_view': {
      const ev = a & 0xff, detail = (a >> 8) & 0xff;
      const what = J.liveEv[ev] || 'event ' + ev;
      if (ev === 1) return 'Opened for ' + fmt.dur(b);
      if (ev === 2) return 'Extended, ' + fmt.dur(b) + ' left';
      if (ev === 3) return 'Closed after ' + fmt.dur(b) + (J.liveEnd[detail] ? ' (' + J.liveEnd[detail] + ')' : '');
      return what + ': ' + (J.liveCmd[detail] || detail) + ', RTCM held ' + fmt.num(b) + ' ms';
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
  ['casters', 'Casters', ['caster_down', 'caster_up', 'caster_stop']],
  ['wifi', 'Wi-Fi', ['wifi_disconnect', 'wifi_up']],
  ['updates', 'Updates', ['app_update', 'app_revert', 'gnss_flash']],
  ['receiver', 'Receiver', ['rx_maintenance', 'live_view', 'sg_test']],
  ['loss', 'Data loss', ['data_loss']],
];

/* esp_reset_reason_t by C value, as L.resetReason names them. */
const RESET_KEYS = Object.keys(L.resetReason);

/*
 * The event list's rows, newest first (eventTable): per start a heading
 * that says when and how it started (its boot and session records, and the
 * restart record that announced it), then its other records newest first,
 * repeats of one kind collapsed. entries: oldest first; atStart: they reach
 * back to the journal's oldest record (else the oldest start may just not
 * be loaded yet); kinds: the filter (null: everything).
 */
function journalItems(entries, atStart, kinds) {
  const groups = journalByBoot(entries);
  const items = [];
  for (let gi = groups.length - 1; gi >= 0; gi--) {
    const g = groups[gi], es = g.entries;
    const boot = es[0].kind === 'boot' ? es[0] : null;
    const sess = boot && es[1] && es[1].kind === 'session' ? es[1] : null;
    /* The restart this start was announced by: folded into the next start's heading when that names the same reason. */
    const nb = groups[gi + 1] && groups[gi + 1].entries[0];
    const reboots = es.filter(e => e.kind === 'reboot');
    const told = nb && nb.kind === 'boot' && J.resetClassKey[nb.a] === 'planned' && reboots.length && reboots[reboots.length - 1].a === nb.b ?
      reboots[reboots.length - 1] : null;
    const day = g.offset !== null ? new Date(g.offset * 1000).toDateString() : '';
    const rows = [];
    for (let i = es.length - 1; i >= 0; i--) {
      const e = es[i];
      if (e === boot || e === sess || e === told || (kinds && kinds.indexOf(e.kind) < 0)) continue;
      rows.push({ k: e.kind, seq: e.seq, ev: J.kind[e.kind] || words(e.kind), det: journalDetail(e),
        when: e.time_unix ? dayTime(e.time_unix, day) : g.offset !== null ? 'about ' + dayTime(g.offset + e.uptime_s, day) :
          fmt.dur(e.uptime_s) + ' after the start' });
    }
    if (!kinds || kinds.indexOf('boot') >= 0 || rows.length) {
      let text = boot ? startSentence(J.resetClassKey[boot.a], sess ? RESET_KEYS[sess.b] : null, J.rebootKey[boot.b]) :
        gi === 0 && !atStart ? 'Start' : 'Start (its first records are gone)';
      if (sess && sess.a >= 0) text += ' (previous run ' + fmt.dur(sess.a) + ')';
      items.push({ head: [g.offset !== null ? fmt.time(g.offset) : '', g.offset !== null ? text : text + ', time not known yet'] });
    }
    for (const r of collapse(rows)) items.push(r);
  }
  return items;
}

/*
 * The event journal, newest first. It reads the newest 64 records (from
 * health.journal.newest_seq), then 64 at a time backwards until it has a
 * page of rows (or the journal's start); "Show older" adds a page the same
 * way, Refresh reads what came since. No record is read twice.
 */
function journalTab(box) {
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
  /* Older records until a page of rows shows (or the journal's start). */
  async function fill() {
    for (let i = 0; i < 64 && !atStart && items().length < shown; i++) {
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
        all.length ? eventTable('Event journal', all.slice(0, shown), open) :
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
      body: [h('p', null, 'Every recorded event is deleted. The start history, the availability record and the logs are kept.'),
        h('p', { class: 'muted' }, 'The connection history starts again empty, and the power-loss alert forgets earlier power cuts.')] });
    if (!ok) return;
    const r = await act(wipe, () => api('DELETE', '/api/journal'), err);
    if (r === undefined) return;
    toast('Event journal cleared.', 'ok');
    shown = EV_PAGE;
    open.clear();
    act(null, start, err);
  }, 'danger', { hidden: true });
  const refreshBtn = btn('Refresh', () => act(refreshBtn, refresh, err));
  const sel = filterSelect(EV_FILTER, v => act(null, async () => {
    kinds = EV_FILTER.find(f => f[0] === v)[2];
    shown = EV_PAGE;
    await fill();
    render();
  }, err));
  mount(box, h('section', { class: 'card' },
    cardHead('Event journal', h('div', { class: 'tools grow' }, sel, refreshBtn)),
    h('p', { class: 'muted small' }, 'Starts, recovery steps, caster and Wi-Fi drops, updates and data losses, newest first. Kept across restarts.'),
    err, out,
    h('div', { class: 'form-actions' }, wipe, older)));
  repeatClicks(out, open, render);
  act(null, start, err);
  return { stop() {} };
}

/* ---- Tasks ----------------------------------------------------------------- */

function tasksTab(box) {
  const out = h('div');
  mount(box, h('section', { class: 'card' }, h('h2', null, 'Tasks'),
    h('p', { class: 'muted small' }, 'CPU share since the previous refresh (every 2 seconds) and the least free stack seen.'), out));
  return {
    stop: poller(2000, async () => {
      const r = await api('GET', '/api/diag/tasks');
      const tasks = (r.tasks || []).slice().sort((x, y) => (y.cpu_pct || 0) - (x.cpu_pct || 0) || (x.name < y.name ? -1 : 1));
      mount(out, table(['Task', 'Priority', 'Core', 'CPU', 'Stack left'], tasks.map(t => [t.name, String(t.priority),
        t.core === null || t.core === undefined ? 'any' : String(t.core), fmt.pct(t.cpu_pct),
        h('span', { class: t.stack_free_min < 512 ? 't-bad' : null }, fmt.bytes(t.stack_free_min))]), { num: [1, 2, 3, 4], stack: true, label: 'Tasks' }));
    }),
  };
}

/* ---- Core dump --------------------------------------------------------------- */

function coredumpTab(box) {
  const err = errBox();
  async function load() {
    const hl = await api('GET', '/api/health');
    const cd = hl.coredump || {};
    if (!cd.present) {
      mount(box, h('section', { class: 'card' }, cardHead('Core dump', badge('None', 'ok')),
        h('p', { class: 'muted' }, 'The station keeps a copy of its memory when it crashes. There is none.')));
      return;
    }
    const dlBtn = btn('Download', async () => {
      const ok = await dialog({ title: 'Download the core dump?', ok: 'Download',
        body: [h('p', null, 'It is a copy of the station\'s memory at the crash and can contain your Wi-Fi and caster passwords. ' +
          'Share it only with people you trust.'),
        h('p', { class: 'muted' }, 'Decode it with idf.py coredump-info and the ELF file of the same version.')],
        check: 'I understand it may contain passwords.' });
      if (!ok) return;
      const a = h('a', { href: '/api/coredump', download: 'coredump.elf', hidden: true });
      doc.body.append(a);
      a.click();
      a.remove();
    }, 'primary');
    const delBtn = btn('Erase', async () => {
      const ok = await dialog({ title: 'Erase the core dump?', ok: 'Erase', okKind: 'danger solid',
        body: h('p', null, 'It cannot be downloaded afterwards.') });
      if (!ok) return;
      const r = await act(delBtn, () => api('DELETE', '/api/coredump'), err);
      if (r !== undefined) load().catch(e => showErr(e, err));
    }, 'danger');
    mount(box, h('section', { class: 'card' }, cardHead('Core dump', badge('Stored', 'warn')),
      kv([
        ['Size', fmt.bytes(cd.size)],
        ['Crashed task', cd.task || DASH],
        ['Program counter', cd.pc ? h('code', null, cd.pc) : DASH],
        ['Backtrace', (cd.backtrace || []).length ? h('code', null, cd.backtrace.join(' ')) : DASH],
      ]),
      err,
      h('div', { class: 'form-actions' }, delBtn, dlBtn)));
  }
  load().catch(e => mount(box, msg('bad', errText(e))));
  return { stop() {} };
}

const DIAG_PAGES = { '': healthTab, availability: availabilityTab, logs: logsTab, journal: journalTab, tasks: tasksTab, coredump: coredumpTab };

Views.diag = {
  title: 'Diagnostics',
  showTitle: true,
  admin: true,
  create(el, arg) {
    const tab = DIAG_PAGES[arg] ? arg : '';
    const box = h('div', { class: 'stack' });
    const seg = h('nav', { class: 'seg', 'aria-label': 'Diagnostics sections', onscroll: () => edgeFade(seg) },
      DIAG_TABS.map(t => h('a', { href: '#/diag' + (t[0] ? '/' + t[0] : ''), class: t[0] === tab ? 'active' : null,
        'aria-current': t[0] === tab ? 'page' : null }, t[1])));
    mount(el, seg, box);
    /* On a phone the sections scroll sideways: the active one in view, a fade where more lie. */
    const fit = () => {
      const a = seg.querySelector('.active');
      const left = a ? a.offsetLeft - seg.offsetLeft : 0;
      if (a && left + a.offsetWidth > seg.clientWidth) seg.scrollLeft = left - 16;
      edgeFade(seg);
    };
    fit();
    window.addEventListener('resize', fit);
    mount(box, h('p', { class: 'loading' }, 'Loading…'));
    const sub = DIAG_PAGES[tab](box);
    return {
      destroy() {
        window.removeEventListener('resize', fit);
        if (sub && sub.stop) sub.stop();
      },
    };
  },
};
