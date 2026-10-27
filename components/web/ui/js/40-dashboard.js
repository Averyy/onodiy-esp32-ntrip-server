/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Status page: a summary that is always whole. The top card (title, one
 * line, its buttons, "Also:" lines), Sky across the page, Receiver and
 * Station, Onocoy and its optional report, NTRIP (beside Onocoy without
 * the report, across the page otherwise), then Performance. Each
 * card says its few facts once and has one link out, in its head, to the
 * page that manages them; nothing is edited here. All of it from
 * stationState() (15-state.js), so no card contradicts another.
 *
 * Patched into the page on every update with no element that has a
 * listener of its own (patch() would replace it, and the focus with it):
 * the buttons carry data-act and the page's root handles their clicks;
 * patchDetect() keeps the focus when "Check again" goes. The title is
 * announced when it changes. Visitors without a session see the top card,
 * the NTRIP servers by name and state (while the form lists any), and the
 * signals with the band table. The root is .status-page: without contact
 * every colour on it goes grey (app.css, body.no-contact).
 */

const DASH_SATELLITES_MS = 5000;

Views.dashboard = {
  title: 'Status',
  create(el) {
    el.classList.add('status-page');
    let gone = false, sig = null, live = null, satelliteError = '';
    let satelliteBusy = false, satelliteReadAt = 0;
    /* Status WebSocket messages contain no satellite detail. One read every
     * 5 s while visible: admin live data OR public signal totals, never both.
     * Visibility resumes a due read immediately instead of waiting for a
     * background-throttled timer. Guard both entry points against overlap. */
    const loadSatellites = async () => {
      const elapsed = Date.now() - satelliteReadAt;
      if (gone || doc.visibilityState === 'hidden' || satelliteBusy ||
          (satelliteReadAt && elapsed >= 0 && elapsed < DASH_SATELLITES_MS)) return;
      satelliteBusy = true;
      satelliteReadAt = Date.now();
      try {
        const admin = isAdmin();
        /* The visible poll retries next time; no second request can start
         * inside api() after the user has hidden or left this page. */
        const data = await api('GET', admin ? '/api/gnss/live' : '/api/gnss/signals', undefined, { once: true });
        if (gone || doc.visibilityState === 'hidden') return;
        if (!data || typeof data !== 'object') throw new Error('The satellite response was incomplete.');
        if (admin) live = data;
        else sig = data;
        satelliteError = '';
        render();
      } catch (e) {
        if (!gone && doc.visibilityState !== 'hidden') {
          satelliteError = errText(e);
          render();
        }
      } finally {
        satelliteBusy = false;
      }
    };
    const stopSatellites = poller(DASH_SATELLITES_MS, loadSatellites);
    const satellitesVisible = () => {
      if (doc.visibilityState !== 'hidden') loadSatellites();
    };
    doc.addEventListener('visibilitychange', satellitesVisible);
    /* The Performance card's lines (GET /api/monitor, admin only): its buckets close every 5 min, so read every 5 min;
     * once more when the first reading came after an empty answer (the page opened right after a restart). */
    let mon = null, monAgain = false;
    const loadMon = async () => {
      mon = await api('GET', '/api/monitor');
      render();
    };
    const stopMon = isAdmin() ? poller(300000, loadMon) : null;
    let report = null, reportKey = '';
    const loadReport = async () => {
      if (!onocoyReportEnabled(S.status)) return;
      const key = reportKey;
      const r = await api('GET', '/api/onocoy/report');
      if (key === reportKey) report = stampOnocoyReport(r);
      render();
    };
    /* Reads the station's cache; only the firmware's daily worker contacts Onocoy. */
    const stopReport = isAdmin() ? poller(15000, loadReport) : null;
    /* Open servers that stream while Onocoy does (GET /api/casters warnings): fetched when the turned-on servers change; the
     * top card says it in an "Also:" line. */
    let openSlots = [], openKey = '';
    const loadOpen = async () => {
      try {
        const d = await api('GET', '/api/casters');
        openSlots = openCasterSlots(d);
      } catch (e) {
        openSlots = [];
      }
      render();
    };
    /* el is this visit's own (openView): after the page is left, nothing draws into the next one. */
    const render = () => {
      const s = S.status;
      if (gone) return;
      if (!s) {
        patch(el, h('p', { class: 'loading' }, 'Waiting for the station…'));
        return;
      }
      const admin = isAdmin() && !s.public;
      const st = stationState(s, admin);
      if (admin) {
        const reportSlot = (s.casters || []).find(c => c.protected && c.enabled && c.onocoy_ready && c.onocoy_stats);
        const nextReportKey = reportSlot ? String(reportSlot.slot) : '';
        if (nextReportKey !== reportKey) {
          reportKey = nextReportKey;
          report = null;
          if (reportKey) loadReport().catch(() => {});
        }
        const on = enabledCasters(s);
        const key = on.some(c => c.protected) && on.length > 1 ? on.map(c => c.slot + ':' + c.host).join('|') : '';
        if (key !== openKey) {
          openKey = key;
          openSlots = [];
          if (key) loadOpen();
        }
      }
      if (admin && mon && !mon.buckets && s.monitor && s.monitor.span_s && !monAgain) {
        monAgain = true;
        loadMon().catch(() => {});
      }
      patchDetect(el, satelliteError ? msg('warn', 'Satellite details could not refresh; displayed values may be old. ' + satelliteError) : null,
        admin ? adminDashboard(s, st, openSlots, live, mon, report) : publicDashboard(s, st, sig));
    };
    detectClicks(el, render);
    dismissPowerClicks(el, render);
    endMaintClicks(el, render);
    leaveSafeClicks(el, render);
    el.addEventListener('click', async e => {
      const b = e.target?.closest?.('[data-act="refresh-onocoy"]');
      if (!b || b.disabled) return;
      b.disabled = true;
      try {
        report = stampOnocoyReport(await api('POST', '/api/onocoy/report/refresh', {}));
        render();
        setTimeout(() => loadReport().catch(() => {}), 1500);
      } catch (err) {
        toast(err.message || 'Could not refresh the Onocoy report.', 'warn');
        loadReport().catch(() => {});
      }
    });
    render();
    return {
      update: render,
      destroy() {
        gone = true;
        stopSatellites();
        doc.removeEventListener('visibilitychange', satellitesVisible);
        if (stopMon) stopMon();
        if (stopReport) stopReport();
      },
    };
  },
};

