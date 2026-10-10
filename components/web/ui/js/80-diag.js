/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics: health and loss counters, per-day availability, the log
 * (live, previous start, download, levels), the event journal, task
 * statistics and the core dump.
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

function healthTab(box) {
  const render = hl => {
    const b = hl.boot || {};
    const heap = hl.heap || {};
    const link = hl.link || {};
    const net = hl.network || {};
    mount(box, h('div', { class: 'grid' },
      h('section', { class: 'card' },
        h('h2', null, 'Health', badge(lbl(L.health, hl.level), healthTone(hl.level))),
        (hl.alerts || []).length ? h('div', { class: 'stack' }, hl.alerts.map(a => {
          const t = alertText({ id: a, source: 'health' });
          return msg('warn', h('strong', null, t[0]), t[1] ? h('div', { class: 'small' }, t[1]) : null);
        })) : h('p', { class: 'muted' }, 'No health alerts.'),
        kv([
          ['Last recovery step', lbl(L.step, hl.last_step)],
          ['Receiver recovery paused', fmt.yesno(hl.ladder_suspended)],
          ['Running for', fmt.dur(hl.uptime_s)],
          ['Receiver stalls', fmt.num(hl.gnss_stalls)],
          ['Recovery steps', fmt.num(hl.recoveries)],
          ['Caster restarts', fmt.num(hl.caster_restarts)],
          ['Network recoveries', fmt.num(hl.network_recoveries)],
          ['Longest RTCM gap', fmt.ms(hl.rtcm_gap_ms)],
          ['Restart blocked by', hl.inhibited_by || 'nothing'],
          ['Update in progress', hl.update_slot === 'none' || !hl.update_slot ? 'none' : words(hl.update_slot)],
        ])),
      h('section', { class: 'card' },
        h('h2', null, 'Memory'),
        kv([
          ['Internal free', fmt.bytes(heap.internal_free)],
          ['Internal lowest', fmt.bytes(heap.internal_min)],
          ['Largest internal block', fmt.bytes(heap.internal_largest)],
          ['PSRAM free', fmt.bytes(heap.psram_free)],
        ])),
      h('section', { class: 'card' },
        h('h2', null, 'This start', b.safe_mode ? badge('Safe mode', 'bad') : b.stable ? badge('Stable', 'ok') : null),
        kv([
          ['Reset', words(b.reset_reason) + ' (' + words(b.reset_class) + ')'],
          ['Planned reason', words(b.prev_reboot_reason)],
          ['Starts since power-on', fmt.num(b.boots_since_power_on)],
          ['Crashes counted', fmt.num(b.crashes)],
          ['Brownouts since power-on', fmt.num(b.brownouts_since_power_on)],
          ['Previous session', fmt.dur(b.prev_session_uptime_s)],
          b.safe_mode ? ['Normal retry in', fmt.dur(b.safe_mode_retry_in_s)] : null,
        ])),
      h('section', { class: 'card' },
        h('h2', null, 'Recent starts'),
        (hl.boot_history || []).length ? table(['#', 'Kind', 'Reset', 'Planned reason', 'Version', 'Ran before'],
          hl.boot_history.map(x => [String(x.boot_no), words(x.reset_class), words(x.reset_reason), words(x.planned_reason),
            x.version || DASH, x.prev_uptime_s === null || x.prev_uptime_s === undefined ? 'unknown' : fmt.dur(x.prev_uptime_s)]), { num: [0] }) :
          h('p', { class: 'muted' }, 'No history yet.')),
      h('section', { class: 'card' }, h('h2', null, 'Receiver link'), autoKv(link),
        h('h3', { class: 'section-title' }, 'Framing'), autoKv(link.framing)),
      h('section', { class: 'card' }, h('h2', null, 'RTCM hub'), autoKv(hl.hub),
        h('h3', { class: 'section-title' }, 'Station'), autoKv(hl.station),
        h('h3', { class: 'section-title' }, 'Observations'), autoKv(hl.obs)),
      h('section', { class: 'card' }, h('h2', null, 'Network'), autoKv(net),
        h('h3', { class: 'section-title' }, 'Recovery'), net.ladder ? kv([
          ['Active', fmt.yesno(net.ladder.active)],
          ['Step', String(net.ladder.rung)],
          ['Held back because', net.ladder.withheld === 'none' ? DASH : lbl(L.ladderWhy, net.ladder.withheld)],
          ['Actions', fmt.num(net.ladder.actions)],
        ]) : null)));
  };
  return { stop: pollInto(box, 5000, '/api/health', render) };
}

