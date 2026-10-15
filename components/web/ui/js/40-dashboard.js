/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Status dashboard: the top card (a title, one line with the most important
 * thing and what to do, at most one button, and a few labelled facts), the
 * alerts the top card does not show, every caster, the Onocoy day (UTC) with
 * the offline time left, the receiver, the network and the station. Built
 * from the status model on every update and patched into the page (no
 * inputs, and no element with a listener of its own: patch() replaces such
 * an element every time, which would take the keyboard focus with it), so
 * the links and buttons keep keyboard focus and text stays selectable
 * across the 1 s updates. The two buttons, "Check again" (no receiver found)
 * and "End now" (receiver maintenance), carry data-act and the page's root
 * handles their clicks (detectClicks, endMaintClicks); patchDetect() keeps
 * the keyboard focus on the line when "Check again" goes. The live view
 * entry is a link that starts the window. Visitors without a session see the
 * public form (names and states only).
 *
 * Every "today" figure (online, offline, the offline time left) comes from
 * the availability ledger, which is kept across restarts.
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
      patchDetect(el, isAdmin() && !s.public ? adminDashboard(s) : publicDashboard(s));
    };
    detectClicks(el, render);
    endMaintClicks(el, render);
    render();
    return { update: render };
  },
};

function enabledCasters(s) {
  return (s.casters || []).filter(c => c.enabled !== false && c.state !== 'disabled');
}

/*
 * The receiver's state as the page tells it. During receiver maintenance the
 * station reports idle while the casters still get the receiver's data; the
 * public form has no maintenance flag, so idle with a caster streaming reads
 * as streaming there.
 */
function rxState(s) {
  const r = s.receiver || {};
  if (r.state === 'idle' && !r.rx_maintenance && enabledCasters(s).some(c => c.state === 'streaming')) return 'streaming';
  return r.state;
}

/* The top card's title while the receiver does not stream (L.station keeps the longer words for Diagnostics). */
const HERO_TITLE = {
  idle: 'Starting up', detecting: 'Looking for the receiver', configuring: 'Configuring the receiver',
  flashing: 'Updating the receiver', flash_recovery: 'Finishing a receiver update', recovering: 'Fixing the receiver',
  no_receiver: 'No receiver found',
};
/* The top card's line while the receiver does not stream, by state. */
const HERO_STATE_LINE = {
  idle: 'The station is listening for the receiver.',
  detecting: 'Trying every link speed the receiver may use.',
  configuring: 'Streaming starts when it is done.',
  flashing: 'Keep the station powered. Streaming resumes after the update.',
  flash_recovery: 'A receiver update was interrupted. Keep the station powered while it finishes.',
  no_receiver: 'Nothing arrives from the receiver.',
};
/* The receiver recovery's last step, in the line "The station is fixing it: ..." (health.last_step). */
const HERO_STEP = {
  resync: 'resyncing the data', link_restart: 'reconnecting to the receiver', rescan: 'searching for the receiver',
  reconfigure: 'configuring the receiver again', receiver_reset: 'restarting the receiver', reboot: 'restarting the station',
};
/* The Receiver card's chip: short, so it never repeats the top card's title. */
const RX_CHIP = {
  streaming: 'Streaming', detecting: 'Searching', no_receiver: 'Not found', configuring: 'Configuring', flashing: 'Updating',
  flash_recovery: 'Finishing update', recovering: 'Fixing', idle: 'Starting',
};
/* Health alerts that say why the receiver sends nothing (the recovery's own evidence). */
const LADDER_REASONS = ['receiver_power', 'receiver_silent', 'no_satellites', 'no_rtcm'];
/* Other alerts for the top card while the casters stream, in this order, with the page that fixes each. */
const HERO_ALERTS = [
  ['settings_degraded', 'Diagnostics', '#/diag'], ['power_lost'], ['power_supply'], ['link_loss', 'Diagnostics', '#/diag'],
  ['config_failed', 'Receiver', '#/receiver'], ['antenna_moved', 'Receiver', '#/receiver'], ['receiver_attention', 'Updates', '#/updates'],
  ['unsupported_model'], ['bands', 'Signals', '#/signals'], ['signals_mismatch', 'Receiver', '#/receiver'],
  ['profile_drift', 'Receiver', '#/receiver'], ['position_mismatch', 'Receiver', '#/receiver'], ['heap_fragmented'],
];

