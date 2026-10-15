/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station's state, decided in one place (stationState): the Status
 * page's top card, its "Onocoy today" verdict (onocoyDay) and receiver rows,
 * the public view, the browser tab's title and the page-wide notice the
 * Status page leaves out all read it, so they never say different things.
 *
 * The first condition that applies wins, from what stops everything to
 * "streaming to Onocoy". The colour comes from that condition alone: a
 * health level with no alert named never changes it. Other named alerts
 * follow as "Also:" lines. Green: streaming as it should; blue: something
 * planned or passing (maintenance, a comparison, a Wi-Fi trial, the
 * receiver starting or updating); amber: needs a look; red: broken.
 */

/* The top card's title while the receiver does not stream (L.station keeps the longer words for Diagnostics). */
const HERO_TITLE = {
  idle: 'Starting up', detecting: 'Looking for the receiver', configuring: 'Configuring the receiver',
  flashing: 'Updating the receiver', flash_recovery: 'Finishing a receiver update', no_receiver: 'No receiver found',
};
/* The top card's line for those states. */
const HERO_STATE_LINE = {
  idle: 'The station is listening for the receiver.',
  detecting: 'Trying every link speed the receiver may use.',
  configuring: 'Streaming starts when it is done.',
  flashing: 'Keep the station powered. Streaming resumes after the update.',
  flash_recovery: 'A receiver update was interrupted. Keep the station powered while it finishes.',
  no_receiver: 'Nothing arrives from the receiver.',
};
/* The receiver recovery's last step, in "The station is fixing it: ..." (health.last_step). */
const HERO_STEP = {
  resync: 'resyncing the data', link_restart: 'reconnecting to the receiver', rescan: 'searching for the receiver',
  reconfigure: 'configuring the receiver again', receiver_reset: 'restarting the receiver', reboot: 'restarting the station',
};
/* Health alerts that say why the receiver sends nothing (the recovery's own evidence). */
const LADDER_REASONS = ['receiver_power', 'receiver_silent', 'no_satellites', 'no_rtcm'];
/* Where an "Also:" line's Details button goes. */
const ALERT_PAGE = {
  settings_degraded: '#/diag', link_loss: '#/diag', power_supply: '#/diag', power_lost: '#/diag', heap_fragmented: '#/diag',
  no_upstream: '#/settings/wifi', receiver_power: '#/diag', receiver_silent: '#/diag', safe_mode: '#/settings/restart',
  no_satellites: '#/receiver', no_rtcm: '#/receiver', bands: '#/receiver', no_receiver: '#/receiver',
  config_failed: '#/receiver', antenna_moved: '#/receiver', unsupported_model: '#/receiver', signals_mismatch: '#/receiver',
  profile_drift: '#/receiver', position_mismatch: '#/receiver', rx_held: '#/receiver',
  receiver_attention: '#/settings/updates', receiver_in_loader: '#/settings/session',
};
/* Where the Onocoy credentials are entered (the caster editor opens on the Onocoy slot). */
const ONOCOY_SETUP_HASH = '#/casters/onocoy';

function enabledCasters(s) {
  return (s.casters || []).filter(c => c.enabled !== false && c.state !== 'disabled');
}

/* A slot with no caster in it (no host, not enabled): not a caster, so not listed. */
function emptySlot(c) {
  return c.empty === true;
}

/** The Onocoy slot (protected) of the status casters, or null. */
function onocoySlot(s) {
  return (s.casters || []).find(c => c.protected) || null;
}