/* ---- Availability -------------------------------------------------------- */

function availabilityTab(box) {
  const render = av => {
    const slots = av.slots || [];
    const prot = slots.find(s => s.protected);
    const pi = prot ? slots.indexOf(prot) : -1;
    const p = av.protected || {};
    const day = av.today || {};
    const hist = av.history || [];
    const sod = av.utc_second_of_day;
    const dayRows = slots.map((s, i) => [s.name || 'Slot ' + (s.slot + 1), fmt.dur((day.down_s || [])[i]), fmt.dur((day.planned_s || [])[i]),
      fmt.num((day.reconnects || [])[i])]);
    mount(box, h('div', { class: 'grid' },
      h('section', { class: 'card' },
        h('h2', null, 'Today', day.date ? badge(day.date + ' UTC', 'plain') : null),
        av.clock === 'none' ? msg('warn', 'No trusted clock yet (GNSS or internet time), so seconds cannot be assigned to a day.') : null,
        kv([
          ['Clock', lbl(L.timeSource, av.clock)],
          ['UTC time', sod === null || sod === undefined ? DASH :
            String(Math.floor(sod / 3600)).padStart(2, '0') + ':' + String(Math.floor((sod % 3600) / 60)).padStart(2, '0')],
          prot ? ['Onocoy offline today', fmt.dur(p.down_today_s)] : null,
          prot ? ['Offline budget left', fmt.dur(p.budget_left_s)] : null,
          prot ? ['Data rate', fmt.permille(p.data_rate_permille)] : null,
          prot ? ['Graced uptime', fmt.permille(p.graced_uptime_permille) + (p.streak_ok === false ? ' (below 99.8 %)' : '')] : null,
          ['Powered off', fmt.dur(day.power_off_s)],
          ['UART overflows', fmt.num(day.rx_overflows)],
        ]),
        h('p', { class: 'muted small foot' }, 'Offline counts seconds a caster should have streamed but did not; planned is the part caused by updates and receiver work. Information only.')),
      h('section', { class: 'card' }, h('h2', null, 'Casters today'),
        slots.length ? table(['Caster', 'Offline', 'Planned', 'Reconnects'], dayRows, { num: [1, 2, 3] }) : h('p', { class: 'muted' }, 'No casters.')),
      h('section', { class: 'card wide' }, h('h2', null, 'Last 30 days'),
        hist.length ? table(['Day (UTC)', prot ? 'Onocoy offline' : 'Offline (slot 1)', 'Planned', 'Reconnects', 'Powered off'],
          hist.map(d => {
            const i = pi >= 0 ? pi : 0;
            const down = (d.down_s || [])[i];
            const over = prot && down > 473;
            return [d.date || String(d.utc_day), h('span', { class: over ? 't-bad' : null }, fmt.dur(down)), fmt.dur((d.planned_s || [])[i]),
              fmt.num((d.reconnects || [])[i]), fmt.dur(d.power_off_s)];
          }), { num: [1, 2, 3, 4] }) : h('p', { class: 'muted' }, 'No earlier days recorded yet.'),
        prot ? h('p', { class: 'muted small foot' }, 'Onocoy\'s streak allows about 473 s offline per UTC day at a perfect data rate.') : null)));
  };
  return { stop: pollInto(box, 10000, '/api/availability', render) };
}

/* ---- Logs ------------------------------------------------------------------ */

const LOG_KEEP = 200000; /* characters kept in the view */

