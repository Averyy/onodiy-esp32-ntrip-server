/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Status dashboard: overall state, receiver and RTCM, the Onocoy day (UTC)
 * with the remaining offline budget, every caster, Wi-Fi, alerts. Built
 * from the status model on every update and patched into the page (no
 * inputs, and no element with a listener of its own: patch() replaces such
 * an element every time, which would take the keyboard focus with it), so
 * the links and buttons keep keyboard focus and text stays selectable
 * across the 1 s updates. The one button, "Check again" on the no-receiver
 * alert, carries data-act and the page's root handles its click
 * (detectClicks). The live view entry is a link that starts the window.
 * Visitors without a session see the public form (names and states only).
 *
 * Every "today" figure (online, offline, the offline budget) comes from the
 * availability ledger, which is kept across restarts.
 */

/* Onocoy's streak needs at least 99.8 % graced uptime per UTC day. */
const STREAK_PERMILLE = 998;

Views.dashboard = {
  title: 'Status',
  create(el) {
    const render = () => {
      const s = S.status;
      if (!s) {
        patch(el, h('p', { class: 'loading' }, 'Waiting for the station…'));
        return;
      }
      patch(el, isAdmin() && !s.public ? adminDashboard(s) : publicDashboard(s));
    };
    detectClicks(el, render);
    render();
    return { update: render };
  },
};

function enabledCasters(s) {
  return (s.casters || []).filter(c => c.enabled !== false && c.state !== 'disabled');
}

/** The overall state: {tone, head} (the hero card, and what the alerts card may say). */
function heroState(s) {
  const rcv = s.receiver || {};
  const hl = s.health || {};
  const on = enabledCasters(s);
  const streaming = on.filter(c => c.state === 'streaming').length;
  let tone = 'ok', head;
  if (s.safe_mode && s.safe_mode.active) {
    tone = 'bad';
    head = 'Safe mode';
  } else if (rcv.rx_maintenance) {
    tone = 'warn';
    head = 'Receiver maintenance';
  } else if (rcv.state !== 'streaming') {
    tone = rcv.state === 'no_receiver' ? 'bad' : 'warn';
    head = lbl(L.station, rcv.state);
  } else if (!on.length) {
    tone = 'warn';
    head = 'No caster enabled';
  } else if (streaming === on.length) {
    head = 'Streaming';
  } else {
    tone = streaming ? 'warn' : 'bad';
    head = streaming ? 'Partly streaming' : 'Not streaming';
  }
  if (hl.level === 'failed') tone = 'bad';
  else if (tone === 'ok' && hl.level && hl.level !== 'ok') tone = 'warn';
  return { tone: tone, head: head, on: on, streaming: streaming };
}

function heroCard(s) {
  const hl = s.health || {};
  const st = heroState(s);
  const facts = [];
  if (st.on.length) facts.push(st.streaming + ' of ' + st.on.length + ' casters streaming');
  /* The health level in words, never the raw level ("failed", "degraded"). */
  if (hl.level) facts.push(lbl(L.healthFact, hl.level));
  if (hl.recovery_mode && hl.recovery_mode !== 'full') facts.push('receiver recovery: ' + lbl(L.recovery, hl.recovery_mode).toLowerCase());
  if (s.uptime_s !== undefined) facts.push('up ' + fmt.dur(s.uptime_s));
  return h('section', { class: 'card hero ' + st.tone }, h('div', { class: 'state' }, st.head), h('div', { class: 'facts' }, facts.join(' · ')));
}

