/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App shell: header, navigation, hash router (#/casters, #/diag/logs, ...),
 * page-wide notices (safe mode, receiver maintenance, the receiver port
 * closed, new Wi-Fi credentials on trial or pending, a signal-group
 * comparison running or finished, receiver update waiting, update banner),
 * dialogs, the restart overlay and the colour theme.
 *
 * A view is {title, admin, create(el, arg) -> {update(status)?, destroy()?,
 * dirty()?, reload()?}}. update() runs on every status change; reload() when
 * the settings changed elsewhere (unless dirty() says a form is being edited).
 */

const Views = {};
const NAV = [
  ['', 'Status'],
  ['casters', 'Casters'],
  ['receiver', 'Receiver'],
  ['signals', 'Signals'],
  ['updates', 'Updates'],
  ['diag', 'Diagnostics'],
  ['system', 'System'],
];
/* Pages a visitor without a session may open when public status is on. */
const PUBLIC_PAGES = ['', 'signals'];

const shell = {
  current: null,     /* {name, arg, view} */
  started: false,
  noticeKey: '',
  navKey: '',
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
    mount(box, themeBtn, h('button', { type: 'button', class: 'btn small ghost', onclick: logout }, 'Log out'));
  } else if (S.publicView) {
    mount(box, themeBtn, h('button', { type: 'button', class: 'btn small', onclick: () => showLogin() }, 'Log in'));
  } else {
    mount(box, themeBtn);
  }
}

/*
 * Connection indicator. Its state word is a polite live region, so a screen
 * reader hears "Live", "Updating" or "No contact" once when it changes; the
 * running count of seconds without contact sits beside it, outside the
 * region (and hidden from assistive technology), or it would be read out
 * every second.
 */
function renderLive(state) {
  const el = doc.getElementById('live');
  const age = S.statusAt ? (Date.now() - S.statusAt) / 1000 : null;
  let cls = '', text = '', count = '';
  if (!shell.started) {
    cls = '';
    text = '';
  } else if (state === 'ws' && age !== null && age < 5) {
    cls = 'on';
    text = 'Live';
  } else if (age !== null && age < 6) {
    cls = 'poll';
    text = 'Updating';
  } else {
    cls = 'down';
    text = age === null ? 'Connecting…' : 'No contact';
    count = age === null ? '' : fmt.dur(age);
  }
  el.className = 'live ' + cls;
  if (!text) {
    clear(el);
    return;
  }
  let word = el.querySelector('.word');
  let ageEl = el.querySelector('.age');
  if (!word || !ageEl) {
    word = h('span', { class: 'word', role: 'status' });
    ageEl = h('span', { class: 'age', 'aria-hidden': 'true' });
    mount(el, word, ageEl);
  }
  if (word.textContent !== text) word.textContent = text;
  if (ageEl.textContent !== count) ageEl.textContent = count;
}

function renderNav() {
  const nav = doc.getElementById('tabs');
  const name = shell.current ? shell.current.name : '';
  const pages = isAdmin() ? NAV : NAV.filter(n => PUBLIC_PAGES.indexOf(n[0]) >= 0);
  const st = S.status || {};
  const upd = st.update || {};
  /* Rebuild only on a change, so a tap on a tab is never lost to a refresh. */
  const key = [name, isAdmin(), !!(upd.app_available || upd.gnss_available || upd.app_reverted_in_stage1)].join('|');
  if (key === shell.navKey && nav.firstChild) return;
  shell.navKey = key;
  mount(nav, pages.map(n => {
    const dot = n[0] === 'updates' && (upd.app_available || upd.gnss_available || upd.app_reverted_in_stage1);
    return h('a', { href: '#/' + n[0], class: name === n[0] ? 'active' : null, 'aria-current': name === n[0] ? 'page' : null },
      n[1], dot ? h('span', { class: 'dot', title: 'Update available' }) : null);
  }));
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

/* A row that scrolls sideways (the tabs, Diagnostics' sub-tabs): a fade at each edge where more of it lies. */
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
  if (!Views[r.name === '' ? 'dashboard' : r.name]) r = { name: '', arg: '' };
  if (!isAdmin() && PUBLIC_PAGES.indexOf(r.name) < 0) r = { name: '', arg: '' };
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
  const def = Views[name === '' ? 'dashboard' : name];
  const el = viewEl();
  clear(el);
  shell.current = { name: name, arg: arg, view: {} };
  try {
    shell.current.view = def.create(el, arg) || {};
  } catch (e) {
    console.error(e);
    mount(el, msg('bad', 'This page could not be shown: ' + e.message));
  }
  const host = S.status && S.status.device ? S.status.device.hostname : '';
  doc.title = def.title + (host ? ' · ' + host : '');
  /* The page's one h1: read out only, or shown as the page's heading where the view has no title card (showTitle). */
  const head = doc.getElementById('page-title');
  head.textContent = def.title;
  head.className = def.showTitle ? 'page-title' : 'sr-only';
  head.hidden = false;
  renderNav();
  renderNotices();
  window.scrollTo(0, 0);
}