/* The top card's buttons: links, or buttons the page's root handles (data-act). */
function heroAction(a) {
  if (a.act === 'detect') return detectButton(a.alert);
  if (a.act) return h('button', { type: 'button', class: 'btn small', 'data-act': a.act }, a.label);
  return link(a.label, a.href, a.primary ? 'btn primary' : 'btn small');
}

/* The state's line (lead: another wording of it); a Wi-Fi trial names the network and counts down (tickTrialLeft). */
function stateLead(s, st, lead) {
  lead = lead || st.lead;
  return st.trial ? ['Trying ' + st.trial + ': ', trialLeftEl(s.network || {}), '. ', lead] : lead;
}

/* links: the pages the cards' links open; a button named after one of them (page) is left out, the card's link says it. */
function heroCard(s, st, also, links) {
  const acts = st.actions.filter(a => !(a.page && (links || []).indexOf(a.href) >= 0)).map(heroAction).filter(Boolean);
  also = also || st.also;
  /* The no-receiver plan line says whether a search runs: it takes the focus when "Check again" goes (patchDetect). */
  const noRx = st.key === 'no_receiver';
  const focusLine = on => (on ? { tabindex: '-1', 'data-detect-text': '' } : {});
  return h('section', { class: 'card hero ' + st.tone },
    h('h2', { class: 'state', 'aria-live': 'polite', 'aria-atomic': 'true' }, st.head),
    h('p', Object.assign({ class: 'line' }, focusLine(noRx && !st.plan)), stateLead(s, st)),
    acts.length ? h('div', { class: 'hero-actions form-actions' }, acts) : null,
    st.plan ? h('p', Object.assign({ class: 'muted small' }, focusLine(noRx)), st.plan) : null,
    also.length ? h('ul', { class: 'also' }, also.map(a => h('li', { class: a.tone },
      h('span', { class: 'ic', 'aria-hidden': 'true' }, a.tone === 'bad' ? '✕' : a.tone === 'info' ? 'i' : '!'),
      h('span', null, h('strong', null, 'Also: ' + a.title + '.'), a.text ? ' ' + a.text : ''),
      link('Details', a.href, 'btn small'),
      a.dismiss ? h('button', { type: 'button', class: 'btn small dismiss', 'data-act': a.dismiss, 'aria-label': 'Dismiss: ' + a.title }, '×') : null))) : null);
}

/* "Power was lost" dismissed from this page: its line stays hidden until the station drops the alert (within a second),
 * for at most 15 s. */
const POWER_DISMISS = { at: 0 };

/* The × on "Also: Power was lost." (POST /api/health/power-lost/dismiss): the power losses so far stop counting. */
function dismissPowerClicks(root, refresh) {
  root.addEventListener('click', async e => {
    const b = e.target && e.target.closest ? e.target.closest('[data-act="dismiss-power"]') : null;
    if (!b || !root.contains(b)) return;
    try {
      await busy(b, () => api('POST', '/api/health/power-lost/dismiss', {}));
      POWER_DISMISS.at = Date.now();
      setTimeout(() => {
        if (refresh && root.isConnected) refresh();
      }, 15500);
    } catch (err) {
      toast(errText(err), 'bad');
    }
    if (refresh && root.isConnected) refresh();
  });
}

