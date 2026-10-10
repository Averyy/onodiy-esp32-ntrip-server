/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App shell: header, navigation, hash router (#/casters, #/diag/logs, ...),
 * page-wide notices (safe mode, receiver update waiting, update banner),
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

function viewEl() {
  return doc.getElementById('view');
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
  const themeBtn = h('button', { type: 'button', class: 'icon-btn', title: 'Light or dark', 'aria-label': 'Switch light or dark',
    onclick: toggleTheme }, '◐');
  if (isAdmin()) {
    mount(box, themeBtn, h('button', { type: 'button', class: 'btn small ghost', onclick: logout }, 'Log out'));
  } else if (S.publicView) {
    mount(box, themeBtn, h('button', { type: 'button', class: 'btn small', onclick: () => showLogin() }, 'Log in'));
  } else {
    mount(box, themeBtn);
  }
}

function renderLive(state) {
  const el = doc.getElementById('live');
  const age = S.statusAt ? (Date.now() - S.statusAt) / 1000 : null;
  let cls = '', text = '';
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
    text = age === null ? 'Connecting…' : 'No contact ' + fmt.dur(age);
  }
  el.className = 'live ' + cls;
  el.textContent = text;
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
}

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
  if (cur && cur.view.dirty && cur.view.dirty() && !window.confirm('Leave this page? Changes that were not saved are lost.')) {
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
  doc.body.classList.add('bare');
  clear(doc.getElementById('tabs'));
  clear(doc.getElementById('notices'));
  renderLive();
}

/** Show a page outside the app (login, setup). */
function bare(...kids) {
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
setInterval(() => renderLive(live.state), 1000);

bus.on('event', m => {
  if (!shell.started) return;
  if (m.event === 'settings_changed') {
    const v = shell.current && shell.current.view;
    if (v && v.reload) {
      if (v.dirty && v.dirty()) toast('Settings were changed elsewhere. Reload this page to see them.', 'warn');
      else v.reload();
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

function renderNotices() {
  const st = S.status;
  const box = doc.getElementById('notices');
  if (!st || !isAdmin()) {
    clear(box);
    shell.noticeKey = '';
    return;
  }
  const items = [];
  const dev = st.device || {};
  const sm = st.safe_mode || {};
  const rcv = st.receiver || {};
  const u = st.update || {};
  const alerts = (st.alerts || []).map(a => a.id);

  if (dev.settings_available === false) {
    items.push(['bad', 'Settings storage unavailable',
      'The station could not use its settings storage this boot. It runs, but changes are not saved. Restarting may help; if not, see the diagnostics.', null]);
  }
  if (sm.active) {
    items.push(['bad', 'Safe mode',
      'The station restarted after ' + (sm.crashes || 'repeated') + ' crashes and is in safe mode: the receiver and the casters are off. ' +
      'Wi-Fi, this page and updates still work.' + (sm.retry_in_s ? ' It tries normal operation again in ' + fmt.dur(Math.ceil(sm.retry_in_s / 60) * 60) + '.' : ''),
      [link('Details', '#/system', 'btn small'), link('Updates', '#/updates', 'btn small')]]);
  }
  const j = rcv.journal;
  const inLoader = alerts.indexOf('receiver_in_loader') >= 0;
  if (j === 'waiting' || j === 'unreadable' || j === 'unknown' || inLoader) {
    let what;
    if (inLoader && (!j || j === 'none')) {
      what = 'The receiver waits at its loader prompt without an update session. Nothing is sent to it until you choose what to do.';
    } else if (j === 'unknown') {
      what = L.journal.unknown[1];
    } else if (j === 'unreadable') {
      what = 'A receiver update session left a record that cannot be read. Clear it to go on.';
    } else {
      what = 'A receiver update session stopped and waits for your decision.';
    }
    items.push(['warn', 'Receiver update needs you', what, [link('Open', '#/updates/session', 'btn small')]]);
  }
  const onUpdates = shell.current && shell.current.name === 'updates';
  if (isAdmin() && !onUpdates && (u.app_available || u.gnss_available || u.app_reverted_in_stage1) && !bannerDismissed(u)) {
    const parts = [];
    if (u.app_available) parts.push('App ' + u.app_version);
    if (u.gnss_available) parts.push('receiver build ' + u.gnss_build);
    let text = parts.length ? parts.join(' and ') + ' available.' : '';
    if (u.app_reverted_in_stage1) {
      text += (text ? ' ' : '') + 'Update ' + u.app_reverted_version + ' was rolled back during its first minute (a crash or a power blip). Install it again?';
    }
    if (u.gnss_available && u.gnss_blocked_reason && u.gnss_blocked_reason !== 'none') {
      text += ' Receiver update possible ' + (u.gnss_blocked_left_s ? 'in ' + fmt.dur(Math.ceil(u.gnss_blocked_left_s / 60) * 60) :
        'once this clears: ' + lbl(L.gnssBlock, u.gnss_blocked_reason).toLowerCase()) + '.';
    }
    items.push(['info', 'Update available', text, [
      link('Update', '#/updates', 'btn small primary'),
      btn('Later', () => {
        laterKey = bannerKey(u);
        store.set('later', laterKey);
        shell.noticeKey = '';
        renderNotices();
      }, 'small')]]);
  }

  const key = JSON.stringify(items.map(i => [i[0], i[1], i[2]]));
  if (key === shell.noticeKey) return;
  shell.noticeKey = key;
  mount(box, items.map(i => h('div', { class: 'notice ' + i[0], role: i[0] === 'bad' ? 'alert' : 'status' },
    h('div', { class: 'text' }, h('strong', null, i[1]), i[2]),
    i[3] ? h('div', { class: 'row' }, i[3]) : null)));
}

/* ---- Dialogs ----------------------------------------------------------- */

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
    const close = result => {
      doc.removeEventListener('keydown', onKey);
      clear(root);
      if (prevFocus && prevFocus.focus) prevFocus.focus();
      resolve(result);
    };
    const onKey = e => {
      if (e.key === 'Escape' && opts.cancel !== false) close(false);
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
    mount(root, h('div', { class: 'backdrop', onclick: e => {
      if (e.target === e.currentTarget && opts.cancel !== false) close(false);
    } }, form));
    doc.addEventListener('keydown', onKey);
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
  const root = doc.getElementById('modal');
  const status = h('p', { class: 'muted small' }, 'Waiting for the station…');
  mount(root, h('div', { class: 'overlay', role: 'alert' },
    h('div', { class: 'card' }, h('div', { class: 'spinner' }), h('h2', null, title || 'The station is restarting'),
      h('p', null, text || 'This page reconnects by itself when the station is back. That usually takes less than a minute.'),
      status)));
  const started = Date.now();
  rebootWatch = setInterval(async () => {
    const secs = Math.round((Date.now() - started) / 1000);
    status.textContent = 'Waiting for the station… ' + fmt.dur(secs);
    if (secs < 4) return;
    try {
      await api('GET', '/api/session', undefined, { timeout: 2500, quiet401: true });
      clearInterval(rebootWatch);
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