/** The overall state: {tone, head, on, streaming} (the top card's title and dot). */
function heroState(s, admin) {
  const rcv = s.receiver || {};
  const hl = s.health || {};
  const on = enabledCasters(s);
  const streaming = on.filter(c => c.state === 'streaming').length;
  const state = rxState(s);
  let tone = 'warn', head;
  if (s.safe_mode && s.safe_mode.active) {
    tone = 'bad';
    head = 'Safe mode';
  } else if (rcv.rx_maintenance) {
    head = 'Receiver maintenance';
  } else if (admin && rcv.first_apply_held) {
    head = 'Receiver not configured yet';
  } else if (state !== 'streaming') {
    head = HERO_TITLE[state] || lbl(L.station, state);
    if (state === 'no_receiver') tone = 'bad';
  } else if (hl.level === 'recovering') {
    head = 'Fixing the receiver';
  } else if (!on.length) {
    head = 'No caster turned on';
  } else if (streaming === on.length) {
    tone = 'ok';
    head = 'Streaming';
  } else if (streaming) {
    head = 'Partly streaming';
  } else {
    tone = 'bad';
    head = 'Not streaming';
  }
  /* The level alone never turns green to amber: an alert the page lists does (failed always comes with one). */
  if (hl.level === 'failed') tone = 'bad';
  else if (tone === 'ok' && admin && (s.alerts || []).length) tone = 'warn';
  return { tone: tone, head: head, on: on, streaming: streaming };
}

const casterName = c => c.name || c.host || 'A caster';

/* "A and B", "A, B and C". */
function andList(names) {
  return names.length < 2 ? names.join('') : names.slice(0, -1).join(', ') + ' and ' + names[names.length - 1];
}

/* Why a caster does not stream, for a sentence: its last failure, else its state; the first letter lowered (not in DNS, TLS, Onocoy). */
function downReason(c) {
  const r = c.reason && c.reason !== 'none' ? lbl(L.fail, c.reason) : lbl(L.caster, c.state);
  return /^([A-Z]{2,}|Onocoy)\b/.test(r) ? r : r.charAt(0).toLowerCase() + r.slice(1);
}

/**
 * The top card's line: the first thing that applies, receiver first, then the
 * casters, then everything else, then "all good". Returns {key, line, action,
 * alertId, notice}: action {label, href, primary} (a link) or {act} (a button
 * the page handles: 'detect', 'end-maint'); alertId the alert the line says
 * (the Alerts card leaves it out); notice the page-wide notice it covers (the
 * Status page leaves that out, noticeItems). admin false: the public form
 * (states only, no alerts, no actions).
 */