/* The top card's "Also:" lines with the duplicate stream (open servers on beside Onocoy: GET /api/casters), in tone order. */
function heroAlso(s, st, openSlots) {
  const open = (openSlots || []).map(n => (s.casters || []).find(c => c.slot === n && !emptySlot(c))).filter(Boolean);
  const hide = Date.now() - POWER_DISMISS.at < 15000;
  const also = st.also.filter(a => !(hide && a.id === 'power_lost'))
    .map(a => (a.id === 'power_lost' ? Object.assign({}, a, { dismiss: 'dismiss-power' }) : a));
  if (!open.length) return also;
  return also.concat([{ id: 'dup_stream', title: 'Onocoy and ' + andList(open.map(casterName)) + ' are both on',
    text: 'A duplicate stream can get the Onocoy account banned.', tone: 'warn', href: '#/ntrip' }])
    .sort((a, b) => (TONE_ORDER[a.tone] || 0) - (TONE_ORDER[b.tone] || 0));
}

/*
 * A card's one link, in its head: want ({label, name, href}) where it
 * applies, else manage; manage too when the top card's button already goes
 * there. name: the link's accessible name, starting with its visible word.
 */
function headLink(st, want, manage) {
  const l = !want || st.actions.some(a => a.href === want.href) ? manage : want;
  return h('a', { href: l.href, class: 'head-link', 'aria-label': l.name }, l.label);
}

/* A card's one muted line when it has nothing of its own to say. */
const mutedLine = text => h('p', { class: 'muted' }, text);

/* The next 00:00 UTC in local time ("8:00 PM"). */
function utcResetTime() {
  const now = new Date();
  return new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() + 1))
    .toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
}

/*
 * Onocoy's day while it counts: the verdict, today's online share in large
 * type (most stations are online all day), and rows: the offline time left
 * before Onocoy's daily limit (amber once the verdict is not On track) and
 * the offline time today; once missed the verdict, one line with the reset
 * and the rows. Otherwise one muted line
 * on when the card shows, with the link that sets Onocoy up or turns it on
 * (Manage when the top card's button does).
 */
function onocoyCard(s, st) {
  const slot = onocoySlot(s);
  const manage = { label: 'Manage', name: 'Manage Onocoy', href: '#/ntrip' };
  const card = (chip, want, ...kids) => h('section', { class: 'card fit-kv' },
    cardHead('Onocoy', chip, headLink(st, want, manage)), kids);
  const d = st.key === 'safe_mode' ? null : onocoyDay(s, st);
  if (!d) {
    if (st.key === 'safe_mode') return card(null, null, mutedLine('Not available in safe mode.'));
    if (!slot || !slot.onocoy_ready) {
      return card(null, { label: 'Set up', name: 'Set up Onocoy', href: ONOCOY_SETUP_HASH }, mutedLine('Onocoy is not set up yet.'));
    }
    return card(null, { label: 'Turn on', name: 'Turn on Onocoy', href: ONOCOY_SETUP_HASH }, mutedLine('Onocoy is turned off.'));
  }
  const c = st.ono;
  const down = d.known ? d.down : c.today_offline_s;
  const leftText = d.left > 0 ? fmt.dur(d.left) : '0s';
  const rows = kv([
    ...(d.known && d.verdict !== 'Missed' ? [['Offline time left', d.tone === 'ok' ? leftText : h('span', { class: 't-' + d.tone }, leftText)]] : []),
    ['Online today', fmt.dur(c.today_online_s)], ['Offline today', fmt.dur(down)]]);
  if (d.verdict === 'Missed') {
    const at = utcResetTime();   /* "8:00 PM", or "8:00 p.m." in a locale that ends it with a period */
    return card(badge(d.verdict, d.tone), null,
      h('p', { class: 'small t-' + d.tone }, d.why + ' Resets at ' + at + (/\.$/.test(at) ? '' : '.')), rows);
  }
  return card(badge(d.verdict, d.tone), null,
    d.why ? h('p', { class: 'small t-' + d.tone }, d.why) : null,
    d.known ? [
      h('div', { class: 'big' }, (c.today_online_s || down) ? onlineShare(c.today_online_s || 0, down || 0) : DASH, ' ',
        h('small', null, 'online today')),
      rows,
      h('div', { class: 'muted small' }, 'Offline limit resets at ' + utcResetTime() + ' · Estimate')] :
      [h('p', { class: 'muted' }, 'Shows once the station has the time (from GNSS or the internet).'), rows]);
}

/* "Locked", "Surveying, 11 min left"... (position_locked is the stored profile's base mode: it runs once the receiver took the
 * profile: checked against its RTCM, or acknowledged and not checked yet without RTCM).
 * A survey needs data: while none arrives (delivering false) it is paused, not counting down, also once the station's window
 * ended (survey_unfinished: the receiver has not finished it). "Not locked" only for a position the station has
 * (position_known; an older firmware without it: as before). In maintenance with the receiver unchecked (rxUnchecked) the
 * station knows neither whether the profile runs nor whether it surveys: it says so. */