function receiverCard(s, admin) {
  const r = s.receiver || {};
  const rate = outputRate();
  const rows = [
    admin ? ['Receiver', r.model ? r.model + (r.firmware ? ' · ' + r.firmware : '') : DASH] : null,
    admin ? ['Last RTCM', h('span', { class: 't-' + ageTone(r.rtcm_age_ms) }, r.rtcm_age_ms === null || r.rtcm_age_ms === undefined ? 'never' : fmt.ms(r.rtcm_age_ms) + ' ago')] : null,
    admin ? ['Output', rate === null ? DASH : fmt.kbps(rate)] : null,
    ['Best constellation', r.best_sys ? lbl(L.sys, r.best_sys) + ' · ' + r.best_bands + ' bands' : r.best_bands ? r.best_bands + ' bands' : DASH],
  ];
  if (admin) {
    let base;
    /* position_locked is the stored profile's base mode: the receiver runs it only once the profile is applied. */
    if (r.position_locked) base = r.profile_applied ? 'Position locked' : 'Fixed position (not applied yet)';
    else if (r.surveying) base = 'Surveying' + (r.survey_window_left_s ? ', ' + fmt.dur(r.survey_window_left_s) + ' left' : '');
    else base = 'Not locked';
    rows.push(['Base', base]);
    rows.push(['Profile', r.profile_applied ? 'Applied' : h('span', { class: 't-warn' }, 'Not applied')]);
    if (r.rx_maintenance) {
      rows.push(['Maintenance', h('span', { class: 't-warn' }, 'On, ' + fmt.dur(Math.ceil((r.maintenance_left_s || 0) / 60) * 60) + ' left: nothing is sent to the receiver')]);
    }
    if (r.uart_closed && !(s.safe_mode && s.safe_mode.active)) rows.push(['Receiver link', h('span', { class: 't-bad' }, 'UART closed (reopening)')]);
  }
  const state = r.rx_maintenance ? badge('Maintenance', 'warn') : badge(lbl(L.station, r.state), stationTone(r.state));
  return h('section', { class: 'card' },
    h('h2', null, 'Receiver', state),
    kv(rows),
    admin ? h('div', { class: 'foot row' }, link('Live view', LIVE_START_HASH), link('Receiver and position', '#/receiver'), link('Signals', '#/signals')) :
      h('div', { class: 'foot' }, link('Signals', '#/signals')));
}

/* Where the Onocoy credentials are entered (the caster editor opens on the Onocoy slot). */
const ONOCOY_SETUP_HASH = '#/casters/onocoy';

/** The Onocoy slot (protected) of the status casters, or null. */
function onocoySlot(s) {
  return (s.casters || []).find(c => c.protected) || null;
}

/** True when the Onocoy slot is enabled and has the console's mountpoint, username and password. */
function onocoyCounting(c) {
  return !!(c && c.enabled !== false && c.onocoy_ready);
}

/* In place of the day's figures while Onocoy is not set up (also fills the dashboard's grid cell). */
function onocoySetupCard(c) {
  const ready = c && c.onocoy_ready;
  return h('section', { class: 'card' },
    h('h2', null, 'Onocoy', badge(ready ? 'Off' : 'Not set up', '')),
    h('p', null, ready ? 'The Onocoy slot is turned off: the station does not stream to Onocoy.' :
      'Onocoy not set up. Onocoy rewards reference stations for their correction data; the station streams to it once ' +
      'you enter the credentials from the Onocoy console.'),
    h('div', { class: 'form-actions' }, link(ready ? 'Turn Onocoy on' : 'Add your Onocoy credentials', ONOCOY_SETUP_HASH, 'btn primary')),
    h('p', { class: 'muted small foot' }, 'Today\'s figures (offline budget, graced uptime) show here once Onocoy streams.'));
}

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