function heroLead(s, admin) {
  const rcv = s.receiver || {};
  const hl = s.health || {};
  const alerts = admin ? s.alerts || [] : [];
  const find = (id, src) => alerts.find(a => a.id === id && (!src || a.source === src)) || null;
  const lead = (key, line, more) => Object.assign({ key: key, line: line, action: null, alertId: null, notice: null }, more || {});
  const go = (label, href, primary) => ({ label: label, href: href, primary: !!primary });
  const on = enabledCasters(s);
  const down = on.filter(c => c.state !== 'streaming');
  const state = rxState(s);

  if (admin) {
    const sm = s.safe_mode || {};
    if (sm.active) {
      return lead('safe_mode', 'The receiver and casters are off after ' + (sm.crashes ? sm.crashes + ' crashes' : 'repeated crashes') + '.' +
        (sm.retry_in_s ? ' The station tries normal operation again in ' + upMin(sm.retry_in_s) + '.' : ''),
      { action: go('Details', '#/system'), alertId: 'safe_mode', notice: 'safe_mode' });
    }
    if (rcv.uart_closed) return lead('uart_closed', UART_CLOSED_TEXT, { action: go('Details', '#/diag'), notice: 'uart_closed' });
    const j = rcv.journal;
    const loader = find('receiver_in_loader');
    if (j === 'waiting' || j === 'unreadable' || j === 'unknown' || loader) {
      const line = loader && (!j || j === 'none') ? 'The receiver waits in its update loader. Choose what to do on the Updates page.' :
        j === 'unknown' ? 'The receiver update state is unknown this boot, so nothing is sent to the receiver.' : JOURNAL_LINE[j];
      return lead('journal', line, { action: go('Open', '#/updates/session'), alertId: loader ? 'receiver_in_loader' : null, notice: 'journal' });
    }
    if (rcv.rx_maintenance) {
      return lead('maintenance', 'The station sends the receiver no commands.' +
        (rcv.maintenance_left_s ? ' Ends in about ' + upMin(rcv.maintenance_left_s) + '.' : ''),
      { action: { act: 'end-maint', label: 'End now' }, notice: 'maintenance' });
    }
    if (rcv.first_apply_held || find('rx_held')) {
      return lead('held', HELD_TEXT, { action: go('Review and configure', '#/receiver'), alertId: 'rx_held', notice: 'held' });
    }
    for (const id of LADDER_REASONS) {
      const a = find(id, 'health');
      if (a) return lead('alert', alertText(a)[1], { alertId: id });
    }
    const nr = find('no_receiver', 'station');
    if (nr) return lead('no_receiver', noReceiverText(nr), { action: { act: 'detect', alert: nr }, alertId: 'no_receiver' });
  }
  if (state !== 'streaming' || hl.level === 'recovering') {
    if (state === 'recovering' || state === 'streaming') {
      const step = admin ? HERO_STEP[hl.last_step] : null;
      return lead('fixing', 'Corrections from the receiver stopped. The station is fixing it' + (step ? ': ' + step : '') + '.');
    }
    return lead('receiver', HERO_STATE_LINE[state] || lbl(L.station, state) + '.');
  }
  if (admin && on.some(c => c.state === 'wait_data')) {
    for (const [id, label, href] of [['antenna_moved', 'Receiver', '#/receiver'], ['receiver_attention', 'Updates', '#/updates'],
      ['config_failed', 'Receiver', '#/receiver']]) {
      const a = find(id, 'station');
      if (a) return lead('alert', alertText(a)[1], { action: go(label, href), alertId: id });
    }
  }
  if (!on.length) {
    if (!admin) return lead('no_caster', 'The station is not streaming to any caster.');
    const ono = onocoySlot(s);
    if (ono && ono.onocoy_ready === false) {
      return lead('no_caster', 'Add your Onocoy credentials to start streaming.', { action: go('Set up Onocoy', ONOCOY_SETUP_HASH, true) });
    }
    if (ono) {
      return lead('no_caster', 'Onocoy is set up but turned off. Turn it on to start streaming.',
        { action: go('Turn Onocoy on', ONOCOY_SETUP_HASH, true) });
    }
    if ((s.casters || []).some(c => !emptySlot(c))) {
      return lead('no_caster', 'Every caster is turned off. Turn one on to start streaming.', { action: go('Casters', '#/casters', true) });
    }
    return lead('no_caster', 'Add a caster to start streaming.', { action: go('Add a caster', '#/casters', true) });
  }
  const stopped = admin ? on.find(c => c.state === 'stopped') : null;
  if (stopped) {
    return lead('stopped', casterName(stopped) + ' stopped: ' + lbl(L.stop, stopped.stop_reason || 'none') + '. It makes no new attempt until you act.',
      { action: go('What to do', '#/casters'), notice: 'onocoy_stopped:' + stopped.slot });
  }
  const noNet = down.length ? find('no_upstream') : null;
  if (noNet) return lead('alert', alertText(noNet)[1], { alertId: 'no_upstream' });
  if (down.length) {
    let line;
    if (down.every(c => c.state === 'wait_data')) {
      line = 'Waiting for corrections from the receiver.';
    } else if (down.length === 1) {
      const c = down[0];
      line = casterName(c) + ' is not streaming' + (admin ? ': ' + downReason(c) : '') + '.' +
        (admin && c.next_retry_s ? ' Next try in ' + fmt.dur(c.next_retry_s) + '.' : '');
    } else {
      line = (down.length === 2 ? andList(down.map(casterName)) : down.length + ' casters') + ' are not streaming.';
    }
    return lead('casters_down', line, admin ? { action: go('Casters', '#/casters') } : null);
  }
  for (const [id, label, href] of HERO_ALERTS) {
    const a = find(id);
    if (a) {
      return lead('alert', alertText(a)[1], { action: label ? go(label, href) : null, alertId: id,
        notice: id === 'settings_degraded' ? 'settings' : null });
    }
  }
  const names = on.map(casterName);
  return lead('streaming', (names.length > 3 ? names.length + ' casters' : andList(names)) +
    (names.length > 1 ? ' are' : ' is') + ' receiving corrections.');
}