function logsTab(box) {
  let cursor = 0, prev = false, paused = false, loading = false;
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

  function restart() {
    clear(pre);
    cursor = 0;
    fetchMore().then(() => {
      if (!pre.firstChild) pre.append(h('span', { class: 'gap' }, prev ? 'No lines from the previous start (they do not survive a power cut or an app change).' : 'No lines yet.'));
    });
    mount(dl, 'Download ' + (prev ? 'previous start' : 'log'));
    dl.href = '/api/logs?format=text' + (prev ? '&boot=prev' : '');
  }

  const offLog = bus.on('log', m => {
    if (prev || paused || loading) return;
    if (m.cursor !== undefined && m.cursor <= cursor) return;
    append(m.text, m.gap);
    if (m.cursor !== undefined) cursor = m.cursor;
  });
  live.subscribe(['status', 'progress', 'log']);
  /* Without the WebSocket, poll for new lines. */
  const stopPoll = poller(2000, () => (!prev && !paused && live.state !== 'ws' ? fetchMore() : Promise.resolve()), true);

  const which = radios('Which start', 'logboot', [['now', 'This start'], ['prev', 'Previous start']], 'now');
  which.onchange(() => {
    prev = which.value() === 'prev';
    restart();
  });
  const pauseBtn = btn('Pause', () => {
    paused = !paused;
    pauseBtn.textContent = paused ? 'Resume' : 'Pause';
    if (!paused) fetchMore();
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
      lvErr));
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
      return 'Reset: ' + (J.resetClass[a] || a) + (b ? '; planned reason: ' + (J.rebootReason[b] || b) : '');
    case 'session':
      return 'Previous session ' + (a < 0 ? 'length unknown' : 'ran ' + fmt.dur(a)) + '; reset reason: ' + (J.resetReason[b] || b);
    case 'reboot':
      return 'Reason: ' + (J.rebootReason[a] || a);
    case 'ladder_step': {
      const outcome = (b >> 8) & 0xff, code = b & 0xff, why = (b >> 16) & 0xff;
      let step = 'step ' + code;
      if (a === 0 && J.gnssRung[code]) step = J.gnssRung[code];
      if (a === 1 && J.netRung[code]) step = J.netRung[code];
      return (J.ladder[a] || 'ladder ' + a) + ' recovery: ' + step + ', ' + (J.outcome[outcome] || outcome) +
        (a === 1 && why ? ' (' + (J.netWhy[why] || why) + ')' : '');
    }
    case 'app_update':
      return 'Result: ' + espErr(a);
    case 'app_revert':
      return 'Trial stage: ' + (J.trial[a] || a);
    case 'gnss_flash':
      return 'Result: ' + espErr(a) + ', attempt ' + b;
    case 'safe_mode':
      return 'Episode ' + a;
    case 'wifi_disconnect':
      return wifiReason(a) + ', signal ' + b + ' dBm';
    case 'data_loss':
      return (a >= 16 ? 'Caster slot ' + (a - 15) : J.loss[a] || 'counter ' + a) + ': +' + fmt.num(b);
    default:
      return 'a = ' + a + ', b = ' + b;
  }
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
    const shown = entries.slice(-500).reverse();
    mount(out, shown.length ? table(['#', 'When', 'Event', 'Details'], shown.map(e => [String(e.seq),
      e.time_unix ? fmt.time(e.time_unix) : h('span', { class: 'muted' }, fmt.dur(e.uptime_s) + ' after start'),
      J.kind[e.kind] || words(e.kind), journalDetail(e)]), { num: [0] }) : h('p', { class: 'muted' }, 'The journal is empty.'));
  }
  const refresh = btn('Refresh', () => act(refresh, fetchNew, err), 'small');
  mount(box, h('section', { class: 'card' },
    h('h2', null, 'Event journal'),
    h('p', { class: 'muted small' }, 'Starts, restarts, recovery steps, updates and data losses, kept across restarts. Newest first.'),
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
        h('span', { class: t.stack_free_min < 512 ? 't-bad' : null }, fmt.bytes(t.stack_free_min))]), { num: [1, 2, 3, 4] }));
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
      DIAG_TABS.map(t => h('a', { href: '#/diag' + (t[0] ? '/' + t[0] : ''), class: t[0] === tab ? 'active' : null }, t[1]))), box);
    mount(box, h('p', { class: 'loading' }, 'Loading…'));
    const sub = DIAG_PAGES[tab](box);
    return {
      destroy() {
        if (sub && sub.stop) sub.stop();
      },
    };
  },
};
