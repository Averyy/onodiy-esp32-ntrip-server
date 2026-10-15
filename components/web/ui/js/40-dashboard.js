/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Status page: the top card (a title, one line with the most important
 * thing and what to do, its button, "Also:" lines for other alerts and a few
 * labelled facts), Onocoy's day (while its slot counts), three receiver rows
 * and, once a second caster is set up, the other casters. Everything comes
 * from stationState() (15-state.js), so no card contradicts another; the
 * browser tab's title follows the top card ("Not streaming · onocoydiy").
 *
 * Built from the status model on every update and patched into the page (no
 * inputs, and no element with a listener of its own: patch() replaces such
 * an element every time, which would take the keyboard focus with it), so
 * links and buttons keep the keyboard focus across the 1 s updates. The
 * buttons ("Check again", "End now", "Leave safe mode") carry data-act and
 * the page's root handles their clicks; patchDetect() keeps the focus on the
 * line when "Check again" goes. The title is announced to screen readers
 * when it changes. Visitors without a session see the public page: the top
 * card, the casters by name and state, and the signals with the band table.
 */

Views.dashboard = {
  title: 'Status',
  create(el) {
    /* The public page's signals (GET /api/gnss/signals, open to visitors while public status is on). */
    let sig = null;
    const stopSig = isAdmin() ? null : poller(5000, async () => {
      sig = await api('GET', '/api/gnss/signals');
      render();
    });
    /* Open casters that stream while Onocoy does (GET /api/casters warnings): fetched when the turned-on casters change. */
    let openSlots = [], openKey = '';
    const loadOpen = async () => {
      try {
        const d = await api('GET', '/api/casters');
        openSlots = openCasterSlots(d);
      } catch (e) {
        openSlots = [];
      }
      if (el.isConnected) render();
    };
    const render = () => {
      const s = S.status;
      if (!s) {
        patch(el, h('p', { class: 'loading' }, 'Waiting for the station…'));
        return;
      }
      const admin = isAdmin() && !s.public;
      const st = stationState(s, admin);
      if (admin) {
        const on = enabledCasters(s);
        const key = on.some(c => c.protected) && on.length > 1 ? on.map(c => c.slot + ':' + c.host).join('|') : '';
        if (key !== openKey) {
          openKey = key;
          openSlots = [];
          if (key) loadOpen();
        }
      }
      patchDetect(el, admin ? adminDashboard(s, st, openSlots) : publicDashboard(s, st, sig));
    };
    detectClicks(el, render);
    endMaintClicks(el, render);
    leaveSafeClicks(el, render);
    render();
    return {
      update: render,
      destroy() {
        if (stopSig) stopSig();
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

function heroCard(s, st) {
  const acts = st.actions.map(heroAction).filter(Boolean);
  /* The no-receiver line says whether a search runs: it takes the focus when "Check again" goes (patchDetect). */
  const noRx = st.key === 'no_receiver';
  const lead = st.trial ? ['Trying ' + st.trial + ': ', trialLeftEl(s.network || {}), '. ', st.lead] : st.lead;
  return h('section', { class: 'card hero ' + st.tone },
    h('h2', { class: 'state', 'aria-live': 'polite', 'aria-atomic': 'true' }, st.head),
    h('p', { class: 'line', tabindex: noRx ? '-1' : null, 'data-detect-text': noRx ? '' : null }, lead),
    acts.length ? h('div', { class: 'hero-actions form-actions' }, acts) : null,
    st.plan ? h('p', { class: 'muted small' }, st.plan) : null,
    st.also.length ? h('ul', { class: 'also' }, st.also.map(a => h('li', { class: a.tone },
      h('span', { class: 'ic', 'aria-hidden': 'true' }, a.tone === 'bad' ? '✕' : a.tone === 'info' ? 'i' : '!'),
      h('span', null, h('strong', null, 'Also: ' + a.title + '.'), a.text ? ' ' + a.text : ''),
      link('Details', a.href, 'btn small')))) : null,
    st.facts.length ? h('dl', { class: 'facts' }, st.facts.map(f => h('div', null, h('dt', null, f[0]), h('dd', null, f[1])))) : null);
}

/* The next 00:00 UTC in local time ("8:00 PM"). */
function utcResetTime() {
  const now = new Date();
  return new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() + 1))
    .toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
}

/* Onocoy's day: the verdict, the offline time left in large type with its bar, and two rows. */
function onocoyCard(s, st) {
  const d = onocoyDay(s, st);
  if (!d) return null;
  const c = st.ono;
  return h('section', { class: 'card fit-kv' },
    cardHead('Onocoy today', badge(d.verdict, d.tone)),
    d.why ? h('p', { class: 'small t-' + d.tone }, d.why) : null,
    d.known ? [
      h('div', { class: 'big t-' + d.tone }, d.left > 0 ? fmt.dur(d.left) : '0' + NB + 's', ' ', h('small', null, 'offline time left')),
      bar(d.used, d.tone, 'Share of today\'s offline time used'),
      h('div', { class: 'muted small' }, 'Resets at ' + utcResetTime() + ' · Estimate')] :
      h('p', { class: 'muted' }, 'Shows once the station has the time (from GNSS or the internet).'),
    kv([['Online today', fmt.dur(c.today_online_s)], ['Offline today', fmt.dur(d.known ? d.down : c.today_offline_s)]]));
}

/* "Locked", "Surveying, 11 min left"... (position_locked is the stored profile's base mode: it runs once the profile is applied). */
function positionText(r) {
  if (r.position_locked) return r.profile_applied ? 'Locked' : 'Fixed, not applied yet';
  if (r.surveying) return 'Surveying' + (r.survey_window_left_s ? ', ' + inMinutes(r.survey_window_left_s) + ' left' : '');
  return 'Not locked';
}

const okMark = ok => h('span', { class: ok ? 't-ok' : 't-warn' }, ok ? '✓' : '✕');

/* The band row: "4 bands on Galileo ✓ (the most this receiver can reach)"; a dash while the receiver does not deliver. */
function bandsView(r, q) {
  if (!q.known) return DASH;
  const ok = !q.fewBands;
  const note = !ok ? 'Onocoy needs 4.' : /UM982/.test(r.model || '') ? 'The most this receiver can reach.' : null;
  return [q.bands + ' bands' + (q.sys ? ' on ' + lbl(L.sys, q.sys) : ''), ' ', okMark(ok),
    note ? h('div', { class: 'muted small' }, note) : null];
}

/* GLONASS: ticked only when it is tracked and the receiver accepted message 1230. */
function glonassView(r, q) {
  if (!q.known) return DASH;
  if (q.noGlonass) return ['Not tracked ', okMark(false)];
  if (q.refused1230) return ['Message 1230 refused ', okMark(false)];
  return r.rtcm1230_supported === true ? ['Tracked ', okMark(true)] : 'Tracked';
}

/* Three receiver rows, no buttons, and a link to the page with the rest. */
function receiverCard(s, st) {
  const r = s.receiver || {};
  return h('section', { class: 'card fit-kv' },
    cardHead('Receiver', link('Details', '#/receiver', 'head-link')),
    kv([['Bands', bandsView(r, st.quality)], ['GLONASS', glonassView(r, st.quality)], ['Position', positionText(r)]]));
}

/** Today's online share: whole at 0 % and 100 %, else two decimals, rounded down (never "100.00" short of 100). */
function onlineShare(on, off) {
  const p = 100 * on / (on + off);
  return (p === 0 || p === 100 ? String(p) : (Math.floor(p * 100) / 100).toFixed(2)) + NB + '%';
}

/* Why a caster that is on does not stream, when the state's name does not say it: the station's hint or the last failure. */
function casterWhy(c) {
  if (c.state === 'stopped') return capFirst(lbl(L.stop, c.stop_reason || 'none')) + '. No new attempt until you act.';
  if (c.state === 'wait_data') return null;
  const fail = c.reason && c.reason !== 'none' ? lbl(L.fail, c.reason) : '';
  const why = c.hint ? capFirst(c.hint) : fail !== lbl(L.caster, c.state) ? fail : '';
  const next = c.next_retry_s ? 'Next try in ' + fmt.dur(c.next_retry_s) + '.' : '';
  return [why ? why.replace(/\.?$/, '.') : '', next].filter(Boolean).join(' ') || null;
}

/*
 * One caster in one line: name · state · today's online share; why and a
 * button when it is on but does not stream (neither again when the top card
 * says it, with its own button: `subject`).
 */
function casterRow(c, admin, subject) {
  const off = c.enabled === false || c.state === 'disabled';
  const tone = casterTone(c.state);
  const share = admin && !off && (c.today_online_s || c.today_offline_s) ?
    onlineShare(c.today_online_s || 0, c.today_offline_s || 0) + ' today' : null;
  const trouble = admin && !off && c.state !== 'streaming' && c.slot !== subject;
  const why = trouble ? casterWhy(c) : null;
  return h('div', { class: 'cline' },
    h('div', null,
      h('span', { class: 'name' }, casterName(c)), ' · ', h('span', { class: tone ? 't-' + tone : 'muted' }, lbl(L.caster, c.state)),
      share ? h('span', { class: 'muted' }, ' · ' + share) : null,
      why ? h('div', { class: 'muted small' }, why) : null),
    trouble ? link(c.state === 'stopped' ? 'What to do' : 'Details', '#/casters', 'btn small') : null);
}

/*
 * The casters besides Onocoy (the public form: every caster), one line
 * each, only once one is set up; with the duplicate-stream line while an
 * open caster streams together with Onocoy.
 */
function castersCard(s, st, admin, openSlots) {
  const ono = admin ? onocoySlot(s) : null;
  const list = (s.casters || []).filter(c => !emptySlot(c) && c !== ono);
  if (!list.length) return null;
  const open = (openSlots || []).map(n => list.find(c => c.slot === n)).filter(Boolean);
  return h('section', { class: 'card' + (admin && st.ono ? ' wide' : '') },
    h('h2', null, admin && ono && ono.onocoy_ready ? 'Other casters' : 'Casters'),
    h('div', { class: 'clines' }, list.map(c => casterRow(c, admin, st.subject))),
    open.length ? msg('warn', h('div', { class: 'cline' }, h('div', null,
      h('strong', null, 'Onocoy and ' + andList(open.map(casterName)) + ' are both on.'),
      ' A duplicate stream can get the Onocoy account banned.'), link('Details', '#/casters', 'btn small'))) : null);
}

/* Order: the top card; Onocoy's day beside the receiver; the other casters under both. */
function adminDashboard(s, st, openSlots) {
  const ono = onocoyCard(s, st);
  const others = castersCard(s, st, true, openSlots);
  const rx = receiverCard(s, st);
  /* A lone receiver card takes the row (no empty cell beside it). */
  if (!ono && !others) rx.classList.add('wide');
  return [heroCard(s, st), h('div', { class: 'grid' }, ono, rx, others)];
}

/* The public page: the top card, the casters, and the signals while the receiver delivers them. */
function publicDashboard(s, st, sig) {
  const casters = castersCard(s, st, false, null);
  const sigCard = st.delivering && sig && (sig.systems || []).length ? signalsCard(sig) : null;
  if (casters) casters.classList.add('wide');
  return [heroCard(s, st), h('div', { class: 'grid' }, casters, sigCard)];
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
 * The alerts (station and health) as message boxes, as Diagnostics shows
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
