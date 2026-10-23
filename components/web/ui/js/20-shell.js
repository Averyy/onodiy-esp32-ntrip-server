/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App shell: header (with the station's state), the five tabs, hash router
 * (#/ntrip, #/logs/console, ...; old addresses redirect), page-wide
 * notices (one line each), dialogs, the restart overlay and the colour theme.
 *
 * A view is {title, admin, heading(arg)?, create(el, arg) -> {update(status)?,
 * destroy()?, dirty()?, reload()?}}. update() runs on every status change;
 * reload() when the settings changed elsewhere (unless dirty() says a form is
 * being edited). heading(arg): [h1 text, shown] for a sub-page.
 */

const Views = {};
const NAV = [['', 'Status'], ['ntrip', 'NTRIP'], ['receiver', 'Receiver'], ['logs', 'Logs'], ['settings', 'Settings']];
/* The view that draws an address whose name is not its key. */
const VIEW_KEY = { '': 'dashboard' };

function viewOf(name) {
  return Views[VIEW_KEY[name] || name];
}

/*
 * Old addresses (the seven tabs, Diagnostics and its parts) and where that
 * content is now: #/signals/live opens #/receiver/live; Casters is NTRIP
 * (#/casters/onocoy: #/ntrip/onocoy); Diagnostics is Logs, its History (and
 * the journal and availability before it) Logs' History, its Logs Logs'
 * Console, its tasks and core dump (and anything else) the Overview.
 */
function movedTo(name, arg) {
  if (name === 'dashboard') return '';
  if (name === 'updates') return 'settings/' + (arg || 'updates');
  if (name === 'diag') {
    return 'logs' + ({ history: '/history', availability: '/history', journal: '/history', logs: '/console' }[(arg || '').split('/')[0]] || '');
  }
  const to = { signals: 'receiver', live: 'receiver/live', system: 'settings/wifi', casters: 'ntrip' }[name];
  return to ? to + (arg ? '/' + arg : '') : null;
}

const shell = {
  current: null,     /* {name, arg, view} */
  started: false,
  noticeKey: '',
  navKey: '',
  title: '',
  noticesOpen: false,
};

/* Where a page goes: inside <main>, after its heading (the page's h1, set by openView). */
function viewEl() {
  return doc.getElementById('view-body');
}

/* ---- Theme ------------------------------------------------------------- */

function applyTheme() {
  const t = store.get('theme', 'auto');
  if (t === 'light' || t === 'dark') doc.documentElement.setAttribute('data-theme', t);
  else doc.documentElement.removeAttribute('data-theme');
}

function setTheme(t) {
  store.set('theme', t === 'auto' ? null : t);
  applyTheme();
}

function toggleTheme() {
  const dark = doc.documentElement.getAttribute('data-theme') === 'dark' ||
    (!doc.documentElement.hasAttribute('data-theme') && window.matchMedia('(prefers-color-scheme: dark)').matches);
  setTheme(dark ? 'light' : 'dark');
}

/* ---- Header ------------------------------------------------------------ */

function setBrand(name) {
  doc.getElementById('brand-name').textContent = name || 'Station';
}

function renderTopActions() {
  const box = doc.getElementById('top-actions');
  const themeBtn = h('button', { type: 'button', class: 'icon-btn', title: 'Light or dark', 'aria-label': 'Switch between light and dark',
    onclick: toggleTheme }, '◐');
  if (isAdmin()) {
    mount(box, themeBtn, h('button', { type: 'button', class: 'btn small ghost', onclick: logout }, 'Logout'));
  } else if (S.publicView) {
    mount(box, themeBtn, h('button', { type: 'button', class: 'btn small', onclick: () => showLogin() }, 'Login'));
  } else {
    mount(box, themeBtn);
  }
}

/* Seconds without a status before the header says "No contact" (the station sends one every second; a poll every 2 s). */
const CONTACT_S = 6;

/* The header's count without contact: seconds for the first minute, then its largest unit ("12 min", "3 h"), so it fits a phone. */
function contactAge(s) {
  if (s < 60) return fmt.dur(s);
  return s < 3600 ? Math.floor(s / 60) + 'min' : s < 86400 ? Math.floor(s / 3600) + 'h' : Math.floor(s / 86400) + 'd';
}

/*
 * The header's indicator: the station's state as the Status page's top card
 * says it (stationState, the public form for a visitor), a dot in its colour
 * and a word (stateWord); its title on hover and in its name, which starts
 * with the word; a tap opens the Status page. Without a status for CONTACT_S
 * it says "No contact" and counts the time (contactAge; the whole time on
 * hover), grey (the station's state is not known; the Status page's colours
 * go grey too, body.no-contact), and "Connecting…" before the first. It
 * shows on every page, the Status page too. The word is a
 * polite live region, so a screen reader hears it once when it changes; the
 * running count sits beside it, outside the region (and hidden from
 * assistive technology), or it would be read out every second. The tooltip
 * sits inside the link, so the link's name (aria-label) is not read again as
 * its description.
 */
function renderLive() {
  const el = doc.getElementById('live');
  if (!shell.started) {
    clear(el);
    el.hidden = false;
    doc.body.classList.remove('no-contact');
    return;
  }
  const s = S.status;
  const age = S.statusAt ? (Date.now() - S.statusAt) / 1000 : null;
  const lost = !!s && age !== null && age >= CONTACT_S;
  let tone = '', text, title, count = '';
  if (!s || age === null) {
    text = 'Connecting…';
    title = 'Connecting to the station';
  } else if (lost) {
    text = 'No contact';
    count = contactAge(age);
    title = 'No contact with the station for ' + fmt.dur(age);
  } else {
    const st = stationState(s, isAdmin() && !s.public);
    tone = st.tone;
    text = stateWord(st);
    title = st.head;
  }
  doc.body.classList.toggle('no-contact', lost);
  let a = el.querySelector('a');
  if (!a) {
    a = h('a', { href: '#/' }, h('span', { class: 'in' }, h('span', { class: 'word', role: 'status' }),
      h('span', { class: 'age', 'aria-hidden': 'true' })));
    mount(el, a);
  }
  const box = a.querySelector('.in'), word = a.querySelector('.word'), ageEl = a.querySelector('.age');
  const name = count ? 'No contact with the station' :
    title.toLowerCase().indexOf(text.replace(/…$/, '').toLowerCase()) === 0 ? title : text + ': ' + title;
  a.className = tone;
  if (box.getAttribute('title') !== title) box.setAttribute('title', title);
  if (a.getAttribute('aria-label') !== name) a.setAttribute('aria-label', name);
  if (word.textContent !== text) word.textContent = text;
  if (ageEl.textContent !== count) ageEl.textContent = count;
}

function renderNav() {
  const nav = doc.getElementById('tabs');
  const admin = isAdmin();
  const name = shell.current ? shell.current.name : '';
  const u = (S.status || {}).update || {};
  const dot = !!(u.app_available || u.gnss_available || u.app_reverted_in_stage1);
  /* The public view is one page: no tabs. */
  nav.hidden = !admin;
  /* Rebuild only on a change, so a tap on a tab is never lost to a refresh. */
  const key = [name, admin, dot].join('|');
  if (key === shell.navKey && nav.firstChild) return;
  shell.navKey = key;
  mount(nav, admin ? NAV.map(n => h('a', { href: '#/' + n[0], class: name === n[0] ? 'active' : null, 'aria-current': name === n[0] ? 'page' : null },
    n[1], dot && n[0] === 'settings' ? h('span', { class: 'dot', role: 'img', 'aria-label': 'update available' }) : null)) : null);
  navShowActive();
}

/* On a phone the tabs scroll sideways: the active one is always in view,
 * and a fade at an edge says that more tabs lie that way. */
function navShowActive() {
  const nav = doc.getElementById('tabs');
  const a = nav.querySelector('a.active');
  if (a) {
    const left = a.offsetLeft - nav.offsetLeft;
    if (left < nav.scrollLeft + 24 || left + a.offsetWidth > nav.scrollLeft + nav.clientWidth - 24) {
      nav.scrollLeft = Math.max(0, left - (nav.clientWidth - a.offsetWidth) / 2);
    }
  }
  navEdges();
}

function navEdges() {
  edgeFade(doc.getElementById('tabs'));
}

/* The tabs when they do not fit: a fade at each edge where more of them lie. */
function edgeFade(el) {
  const more = el.scrollWidth - el.clientWidth;
  el.classList.toggle('more-left', more > 2 && el.scrollLeft > 2);
  el.classList.toggle('more-right', more > 2 && el.scrollLeft < more - 2);
}

doc.getElementById('tabs').addEventListener('scroll', navEdges, { passive: true });
window.addEventListener('resize', () => {
  if (shell.started) navShowActive();
});

/* The skip link jumps to the page's content (the address keeps its route). */
doc.getElementById('skip').addEventListener('click', e => {
  e.preventDefault();
  doc.getElementById('view').focus();
});

/* ---- Router ------------------------------------------------------------ */

function parseHash() {
  const parts = location.hash.replace(/^#\/?/, '').split('/');
  return { name: parts[0] || '', arg: parts.slice(1).join('/') };
}

function route() {
  if (!shell.started) return;
  let r = parseHash();
  const to = movedTo(r.name, r.arg);
  if (to !== null) {
    history.replaceState(null, '', '#/' + to);
    r = parseHash();
  }
  /* A visitor without a session sees the one public page. */
  if (!viewOf(r.name) || (!isAdmin() && r.name)) r = { name: '', arg: '' };
  const cur = shell.current;
  if (cur && cur.name === r.name && cur.arg === r.arg) return;
  if (cur && cur.view.dirty && cur.view.dirty() && !window.confirm('Leave this page? Unsaved changes are lost.')) {
    history.replaceState(null, '', '#/' + cur.name + (cur.arg ? '/' + cur.arg : ''));
    return;
  }
  openView(r.name, r.arg);
}

function openView(name, arg) {
  const cur = shell.current;
  if (cur && cur.view.destroy) {
    try {
      cur.view.destroy();
    } catch (e) {
      console.error(e);
    }
  }
  const def = viewOf(name);
  /* Each visit draws into its own element: an answer that arrives after the page was left lands nowhere. */
  const el = h('div', { class: 'page-root' });
  mount(viewEl(), el);
  shell.current = { name: name, arg: arg, view: {} };
  try {
    shell.current.view = def.create(el, arg) || {};
  } catch (e) {
    console.error(e);
    mount(el, msg('bad', 'This page could not be shown: ' + e.message));
  }
  /* The page's one h1: read out only, or shown as the page's heading where the view has no title card. */
  const t = (def.heading && def.heading(arg)) || [def.title, def.showTitle];
  const head = doc.getElementById('page-title');
  head.textContent = shell.title = t[0];
  head.className = t[1] ? 'page-title' : 'sr-only';
  head.hidden = false;
  setDocTitle();
  renderNav();
  renderNotices();
  window.scrollTo(0, 0);
}

/* The browser tab's title follows the Status page's top card on every page ("Not streaming · onodiy"). */
function setDocTitle() {
  const s = S.status;
  const host = s && s.device ? s.device.hostname : '';
  const t = (s ? stationState(s, isAdmin() && !s.public).head : shell.title) + (host ? ' · ' + host : '');
  if (doc.title !== t) doc.title = t;
}

function startShell() {
  shell.started = true;
  doc.body.classList.remove('bare');
  renderTopActions();
  renderLive();
  renderNav();
  shell.current = null;
  route();
}

function stopShell() {
  if (shell.current && shell.current.view.destroy) {
    try {
      shell.current.view.destroy();
    } catch (e) {
      console.error(e);
    }
  }
  shell.current = null;
  shell.started = false;
  shell.noticeKey = '';
  doc.getElementById('page-title').hidden = true;   /* bare pages carry their own heading */
  doc.title = doc.getElementById('brand-name').textContent;
  doc.body.classList.add('bare');
  clear(doc.getElementById('tabs'));
  clear(doc.getElementById('notices'));
  renderLive();
}

/** Show a page outside the app (login, setup). */
function bare(...kids) {
  if (openDialogClose) openDialogClose(false);   /* it would keep the new page inert */
  stopShell();
  renderTopActions();
  mount(viewEl(), kids);
  window.scrollTo(0, 0);
}

window.addEventListener('hashchange', route);

/*
 * Live updates rebuild parts of the page every second. While a finger or
 * button is down they wait, so a tap that spans a refresh still lands on
 * the element it started on (the click fires before the deferred update).
 */
const press = { down: false, since: 0, pending: false };

function refreshView() {
  if (!shell.started) return;
  if (press.down && Date.now() - press.since < 3000) {
    press.pending = true;
    return;
  }
  setDocTitle();
  renderNotices();
  renderNav();
  const v = shell.current && shell.current.view;
  if (v && v.update) v.update(S.status);
}

doc.addEventListener('pointerdown', () => {
  press.down = true;
  press.since = Date.now();
}, true);
const pressEnd = () => {
  press.down = false;
  if (press.pending) {
    press.pending = false;
    refreshView();
  }
};
doc.addEventListener('pointerup', () => setTimeout(pressEnd, 0), true);
doc.addEventListener('pointercancel', pressEnd, true);
window.addEventListener('blur', pressEnd);

bus.on('status', st => {
  if (st && st.device && st.device.hostname) setBrand(st.device.hostname);
  renderLive();
  refreshView();
});
setInterval(() => {
  renderLive();
  tickTrialLeft();
}, 1000);

/*
 * The station's settings_changed: the open page reloads its data, or, while
 * a form is being edited, says that someone else changed the settings (this
 * tab's own save it knows already). One change can come as a few events in a
 * row (a batch, then a key erased): they wait SETTINGS_SETTLE_MS for each
 * other and count once, one reload, one warning, for the page the last one
 * came on (a page opened meanwhile may have read its data before it: at
 * most one reload more).
 */
const SETTINGS_SETTLE_MS = 300;
const settingsWait = { timer: null, page: null, other: false };

function settingsChanged(m) {
  if (!(m.client && m.client === CLIENT_ID)) settingsWait.other = true;
  settingsWait.page = shell.current;
  if (settingsWait.timer) return;
  settingsWait.timer = setTimeout(() => {
    const page = settingsWait.page, other = settingsWait.other;
    settingsWait.timer = null;
    settingsWait.page = null;
    settingsWait.other = false;
    /* Another page since the last event: it read its data when it opened. */
    if (!shell.started || !page || page !== shell.current) return;
    const v = page.view;
    if (!v.reload) return;
    if (v.dirty && v.dirty()) {
      if (other) toast('Settings were changed elsewhere. Reload this page to see them.', 'warn');
    } else {
      v.reload();
    }
  }, SETTINGS_SETTLE_MS);
}

bus.on('event', m => {
  if (!shell.started) return;
  if (m.event === 'settings_changed') {
    settingsChanged(m);
  } else if (m.event === 'network' && m.what === 'offline') {
    toast('The station lost its Wi-Fi connection.', 'warn');
  } else if (m.event === 'receiver_restarted') {
    toast('The receiver restarted.', 'info');
  }
});

/* ---- Notices on every page --------------------------------------------- */

function bannerKey(u) {
  return [u.app_available ? u.app_version : '', u.gnss_available ? u.gnss_build : '', u.app_reverted_in_stage1 ? u.app_reverted_version : ''].join('|');
}

let laterKey = null; /* fallback when the browser storage is unavailable */

function bannerDismissed(u) {
  const k = bannerKey(u);
  return store.get('later', laterKey) === k;
}

/*
 * Seconds left of a Wi-Fi credential trial: the last status's
 * cred_trial_left_s, counted down here from when that status arrived, so the
 * countdown goes on while the station is out of reach (it usually is during
 * a trial: it left the network this page reached it on).
 */
function trialLeft(net) {
  const left = (net && net.cred_trial_left_s) || 0;
  const age = S.statusAt ? (Date.now() - S.statusAt) / 1000 : 0;
  return Math.max(0, Math.ceil(left - age));
}

function trialLeftText(net) {
  const left = trialLeft(net);
  return left > 0 ? fmt.dur(left) + ' left' : 'ending now';
}

/* Every countdown of the trial on the page ([data-trial-left]: the notice,
 * the dashboard's network card), once a second. */
function tickTrialLeft() {
  const net = S.status && S.status.network;
  if (!net || !net.cred_trial) return;
  const text = trialLeftText(net);
  for (const el of doc.querySelectorAll('[data-trial-left]')) {
    if (el.textContent !== text) el.textContent = text;
  }
}

/* The trial's countdown as an element: role timer, so a screen reader does not read every second out. */
function trialLeftEl(net) {
  return h('span', { 'data-trial-left': '', role: 'timer' }, trialLeftText(net));
}

/* Minutes for a notice ("about 14 min"): whole minutes, rounded up. */
const upMin = s => fmt.dur(Math.ceil(s / 60) * 60);

/*
 * The page a notice is on: the route's name, 'receiver/settings',
 * 'logs/history' and 'logs/console' for those pages; '' is the Status page.
 * Any other address under them opens the page itself (#/receiver/live the
 * Receiver page, #/receiver/settings/manage Receiver settings, #/logs/foo
 * the Overview), and is that page here too.
 */
function pageKey() {
  const c = shell.current;
  if (!c) return '';
  const sub = (c.arg || '').split('/')[0];
  if (c.name === 'receiver') return sub === 'settings' ? 'receiver/settings' : 'receiver';
  if (c.name === 'logs') return sub && LOGS_PAGES[sub] ? 'logs/' + sub : 'logs';
  return c.name;
}

const TONE_ORDER = { bad: 0, warn: 1, info: 2 };

/**
 * The page-wide notices for a status on `page` (pageKey()), worst first:
 * [{key, tone, title, text, actions, urgent, trial}], one line and one
 * button each. urgent: it needs the user now and never folds away. A page
 * whose own card says a thing leaves its notice out; the Status page the one
 * its top card says (stationState().notice), the update (its Station card
 * says it) and every stopped caster (the top card names each, as its subject
 * or in an "Also:" line); Logs' Overview the one its Health card says
 * (healthNotice()), only while that card is on the page (healthCardShown:
 * not while it loads or its first request failed).
 */
function noticeItems(st, page) {
  const items = [];
  const add = (key, tone, title, text, actions, urgent, trial) => items.push({ key: key, tone: tone, title: title, text: text || '',
    actions: actions || null, urgent: !!urgent, trial: !!trial });
  const go = (label, href) => link(label, href, 'btn small');
  const dev = st.device || {};
  const sm = st.safe_mode || {};
  const rcv = st.receiver || {};
  const u = st.update || {};
  const net = st.network || {};

  if (dev.settings_available === false) add('settings', 'bad', 'Settings cannot be saved', 'Restart the station.', go('Restart', '#/settings/restart'), true);
  if (st.health && st.health.factory_reset_refused) {
    add('factory_reset', 'warn', 'Factory reset refused', 'Hold BOOT again after the receiver update.', go('Details', '#/logs'), true);
  }
  /* Settings has the safe-mode box itself, with "Leave safe mode". */
  if (sm.active && page !== 'settings') add('safe_mode', 'bad', 'Safe mode', 'The receiver and NTRIP servers are off.', go('Details', '#/settings/restart'), true);
  /* Settings has maintenance next to the receiver update, with End now. */
  if (rcv.rx_maintenance && page !== 'settings') {
    add('maintenance', 'info', 'Receiver maintenance', rcv.maintenance_left_s ? 'Ends in about ' + upMin(rcv.maintenance_left_s) + '.' : '',
      btn('End now', endMaintenance, 'small'), true);
  }
  if (rcv.first_apply_held && page !== 'receiver') add('held', 'info', 'Receiver not configured yet', '', go('Review and configure', '#/receiver'));
  /* The Receiver page's own panel says it there. */
  if (rcv.uart_closed && !sm.active && page !== 'receiver') {
    add('uart_closed', 'bad', 'Receiver port closed', 'The station keeps trying.', go('Details', '#/logs'), true);
  }
  /* Settings shows a Wi-Fi trial and new settings waiting in its Wi-Fi card, with the buttons. */
  const pend = net.pending_ssid || 'the new network';
  if (page !== 'settings' && net.cred_trial) {
    /* No button: the trial ends by itself, either way. The countdown ticks without a rebuild. */
    add('wifi', 'info', 'Trying ' + pend, '', null, false, true);
  } else if (page !== 'settings' && net.cred_pending) {
    add('wifi', 'warn', net.cred_reverted_why === 'auth_failed' && net.cred_reverted ? pend + ' refused the new password' :
      net.cred_reverted ? 'No address on ' + pend : 'New Wi-Fi settings for ' + pend + ' not confirmed yet', '', go('Wi-Fi', '#/settings/wifi'));
  }
  /* A signal-group comparison running or finished: [tone, title, text, button]; Receiver settings has its card. */
  const sgn = page === 'receiver/settings' ? null : sgTestNotice(st);
  if (sgn) add('signal_test', sgn[0], sgn[1], sgn[2], sgn[3]);
  const j = rcv.journal;
  const inLoader = (st.alerts || []).some(a => a.id === 'receiver_in_loader');
  const session = j === 'waiting' || j === 'unreadable' || j === 'unknown';
  /* Settings shows a waiting receiver update in its session card. */
  if ((session && page !== 'settings') || (inLoader && !session)) {
    add('journal', 'warn', 'Receiver update needs you', !session ? 'The receiver waits in its update loader.' :
      j === 'unreadable' ? 'Its record is damaged.' : 'Keep the station powered.', go('Open', '#/settings/session'), true);
  }
  /* A slot the Onocoy login guard stopped tries nothing until the user acts; the NTRIP page shows it on its card. */
  for (const c of page === 'ntrip' ? [] : st.casters || []) {
    if (c.state === 'stopped') {
      add('onocoy_stopped:' + c.slot, 'bad', (c.name || 'Onocoy') + ' stopped: ' + lbl(L.stop, c.stop_reason || 'none'), '',
        go('What to do', '#/ntrip'), true);
    }
  }
  if (page !== 'settings' && (u.app_available || u.gnss_available || u.app_reverted_in_stage1) && !bannerDismissed(u)) {
    const parts = [];
    if (u.app_available) parts.push('app ' + u.app_version);
    if (u.gnss_available) parts.push('receiver build ' + u.gnss_build);
    add('update', 'info', 'Update available', capFirst(parts.join(' and ')) + (parts.length ? '.' : '') +
      (u.app_reverted_in_stage1 ? (parts.length ? ' ' : '') + u.app_reverted_version + ' was rolled back in its first minute.' : ''), [
      btn('Later', () => {
        laterKey = bannerKey(u);
        store.set('later', laterKey);
        shell.noticeKey = '';
        renderNotices();
      }, 'small ghost'),
      go('Settings page', '#/settings/updates')]);   /* it opens the Updates card; the update starts there */
  }
  const covered = page === '' ? stationState(st, true).notice : page === 'logs' && healthCardShown ? healthNotice(st) : null;
  /* The Status page says a stopped Onocoy slot in its top card; an update shows as the banner there too (its Station
   * card only names the version, easy to miss). */
  const statusSays = i => page === '' && i.key.indexOf('onocoy_stopped:') === 0;
  return items.filter(i => i.key !== covered && !statusSays(i)).sort((a, b) => TONE_ORDER[a.tone] - TONE_ORDER[b.tone]);
}

/** The notices a page shows: every urgent one and the first two others; the rest only when open. {shown, more}. */
function foldNotices(items, open) {
  const folded = items.filter(i => !i.urgent).slice(2);
  return { shown: open ? items : items.filter(i => folded.indexOf(i) < 0), more: folded.length };
}

function renderNotices() {
  const st = S.status;
  const box = doc.getElementById('notices');
  if (!st || !isAdmin()) {
    clear(box);
    shell.noticeKey = '';
    return;
  }
  const items = noticeItems(st, pageKey());
  const key = JSON.stringify([shell.noticesOpen, items.map(i => [i.key, i.tone, i.title, i.text])]);
  if (key === shell.noticeKey) {
    tickTrialLeft();
    return;
  }
  shell.noticeKey = key;
  const net = st.network || {};
  const f = foldNotices(items, shell.noticesOpen);
  mount(box, f.shown.map(i => h('div', { class: 'notice ' + i.tone, role: i.tone === 'bad' ? 'alert' : 'status' },
    h('div', { class: 'text' }, h('strong', null, i.title, i.trial ? [': ', trialLeftEl(net)] : null, i.text ? '.' : null),
      i.text ? ' ' + i.text : null),
    i.actions)),
  f.more ? btn(shell.noticesOpen ? 'Show less' : '+' + f.more + ' more', () => {
    shell.noticesOpen = !shell.noticesOpen;
    renderNotices();
    box.querySelector('.more-notices').focus();
  }, 'small ghost more-notices') : null);
}

/* ---- Receiver maintenance and pending Wi-Fi credentials (notices, pages) ---- */

/* Receiver maintenance in the owner's words (its start dialog in Settings). */
const MAINT_WHAT = 'The station stops sending commands to the receiver while you use a PC tool on its USB port. Streaming continues.';
/* What a configuration by the station saves along with its own settings (the start and end dialogs). */
const MAINT_SAVE_NOTE = 'Changes you make from the PC are saved the next time the station configures the receiver.';
/* The first configuration waits for the user; the receiver port does not open (the Status page's top card). */
const HELD_TEXT = 'The station waits for your OK before it configures the receiver.';
const UART_CLOSED_TEXT = 'The station cannot open its port to the receiver and keeps trying.';
/* A receiver update that needs the user (receiver.journal). */
const JOURNAL_LINE = {
  waiting: 'A receiver update stopped and needs your decision.',
  unreadable: 'A receiver update left a damaged record. Clear it to go on.',
};

async function endMaintenance(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'End receiver maintenance?', ok: 'End now',
    body: [h('p', null, 'The station starts sending commands to the receiver again. Close the PC tool first.'),
      h('p', { class: 'muted small' }, MAINT_SAVE_NOTE)] });
  if (!ok) return false;
  const r = await act(b, () => api('DELETE', '/api/gnss/maintenance'));
  if (r !== undefined) toast('Maintenance ended. The station checks the receiver now.', 'ok');
  return r !== undefined;
}

/** "Leave safe mode" (the Status page's top card, Settings): asks, then restarts normally. */
async function leaveSafeMode(b, err) {
  const ok = await dialog({ title: 'Leave safe mode?', ok: 'Restart normally',
    body: h('p', null, 'The station restarts normally. If the problem is still there, it can crash again and go back to safe mode.') });
  if (!ok) return false;
  const r = await act(b, () => api('POST', '/api/system/safe-mode/clear'), err);
  if (r !== undefined) showRestarting();
  return r !== undefined;
}

async function retryPendingWifi(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'Try the new Wi-Fi settings again?', ok: 'Try again',
    body: [h('p', null, 'The station leaves its current network and tries them for up to 90 seconds. ' +
      'If it gets no address, it comes back by itself.'),
    h('p', { class: 'muted' }, 'This page may lose the station meanwhile.')] });
  if (!ok) return false;
  const r = await act(b, () => api('POST', '/api/wifi/pending/retry'));
  if (r !== undefined) toast('Trying the new Wi-Fi settings…', 'ok');
  return r !== undefined;
}