function budgetCard(c, avail) {
  /* All from the availability ledger (kept across restarts): today's offline
   * time, what is left of the day's budget, and the budget itself at the
   * current data rate. Without a trusted clock the day is unknown. */
  const left = avail && avail.budget_left_s !== undefined ? avail.budget_left_s : c.budget_left_s;
  const down = avail && avail.protected_down_today_s !== undefined ? avail.protected_down_today_s : c.today_offline_s;
  const allowed = c.budget_s;
  const known = left !== null && left !== undefined && down !== null && down !== undefined &&
    allowed !== null && allowed !== undefined;
  const used = known && allowed > 0 ? Math.min(1, down / allowed) : known ? 1 : 0;
  /* Never red while the streak holds: the budget is an estimate, the streak the verdict. */
  const tone = !known ? '' : left <= 0 && !c.streak_ok ? 'bad' : used > 0.6 ? 'warn' : 'ok';
  const mid = fmt.utcMidnight();
  const over = known && down > allowed;
  return h('section', { class: 'card' },
    h('h2', null, 'Onocoy today', c.streak_ok === null || c.streak_ok === undefined ? null :
      badge(c.streak_ok ? 'Streak on track' : 'Streak missed', c.streak_ok ? 'ok' : 'bad')),
    !known ?
      h('p', { class: 'muted' }, 'The offline budget needs a trusted clock (GNSS or internet time).') :
      [h('div', { class: 'big ' + tone }, h('span', { class: 't-' + (tone || 'ok') }, left <= 0 ? '0\u00a0s' : fmt.dur(left))),
        h('div', { class: 'muted small' }, 'offline budget left today'),
        h('div', { class: tone }, bar(used, '', 'Share of today\'s offline budget used')),
        h('div', { class: 'muted small' }, (over ? fmt.dur(down) + ' offline so far: over the day\'s budget of about ' + fmt.dur(allowed) +
          ' by ' + fmt.dur(down - allowed) + '.' : fmt.dur(down) + ' offline so far of a budget of about ' + fmt.dur(allowed) + '.') +
          ' Resets at 00:00 UTC (' + mid.local + ' here).')],
    kv([
      ['Online today', fmt.dur(c.today_online_s)],
      ['Offline today', fmt.dur(c.today_offline_s)],
      ['Graced uptime so far', soFarView(c.graced_uptime_so_far_permille, c.streak_ok)],
      ['Data rate', c.data_rate_permille === null || c.data_rate_permille === undefined ? 'nothing sent yet today' : fmt.permille(c.data_rate_permille)],
    ]),
    h('p', { class: 'muted small foot' }, 'Our estimate of Onocoy\'s accounting for the current UTC day (it needs at least 99.8 % graced uptime). ' +
      'Information only.'));
}

function casterLine(c, admin) {
  const tone = casterTone(c.state);
  const reason = c.reason && c.reason !== 'none' ? lbl(L.fail, c.reason) : '';
  const metrics = [];
  if (admin) {
    if (c.state === 'streaming') {
      if (c.connected_s !== null && c.connected_s !== undefined) metrics.push(['Connected', fmt.dur(c.connected_s)]);
      if (c.lag_ms_now !== null && c.lag_ms_now !== undefined) metrics.push(['Lag', fmt.ms(c.lag_ms_now)]);
      const r = slotRate(c.slot, 10);
      if (r !== null) metrics.push(['Rate', fmt.kbps(r)]);
    } else if (c.state_age_s !== null && c.state_age_s !== undefined) {
      metrics.push(['For', fmt.dur(c.state_age_s)]);
    }
    if (c.next_retry_s) metrics.push(['Retry in', fmt.dur(c.next_retry_s)]);
    if (c.unreached_s) metrics.push(['Not reached for', fmt.dur(c.unreached_s)]);
    const on = c.today_online_s || 0, off = c.today_offline_s || 0;
    if (on + off > 0) metrics.push(['Today', (100 * on / (on + off)).toFixed(on === on + off ? 0 : 2) + ' % online']);
    if (c.bytes_sent) metrics.push(['Sent', fmt.bytes(c.bytes_sent)]);
  }
  return h('div', { class: 'caster' },
    h('div', { class: 'spread' },
      h('span', null, h('span', { class: 'name' }, c.name || ('Slot ' + (c.slot + 1))),
        admin && c.host ? h('span', { class: 'muted small' }, ' · ' + c.host) : null),
      badge(lbl(L.caster, c.state), tone)),
    reason && c.state !== 'streaming' ? h('div', { class: 'muted small' }, reason) : null,
    metrics.length ? h('div', { class: 'metrics' }, metrics.map(m => h('span', null, m[0] + ' ', h('b', null, m[1])))) : null,
    admin && c.hint ? h('div', { class: 'hintline' }, c.hint) : null);
}

/* An Onocoy slot without its credentials: a call to set it up instead of a state. */
function onocoyNotSetUp(c) {
  return h('div', { class: 'caster' },
    h('div', { class: 'spread' }, h('span', { class: 'name' }, c.name || 'Onocoy'), badge('Not set up', '')),
    h('div', { class: 'muted small' }, 'Onocoy not set up.'),
    h('div', { class: 'form-actions' }, link('Add your Onocoy credentials', ONOCOY_SETUP_HASH, 'btn primary small')));
}