/* The top card's one button, or null. */
function heroAction(lead) {
  const a = lead.action;
  if (!a) return null;
  let b;
  if (a.act === 'detect') b = detectButton(a.alert);
  else if (a.act) b = h('button', { type: 'button', class: 'btn small', 'data-act': a.act }, a.label);
  else b = link(a.label, a.href, a.primary ? 'btn primary' : 'btn small');
  return b ? h('div', { class: 'hero-actions' }, b) : null;
}

function heroCard(s, admin) {
  const st = heroState(s, admin);
  const lead = heroLead(s, admin);
  const facts = [];
  if (st.on.length) facts.push(['Casters', st.streaming + ' of ' + st.on.length + ' streaming']);
  if (s.uptime_s !== undefined) facts.push(['Running for', fmt.dur(s.uptime_s)]);
  /* The no-receiver line says whether a search runs: it takes the focus when "Check again" goes (patchDetect). */
  const noRx = lead.key === 'no_receiver';
  return h('section', { class: 'card hero ' + st.tone },
    h('h2', { class: 'state' }, st.head),
    h('p', { class: 'line', tabindex: noRx ? '-1' : null, 'data-detect-text': noRx ? '' : null }, lead.line),
    heroAction(lead),
    facts.length ? h('dl', { class: 'facts' }, facts.map(f => h('div', null, h('dt', null, f[0]), h('dd', null, f[1])))) : null);
}

/* A card's buttons, at the right on a desktop and full width on a phone (the card carries has-actions). */
function cardActions(...kids) {
  return h('div', { class: 'form-actions card-actions' }, kids);
}