function startShell() {
  shell.started = true;
  doc.body.classList.remove('bare');
  renderTopActions();
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
  renderLive(live.state);
  refreshView();
});
bus.on('live', renderLive);
setInterval(() => {
  renderLive(live.state);
  tickTrialLeft();
}, 1000);

bus.on('event', m => {
  if (!shell.started) return;
  if (m.event === 'settings_changed') {
    const v = shell.current && shell.current.view;
    const own = !!m.client && m.client === CLIENT_ID;
    if (v && v.reload) {
      /* This tab's own save: the page knows already; a form being edited stays as it is. */
      if (v.dirty && v.dirty()) {
        if (!own) toast('Settings were changed elsewhere. Reload this page to see them.', 'warn');
      } else {
        v.reload();
      }
    }
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

/**
 * The page-wide notices for a status, on the page named `page` (shell route
 * name: '' is the Status page): [{key, tone, title, text, actions, trial}].
 * key names what a notice is about: the Status page leaves out the one its
 * top card already says, with its button (heroLead).
 */
function noticeItems(st, page) {
  const items = [];
  const add = (key, tone, title, text, actions, trial) => items.push({ key: key, tone: tone, title: title, text: text,
    actions: actions || null, trial: !!trial });
  const dev = st.device || {};
  const sm = st.safe_mode || {};
  const rcv = st.receiver || {};
  const u = st.update || {};
  const alerts = (st.alerts || []).map(a => a.id);

  if (dev.settings_available === false) {
    add('settings', 'bad', 'Settings cannot be saved', 'The station runs, but changes are not saved this boot. Restart it; ' +
      'if that does not help, see Diagnostics.', [link('Diagnostics', '#/diag', 'btn small')]);
  }
  if (st.health && st.health.factory_reset_refused) {
    add('factory_reset', 'warn', 'Factory reset refused', 'The BOOT button was held for a factory reset during a receiver update. ' +
      'Hold it again after the update.', [link('Details', '#/diag', 'btn small')]);
  }
  /* The System page has the safe-mode box itself, with "Leave safe mode". */
  if (sm.active && page !== 'system') {
    add('safe_mode', 'bad', 'Safe mode', 'After ' + (sm.crashes || 'repeated') + ' crashes the receiver and casters are off. ' +
      'Wi-Fi, this page and updates still work.' + (sm.retry_in_s ? ' Normal operation is tried again in ' + upMin(sm.retry_in_s) + '.' : ''),
    [link('Updates', '#/updates', 'btn small'), link('Details', '#/system', 'btn small')]);
  }
  /* The Receiver page has the maintenance card itself, with the same buttons. */
  if (rcv.rx_maintenance && page !== 'receiver') {
    add('maintenance', 'warn', 'Receiver maintenance', MAINT_WHAT + (rcv.maintenance_left_s ? ' Ends by itself in about ' +
      upMin(rcv.maintenance_left_s) + '.' : '') + ' ' + MAINT_SAVE_NOTE,
    [link('Details', '#/receiver', 'btn small'), btn('End now', endMaintenance, 'small')]);
  }
  if (rcv.first_apply_held && page !== 'receiver') {
    add('held', 'info', 'Receiver not configured yet', HELD_TEXT, [link('Review and configure', '#/receiver', 'btn small')]);
  }
  if (rcv.uart_closed && !sm.active) {
    add('uart_closed', 'bad', 'Receiver port closed', UART_CLOSED_TEXT + ' Nothing arrives from the receiver meanwhile.',
      [link('Details', '#/diag', 'btn small')]);
  }
  const net = st.network || {};
  const pend = net.pending_ssid || 'the new network';
  /* The System page shows the trial and pending credentials itself (its Wi-Fi card, with the buttons): no notice there. */
  if (page !== 'system' && net.cred_trial) {
    /* No buttons while a trial runs: it ends by itself, either way. The countdown ticks without a rebuild. */
    add('wifi', 'info', 'Trying ' + pend, 'If it gets no address there, the station goes back to the previous network by itself.', null, true);
  } else if (page !== 'system' && net.cred_pending) {
    let title, text;
    const back = 'The station is back on ' + (net.ssid || 'the previous network') + '. Try again, or discard the new settings.';
    if (net.cred_reverted && net.cred_reverted_why === 'auth_failed') {
      title = pend + ' refused the new password';
      text = back;
    } else if (net.cred_reverted) {
      title = 'No address on ' + pend;
      text = back;
    } else {
      title = 'New Wi-Fi settings for ' + pend + ' not confirmed yet';
      text = 'They take over once the station gets an address there. Try again, or discard them.';
    }
    add('wifi', 'warn', title, text, [btn('Discard', discardPendingWifi, 'small'), btn('Try again', retryPendingWifi, 'small')]);
  }
  /* A signal-group comparison running or finished (60-receiver.js): [tone, title, text, actions]. */
  const sgn = sgTestNotice(st);
  if (sgn) add('signal_test', sgn[0], sgn[1], sgn[2], sgn[3]);
  const j = rcv.journal;
  const inLoader = alerts.indexOf('receiver_in_loader') >= 0;
  if (j === 'waiting' || j === 'unreadable' || j === 'unknown' || inLoader) {
    const what = inLoader && (!j || j === 'none') ? 'The receiver waits in its update loader. Nothing is sent to it until you choose what to do.' :
      j === 'unknown' ? L.journal.unknown[1] : j === 'unreadable' ? JOURNAL_LINE.unreadable : JOURNAL_LINE.waiting;
    add('journal', 'warn', 'Receiver update needs you', what, [link('Open', '#/updates/session', 'btn small')]);
  }
  /* A slot the Onocoy login guard stopped: nothing is tried until the user acts. The casters page shows the
   * stop on the slot's card itself, with what to do and Try again: no notice there that points to it. */
  for (const c of page === 'casters' ? [] : st.casters || []) {
    if (c.state === 'stopped') {
      add('onocoy_stopped:' + c.slot, 'bad', (c.name || 'Onocoy') + ' stopped: ' + lbl(L.stop, c.stop_reason || 'none'),
        'It makes no new attempt until you act.', [link('What to do', '#/casters', 'btn small')]);
    }
  }
  if (page !== 'updates' && (u.app_available || u.gnss_available || u.app_reverted_in_stage1) && !bannerDismissed(u)) {
    const parts = [];
    if (u.app_available) parts.push('App ' + u.app_version);
    if (u.gnss_available) parts.push('receiver build ' + u.gnss_build);
    let text = parts.length ? parts.join(' and ') + (parts.length > 1 ? ' are' : ' is') + ' available.' : '';
    if (u.app_reverted_in_stage1) {
      text += (text ? ' ' : '') + 'Update ' + u.app_reverted_version + ' was rolled back in its first minute (a crash or a power dip). Install it again?';
    }
    if (u.gnss_available && u.gnss_blocked_reason && u.gnss_blocked_reason !== 'none') {
      text += ' Receiver update possible ' + (u.gnss_blocked_left_s ? 'in ' + upMin(u.gnss_blocked_left_s) :
        'once this clears: ' + lbl(L.gnssBlock, u.gnss_blocked_reason).toLowerCase()) + '.';
    }
    add('update', 'info', 'Update available', text, [
      btn('Later', () => {
        laterKey = bannerKey(u);
        store.set('later', laterKey);
        shell.noticeKey = '';
        renderNotices();
      }, 'small'),
      link('Update', '#/updates', 'btn small primary')]);
  }
  if (page === '') {
    const covered = heroLead(st, true).notice;
    if (covered) return items.filter(i => i.key !== covered);
  }
  return items;
}

function renderNotices() {
  const st = S.status;
  const box = doc.getElementById('notices');
  if (!st || !isAdmin()) {
    clear(box);
    shell.noticeKey = '';
    return;
  }
  const items = noticeItems(st, shell.current ? shell.current.name : '');
  const net = st.network || {};
  const key = JSON.stringify(items.map(i => [i.tone, i.title, i.text, i.trial]));
  if (key === shell.noticeKey) {
    tickTrialLeft();
    return;
  }
  shell.noticeKey = key;
  mount(box, items.map(i => h('div', { class: 'notice ' + i.tone, role: i.tone === 'bad' ? 'alert' : 'status' },
    h('div', { class: 'text' }, h('strong', null, i.title, i.trial ? [': ', trialLeftEl(net)] : null), i.text),
    i.actions ? h('div', { class: 'form-actions' }, i.actions) : null)));
}

/* ---- Receiver maintenance and pending Wi-Fi credentials (notices, pages) ---- */

/* Receiver maintenance in the owner's words (notices, the Status page, the Receiver page). */
const MAINT_WHAT = 'The station stops sending commands to the receiver while you use a PC tool on its USB port. Streaming continues.';
/* What a configuration by the station saves along with its own settings (notice, end dialog, Receiver page). */
const MAINT_SAVE_NOTE = 'Changes you make from the PC are saved the next time the station configures the receiver.';
/* The first configuration waits for the user; the receiver port does not open (notices and the Status page say the same). */
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
 * Modal dialog. opts: title, body (nodes), ok (label), okKind ('primary' or
 * 'danger solid'), cancel (label, false = none), check (label of a box that
 * must be ticked), typed (word that must be typed), onOk (async; throwing
 * keeps the dialog open and shows the error). Resolves true on OK.
 */
function dialog(opts) {
  return new Promise(resolve => {
    const root = doc.getElementById('modal');
    const prevFocus = doc.activeElement;
    const err = errBox();
    const okBtn = h('button', { type: 'submit', class: 'btn ' + (opts.okKind || 'primary') }, opts.ok || 'OK');
    const chk = opts.check ? checkbox(opts.check, false) : null;
    const typed = opts.typed ? textInput('', { 'aria-label': 'Type ' + opts.typed + ' to confirm' }) : null;
    const update = () => {
      okBtn.disabled = (chk && !chk.input.checked) || (typed && typed.value.trim() !== opts.typed);
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
        if (okBtn.disabled) return;
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
      opts.cancel === false ? null : btn(opts.cancel || 'Cancel', () => close(false), 'ghost'),
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
    (typed || chk && chk.input || okBtn).focus();
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
   * the station is back (sessions do not survive a restart). */
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

/* ---- Log in / out ------------------------------------------------------ */

async function logout() {
  try {
    await api('DELETE', '/api/session', undefined, { quiet401: true });
  } catch (e) {
    /* the session is gone either way */
  }
  live.stop();
  S.session = Object.assign({}, S.session, { authenticated: false });
  showLogin();
}

bus.on('unauthorized', () => {
  if (!S.session || !S.session.authenticated) return;
  live.stop();
  S.session = Object.assign({}, S.session, { authenticated: false });
  showLogin('Your session ended. Log in again.');
});