function castersCard(s, admin) {
  const list = (s.casters || []).filter(c => admin ? (c.enabled || c.host || c.state !== 'disabled') : true);
  return h('section', { class: 'card' + (admin ? ' wide' : '') },
    h('h2', null, 'Casters'),
    list.length ? h('div', { class: 'caster-list' }, list.map(c => admin && c.protected && c.onocoy_ready === false ?
      onocoyNotSetUp(c) : casterLine(c, admin))) :
      h('p', { class: 'muted' }, 'No caster set up. ', admin ? link('Add one', '#/casters') : null),
    admin ? h('div', { class: 'foot' }, link('Manage casters', '#/casters')) : null);
}

function networkCard(s) {
  const n = s.network || {};
  const stale = typeof n.probe_age_s === 'number' && n.probe_age_s > 600;
  const rows = [
    ['Wi-Fi', n.ssid ? n.ssid + (n.online ? '' : ' (not connected)') : 'Not set up'],
    n.cred_trial ? ['New network', 'trying ' + (n.pending_ssid || '') + (n.cred_trial_left_s ? ', ' + fmt.dur(n.cred_trial_left_s) + ' left' : '')] :
      n.cred_pending ? ['New network', h('span', { class: 't-warn' }, (n.pending_ssid || '') + ' not confirmed' +
        (n.cred_reverted ? (n.cred_reverted_why === 'auth_failed' ? ' (password refused; back on the previous network)' : ' (back on the previous network)') : ''))] : null,
    !n.online && n.assoc_no_ip_s ? ['Connected without an address for', fmt.dur(n.assoc_no_ip_s)] : null,
    ...(n.online ? stationAddressRows(n) : [['Offline for', fmt.dur(n.offline_s)]]),
    n.online ? ['Signal', rssiView(n.rssi)] : null,
    ['Internet', n.upstream === 'ok' ? h('span', { class: 't-ok' }, 'Reachable' + (stale ? ' (checked ' + fmt.dur(n.probe_age_s) + ' ago)' : '')) :
      h('span', { class: n.upstream === 'none' ? 'muted' : 't-warn' }, lbl(L.upstream, n.upstream))],
    n.ap_active ? ['Access point', 'On (' + (n.ap_reasons || []).map(r => lbl(L.apReason, r)).join(', ') + ')' +
      (typeof n.ap_close_in_s === 'number' ? ', closes in ' + fmt.dur(n.ap_close_in_s) : '')] : null,
  ];
  return h('section', { class: 'card' },
    h('h2', null, 'Network', badge(n.online ? 'Online' : 'Offline', n.online ? 'ok' : 'bad')),
    kv(rows),
    h('div', { class: 'foot' }, link('Wi-Fi settings', '#/system')));
}

function alertText(a) {
  const table = a.source === 'station' ? L.salert : L.halert;
  const t = table[a.id];
  let title = t ? t[0] : a.id, text = t ? t[1] : '';
  if (a.id === 'config_failed' && a.retry_in_s) text += ' Next try in ' + fmt.dur(a.retry_in_s) + '.';
  if (a.source === 'station' && a.id === 'no_receiver') text = noReceiverText(a);
  return [title, text];
}

function alertTone(a) {
  return a.id === 'safe_mode' || a.id === 'receiver_power' || a.id === 'settings_degraded' || a.id === 'no_receiver' ? 'bad' : 'warn';
}

/*
 * "Check again" (POST /api/gnss/detect) on the no-receiver alert, on pages
 * rebuilt every refresh (the dashboard patches, the Health tab mounts): a
 * button without a listener of its own, so patch() keeps it and the
 * keyboard focus on it; the page's root handles its click (detectClicks).
 * After a request it reads "Searching…" for a few seconds, until the
 * station's own state says it searches.
 */
const DETECT = { askedAt: 0, busy: false };

const detectWaiting = () => DETECT.busy || Date.now() - DETECT.askedAt < 4000;

function detectButton(a) {
  if (a.next_detect_in_s !== null && a.next_detect_in_s !== undefined && a.next_detect_in_s <= 0) return null;   /* searching now */
  /* aria-disabled, not disabled: a disabled button would drop the keyboard focus. */
  const waiting = detectWaiting();
  return h('div', { class: 'form-actions' }, h('button', { type: 'button', class: 'btn small', 'data-act': 'detect',
    'aria-disabled': waiting ? 'true' : null }, waiting ? 'Searching…' : 'Check again'));
}