function receiverCard(s, admin) {
  const r = s.receiver || {};
  const hl = s.health || {};
  const state = rxState(s);
  let chip;
  if (admin && s.safe_mode && s.safe_mode.active) chip = badge('Off', '');
  else if (r.rx_maintenance) chip = badge('Maintenance', 'warn');
  else if (state === 'streaming' && hl.level === 'recovering') chip = badge('Fixing', 'warn');
  else chip = badge(lbl(RX_CHIP, state), stationTone(state));
  const rows = [
    admin ? ['Receiver', r.model || DASH] : null,
    admin ? ['Last corrections', h('span', { class: 't-' + ageTone(r.rtcm_age_ms) },
      r.rtcm_age_ms === null || r.rtcm_age_ms === undefined ? 'Never' : fmt.ms(r.rtcm_age_ms) + ' ago')] : null,
    admin ? ['Output rate', outputRate() === null ? DASH : fmt.kbps(outputRate())] : null,
    ['Best constellation', r.best_sys ? lbl(L.sys, r.best_sys) + ' · ' + r.best_bands + ' bands' : r.best_bands ? r.best_bands + ' bands' : DASH],
  ];
  let plan = null;
  if (admin) {
    let pos;
    /* position_locked is the stored profile's base mode: the receiver runs it only once the profile is applied. */
    if (r.position_locked) pos = r.profile_applied ? 'Locked' : 'Fixed, not applied yet';
    else if (r.surveying) pos = 'Surveying' + (r.survey_window_left_s ? ', ' + inMinutes(r.survey_window_left_s) + ' left' : '');
    else pos = 'Not locked';
    rows.push(['Position', pos]);
    rows.push(['Profile', r.profile_applied ? 'Applied' :
      h('span', { class: 't-warn' }, r.first_apply_held ? 'Not applied (waiting for you)' : 'Not applied')]);
    if (hl.recovery_mode && hl.recovery_mode !== 'full') {
      rows.push(['If corrections stop', [lbl(L.recovery, hl.recovery_mode), ' ', link('Change', '#/receiver')]]);
    }
    /* The station's own restart for the receiver: said only while the receiver does not deliver (the recovery's
     * episode may still report a held restart for a while after the corrections came back). */
    if (state !== 'streaming' || hl.level === 'recovering' || hl.level === 'failed') plan = restartPlanText(hl);
  }
  return h('section', { class: 'card has-actions' },
    cardHead('Receiver', chip),
    kv(rows),
    plan ? h('p', { class: 'muted small' }, plan) : null,
    admin ? cardActions(link('Live view', LIVE_START_HASH, 'btn small'), link('Receiver settings', '#/receiver', 'btn small')) :
      cardActions(link('Signals', '#/signals', 'btn small')));
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

/**
 * "Graced uptime so far" in words and tone: green when it meets 99.8 %, red
 * when it misses it and the streak is lost; below 99.8 % while the streak
 * still holds (early in the day, the budget not used up) neither: not red,
 * since the streak is the verdict, and not green, since it misses the
 * threshold it is measured against.
 */
function soFarView(sofar, streakOk, bare) {
  if (sofar === null || sofar === undefined) return DASH;
  const meets = sofar >= STREAK_PERMILLE;
  const tone = meets ? 't-ok' : streakOk ? null : 't-bad';
  /* bare: the value only, where the tone and a streak chip say the rest. */
  const note = bare ? '' : meets ? (streakOk === false ? '' : ' (streak on track)') : streakOk ? ' (below 99.8 %; the streak still holds)' : ' (below 99.8 %)';
  return h('span', { class: tone }, fmt.permille(sofar) + note);
}

function budgetCard(c, avail) {
  /* All from the availability ledger (kept across restarts): today's offline
   * time, what is left of the day's allowance, and the allowance itself at
   * the current data rate. Without a trusted clock the day is unknown. */
  const left = avail && avail.budget_left_s !== undefined ? avail.budget_left_s : c.budget_left_s;
  const down = avail && avail.protected_down_today_s !== undefined ? avail.protected_down_today_s : c.today_offline_s;
  const allowed = c.budget_s;
  const known = left !== null && left !== undefined && down !== null && down !== undefined &&
    allowed !== null && allowed !== undefined;
  const used = known && allowed > 0 ? Math.min(1, down / allowed) : known ? 1 : 0;
  /* Never red while the streak holds: the allowance is an estimate, the streak the verdict. */
  const tone = !known ? '' : left <= 0 && !c.streak_ok ? 'bad' : used > 0.6 ? 'warn' : 'ok';
  const now = new Date();
  const reset = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() + 1))
    .toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
  const over = known && down > allowed;
  return h('section', { class: 'card' },
    cardHead('Onocoy today', c.streak_ok === null || c.streak_ok === undefined ? null :
      badge(c.streak_ok ? 'Streak on track' : 'Streak missed', c.streak_ok ? 'ok' : 'bad')),
    !known ?
      h('p', { class: 'muted' }, 'Shows once the station has the time (from GNSS or the internet).') :
      [h('div', { class: 'big ' + tone }, h('span', { class: 't-' + (tone || 'ok') }, left <= 0 ? '0' + NB + 's' : fmt.dur(left))),
        h('div', { class: 'muted small' }, 'offline time left today'),
        h('div', { class: tone }, bar(used, '', 'Share of today\'s offline time used')),
        h('div', { class: 'muted small' }, 'Resets at 00:00 UTC (' + reset + ' your time).')],
    kv([
      ['Online today', fmt.dur(c.today_online_s)],
      known ? ['Offline today', over ? h('span', { class: c.streak_ok === false ? 't-bad' : null },
        fmt.dur(down) + ', ' + fmt.dur(down - allowed) + ' over') : fmt.dur(down) + (allowed > 0 ? ' of about ' + fmt.dur(allowed) : '')] : null,
      ['Graced uptime so far', soFarView(c.graced_uptime_so_far_permille, c.streak_ok, true)],
      ['Data rate', c.data_rate_permille === null || c.data_rate_permille === undefined ? 'Nothing sent yet' : fmt.permille(c.data_rate_permille)],
    ]),
    h('p', { class: 'muted small' }, 'Our estimate; Onocoy\'s own count decides. The streak needs 99.8 % graced uptime each UTC day.'));
}