function positionText(r, delivering) {
  const unchecked = rxUnchecked(r.rx_maintenance, r.identity_confirmed);
  const surveying = r.surveying || r.survey_unfinished;
  if (r.position_locked) return r.profile_applied || r.profile_acknowledged || unchecked ? 'Locked' : 'Fixed, not applied yet';
  if (surveying && !delivering) return 'Surveying, paused';
  if (surveying) return 'Surveying' + (r.survey_window_left_s ? ', ' + inMinutes(r.survey_window_left_s) + ' left' : '');
  if (unchecked) return NOT_CHECKED;
  return r.position_known === false ? 'No position yet' : 'Not locked';
}

/*
 * The quality facts about Onocoy the page says elsewhere ('bands', 'glonass',
 * 'g1230'): the top card's line (key quality), Onocoy's verdict line (day.why)
 * and the bands "Also:" line. The Bands row says only the rest.
 */
function qualitySaid(st, day) {
  const q = st.quality;
  const said = [];
  const first = q.fewBands ? 'bands' : q.noGlonass ? 'glonass' : 'g1230';
  if (st.key === 'quality') {
    said.push(first);
    if (q.fewBands && q.noGlonass) said.push('glonass');
  }
  if (day && day.why && day.why === qualityShort(q)) said.push(first);
  if (st.also.some(a => a.id === 'bands')) said.push('bands');
  return said;
}

/*
 * The Signal row: the strength on L1, the band the antenna check judges
 * (receptionChecks; the mean over every band without L1's), as "Strong ·
 * 44 dB-Hz on L1", Fair or Weak in amber. While Onocoy counts, what its
 * quality score misses follows in amber, unless the page says it already
 * (qualitySaid). A dash until the receiver delivers. t: GET /api/gnss/live's
 * totals (the strength), missing until the page has read it.
 */
function signalView(st, day, t) {
  const q = st.quality;
  if (!q.known) return DASH;
  const l1 = t && t.bands && t.bands.l1 && t.bands.l1.cnr_dbhz;
  const c = l1 || (t && t.cnr_dbhz);
  let strength = null;
  if (c) {
    const word = c >= RX_CHECK.cnrGood ? 'Strong' : c >= RX_CHECK.cnrFair ? 'Fair' : 'Weak';
    const text = word + ' · ' + c + NB + 'dB\u2011Hz' + (l1 ? ' on L1' : '');
    strength = word === 'Strong' ? text : h('span', { class: 't-warn' }, text);
  }
  const notes = [];
  if (st.ono) {
    const said = qualitySaid(st, day);
    const tell = k => said.indexOf(k) < 0;
    if (q.fewBands && tell('bands')) notes.push(plural(q.bands, 'band', 'bands') + ', Onocoy needs 4');
    if (q.noGlonass && tell('glonass')) notes.push('no GLONASS');
    if (q.refused1230 && tell('g1230')) notes.push('GLONASS does not count');
  }
  if (!strength && !notes.length) return DASH;
  return [strength, ...notes.map((n, i) => h('span', { class: 't-warn' }, (strength || i ? ' · ' : '') + n))];
}

/* The corrections from the receiver: arriving (st.arriving: by the station's own freshness), none this boot, or none for how long. */
function correctionsText(age, arriving) {
  if (arriving) return 'Arriving';
  return typeof age === 'number' ? 'None for ' + fmt.dur(age / 1000) : 'None yet';
}

/* States in which the receiver's rows mean nothing (the top card says why): every row a dash. */
const RX_DASH_KEYS = ['safe_mode', 'journal', 'updating', 'uart_closed', 'no_receiver'];
/* The receiver's own states in which they mean nothing, whatever the top card says first (settings that cannot be saved). */
const RX_DASH_STATES = ['idle', 'detecting', 'no_receiver', 'flashing', 'flash_recovery'];

/* The receiver's rows mean nothing: a state the top card explains, or the receiver missing, updating or behind a closed port. */
function rxDashed(s, st) {
  const r = s.receiver || {};
  return RX_DASH_KEYS.indexOf(st.key) >= 0 || !!r.uart_closed || r.journal === 'running' ||
    (RX_DASH_STATES.indexOf(rxState(s)) >= 0 && !r.rx_maintenance) ||
    (s.alerts || []).some(a => a.id === 'no_receiver' && a.source === 'station');
}

/*
 * Three rows, always in this order: Signal, Corrections, Position (the Sky
 * card has the satellites and their bands). The signal only while the
 * receiver delivers and the corrections arrive (st.quality.known,
 * st.arriving: the station's 10 s), so it never sits beside no corrections.
 * No colour but a fair or weak signal, Onocoy's quality needs (signalView)
 * and a fixed position the station finds far from the receiver's own (amber,
 * as the top card or its "Also:" line says it): never green beside a red top
 * card. live: GET /api/gnss/live, as the Sky card has it.
 */