/** Handle "Check again" clicks inside root; refresh() redraws the page at once. */
function detectClicks(root, refresh) {
  root.addEventListener('click', async e => {
    const b = e.target && e.target.closest ? e.target.closest('[data-act="detect"]') : null;
    if (!b || !root.contains(b) || detectWaiting()) return;
    DETECT.busy = true;
    if (refresh) refresh();
    try {
      await api('POST', '/api/gnss/detect', {});
      DETECT.askedAt = Date.now();
    } catch (ex) {
      showErr(ex);
    } finally {
      DETECT.busy = false;
      if (refresh) refresh();
      setTimeout(() => {
        if (refresh && root.isConnected) refresh();
      }, 4100);
    }
  });
}

/**
 * The alerts (station and health) as message boxes; the Health page shows
 * the same. opts.detect: the no-receiver alert offers "Check again" (the
 * page handles it with detectClicks).
 */
function alertList(alerts, opts) {
  const detect = !!(opts && opts.detect);
  return h('div', { class: 'stack' }, alerts.map(a => {
    const t = alertText(a);
    const check = detect && a.source === 'station' && a.id === 'no_receiver' ? detectButton(a) : null;
    return msg(alertTone(a), h('strong', null, t[0]), t[1] ? h('div', { class: 'small' }, t[1]) : null, check);
  }));
}

function alertsCard(s) {
  const alerts = s.alerts || [];
  const tone = heroState(s).tone;
  const plan = restartPlanText(s.health);
  return h('section', { class: 'card' },
    h('h2', null, 'Alerts', alerts.length ? badge(String(alerts.length), alerts.some(a => alertTone(a) === 'bad') ? 'bad' : 'warn') :
      badge('None', tone === 'ok' ? 'ok' : '')),
    alerts.length ? alertList(alerts, { detect: true }) :
      /* Never "nothing" while the state above says otherwise. */
      h('p', { class: 'muted' }, tone === 'ok' ? 'Nothing needs attention.' : 'See the receiver card above.'),
    s.health && s.health.last_step && s.health.last_step !== 'none' ?
      h('p', { class: 'muted small foot' }, 'Last recovery step: ' + lbl(L.step, s.health.last_step) + '.') : null,
    plan ? h('p', { class: 'small foot' }, plan) : null,
    s.health && s.health.recovery_mode ? h('p', { class: 'muted small foot' }, 'Automatic receiver recovery: ' + lbl(L.recovery, s.health.recovery_mode).toLowerCase() +
      (s.health.recovery_mode === 'alerts_only' ? ' (a stall is shown here, nothing is sent to the receiver).' : '.'), ' ',
    link('Change', '#/receiver')) : null);
}

function deviceCard(s) {
  const d = s.device || {};
  return h('section', { class: 'card' },
    h('h2', null, 'Station'),
    kv([
      ['Name', d.hostname],
      ['Version', d.version ? [d.version, d.dev_build ? ' ' : null, d.dev_build ? badge('development build', 'plain') : null] : DASH],
      ['Running for', fmt.dur(s.uptime_s)],
      ['Time', d.time_unix ? fmt.time(d.time_unix) + ' (' + lbl(L.timeSource, d.time_source) + ')' : 'Not set yet'],
    ]),
    h('div', { class: 'foot' }, link('Diagnostics', '#/diag')));
}

function adminDashboard(s) {
  const ono = onocoySlot(s);
  return [
    heroCard(s),
    h('div', { class: 'grid' },
      receiverCard(s, true),
      onocoyCounting(ono) ? budgetCard(ono, s.availability) : onocoySetupCard(ono),
      castersCard(s, true),
      networkCard(s),
      alertsCard(s),
      deviceCard(s)),
  ];
}

function publicDashboard(s) {
  const hl = s.health || {};
  return [
    heroCard(s),
    h('div', { class: 'grid' },
      receiverCard(s, false),
      castersCard(s, false),
      h('section', { class: 'card' }, h('h2', null, 'Health', badge(lbl(L.health, hl.level), healthTone(hl.level))),
        kv([['Running for', fmt.dur(s.uptime_s)]]))),
  ];
}
