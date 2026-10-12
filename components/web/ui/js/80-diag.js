/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics: health and loss counters, per-day availability with the
 * connection history next to it, the log (live, previous start, download,
 * levels), the event journal, task statistics and the core dump.
 */

const DIAG_TABS = [
  ['', 'Health'],
  ['availability', 'Availability'],
  ['logs', 'Logs'],
  ['journal', 'Event journal'],
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
    ['Previous start', l.prev_boot_available ? fmt.num(l.prev_boot_lines) + ' lines readable' : 'not readable'],
  ]);
}

/* "Restart 3 after power-on", or "Power-on" for the first start after one. */
function bootWhen(x) {
  if (x.reset_class === 'power_on' || x.boot_no <= 1) return 'Power-on';
  return 'Restart ' + (x.boot_no - 1) + ' after power-on';
}

function healthTab(box) {
  let lastHl = null;
  const render = hl => {
    lastHl = hl;
    const b = hl.boot || {};
    const heap = hl.heap || {};
    const link = hl.link || {};
    const net = hl.network || {};
    /* The same alerts as the dashboard (the station's and health's), under the badge. */
    const st = S.status || {};
    const alerts = Array.isArray(st.alerts) ? st.alerts : (hl.alerts || []).map(a => ({ id: a, source: 'health' }));
    const plan = restartPlanText(hl);
    const tone = healthTone(hl.level);
    mount(box, h('div', { class: 'grid' },
      h('section', { class: 'card' },
        h('h2', null, 'Health', badge(lbl(L.health, hl.level), tone)),
        alerts.length ? alertList(alerts, { detect: true }) :
          h('p', { class: 'muted' }, tone === 'ok' || tone === '' ? 'No alerts.' : 'No alert names the cause: see the receiver and caster pages.'),
        plan ? h('p', { class: 'small' }, plan) : null,
        kv([
          ['Automatic receiver recovery', hl.recovery_mode ? lbl(L.recovery, hl.recovery_mode) : DASH],
          link.open === false && link.mode !== 'off' ? ['Receiver UART', h('span', { class: 't-bad' }, 'closed (reopening)')] : null,
          ['Last recovery step', lbl(L.step, hl.last_step)],
          ['Receiver recovery paused', fmt.yesno(hl.ladder_suspended) + ((hl.station || {}).rx_maintenance ? ' (receiver maintenance)' : '')],
          ['Running for', fmt.dur(hl.uptime_s)],
          ['Receiver stalls', fmt.num(hl.gnss_stalls)],
          ['Recovery steps', fmt.num(hl.recoveries)],
          ['Caster restarts', fmt.num(hl.caster_restarts)],
          ['Network recoveries', fmt.num(hl.network_recoveries)],
          ['Since the last RTCM', hl.rtcm_gap_ms === null || hl.rtcm_gap_ms === undefined ? 'never received' : fmt.ms(hl.rtcm_gap_ms)],
          hl.gnss_retry_in_s ? ['Next receiver recovery pass in', fmt.dur(hl.gnss_retry_in_s)] : null,
          hl.inhibited_by ? ['Restarts held back by', hl.inhibited_by] : null,
          hl.update_slot && hl.update_slot !== 'none' ? ['Update running', hl.update_slot === 'app' ? 'an app update' : 'a receiver update'] : null,
          hl.factory_reset_refused_age_s !== null && hl.factory_reset_refused_age_s !== undefined ?
            ['BOOT-button factory reset', h('span', { class: 't-warn' }, 'refused ' + fmt.dur(hl.factory_reset_refused_age_s) +
              ' ago (a receiver update was running)')] : null,
          hl.admin_resets ? ['BOOT-button password resets', fmt.num(hl.admin_resets)] : null,
        ])),
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
      h('section', { class: 'card' },
        h('h2', null, 'This start', b.safe_mode ? badge('Safe mode', 'bad') : b.stable ? badge('Stable', 'ok') : null),
        h('p', null, startSentence(b.reset_class, b.reset_reason, b.prev_reboot_reason) + '.'),
        kv([
          ['Starts since power-on', fmt.num(b.boots_since_power_on)],
          ['Crashes counted', fmt.num(b.crashes)],
          ['Brownouts since power-on', fmt.num(b.brownouts_since_power_on)],
          ['Previous session', fmt.dur(b.prev_session_uptime_s)],
          b.safe_mode ? ['Normal retry in', fmt.dur(b.safe_mode_retry_in_s)] : null,
        ])),
      h('section', { class: 'card' },
        h('h2', null, 'Recent starts'),
        (hl.boot_history || []).length ? table(['When', 'How', 'Version', 'Ran before'],
          hl.boot_history.map(x => [bootWhen(x), startSentence(x.reset_class, x.reset_reason, x.planned_reason),
            x.version || DASH, x.prev_uptime_s === null || x.prev_uptime_s === undefined ? 'unknown' : fmt.dur(x.prev_uptime_s)]), { stack: true }) :
          h('p', { class: 'muted' }, 'No history yet.'),
        h('p', { class: 'muted small foot' }, 'Newest first. "Ran before" is how long the station ran before that start.')),
      h('section', { class: 'card' }, h('h2', null, 'Receiver link'), autoKv(link),
        h('h3', { class: 'section-title' }, 'Framing'), autoKv(link.framing)),
      h('section', { class: 'card' }, h('h2', null, 'RTCM hub'), autoKv(hl.hub),
        h('h3', { class: 'section-title' }, 'Station'), autoKv(hl.station),
        h('h3', { class: 'section-title' }, 'Observations'), autoKv(hl.obs)),
      h('section', { class: 'card' }, h('h2', null, 'Event journal'), journalStats(hl.journal),
        h('h3', { class: 'section-title' }, 'Log'), logStats(hl.log)),
      h('section', { class: 'card' }, h('h2', null, 'Network'), autoKv(net),
        h('h3', { class: 'section-title' }, 'Recovery'), net.ladder ? kv([
          ['Active', fmt.yesno(net.ladder.active)],
          ['Step', String(net.ladder.rung)],
          ['Held back because', net.ladder.withheld === 'none' ? DASH : lbl(L.ladderWhy, net.ladder.withheld)],
          ['Triggers now', (net.ladder.triggers || []).length ? net.ladder.triggers.map(t => lbl(L.trigger, t)).join(', ') : 'none'],
          ['Actions', fmt.num(net.ladder.actions)],
        ]) : null)));
  };
  detectClicks(box, () => {
    if (lastHl) render(lastHl);
  });
  return { stop: pollInto(box, 5000, '/api/health', render) };
}