function receiverCard(s, st, lv) {
  const r = s.receiver || {};
  const dash = rxDashed(s, st);
  const q = st.quality;
  const live = !dash && q.known && st.arriving;
  const pos = dash ? DASH : positionText(r, st.delivering);
  const far = !dash && r.position_locked && (s.alerts || []).some(a => a.id === 'fixed_far' && a.source === 'station');
  return h('section', { class: 'card fit-kv' },
    cardHead('Receiver', headLink(st, null, { label: 'Manage', name: 'Manage the receiver', href: '#/receiver' })),
    kv([['Signal', live ? signalView(st, onocoyDay(s, st), lv && lv.totals) : DASH],
      ['Corrections', dash ? DASH : correctionsText(r.rtcm_age_ms, st.arriving)],
      ['Position', pos === NOT_CHECKED ? NOT_CHECKED_CHIP : far ? h('span', { class: 't-warn' }, 'Fixed, check it') : pos]]));
}

/** Today's online share: whole at 0 % and 100 %, else two decimals, rounded down (never "100.00" short of 100). */
function onlineShare(on, off) {
  const p = 100 * on / (on + off);
  return (p === 0 || p === 100 ? String(p) : (Math.floor(p * 100) / 100).toFixed(2)) + '%';
}

/*
 * One server in one line: name · state · today's share (admin, a server
 * that is on, not Onocoy while its own card shows its day). No why and no
 * button: the top card says why (its line or an "Also:" line). The state of
 * a server the top card's line names (st.named) in neutral words, not a
 * second coloured statement. Each separator stays with the part after it.
 */
function casterRow(c, admin, st) {
  const tone = (st.named || []).indexOf(c.slot) >= 0 ? '' : casterTone(c.state);
  const share = admin && casterOn(c) && !(st.ono && st.ono.slot === c.slot) && (c.today_online_s || c.today_offline_s) ?
    onlineShare(c.today_online_s || 0, c.today_offline_s || 0) + ' today' : null;
  return h('div', { class: 'cline' },
    h('div', null,
      h('span', { class: 'name' }, breakable(casterName(c))),
      ' ', h('span', { class: 'nowrap' }, '· ', h('span', { class: tone ? 't-' + tone : 'muted' }, lbl(L.caster, c.state))),
      share ? [' ', h('span', { class: 'muted nowrap' }, '· ' + share)] : null));
}

/* Your rover in one line, built like casterRow: "1 connected" in green, else its state in a word (not listening: red). */
function roverRow(lc) {
  const n = lc.clients || 0, word = lbl(L.rover, lc.state === 'off' ? 'starting' : lc.state);
  const tone = n ? 'ok' : lc.state === 'error' ? 'bad' : '';
  return h('div', { class: 'cline' }, h('div', null, h('span', { class: 'name' }, 'Local server'), ' ',
    h('span', { class: 'nowrap' }, '· ', h('span', { class: tone ? 't-' + tone : 'muted' },
      n ? n + ' connected' : word.charAt(0).toLowerCase() + word.slice(1)))));
}

/*
 * The NTRIP servers, one line each: for the admin every one set up, Onocoy
 * first while it counts and left out otherwise (its own card says it); "No
 * other servers." beside an Onocoy that is set up, else "No servers yet.",
 * with a link to add one; in safe mode when it shows. Your rover, while it is
 * on, one line after them (admin only). The public form: every server it
 * lists, no link (no tabs there); no card when it lists none (safe mode, none
 * set up: the top card says it).
 */
function castersCard(s, st, admin, wide) {
  const ono = admin ? onocoySlot(s) : null;
  const counting = admin && st.ono ? st.ono.slot : -1;
  const list = (s.casters || []).filter(c => !emptySlot(c) && (c !== ono || c.slot === counting))
    .sort((a, b) => (b.slot === counting) - (a.slot === counting) || a.slot - b.slot);
  const safe = st.key === 'safe_mode';
  if (!admin && (safe || !list.length)) return null;
  const add = !safe && !list.length ? { label: 'Add', name: 'Add a server', href: '#/ntrip' } : null;
  const none = safe ? 'Not available in safe mode.' : ono && ono.onocoy_ready !== false ? 'No other servers.' : 'No servers yet.';
  const lines = list.length && !safe ? list.map(c => casterRow(c, admin, st)) : [];
  const rover = admin && !safe && s.local_caster && s.local_caster.enabled ? roverRow(s.local_caster) : null;
  const body = [lines.length ? null : mutedLine(none), lines.length || rover ? h('div', { class: 'clines' }, lines, rover) : null];
  return h('section', { class: admin && !wide ? 'card' : 'card wide' },
    admin ? cardHead('NTRIP', headLink(st, add, { label: 'Manage', name: 'Manage NTRIP servers', href: '#/ntrip' })) : h('h2', null, 'NTRIP'),
    body);
}

/* Parts joined by " · ", each separator kept with the part after it: a narrow screen breaks before a separator, never after. */
function dotted(parts) {
  return parts.map((p, i) => (i ? [' ', h('span', { class: 'nowrap' }, '· ', p)] : p));
}