/** Today's online share: whole at 0 % and 100 %, else two decimals, rounded down (never "100.00" short of 100). */
function onlineShare(on, off) {
  const p = 100 * on / (on + off);
  return (p === 0 || p === 100 ? String(p) : (Math.floor(p * 100) / 100).toFixed(2)) + NB + '%';
}

/**
 * One caster on the dashboard: the name (and host, for the admin) with the
 * state chip, one line when it says more than the chip, the figures, and the
 * one thing to do, if any (at the right on a desktop). An Onocoy slot not set
 * up or turned off offers the fix.
 */
function casterLine(c, admin) {
  const ono = admin && c.protected;
  const notSetUp = ono && c.onocoy_ready === false;
  const off = c.enabled === false || c.state === 'disabled';
  const metrics = [];
  if (admin && !notSetUp && !off) {
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
    const on = c.today_online_s || 0, offS = c.today_offline_s || 0;
    if (on + offS > 0) metrics.push(['Today', onlineShare(on, offS) + ' online']);
    if (c.bytes_sent) metrics.push(['Sent', fmt.bytes(c.bytes_sent)]);
  }
  let line = null, act = null;
  if (notSetUp) {
    line = h('div', { class: 'muted small' }, 'Rewards your station for its correction data.');
    act = link('Set up', ONOCOY_SETUP_HASH, 'btn small');
  } else if (ono && off) {
    act = link('Turn on', ONOCOY_SETUP_HASH, 'btn small');
  } else if (admin && c.state === 'stopped') {
    const why = lbl(L.stop, c.stop_reason || 'none');
    line = h('div', { class: 't-bad small' }, (why ? capFirst(why) + '. ' : '') + 'No new attempt until you act.');
    act = link('What to do', '#/casters', 'btn small');
  } else if (admin) {
    line = casterDetail(c);
  }
  return h('div', { class: 'caster' + (act ? ' has-act' : '') },
    h('div', { class: 'item-head' },
      h('div', null, h('span', { class: 'name' }, casterName(c)), admin && c.host ? h('span', { class: 'muted small' }, ' ·' + NB + c.host) : null),
      notSetUp ? badge('Not set up', '') : badge(lbl(L.caster, c.state), casterTone(c.state))),
    line,
    metrics.length ? h('div', { class: 'metrics' }, metrics.map(m => h('span', null, m[0] + ' ', h('b', null, m[1])))) : null,
    act ? h('div', { class: 'form-actions' }, act) : null);
}

/* A slot with no caster in it (no host, not enabled): not a caster, so not listed. */
function emptySlot(c) {
  return c.empty === true;
}