/** True when the Onocoy slot is enabled and has the console's mountpoint, username and password. */
function onocoyCounting(c) {
  return !!(c && c.enabled !== false && c.onocoy_ready);
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

/**
 * Onocoy's quality minimums this board can miss (4 bands on one
 * constellation, GLONASS, message 1230 accepted), from the status, and only
 * while the receiver delivers: {known, bands, sys, fewBands, noGlonass,
 * refused1230, any}. Unknown values (an older firmware, no summary yet,
 * never configured) flag nothing.
 */
function rxQuality(rcv, delivering) {
  const known = !!delivering && rcv.best_bands > 0;
  const q = {
    known: known, bands: known ? rcv.best_bands : null, sys: known ? rcv.best_sys || null : null,
    fewBands: known && rcv.best_bands < 4,
    noGlonass: known && rcv.glonass === false,
    refused1230: known && rcv.glonass !== false && rcv.rtcm1230_supported === false,
  };
  q.any = q.fewBands || q.noGlonass || q.refused1230;
  return q;
}

/* The top card's line for a quality problem while Onocoy streams. */
function qualityLead(q, name) {
  if (q.fewBands && q.noGlonass) return name + ' gets ' + q.bands + ' bands and no GLONASS; its quality score needs 4 bands and GLONASS.';
  if (q.fewBands) return name + ' gets ' + q.bands + ' bands; its quality score needs 4 on one constellation.';
  if (q.noGlonass) return name + ' gets no GLONASS; its quality score needs it.';
  return 'The receiver refused message 1230, so GLONASS does not count for ' + name + '\'s quality score.';
}

/**
 * The station's state from a status (admin false: the public form, which
 * names states only). Returns {key, head, tone, lead, actions, alertId,
 * notice, trial, also, facts, delivering, quality, ono, onoStreaming}:
 *   key: which condition (one per entry of the list below);
 *   head, tone ('ok', 'info', 'warn', 'bad'), lead: the title, its colour and
 *     one sentence with the most important thing and what to do;
 *   actions: the one button (two in safe mode), each {label, href, primary}
 *     (a link) or {act, label} (a button the page handles through data-act:
 *     'detect', 'end-maint', 'leave-safe'), so a refresh keeps the focus;
 *   alertId: the alert the line says (never an "Also:" line too); notice: the
 *     page-wide notice it covers (the Status page leaves that one out);
 *     subject: the caster slot the line is about (its row does not repeat why);
 *   plan: when the station restarts itself to get the receiver back, while
 *     it fixes or looks for it (restartPlanText), else null;
 *   trial: the Wi-Fi network on trial (the card adds its countdown);
 *   also: the other named alerts [{id, title, text, tone, href}];
 *   facts: [label, text] ("Casters 1 of 2 streaming" with more than one
 *     turned on, unless the line names them all as streaming; the recovery
 *     choice when it is not the default);
 *   delivering: corrections come from the receiver; quality: rxQuality();
 *   ono: the Onocoy slot while it counts (on and set up), else null;
 *   onoStreaming: it streams with corrections behind it.
 *
 * Order: safe mode; receiver update needs you; settings storage; updating
 * the receiver; receiver port closed (a closed port explains a missing
 * receiver, so it goes before "No receiver found"); no receiver; receiver
 * stopped sending data; no satellites; no corrections; Onocoy stopped by its
 * login guard; receiver maintenance; comparing signal groups; trying new
 * Wi-Fi; receiver not configured yet, looking for or configuring the
 * receiver; Onocoy not set up; no caster turned on, not or partly streaming;
 * streaming with too few bands or no GLONASS for Onocoy; streaming.
 */
function stationState(s, admin) {
  const rcv = s.receiver || {};
  const hl = s.health || {};
  const alerts = admin ? s.alerts || [] : [];
  const find = (id, src) => alerts.find(a => a.id === id && (!src || a.source === src)) || null;
  const on = enabledCasters(s);
  const streaming = on.filter(c => c.state === 'streaming');
  const down = on.filter(c => c.state !== 'streaming');
  const state = rxState(s);
  const sm = (admin && s.safe_mode) || {};
  const ladder = LADDER_REASONS.map(id => find(id, 'health')).filter(Boolean)[0] || null;
  /* During receiver maintenance the station says idle while the corrections keep coming. */
  const maintFlowing = !!rcv.rx_maintenance && typeof rcv.rtcm_age_ms === 'number' && rcv.rtcm_age_ms < 5000;
  const delivering = !sm.active && (state === 'streaming' || maintFlowing) && hl.level !== 'recovering' && !ladder;
  const onoSlot = admin ? onocoySlot(s) : null;
  const ono = onocoyCounting(onoSlot) ? onoSlot : null;
  const onoStreaming = !!(ono && ono.state === 'streaming' && delivering);
  const quality = rxQuality(rcv, delivering);
  const alertsOnly = hl.recovery_mode === 'alerts_only';
  const go = (label, href, primary) => ({ label: label, href: href, primary: !!primary });
  const pick = (key, head, tone, lead, more) => Object.assign({ key: key, head: head, tone: tone, lead: lead, actions: [],
    alertId: null, notice: null, trial: null, subject: null }, more || {});

  const decide = () => {
    if (sm.active) {
      return pick('safe_mode', 'Safe mode', 'bad', 'The receiver and casters are off after ' +
        (sm.crashes ? sm.crashes + ' crashes' : 'repeated crashes') + '.' +
        (sm.retry_in_s ? ' The station tries normal operation again in ' + upMin(sm.retry_in_s) + '.' : ''),
      { actions: [go('Updates', '#/settings/updates'), { act: 'leave-safe', label: 'Leave safe mode' }], alertId: 'safe_mode',
        notice: 'safe_mode' });
    }
    if (admin) {
      const j = rcv.journal;
      const loader = find('receiver_in_loader');
      if (j === 'waiting' || j === 'unreadable' || j === 'unknown' || loader) {
        const lead = loader && (!j || j === 'none') ? 'The receiver waits in its update loader. Keep the station powered and choose what to do.' :
          j === 'unknown' ? 'The receiver update state is unknown this boot, so nothing is sent to the receiver.' :
            j === 'waiting' ? JOURNAL_LINE.waiting + ' Keep the station powered.' : JOURNAL_LINE.unreadable;
        return pick('journal', 'Receiver update needs you', 'warn', lead,
          { actions: [go('Open', '#/settings/session')], alertId: loader ? 'receiver_in_loader' : null, notice: 'journal' });
      }
      if ((s.device && s.device.settings_available === false) || find('settings_degraded')) {
        return pick('settings', 'Settings cannot be saved', 'bad', 'The station runs, but changes are not saved this boot. ' +
          'Restart it; if that does not help, see Diagnostics.',
        { actions: [go('Diagnostics', '#/diag')], alertId: find('settings_degraded') ? 'settings_degraded' : null, notice: 'settings' });
      }
    }
    if (state === 'flashing' || state === 'flash_recovery' || (admin && rcv.journal === 'running')) {
      const st = state === 'flash_recovery' ? 'flash_recovery' : 'flashing';
      return pick('updating', HERO_TITLE[st], st === 'flashing' ? 'info' : 'warn', HERO_STATE_LINE[st],
        { actions: admin ? [go('Open', '#/settings/session')] : [] });
    }
    if (admin && rcv.uart_closed) {
      return pick('uart_closed', 'Receiver port closed', 'bad', UART_CLOSED_TEXT, { actions: [go('Details', '#/diag')], notice: 'uart_closed' });
    }
    const nr = find('no_receiver', 'station');
    if (nr || state === 'no_receiver') {
      if (!admin) return pick('no_receiver', HERO_TITLE.no_receiver, 'bad', HERO_STATE_LINE.no_receiver);
      const a = nr || { next_detect_in_s: rcv.next_detect_in_s };
      return pick('no_receiver', HERO_TITLE.no_receiver, 'bad', noReceiverText(a),
        { actions: [{ act: 'detect', alert: a, label: 'Check again' }], alertId: nr ? 'no_receiver' : null });
    }
    if (ladder && (ladder.id === 'receiver_power' || ladder.id === 'receiver_silent')) {
      return pick('rx_stopped', 'Receiver stopped sending data', 'bad',
        alertText(ladder)[1] + (alertsOnly ? ' Automatic recovery is off (your choice).' : ''), { alertId: ladder.id });
    }
    if (state === 'recovering' || (state === 'streaming' && hl.level === 'recovering')) {
      const step = admin ? HERO_STEP[hl.last_step] : null;
      return pick('rx_stopped', 'Receiver stopped sending data', 'warn', 'The station is fixing it' + (step ? ': ' + step : '') + '.');
    }
    if (ladder && ladder.id === 'no_satellites') {
      return pick('no_satellites', 'Antenna sees no satellites', 'bad', alertText(ladder)[1], { alertId: ladder.id });
    }
    if (ladder) {
      return pick('no_rtcm', 'Receiver sends no corrections', 'bad', alertText(ladder)[1] +
        (alertsOnly ? ' Automatic recovery is off (your choice).' : ''), { alertId: ladder.id });
    }
    const stopped = admin ? on.find(c => c.state === 'stopped') : null;
    if (stopped) {
      return pick('stopped', casterName(stopped) + ' stopped', 'bad', capFirst(lbl(L.stop, stopped.stop_reason || 'none')) +
        '. The station makes no new attempt until you act.',
      { actions: [go('What to do', '#/casters')], notice: 'onocoy_stopped:' + stopped.slot, subject: stopped.slot });
    }
    if (admin && rcv.rx_maintenance) {
      return pick('maintenance', 'Receiver maintenance', 'info', 'The station sends the receiver no commands.' +
        (rcv.maintenance_left_s ? ' Ends in about ' + upMin(rcv.maintenance_left_s) + '.' : ''),
      { actions: [{ act: 'end-maint', label: 'End now' }], notice: 'maintenance' });
    }
    const t = admin ? rcv.signal_test : null;
    if (t && t.state === 'deciding') {
      return pick('signal_test', 'Choose a signal group', 'warn', 'Both groups are measured. Keep one in Receiver settings' +
        (t.decide_left_s ? '; with no choice the old group comes back in about ' + sgtHours(t.decide_left_s) : '') + '.',
      { actions: [go('Compare and choose', '#/receiver/settings')], notice: 'signal_test' });
    }
    if (t && SGT_RUNNING.indexOf(t.state) >= 0) {
      return pick('signal_test', 'Comparing signal groups', 'info', sgtLine(Object.assign({}, t, { running_group: t.group })) +
        ' Other receiver changes wait until it ends.', { actions: [go('Details', '#/receiver/settings')], notice: 'signal_test' });
    }
    const net = (admin && s.network) || {};
    if (net.cred_trial) {
      return pick('wifi_trial', 'Trying new Wi-Fi', 'info', 'If it gets no address there, the station goes back to the previous ' +
        'network by itself.', { trial: net.pending_ssid || 'the new network', notice: 'wifi' });
    }
    if (admin && (rcv.first_apply_held || find('rx_held'))) {
      return pick('held', 'Receiver not configured yet', 'warn', HELD_TEXT,
        { actions: [go('Review and configure', '#/receiver')], alertId: 'rx_held', notice: 'held' });
    }
    if (state !== 'streaming') {
      return pick('receiver', HERO_TITLE[state] || lbl(L.station, state), 'info', HERO_STATE_LINE[state] || '');
    }
    if (onoSlot && onoSlot.onocoy_ready === false) {
      return pick('onocoy_setup', 'Onocoy not set up', 'warn', on.length ? 'Add your Onocoy credentials to stream to Onocoy too.' :
        'Add your Onocoy credentials to start streaming.', { actions: [go('Set up Onocoy', ONOCOY_SETUP_HASH, true)] });
    }
    if (!on.length) {
      if (!admin) return pick('no_caster', 'Not streaming', 'warn', 'The station is not streaming to any caster.');
      if (onoSlot) {
        return pick('no_caster', 'No caster turned on', 'warn', 'Onocoy is set up but turned off. Turn it on to start streaming.',
          { actions: [go('Turn Onocoy on', ONOCOY_SETUP_HASH, true)] });
      }
      if ((s.casters || []).some(c => !emptySlot(c))) {
        return pick('no_caster', 'No caster turned on', 'warn', 'Every caster is turned off. Turn one on to start streaming.',
          { actions: [go('Casters', '#/casters', true)] });
      }
      return pick('no_caster', 'No caster turned on', 'warn', 'Add a caster to start streaming.',
        { actions: [go('Add a caster', '#/casters', true)] });
    }
    if (down.length) {
      const head = streaming.length ? 'Partly streaming' : 'Not streaming';
      const tone = streaming.length ? 'warn' : 'bad';
      const more = admin ? { actions: [go('Casters', '#/casters')] } : {};
      const noNet = find('no_upstream');
      if (noNet) return pick('casters_down', head, tone, alertText(noNet)[1], Object.assign(more, { alertId: 'no_upstream' }));
      if (down.every(c => c.state === 'wait_data')) {
        /* The receiver delivers, yet the casters wait: an alert of the station's may say why. */
        for (const [id, label, href] of [['antenna_moved', 'Receiver', '#/receiver'], ['receiver_attention', 'Updates', '#/settings/updates'],
          ['config_failed', 'Receiver', '#/receiver']]) {
          const a = find(id, 'station');
          if (a) return pick('casters_down', head, tone, alertText(a)[1], { actions: [go(label, href)], alertId: id });
        }
        return pick('casters_down', head, tone, 'Waiting for corrections from the receiver.', more);
      }
      let lead;
      if (down.length === 1) {
        const c = down[0];
        lead = casterName(c) + ' is not streaming' + (admin ? ': ' + downReason(c) : '') + '.' +
          (admin && c.next_retry_s ? ' Next try in ' + fmt.dur(c.next_retry_s) + '.' : '');
        more.subject = c.slot;
      } else {
        lead = (down.length === 2 ? andList(down.map(casterName)) : down.length + ' casters') + ' are not streaming.';
      }
      return pick('casters_down', head, tone, lead, more);
    }
    if (onoStreaming && quality.any) {
      return pick('quality', quality.fewBands ? 'Too few bands for Onocoy' : 'No GLONASS for Onocoy', 'warn',
        qualityLead(quality, casterName(ono)), { actions: [go('Receiver', '#/receiver')],
          alertId: quality.fewBands && find('bands') ? 'bands' : null });
    }
    const names = on.map(casterName);
    return pick('streaming', 'Streaming', 'ok', (names.length > 3 ? names.length + ' casters' : andList(names)) +
      (names.length > 1 ? ' are' : ' is') + ' receiving corrections.');
  };

  const r = decide();
  const order = { bad: 0, warn: 1, info: 2 };
  r.also = alerts.filter(a => a.id !== r.alertId)
    .map(a => ({ id: a.id, title: alertText(a)[0], text: alertText(a)[1], tone: alertTone(a), href: ALERT_PAGE[a.id] || '#/diag' }))
    .sort((a, b) => (order[a.tone] || 0) - (order[b.tone] || 0));
  r.facts = [];
  /* The count adds nothing to a line that names every caster as streaming. */
  if (on.length > 1 && r.key !== 'streaming') r.facts.push(['Casters', streaming.length + ' of ' + on.length + ' streaming']);
  if (admin && hl.recovery_mode && hl.recovery_mode !== 'full') r.facts.push(['If corrections stop', lbl(L.recovery, hl.recovery_mode)]);
  /* While the receiver is being fixed or looked for: when the station restarts itself for it (full recovery only). */
  r.plan = admin && ['rx_stopped', 'no_receiver', 'no_satellites', 'no_rtcm'].indexOf(r.key) >= 0 ? restartPlanText(hl) : null;
  r.delivering = delivering;
  r.quality = quality;
  r.ono = ono;
  r.onoStreaming = onoStreaming;
  return r;
}

/* One line on a quality problem for the Onocoy verdict. */
function qualityShort(q) {
  if (q.fewBands) return 'Only ' + q.bands + ' bands: Onocoy\'s quality score needs 4.';
  if (q.noGlonass) return 'No GLONASS: Onocoy\'s quality score needs it.';
  return 'GLONASS does not count: the receiver refused message 1230.';
}

/**
 * Onocoy's day, while its slot counts (null otherwise): {verdict, tone, why,
 * left, down, allowed, known, used}. Missed while the station's streak_ok is
 * false. At risk while Onocoy does not stream now, more than 60 % of the
 * day's offline time is used, the current data rate is under 98 % after 10
 * minutes connected, or a quality minimum is missed. On track otherwise.
 * `why` names the cause in one line, the data rate only when it is the
 * cause, and nothing the top card already says. Every figure from the
 * station: today's offline time and what is left of it from the
 * availability ledger, kept across restarts.
 */
function onocoyDay(s, st) {
  const c = st.ono;
  if (!c) return null;
  const a = s.availability || {};
  const left = a.budget_left_s !== undefined ? a.budget_left_s : c.budget_left_s;
  const down = a.protected_down_today_s !== undefined ? a.protected_down_today_s : c.today_offline_s;
  const allowed = c.budget_s;
  const known = [left, down, allowed].every(v => v !== null && v !== undefined);
  const used = !known ? 0 : allowed > 0 ? Math.min(1, down / allowed) : 1;
  const d = { verdict: 'On track', tone: 'ok', why: null, left: left, down: down, allowed: allowed, known: known, used: used };
  if (c.streak_ok === false) {
    d.verdict = 'Missed';
    d.tone = 'bad';
    d.why = allowed === 0 ? 'Too many epochs were lost today to keep the streak.' : 'Today\'s offline time is over the budget.';
    return d;
  }
  const rate = c.data_rate_permille;
  const causes = [];
  if (!st.onoStreaming) causes.push('Offline time is counting now.');
  if (known && used > 0.6) causes.push('Most of today\'s offline time is used.');
  if (rate !== null && rate !== undefined && rate < 980 && (c.connected_s || 0) >= 600) {
    causes.push('Data rate ' + fmt.permille(rate) + ': lost epochs cut today\'s offline time.');
  }
  if (st.quality.any) causes.push(st.key === 'quality' ? null : qualityShort(st.quality));
  if (causes.length) {
    d.verdict = 'At risk';
    d.tone = 'warn';
    d.why = causes.find(Boolean) || null;
  }
  return d;
}