/* The Wi-Fi row: the network and its signal, the word with its figure ("Weak (-82 dBm)"), or why it is offline; during a
 * trial the signal only (the top card names the network). One line: a long network name ends in an ellipsis. */
function wifiView(n) {
  if (!n.ssid) return 'Not set up';
  const known = typeof n.rssi === 'number' && n.rssi < 0;
  if (n.cred_trial) return known ? rssiView(n.rssi) : DASH;
  const name = h('span', { class: 'ssid', title: n.ssid }, n.ssid);
  const then = !n.online ? (known ? 'no address yet' : 'Offline') : known ? rssiView(n.rssi) : null;
  return h('span', { class: 'wifi-line' }, name, then ? [' ', h('span', { class: 'nowrap' }, '· ', then)] : null);
}

/*
 * The Firmware row, in the Updates page's words (renderApp): the version
 * running, then what is offered ("v0.4.0 available", a receiver update) or
 * rolled back in its first minute; "up to date" only when the latest release
 * is the one running (never on a development build, whose version says
 * "+dev"). {parts, update}: update when the Updates page has something to do.
 */
function firmwareWords(dev, u) {
  const parts = [dev.version || DASH];
  const app = u.app_available && u.app_version && !(u.app_reverted_in_stage1 && u.app_reverted_version === u.app_version);
  const back = !app && u.app_reverted_in_stage1 && u.app_reverted_version;
  if (app) parts.push(u.app_version + ' available');
  if (back) parts.push(u.app_reverted_version + ' rolled back');
  if (u.gnss_available) parts.push(app ? 'receiver update' : 'receiver update available');
  const update = !!(app || back || u.gnss_available);
  if (!update && !dev.dev_build && u.app_version && u.app_version === dev.version) parts.push('up to date');
  return { parts: parts, update: update };
}

/* The station itself: its Wi-Fi, how long it runs, its firmware; Update while the Updates page has something. */
function stationCard(s, st) {
  const fw = firmwareWords(s.device || {}, s.update || {});
  return h('section', { class: 'card fit-kv' },
    cardHead('Station', headLink(st, fw.update ? { label: 'Update', name: 'Update the firmware', href: '#/settings/updates' } : null,
      { label: 'Manage', name: 'Manage station settings', href: '#/settings' })),
    kv([['Wi-Fi', wifiView(s.network || {})], ['Running for', fmt.dur(s.uptime_s)], ['Firmware', dotted(fw.parts)]]));
}

/*
 * Performance: the router's, internet's and server's ping times, the Wi-Fi signal,
 * CPU and free memory, each its latest reading (status `monitor`, a new one
 * every 30 s), its last 24 h in words, and a line over the same time (GET
 * /api/monitor, 5-min buckets, read by the page every 5 min while it is
 * shown): the newest at the right edge, red marks where a ping got no reply.
 */
const MON_DAY_S = 86400;

/* A line over the time the figures cover (the card's "Last ..."; at most 24 h): values oldest first (null = none) from
 * the left edge to the right; scaled to lo..hi (default: the values' own range; zero: from 0; span: at least this much
 * range, so a small jitter stays a flat line). A lone value is a dot. Its scale is written under it ("0 to 45ms": the
 * line's bottom and top; unit: after the figures, fmtv: how a figure is written). */
function sparkline(vals, label, o) {
  const H = 32, n = vals.length, W = Math.max(n - 1, 1);
  const x = i => (n === 1 ? W : i);
  const known = vals.filter(v => v !== null && v !== undefined);
  let lo = o.lo !== undefined ? o.lo : known.length ? Math.min(...known) : 0;
  const hi0 = o.hi !== undefined ? o.hi : known.length ? Math.max(...known) : 1;
  if (o.zero) lo = Math.min(lo, 0);
  const hi = Math.max(hi0, lo + (o.span || 1));
  const y = v => (H - 3 - (v - lo) / (hi - lo) * (H - 6)).toFixed(1);
  const has = i => i >= 0 && i < n && vals[i] !== null && vals[i] !== undefined;
  let d = '';
  for (let i = 0; i < n; i++) {
    if (!has(i)) continue;
    d += (has(i - 1) ? 'L' : 'M') + x(i) + ' ' + y(vals[i]);
    if (!has(i - 1) && !has(i + 1)) d += 'h0';
  }
  if (n === 1 && known.length) d = 'M0 ' + y(vals[0]) + 'H' + W;   /* one bucket so far: a flat line, not a dot at the edge */
  let loss = '';
  (o.lost || []).forEach((k, i) => {
    if (k) loss += 'M' + x(i) + ' 0V' + H;
  });
  const fv = o.fmtv || (v => String(Math.round(v)));
  return h('div', { class: 'spark-box' },
    svg('svg', { class: 'spark', viewBox: '0 0 ' + W + ' ' + H, preserveAspectRatio: 'none', role: 'img', 'aria-label': label },
      loss ? svg('path', { class: 'spark-loss', d: loss }) : null,
      d ? svg('path', { class: 'spark-line', d: d }) : null),
    h('span', { class: 'spark-scale muted' }, known.length ? fv(lo) + ' to ' + fv(hi) + (o.unit || '') : ''));
}