async function discardPendingWifi(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'Discard the new Wi-Fi settings?', ok: 'Discard', okKind: 'danger solid',
    body: h('p', null, 'The station keeps its current network and forgets the new settings.') });
  if (!ok) return false;
  const r = await act(b, () => api('POST', '/api/wifi/pending/discard'));
  if (r !== undefined) toast('New Wi-Fi settings discarded.', 'ok');
  return r !== undefined;
}

/* ---- Dialogs ----------------------------------------------------------- */

let openDialogClose = null;   /* close(result) of the dialog that is open */

/**
 * Modal dialog. opts: title, body (nodes), ok (label, false = none: a dialog
 * that only says something, closed with the cancel button), okKind
 * ('primary' or 'danger solid'), cancel (label, false = none), check (label
 * of a box that must be ticked), typed (word that must be typed), onOk
 * (async; throwing keeps the dialog open and shows the error). Resolves true
 * on OK.
 */
function dialog(opts) {
  return new Promise(resolve => {
    const root = doc.getElementById('modal');
    const prevFocus = doc.activeElement;
    const err = errBox();
    const okBtn = opts.ok === false ? null : h('button', { type: 'submit', class: 'btn ' + (opts.okKind || 'primary') }, opts.ok || 'OK');
    const chk = opts.check ? checkbox(opts.check, false) : null;
    const typed = opts.typed ? textInput('', { 'aria-label': 'Type ' + opts.typed + ' to confirm' }) : null;
    const update = () => {
      if (okBtn) okBtn.disabled = (chk && !chk.input.checked) || (typed && typed.value.trim() !== opts.typed);
    };
    /* Modal for real: the page behind it is inert (no focus, no clicks, out
     * of the accessibility tree) and Tab wraps inside the dialog. */
    const behind = [doc.querySelector('header.top'), doc.getElementById('tabs'), doc.getElementById('notices'), doc.getElementById('view')];
    const setInert = on => {
      for (const el of behind) {
        if (!el) continue;
        if (on) el.setAttribute('inert', '');
        else el.removeAttribute('inert');
      }
    };
    let closed = false;
    const close = result => {
      if (closed) return;
      closed = true;
      if (openDialogClose === close) openDialogClose = null;
      doc.removeEventListener('keydown', onKey);
      window.removeEventListener('hashchange', onHash);
      clear(root);
      setInert(false);
      if (prevFocus && prevFocus.focus) prevFocus.focus();
      resolve(result);
    };
    const onHash = () => close(false);   /* the page changed underneath (back button) */
    const onKey = e => {
      if (e.key === 'Escape' && opts.cancel !== false) {
        close(false);
      } else if (e.key === 'Tab') {
        const items = Array.from(form.querySelectorAll('button, input, select, textarea, a[href]'))
          .filter(el => !el.disabled && !el.hidden && el.offsetParent !== null);
        if (!items.length) return;
        const first = items[0], last = items[items.length - 1];
        if (!form.contains(doc.activeElement)) {
          e.preventDefault();
          first.focus();
        } else if (e.shiftKey && doc.activeElement === first) {
          e.preventDefault();
          last.focus();
        } else if (!e.shiftKey && doc.activeElement === last) {
          e.preventDefault();
          first.focus();
        }
      }
    };
    const form = h('form', { class: 'dialog', role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': 'dlg-title',
      onsubmit: async e => {
        e.preventDefault();
        if (!okBtn || okBtn.disabled) return;
        if (opts.onOk) {
          const ok = await act(okBtn, opts.onOk, err);
          if (ok === undefined && !err.hidden) return;
        }
        close(true);
      } },
    h('h2', { id: 'dlg-title' }, opts.title),
    h('div', { class: 'stack' }, opts.body,
      chk ? chk.el : null,
      typed ? field('Type ' + opts.typed + ' to confirm', typed).el : null,
      err),
    h('div', { class: 'form-actions' },
      opts.cancel === false ? null : btn(opts.cancel || 'Cancel', () => close(false), okBtn ? 'ghost' : ''),
      okBtn));
    if (chk) chk.input.addEventListener('change', update);
    if (typed) typed.addEventListener('input', update);
    update();
    if (openDialogClose) openDialogClose(false);   /* one dialog at a time */
    mount(root, h('div', { class: 'backdrop', onclick: e => {
      if (e.target === e.currentTarget && opts.cancel !== false) close(false);
    } }, form));
    openDialogClose = close;
    setInert(true);
    doc.addEventListener('keydown', onKey);
    window.addEventListener('hashchange', onHash);
    (typed || chk && chk.input || okBtn || form.querySelector('.form-actions .btn')).focus();
  });
}