/* ---- Availability -------------------------------------------------------- */

/* One connection-history record in words: [event, where, details]. */
function connRow(e) {
  const slot = e.slot_name || (e.slot === null || e.slot === undefined ? '' : 'Slot ' + (e.slot + 1));
  switch (e.kind) {
    case 'caster_down':
      return ['Session ended', slot, 'after ' + fmt.dur(e.duration_s) + (e.class && e.class !== 'none' ? ': ' + lbl(L.fail, e.class).toLowerCase() : '') +
        (e.count ? '; ' + e.count + ' shorter session' + (e.count === 1 ? '' : 's') + ' not listed' : '')];
    case 'caster_up':
      return ['Streaming again', slot, 'after ' + fmt.dur(e.duration_s) + (e.count ? ', ' + e.count + ' failed attempt' + (e.count === 1 ? '' : 's') : '')];
    case 'caster_stop':
      return ['Stopped: needs you', slot, lbl(L.stop, e.stop_reason) + '; no new attempt until you act'];
    case 'wifi_up':
      return ['Wi-Fi back', 'Wi-Fi', 'after ' + fmt.dur(e.duration_s) + (e.count ? ', ' + e.count + ' connect attempt' + (e.count === 1 ? '' : 's') : '')];
    case 'wifi_disconnect':
      return ['Wi-Fi dropped', 'Wi-Fi', wifiReason(e.reason) + (e.rssi ? ', signal ' + e.rssi + ' dBm' : '')];
    default:
      return [words(e.kind), slot, ''];
  }
}

/** The connection-history card (GET /api/connections), refreshed every minute and on demand. */
function connHistory() {
  const out = h('div');
  const err = errBox();
  const load = async () => {
    const r = await api('GET', '/api/connections');
    const list = r.entries || [];
    mount(out, list.length ? table(['When', 'Event', 'Where', 'Details'], list.map(e => {
      const c = connRow(e);
      const bad = e.kind === 'caster_down' || e.kind === 'caster_stop' || e.kind === 'wifi_disconnect';
      return [e.time_unix ? fmt.time(e.time_unix) : h('span', { class: 'muted' }, fmt.dur(e.uptime_s) + ' after a start'),
        h('span', { class: bad ? 't-warn' : 't-ok' }, c[0]), c[1] || DASH, c[2]];
    })) : h('p', { class: 'muted' }, 'No caster sessions ended or Wi-Fi drops recorded.'),
    r.complete === false && list.length ? h('p', { class: 'muted small foot' }, 'The newest ' + list.length + ' records; older ones are in the event journal.') : null);
  };
  const refresh = btn('Refresh', () => act(refresh, load, err), 'small');
  const el = h('section', { class: 'card wide' }, h('h2', null, 'Connection history'),
    h('p', { class: 'muted small' }, 'Caster sessions ending and coming back, Wi-Fi drops and returns, newest first. ' +
      'Not every session shorter than a minute is listed: the next record counts the ones left out.'),
    h('div', { class: 'row' }, refresh), err, h('div', { class: 'foot' }, out));
  const stop = poller(60000, () => load().catch(e => showErr(e, err)));
  return { el: el, stop: stop };
}