const pingMs = v => (v < 1 ? '<1ms' : fmt.ms(v));

function pingNow(p) {
  if (!p || p.state === 'none') return DASH;
  return p.state === 'lost' ? h('span', { class: 't-warn' }, 'No reply') : pingMs(p.ms);
}

function pingDay(p) {
  if (!p || !p.sent) return 'No readings yet';
  const lost = p.lost ? p.lost + ' lost (' + (100 * p.lost / p.sent).toFixed(1) + '%)' : 'none lost';
  if (p.avg_ms === null) return 'No replies · ' + lost;
  return dotted(['avg ' + pingMs(p.avg_ms), 'worst ' + pingMs(p.max_ms), lost]);
}

function perfRow(name, now, day, line) {
  return h('div', { class: 'perf-row' },
    h('span', { class: 'perf-text' }, h('span', { class: 'perf-name' }, name), ' ', h('span', { class: 'perf-day muted' }, day)),
    h('span', { class: 'perf-now' }, now), line);
}

function perfCard(s, st, mon) {
  const m = s.monitor;
  const span = m ? Math.min(m.span_s || 0, MON_DAY_S) : 0;
  const head = cardHead('Performance', null,
    headLink(st, null, { label: 'Details', name: 'Details on the Logs page', href: '#/logs' }));
  const wrap = (...kids) => h('section', { class: 'card wide perf-card' }, head, kids);
  if (st.key === 'safe_mode') return wrap(mutedLine('Not available in safe mode.'));
  if (!m || !m.running || !span) return wrap(mutedLine('Waiting for the first reading (every 30s).'));
  /* All lines share one time axis; buckets before the first reading of any figure are empty and left out. */
  const series = k => (mon && mon[k]) || [];
  const starts = ['router_avg', 'server_avg', 'internet_avg', 'rssi_avg', 'cpu_max', 'mem_min']
    .map(k => series(k).findIndex(v => v !== null && v !== undefined)).filter(i => i >= 0);
  const first = starts.length ? Math.min(...starts) : 0;
  const col = k => series(k).slice(first);
  const server = m.server.name ? m.server.name + ' ping' : 'Server ping';
  const r = m.rssi, c = m.cpu, mem = m.memory;
  return wrap(h('div', { class: 'perf' },
    perfRow('Router ping', pingNow(m.router), pingDay(m.router),
      sparkline(col('router_avg'), 'Router ping over the same time', { zero: true, span: 10, unit: 'ms', lost: col('router_lost') })),
    perfRow(server, pingNow(m.server), pingDay(m.server),
      sparkline(col('server_avg'), server + ' over the same time', { zero: true, span: 10, unit: 'ms', lost: col('server_lost') })),
    perfRow(h('span', { title: 'Cloudflare (' + (m.internet?.name || '1.1.1.1') + ')' }, 'Cloudflare ping'),
      pingNow(m.internet), pingDay(m.internet),
      sparkline(col('internet_avg'), 'Cloudflare ping over the same time', { zero: true, span: 10, unit: 'ms', lost: col('internet_lost') })),
    perfRow('Wi-Fi signal', r.now !== null ? rssiView(r.now, true) : DASH,
      r.avg !== null ? dotted(['avg ' + dbmText(r.avg), 'weakest ' + dbmText(r.min)]) : 'No readings yet',
      sparkline(col('rssi_avg'), 'Wi-Fi signal over the same time', { span: 10, unit: NB + 'dBm', fmtv: v => String(Math.round(v)).replace('-', '\u2212') })),
    perfRow('CPU', fmt.pct(c.now), c.avg !== null ? dotted(['avg ' + fmt.pct(c.avg), 'peak ' + fmt.pct(c.max)]) : 'No readings yet',
      sparkline(col('cpu_max'), 'CPU peaks over the same time', { lo: 0, hi: 100, unit: '%' })),
    perfRow('Free memory', fmt.bytes(mem.free), mem.min !== null ? 'lowest ' + fmt.bytes(mem.min) : 'No readings yet',
      sparkline(col('mem_min'), 'Free memory over the same time', { zero: true, fmtv: fmt.bytes }))),
    h('p', { class: 'muted small' }, span >= MON_DAY_S - 300 ? 'Calculated from the last 24h.' :
      'Calculated from the ' + fmt.dur(span) + ' since the station started.'));
}

/* Each constellation with satellites: its count, and a dot in its colour under every band it is tracked on (the count
 * on that band in the dot's title), a small grey one under the others. */