function castersCard(s, admin) {
  const list = (s.casters || []).filter(c => !emptySlot(c) && (admin ? (c.enabled || c.host || c.state !== 'disabled') : true));
  /* The admin's list is full width (rows with figures and buttons); the public one sits beside the receiver. */
  return h('section', { class: 'card' + (admin ? ' wide has-actions' : '') },
    h('h2', null, 'Casters'),
    list.length ? h('div', { class: 'caster-list' }, list.map(c => casterLine(c, admin))) :
      admin ? null : h('p', { class: 'muted' }, 'No caster set up yet.'),
    !admin ? null : list.length ? cardActions(link('Manage casters', '#/casters', 'btn small')) :
      cardActions(h('span', { class: 'note muted' }, 'No caster set up yet.'), link('Add a caster', '#/casters', 'btn primary small')));
}

function networkCard(s) {
  const n = s.network || {};
  const stale = typeof n.probe_age_s === 'number' && n.probe_age_s > 600;
  const pend = n.pending_ssid || 'the new network';
  const rows = [
    ['Wi-Fi', n.ssid || 'Not set up'],
    n.cred_trial ? ['New network', ['Trying ' + pend + ', ', trialLeftEl(n)]] :
      n.cred_pending ? ['New network', h('span', { class: 't-warn' }, pend + ': ' + (!n.cred_reverted ? 'not confirmed yet' :
        n.cred_reverted_why === 'auth_failed' ? 'password refused' : 'no address'))] : null,
    !n.online && n.assoc_no_ip_s ? ['No IP address for', fmt.dur(n.assoc_no_ip_s)] : null,
    ...(n.online ? stationAddressRows(n) : [['Offline for', fmt.dur(n.offline_s)]]),
    n.online ? ['Signal', rssiView(n.rssi, true)] : null,
    ['Internet', n.upstream === 'ok' ? h('span', { class: 't-ok' }, 'Reachable' + (stale ? ' (checked ' + fmt.dur(n.probe_age_s) + ' ago)' : '')) :
      h('span', { class: n.upstream === 'none' ? 'muted' : 't-warn' }, lbl(L.upstream, n.upstream))],
    n.ap_active ? ['Setup network', 'Open (' + (n.ap_reasons || []).map(r => lbl(L.apReason, r)).join(', ') + ')' +
      (typeof n.ap_close_in_s === 'number' ? ', closes in ' + fmt.dur(n.ap_close_in_s) : '')] : null,
  ];
  return h('section', { class: 'card has-actions' },
    cardHead('Network', badge(n.online ? 'Online' : 'Offline', n.online ? 'ok' : 'bad')),
    kv(rows),
    cardActions(link('Wi-Fi settings', '#/system', 'btn small')));
}

function alertText(a) {
  const table = a.source === 'station' ? L.salert : L.halert;
  const t = table[a.id];
  let title = t ? t[0] : a.id, text = t ? t[1] : '';
  if (a.id === 'config_failed') text = configFailedText(a.auto_retry, a.retry_in_s);
  if (a.source === 'station' && a.id === 'no_receiver') text = noReceiverText(a);
  return [title, text];
}

function alertTone(a) {
  if (a.id === 'rx_held') return 'info';   /* waiting for the user: not a fault */
  return a.id === 'safe_mode' || a.id === 'receiver_power' || a.id === 'receiver_silent' || a.id === 'settings_degraded' ||
    a.id === 'no_receiver' ? 'bad' : 'warn';
}

/*
 * "Check again" (POST /api/gnss/detect) on the no-receiver alert, on pages
 * rebuilt every refresh (the dashboard and the Health tab, both through
 * patchDetect()): a button without a listener of its own, so patch() keeps
 * it and the keyboard focus on it; the page's root handles its click
 * (detectClicks). After a request it reads "Searching…" for a few seconds,
 * until the station's own state says it searches; then the button goes,
 * and the keyboard focus moves to the alert's text, which says so.
 */
const DETECT = { askedAt: 0, busy: false };

const detectWaiting = () => DETECT.busy || Date.now() - DETECT.askedAt < 4000;