function availabilityTab(box) {
  const hist = connHistory();
  const render = av => {
    const slots = av.slots || [];
    const prot = slots.find(s => s.protected);
    const pi = prot ? slots.indexOf(prot) : -1;
    const p = av.protected || {};
    const day = av.today || {};
    const days = av.history || [];
    const sod = av.utc_second_of_day;
    const dayRows = slots.map((s, i) => [s.name || 'Slot ' + (s.slot + 1), fmt.dur((day.up_s || [])[i]), fmt.dur((day.down_s || [])[i]),
      fmt.dur((day.planned_s || [])[i]), fmt.num((day.reconnects || [])[i])]);
    /* Onocoy's figures only while its slot is set up and on: numbers for a slot that cannot stream mislead. */
    const counting = prot && p.onocoy_ready !== false && p.enabled !== false;
    const sofar = p.graced_uptime_so_far_permille;
    mount(box, h('div', { class: 'grid' },
      h('section', { class: 'card' },
        h('h2', null, 'Today', day.date ? badge(day.date + ' UTC', 'plain') : null),
        av.clock === 'none' ? msg('warn', 'No trusted clock yet (GNSS or internet time), so seconds cannot be assigned to a day.') : null,
        kv([
          ['Clock', lbl(L.timeSource, av.clock)],
          ['UTC time', sod === null || sod === undefined ? DASH :
            String(Math.floor(sod / 3600)).padStart(2, '0') + ':' + String(Math.floor((sod % 3600) / 60)).padStart(2, '0')],
          prot && !counting ? ['Onocoy', h('span', null, p.onocoy_ready === false ? 'Onocoy not set up. ' : 'Onocoy is off. ',
            link(p.onocoy_ready === false ? 'Add your Onocoy credentials' : 'Turn it on', '#/casters/onocoy'))] : null,
          counting ? ['Onocoy offline today', fmt.dur(p.down_today_s)] : null,
          counting && p.budget_s !== null && p.budget_s !== undefined ? ['Offline budget today', 'about ' + fmt.dur(p.budget_s) +
            ' at the current data rate'] : null,
          counting ? ['Offline budget left', fmt.dur(p.budget_left_s)] : null,
          counting ? ['Data rate', p.data_rate_permille === null || p.data_rate_permille === undefined ? 'nothing sent yet today' :
            fmt.permille(p.data_rate_permille)] : null,
          counting ? ['Graced uptime so far', soFarView(sofar, p.streak_ok)] : null,
          /* (online today + the 300 s grant) / 24 h: what the whole day reaches if nothing more is sent. */
          counting && p.graced_uptime_permille !== null && p.graced_uptime_permille !== undefined ?
            ['Day covered so far (online + 5 min grant)', fmt.permille(p.graced_uptime_permille)] : null,
          ['Powered off', fmt.dur(day.power_off_s)],
          ['UART overflows', fmt.num(day.rx_overflows)],
        ]),
        h('p', { class: 'muted small foot' }, 'Offline counts seconds a caster should have streamed but did not; planned is the part caused by receiver configuration and updates, and by restarts you started (until each caster is back, at most a couple of minutes after the restart). Information only.')),
      h('section', { class: 'card' }, h('h2', null, 'Casters today'),
        slots.length ? table(['Caster', 'Online', 'Offline', 'Planned', 'Reconnects'], dayRows, { num: [1, 2, 3, 4], stack: true, label: 'Casters today' }) :
          h('p', { class: 'muted' }, 'No casters.')),
      h('section', { class: 'card wide' }, h('h2', null, 'Last 30 days'),
        days.length ? table(['Day (UTC)', prot ? 'Onocoy offline' : 'Offline (slot 1)', 'Planned', 'Reconnects', 'Powered off'],
          days.map(d => {
            const i = pi >= 0 ? pi : 0;
            const down = (d.down_s || [])[i];
            const over = counting && down > 473;
            return [d.date || String(d.utc_day), h('span', { class: over ? 't-bad' : null }, fmt.dur(down)), fmt.dur((d.planned_s || [])[i]),
              fmt.num((d.reconnects || [])[i]), fmt.dur(d.power_off_s)];
          }), { num: [1, 2, 3, 4] }) : h('p', { class: 'muted' }, 'No earlier days recorded yet.'),
        prot ? h('p', { class: 'muted small foot' }, 'Onocoy\'s streak allows about 473 s offline per UTC day at a perfect data rate.') : null),
      hist.el));
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

  const tag = textInput('*', { maxlength: 15, 'aria-label': 'Tag' });
  const level = selectInput([['none', 'none'], ['error', 'error'], ['warn', 'warn'], ['info', 'info'], ['debug', 'debug'], ['verbose', 'verbose']], 'info');
  const lvErr = errBox();
  const setBtn = btn('Set', async () => {
    const t = tag.value.trim();
    if (!t || (t !== '*' && t.length > 15)) {
      say(lvErr, 'A tag (component name, at most 15 characters) or * for all.');
      return;
    }
    const r = await act(setBtn, () => api('PUT', '/api/diag/log-level', { tag: t, level: level.value }), lvErr);
    if (r !== undefined) toast('Log level of ' + t + ' set to ' + level.value + ' until the next restart.', 'ok');
  }, 'small');

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
  }, capErr), 'small');

  const capCard = h('section', { class: 'card', hidden: true },
    h('h2', null, 'Receiver byte capture'),
    h('p', { class: 'muted small' }, 'This build records every byte the station exchanged with the receiver during a receiver update or a bench test ' +
      '(the last 256 KB, data blocks as one line each).'),
    h('div', { class: 'row' }, capBtn),
    capErr);
  api('GET', '/api/health').then(hl => {
    capCard.hidden = !(hl && hl.byte_capture_built);
  }).catch(() => {});
  mount(box,
    h('section', { class: 'card' },
      h('h2', null, 'Log'),
      h('div', { class: 'spread' }, which.el, h('div', { class: 'row' }, pauseBtn, dl)),
      h('div', { class: 'foot' }, pre),
      h('div', { class: 'row foot' }, follow.el),
      err),
    h('section', { class: 'card' },
      h('h2', null, 'Log level'),
      h('p', { class: 'muted small' }, 'Per component tag, until the next restart. Debug output costs memory and console time; set it back when done.'),
      h('div', { class: 'cols c3' }, field('Tag', tag).el, field('Level', level).el, h('div', { class: 'field' }, h('span', { class: 'lbl' }, ' '), setBtn)),
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
    case 'reboot':
      return 'Restart: ' + (lbl(L.plannedReason, J.rebootKey[a]) || J.rebootReason[a] || a);
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
      return wifiReason(a) + ', signal ' + b + ' dBm';
    case 'data_loss':
      return (a >= 16 ? 'Caster slot ' + (a - 15) : J.loss[a] || 'counter ' + a) + ': +' + fmt.num(b);
    case 'caster_down': {
      const cls = (a >> 8) & 0xff, skipped = a >>> 16;
      return 'Caster slot ' + ((a & 0xff) + 1) + ': session ended after ' + fmt.dur(b) +
        (cls ? ' (' + lbl(L.fail, J.fail[cls] || cls) + ')' : '') +
        (skipped ? '; ' + skipped + ' short session' + (skipped === 1 ? '' : 's') + ' not listed' : '');
    }
    case 'caster_up': {
      const tries = a >>> 16;
      return 'Caster slot ' + ((a & 0xff) + 1) + ': streaming again after ' + fmt.dur(b) +
        (tries ? ', ' + tries + ' failed attempt' + (tries === 1 ? '' : 's') : '');
    }
    case 'caster_stop': {
      const why = (a >> 8) & 0xff;
      return 'Caster slot ' + ((a & 0xff) + 1) + ': stopped (' + lbl(L.stop, J.stop[why] || why) + '); no new attempt until you act';
    }
    case 'wifi_up':
      return 'IP address back after ' + fmt.dur(b) + ', ' + a + ' connect attempt' + (a === 1 ? '' : 's');
    case 'rx_maintenance':
      return a ? 'Started for ' + fmt.dur(b) : 'Ended after ' + fmt.dur(b);
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

function journalTab(box) {
  let cursor = 0;
  const entries = [];
  const out = h('div');
  const err = errBox();
  async function fetchNew() {
    for (let i = 0; i < 40; i++) {
      const r = await api('GET', '/api/journal?cursor=' + cursor);
      for (const e of r.entries || []) entries.push(e);
      cursor = r.cursor;
      if (!r.more) break;
    }
    const groups = journalByBoot(entries.slice(-500)).reverse();
    const rows = [];
    for (const g of groups) {
      const first = g.entries[0];
      const startTime = g.offset !== null ? fmt.time(g.offset) : null;
      rows.push(h('tr', { class: 'sep' }, h('th', { colspan: '3', scope: 'rowgroup' },
        'Start' + (startTime ? ' at about ' + startTime : '') + (first.kind === 'boot' ? '' : ' (its first records were overwritten)'))));
      for (const e of g.entries.slice().reverse()) {
        const when = e.time_unix ? fmt.time(e.time_unix) : g.offset !== null ? '≈ ' + fmt.time(g.offset + e.uptime_s) :
          fmt.dur(e.uptime_s) + ' after the start';
        rows.push(h('tr', null,
          h('td', { 'data-label': 'When' }, e.time_unix ? when : h('span', { class: 'muted' }, when)),
          h('td', { 'data-label': 'Event' }, J.kind[e.kind] || words(e.kind)),
          h('td', { 'data-label': 'Details' }, journalDetail(e), h('span', { class: 'muted small' }, ' #' + e.seq))));
      }
    }
    mount(out, rows.length ? scrollBox('Event journal', h('table', { class: 'journal stack-sm' },
      h('thead', null, h('tr', null, h('th', null, 'When'), h('th', null, 'Event'), h('th', null, 'Details'))),
      h('tbody', null, rows))) : h('p', { class: 'muted' }, 'The journal is empty.'));
  }
  const refresh = btn('Refresh', () => act(refresh, fetchNew, err), 'small');
  mount(box, h('section', { class: 'card' },
    h('h2', null, 'Event journal'),
    h('p', { class: 'muted small' }, 'Starts, restarts, recovery steps, updates and data losses, kept across restarts. Newest first, by start; ' +
      'times marked ≈ are worked out from a later record of the same start (the clock was not set yet when they were written).'),
    h('div', { class: 'row' }, refresh), err, h('div', { class: 'foot' }, out)));
  act(null, fetchNew, err);
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
      mount(box, h('section', { class: 'card' }, h('h2', null, 'Core dump', badge('None', 'ok')),
        h('p', { class: 'muted' }, 'The station stores a core dump when it crashes. There is none.')));
      return;
    }
    const dlBtn = btn('Download', async () => {
      const ok = await dialog({ title: 'Download the core dump?', ok: 'Download',
        body: [h('p', null, 'A core dump is a copy of the station\'s memory at the crash. It can contain your Wi-Fi and caster passwords.'),
          h('p', { class: 'muted' }, 'Share it only with people you trust. Decode it with idf.py coredump-info and the ELF file of the same version.')],
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
    mount(box, h('section', { class: 'card' }, h('h2', null, 'Core dump', badge('Stored', 'warn')),
      kv([
        ['Size', fmt.bytes(cd.size)],
        ['Crashed task', cd.task || DASH],
        ['Program counter', cd.pc ? h('code', null, cd.pc) : DASH],
        ['Backtrace', (cd.backtrace || []).length ? h('code', null, cd.backtrace.join(' ')) : DASH],
      ]),
      err,
      h('div', { class: 'form-actions' }, dlBtn, delBtn)));
  }
  load().catch(e => mount(box, msg('bad', errText(e))));
  return { stop() {} };
}

const DIAG_PAGES = { '': healthTab, availability: availabilityTab, logs: logsTab, journal: journalTab, tasks: tasksTab, coredump: coredumpTab };

Views.diag = {
  title: 'Diagnostics',
  admin: true,
  create(el, arg) {
    const tab = DIAG_PAGES[arg] ? arg : '';
    const box = h('div', { class: 'stack' });
    mount(el, h('nav', { class: 'seg', 'aria-label': 'Diagnostics sections' },
      DIAG_TABS.map(t => h('a', { href: '#/diag' + (t[0] ? '/' + t[0] : ''), class: t[0] === tab ? 'active' : null,
        'aria-current': t[0] === tab ? 'page' : null }, t[1]))), box);
    mount(box, h('p', { class: 'loading' }, 'Loading…'));
    const sub = DIAG_PAGES[tab](box);
    return {
      destroy() {
        if (sub && sub.stop) sub.stop();
      },
    };
  },
};