function skyGrid(t) {
  const bands = liveBands(t);
  const systems = (t.systems || []).filter(x => x.satellites > 0);
  return h('table', { class: 'sky-grid' },
    h('thead', null, h('tr', null, h('th', null, h('span', { class: 'sr-only' }, 'Constellation')), h('th', { class: 'num' }, 'Satellites'),
      bands.map(b => h('th', null, BAND_HEAD[b])))),
    h('tbody', null, systems.map(x => {
      const name = lbl(L.sys, x.sys);
      return h('tr', null,
        h('td', { class: 'name' }, h('span', { class: 'sky-name' }, h('i', { class: 'sys-' + x.sys }), name)),
        h('td', { class: 'num' }, fmt.num(x.satellites)),
        bands.map(b => {
          const n = (x.bands || {})[b] || 0;
          const what = name + ' ' + BAND_HEAD[b] + ': ' + (n ? n + ' satellite' + (n === 1 ? '' : 's') : 'not tracked');
          return h('td', null, h('i', { class: n ? 'band-dot sys-' + x.sys : 'band-dot off', role: 'img', 'aria-label': what, title: what }));
        }));
    })));
}

/*
 * The sky at a glance, a small copy of the Receiver page's Signals card: the
 * satellites on a sky plot without their names, in their constellation's
 * colour and fainter the weaker their signal (while the receiver reports
 * where they are), how many are tracked, and each constellation's count
 * with the bands it is tracked on (the signal strength is the Receiver
 * card's). From GET /api/gnss/live (read every 5 s by the page), only while
 * the corrections arrive, like the Receiver card's signal.
 */
function skyCard(s, st, live) {
  const t = live && live.totals;
  const head = cardHead('Sky', headLink(st, null, { label: 'Details', name: 'Details on the Receiver page', href: '#/receiver' }));
  if (rxDashed(s, st) || !st.arriving || !t || !t.tracked) return h('section', { class: 'card wide sky-card' }, head, mutedLine('No satellites tracked yet.'));
  const sats = live.satellites || [];
  return h('section', { class: 'card wide sky-card' }, head,
    h('div', { class: 'sky-glance' },
      sats.some(hasSkyPos) ? h('figure', { class: 'sky-fig' }, liveSkyPlot(sats, true)) : null,
      h('div', { class: 'sky-total' }, h('div', { class: 'big' }, fmt.num(t.tracked)), h('div', null, 'satellites')),
      skyGrid(t)));
}

/* Sky, Receiver and Station, Onocoy and its optional report, NTRIP, then Performance.
 * NTRIP fills a row when Onocoy already has the report beside it. */
function adminDashboard(s, st, openSlots, live, mon, report) {
  const hasReport = onocoyReportEnabled(s);
  const cards = [skyCard(s, st, live), receiverCard(s, st, live), stationCard(s, st), onocoyCard(s, st),
    hasReport ? onocoyReportCard(report) : null, castersCard(s, st, true, hasReport),
    perfCard(s, st, mon)];
  const links = cards.filter(Boolean).map(c => c.querySelector('.head-link')).filter(Boolean).map(a => a.getAttribute('href'));
  return [heroCard(s, st, heroAlso(s, st, openSlots), links), h('div', { class: 'grid' }, cards)];
}

/* The public page: the top card, the NTRIP servers while the form lists any, and the signals while the receiver delivers them. */
function publicDashboard(s, st, sig) {
  const sigCard = st.delivering && sig && (sig.systems || []).length ? signalsCard(sig) : null;
  const cards = [castersCard(s, st, false), sigCard].filter(Boolean);
  return [heroCard(s, st), cards.length ? h('div', { class: 'grid' }, cards) : null];
}

/*
 * "Check again" (POST /api/gnss/detect) on pages rebuilt every refresh
 * (patchDetect()): no listener of its own, so patch() keeps it and the
 * focus; the root handles its click (detectClicks). It reads "Searching"
 * until the station says it searches; then it goes, and the focus moves to
 * the line that says so.
 */
const DETECT = { askedAt: 0, busy: false };

const detectWaiting = () => DETECT.busy || Date.now() - DETECT.askedAt < 4000;

function detectButton(a) {
  if (a.next_detect_in_s !== null && a.next_detect_in_s !== undefined && a.next_detect_in_s <= 0) return null;   /* searching now */
  /* aria-disabled, not disabled: a disabled button would drop the keyboard focus. */
  const waiting = detectWaiting();
  return h('button', { type: 'button', class: 'btn small', 'data-act': 'detect', 'aria-disabled': waiting ? 'true' : null },
    waiting ? 'Searching' : 'Check again');
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

/** Handle "Leave safe mode" (data-act="leave-safe") inside root. */
function leaveSafeClicks(root, refresh) {
  root.addEventListener('click', async e => {
    const b = e.target && e.target.closest ? e.target.closest('[data-act="leave-safe"]') : null;
    if (!b || !root.contains(b)) return;
    await leaveSafeMode(b);
    if (refresh && root.isConnected) refresh();
  });
}

/**
 * The alerts (station and health) as message boxes, as Logs' Overview shows
 * them. opts.detect: the no-receiver alert offers "Check again" (the page
 * handles it with detectClicks).
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
