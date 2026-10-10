/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Status dashboard: overall state, receiver and RTCM, the Onocoy day (UTC)
 * with the remaining offline budget, every caster, Wi-Fi, alerts. Built
 * from the status model on every update and patched into the page (no
 * inputs on this page), so the links keep keyboard focus and text stays
 * selectable across the 1 s updates. Visitors without a session see the
 * public form (names and states only).
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
    render();
    return { update: render };
  },
};

function enabledCasters(s) {
  return (s.casters || []).filter(c => c.enabled !== false && c.state !== 'disabled');
}

function heroCard(s) {
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
  const facts = [];
  if (on.length) facts.push(streaming + ' of ' + on.length + ' casters streaming');
  if (hl.level) facts.push(lbl(L.health, hl.level).toLowerCase());
  if (hl.recovery_mode && hl.recovery_mode !== 'full') facts.push('receiver recovery: ' + lbl(L.recovery, hl.recovery_mode));
  if (s.uptime_s !== undefined) facts.push('up ' + fmt.dur(s.uptime_s));
  return h('section', { class: 'card hero ' + tone }, h('div', { class: 'state' }, head), h('div', { class: 'facts' }, facts.join(' · ')));
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
    if (r.position_locked) base = 'Position locked';
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
    admin ? h('div', { class: 'foot row' }, btn('Live view', openLiveView, 'small'), link('Receiver and position', '#/receiver'), link('Signals', '#/signals')) :
      h('div', { class: 'foot' }, link('Signals', '#/signals')));
}

function budgetCard(c, avail) {
  const left = avail && avail.budget_left_s !== undefined ? avail.budget_left_s : c.budget_left_s;
  const down = avail && avail.protected_down_today_s !== undefined ? avail.protected_down_today_s : c.today_offline_s;
  const total = left !== null && left !== undefined && down !== null && down !== undefined ? left + down : null;
  const used = total ? Math.min(1, down / total) : 0;
  const tone = left === null || left === undefined ? '' : left <= 0 ? 'bad' : used > 0.6 ? 'warn' : 'ok';
  const mid = fmt.utcMidnight();
  return h('section', { class: 'card' },
    h('h2', null, 'Onocoy today', c.streak_ok === null || c.streak_ok === undefined ? null :
      badge(c.streak_ok ? 'Streak on track' : 'Streak missed', c.streak_ok ? 'ok' : 'bad')),
    left === null || left === undefined ?
      h('p', { class: 'muted' }, 'The offline budget needs a trusted clock (GNSS or internet time).') :
      [h('div', { class: 'big ' + tone }, h('span', { class: 't-' + (tone || 'ok') }, left <= 0 ? '0 s' : fmt.dur(left))),
        h('div', { class: 'muted small' }, 'offline budget left today'),
        h('div', { class: tone }, bar(used)),
        h('div', { class: 'muted small' }, fmt.dur(down) + ' offline so far of about ' + fmt.dur(total) + '. Resets at 00:00 UTC (' + mid.local + ' here).')],
    kv([
      ['Online today', fmt.dur(c.today_online_s)],
      ['Offline today', fmt.dur(c.today_offline_s)],
      ['Graced uptime', c.graced_uptime_permille === null || c.graced_uptime_permille === undefined ? DASH :
        h('span', { class: c.graced_uptime_permille >= STREAK_PERMILLE ? 't-ok' : 't-bad' },
          fmt.permille(c.graced_uptime_permille) + ' (needs 99.8 %)')],
      ['Data rate', fmt.permille(c.data_rate_permille)],
    ]),
    h('p', { class: 'muted small foot' }, 'Our estimate of Onocoy\'s accounting for the current UTC day. Information only.'));
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

function castersCard(s, admin) {
  const list = (s.casters || []).filter(c => admin ? (c.enabled || c.host || c.state !== 'disabled') : true);
  return h('section', { class: 'card' + (admin ? ' wide' : '') },
    h('h2', null, 'Casters'),
    list.length ? h('div', { class: 'caster-list' }, list.map(c => casterLine(c, admin))) :
      h('p', { class: 'muted' }, 'No caster set up. ', admin ? link('Add one', '#/casters') : null),
    admin ? h('div', { class: 'foot' }, link('Manage casters', '#/casters')) : null);
}

function networkCard(s) {
  const n = s.network || {};
  const rows = [
    ['Wi-Fi', n.ssid ? n.ssid + (n.online ? '' : ' (not connected)') : 'Not set up'],
    n.cred_trial ? ['New network', 'trying ' + (n.pending_ssid || '') + (n.cred_trial_left_s ? ', ' + fmt.dur(n.cred_trial_left_s) + ' left' : '')] :
      n.cred_pending ? ['New network', h('span', { class: 't-warn' }, (n.pending_ssid || '') + ' not confirmed' + (n.cred_reverted ? ' (back on the previous network)' : ''))] : null,
    !n.online && n.assoc_no_ip_s ? ['Connected without an address for', fmt.dur(n.assoc_no_ip_s)] : null,
    n.online ? ['Address', n.ip] : ['Offline for', fmt.dur(n.offline_s)],
    n.online ? ['Signal', rssiView(n.rssi)] : null,
    ['Internet', n.upstream === 'ok' ? h('span', { class: 't-ok' }, 'Reachable') :
      h('span', { class: n.upstream === 'none' ? 'muted' : 't-warn' }, lbl(L.upstream, n.upstream))],
    n.ap_active ? ['Access point', 'On (' + (n.ap_reasons || []).map(r => lbl(L.apReason, r)).join(', ') + ')'] : null,
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
  return [title, text];
}

function alertsCard(s) {
  const alerts = s.alerts || [];
  return h('section', { class: 'card' },
    h('h2', null, 'Alerts', alerts.length ? badge(String(alerts.length), 'warn') : badge('None', 'ok')),
    alerts.length ? h('div', { class: 'stack' }, alerts.map(a => {
      const t = alertText(a);
      return msg(a.id === 'safe_mode' || a.id === 'receiver_power' || a.id === 'settings_degraded' ? 'bad' : 'warn',
        h('strong', null, t[0]), t[1] ? h('div', { class: 'small' }, t[1]) : null);
    })) : h('p', { class: 'muted' }, 'Nothing needs attention.'),
    s.health && s.health.last_step && s.health.last_step !== 'none' ?
      h('p', { class: 'muted small foot' }, 'Last recovery step: ' + lbl(L.step, s.health.last_step) + '.') : null,
    s.health && s.health.reboot_recheck ? h('p', { class: 'muted small foot' },
      'ESP32 reboot blocked; checked again after the next detection pass when the cap allows.') : null,
    s.health && s.health.recovery_mode ? h('p', { class: 'muted small foot' }, 'Automatic receiver recovery: ' + lbl(L.recovery, s.health.recovery_mode) +
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
  const prot = (s.casters || []).find(c => c.protected && c.enabled !== false);
  return [
    heroCard(s),
    h('div', { class: 'grid' },
      receiverCard(s, true),
      prot ? budgetCard(prot, s.availability) : null,
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