function detectButton(a) {
  if (a.next_detect_in_s !== null && a.next_detect_in_s !== undefined && a.next_detect_in_s <= 0) return null;   /* searching now */
  /* aria-disabled, not disabled: a disabled button would drop the keyboard focus. */
  const waiting = detectWaiting();
  return h('button', { type: 'button', class: 'btn small', 'data-act': 'detect', 'aria-disabled': waiting ? 'true' : null },
    waiting ? 'Searching…' : 'Check again');
}

/**
 * patch(el, ...kids) for a page that may show "Check again": when the button
 * had the keyboard focus and is gone afterwards, the focus moves to a new
 * "Check again" if there is one (the alert moved), else to the alert's text
 * (focusable from script only), never to the page's body.
 */
function patchDetect(el, ...kids) {
  const a = doc.activeElement;
  const had = !!(a && a.matches && a.matches('[data-act="detect"]') && el.contains(a));
  patch(el, ...kids);
  if (had && !a.isConnected) {
    const next = el.querySelector('[data-act="detect"]') || el.querySelector('[data-detect-text]');
    if (next) next.focus();
  }
  return el;
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

/** Handle "End now" (receiver maintenance, data-act="end-maint") inside root; refresh() redraws the page. */
function endMaintClicks(root, refresh) {
  root.addEventListener('click', async e => {
    const b = e.target && e.target.closest ? e.target.closest('[data-act="end-maint"]') : null;
    if (!b || !root.contains(b)) return;
    await endMaintenance({ currentTarget: b });
    if (refresh && root.isConnected) refresh();
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
    const noRx = detect && a.source === 'station' && a.id === 'no_receiver';
    const check = noRx ? detectButton(a) : null;
    /* The text that says whether it searches takes the focus when "Check again" goes (patchDetect). */
    return msg(alertTone(a), h('strong', null, t[0]), t[1] ? h('div', { class: 'small', tabindex: noRx ? '-1' : null,
      'data-detect-text': noRx ? '' : null }, t[1]) : null, check ? h('div', { class: 'form-actions' }, check) : null);
  }));
}

/* The alerts the top card's line does not say (alertId): none, no card. */
function alertsCard(s, lead) {
  const alerts = (s.alerts || []).filter(a => a.id !== lead.alertId);
  if (!alerts.length) return null;
  return h('section', { class: 'card wide' },
    cardHead('Alerts', badge(String(alerts.length), alerts.some(a => alertTone(a) === 'bad') ? 'bad' : 'warn')),
    alertList(alerts, { detect: true }));
}

function deviceCard(s) {
  const d = s.device || {};
  return h('section', { class: 'card has-actions' },
    h('h2', null, 'Station'),
    kv([
      ['Name', d.hostname],
      ['Version', d.version ? [d.version, d.dev_build ? ' ' : null, d.dev_build ? badge('development build', 'plain') : null] : DASH],
      ['Time', d.time_unix ? fmt.time(d.time_unix) + ' (' + lbl(L.timeSource, d.time_source) + ')' : 'Not set yet'],
    ]),
    cardActions(link('Diagnostics', '#/diag', 'btn small')));
}

/* Order: the top card; the other alerts and the casters, full width; Onocoy's day (while it counts) beside the receiver;
 * the network beside the station (with dense flow, the receiver moves up beside the network when Onocoy does not count). */
function adminDashboard(s) {
  const ono = onocoySlot(s);
  return [
    heroCard(s, true),
    h('div', { class: 'grid' },
      alertsCard(s, heroLead(s, true)),
      castersCard(s, true),
      onocoyCounting(ono) ? budgetCard(ono, s.availability) : null,
      receiverCard(s, true),
      networkCard(s),
      deviceCard(s)),
  ];
}

function publicDashboard(s) {
  return [
    heroCard(s, false),
    h('div', { class: 'grid' },
      castersCard(s, false),
      receiverCard(s, false)),
  ];
}