/* ---- Restart overlay --------------------------------------------------- */

let rebootWatch = 0;

/**
 * Full-page "restarting" state. Polls GET /api/session until the station
 * answers again, then reloads the page (the new image may bring a new UI).
 */
function showRestarting(title, text) {
  if (rebootWatch) return;
  live.stop();
  /* The page behind stops polling: its requests would only draw 401s once
   * the station is back (a session not remembered ends with a restart). */
  const cur = shell.current;
  if (cur && cur.view && cur.view.destroy) {
    try {
      cur.view.destroy();
    } catch (e) {
      console.error(e);
    }
  }
  if (cur) cur.view = {};
  const wasAdmin = isAdmin();
  const root = doc.getElementById('modal');
  const status = h('p', { class: 'muted small' }, 'Waiting…');
  mount(root, h('div', { class: 'overlay', role: 'alert' },
    h('div', { class: 'card' }, h('div', { class: 'spinner' }), h('h2', null, title || 'Restarting'),
      h('p', null, text || 'This page reconnects by itself when the station is back, usually within a minute.'),
      status)));
  const started = Date.now();
  rebootWatch = setInterval(async () => {
    const secs = Math.round((Date.now() - started) / 1000);
    status.textContent = 'Waiting… ' + fmt.dur(secs);
    if (secs < 4) return;
    try {
      await api('GET', '/api/session', undefined, { timeout: 2500, quiet401: true });
      clearInterval(rebootWatch);
      /* The login after the reload says why it is there (30-auth.js). */
      if (wasAdmin) {
        try {
          sessionStorage.setItem('station.restartedAt', String(Date.now()));
        } catch (e) {
          /* no storage (captive-portal browser): the login has no note */
        }
      }
      location.reload();
    } catch (e) {
      if (secs > 180) status.textContent = 'Still waiting (' + fmt.dur(secs) + '). If the station moved to another network, open it there.';
    }
  }, 2000);
}

bus.on('rebooting', () => {
  if (shell.started || S.publicView) showRestarting();
});

/* ---- Login / logout ---------------------------------------------------- */

async function logout() {
  try {
    await api('DELETE', '/api/session', undefined, { quiet401: true });
  } catch (e) {
    /* the session is gone either way */
  }
  live.stop();
  forgetSecrets();
  S.session = Object.assign({}, S.session, { authenticated: false });
  showLogin();
}

bus.on('unauthorized', () => {
  if (!S.session || !S.session.authenticated) return;
  live.stop();
  forgetSecrets();
  S.session = Object.assign({}, S.session, { authenticated: false });
  showLogin('Your session ended. Login again.');
});
