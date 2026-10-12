/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App shell: header, navigation, hash router (#/casters, #/diag/logs, ...),
 * page-wide notices (safe mode, receiver maintenance, receiver UART
 * reopening, new Wi-Fi credentials on trial or pending, a signal-group
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
  const nav = doc.getElementById('tabs');
  const more = nav.scrollWidth - nav.clientWidth;
  nav.classList.toggle('more-left', more > 2 && nav.scrollLeft > 2);
  nav.classList.toggle('more-right', more > 2 && nav.scrollLeft < more - 2);
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
  const head = doc.getElementById('page-title');
  head.textContent = def.title;
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
  if (st.health && st.health.factory_reset_refused) {
    items.push(['warn', 'Factory reset refused',
      'Someone held the BOOT button for a factory reset while a receiver update was running, so the station refused it. ' +
      'Hold the button again once the update has finished.', [link('Details', '#/diag', 'btn small')]]);
  }
  if (sm.active) {
    items.push(['bad', 'Safe mode',
      'The station restarted after ' + (sm.crashes || 'repeated') + ' crashes and is in safe mode: the receiver and the casters are off. ' +
      'Wi-Fi, this page and updates still work.' + (sm.retry_in_s ? ' It tries normal operation again in ' + fmt.dur(Math.ceil(sm.retry_in_s / 60) * 60) + '.' : ''),
      [link('Details', '#/system', 'btn small'), link('Updates', '#/updates', 'btn small')]]);
  }
  if (rcv.rx_maintenance) {
    const left = rcv.maintenance_left_s ? ' It ends by itself in about ' + fmt.dur(Math.ceil(rcv.maintenance_left_s / 60) * 60) + '.' : '';
    items.push(['warn', 'Receiver maintenance', 'Nothing is sent to the receiver while you work on it through its other port; ' +
      'the casters still get its RTCM.' + left + ' ' + MAINT_SAVE_NOTE,
    [btn('End now', endMaintenance, 'small'), link('Details', '#/receiver', 'btn small')]]);
  }
  if (rcv.first_apply_held && !(shell.current && shell.current.name === 'receiver')) {
    items.push(['info', 'Receiver not configured yet', 'This build holds the receiver\'s first configuration until you confirm it: ' +
      'so far the station has sent it only queries. See what it would send, then configure it.',
    [link('Review and configure', '#/receiver', 'btn small')]]);
  }
  if (rcv.uart_closed && !sm.active) {
    items.push(['bad', 'Receiver UART closed (reopening)', 'The station could not open its serial port to the receiver and keeps trying on its own. ' +
      'Nothing arrives from the receiver meanwhile.', [link('Details', '#/diag', 'btn small')]]);
  }
  const net = st.network || {};
  const pend = net.pending_ssid || 'the new network';
  if (net.cred_trial) {
    /* No buttons while a trial runs: it ends by itself, either way. The
     * countdown (a fifth field: kept out of the key) ticks without a rebuild. */
    items.push(['info', 'Trying the new credentials for ' + pend,
      'Without an address there the station goes back to its previous network by itself.', null, 'trial']);
  } else if (net.cred_pending) {
    /* After a revert the network in use is the previous one: the same name means only the password changed. */
    const what = net.ssid && net.pending_ssid === net.ssid ? 'the new password for ' + pend : 'the new credentials for ' + pend;
    let title, text;
    if (net.cred_reverted && net.cred_reverted_why === 'auth_failed') {
      title = pend + ' refused the new password';
      text = 'The station is back on its previous credentials' + (net.ssid ? ' (' + net.ssid + ')' : '') + '. Try them again, or discard them.';
    } else if (net.cred_reverted) {
      title = 'No address with ' + what;
      text = 'The station is back on its previous network (' + (net.ssid || 'unknown') + '). Try them again, or discard them.';
    } else {
      title = 'Not confirmed yet: ' + what;
      text = 'They become the station\'s once they get an address. Try them again, or discard them.';
    }
    items.push(['warn', title, text, [btn('Try again', retryPendingWifi, 'small'), btn('Discard', discardPendingWifi, 'small')]]);
  }
  /* A signal-group comparison running or finished (60-receiver.js). */
  const sgn = sgTestNotice(S.status);
  if (sgn) items.push(sgn);
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
  /* A slot the Onocoy login guard stopped: nothing is tried until the user acts. The casters page shows the
   * stop on the slot's card itself, with what to do and Try again: no notice there that points to it. */
  const onCasters = shell.current && shell.current.name === 'casters';
  for (const c of onCasters ? [] : st.casters || []) {
    if (c.state === 'stopped') {
      items.push(['bad', 'Onocoy stopped: ' + lbl(L.stop, c.stop_reason || 'none'),
        (c.name || 'Slot ' + (c.slot + 1)) + ' makes no new attempt until you act. The casters page says why and what to do.',
        [link('Details', '#/casters', 'btn small')]]);
    }
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

  const key = JSON.stringify(items.map(i => [i[0], i[1], i[2], i[4] || null]));
  if (key === shell.noticeKey) {
    tickTrialLeft();
    return;
  }
  shell.noticeKey = key;
  mount(box, items.map(i => h('div', { class: 'notice ' + i[0], role: i[0] === 'bad' ? 'alert' : 'status' },
    h('div', { class: 'text' }, h('strong', null, i[1], i[4] === 'trial' ? [': ', trialLeftEl(net)] : null), i[2]),
    i[3] ? h('div', { class: 'row' }, i[3]) : null)));
}

/* ---- Receiver maintenance and pending Wi-Fi credentials (notices, pages) ---- */

/* What a configuration by the station saves along with its own settings (maintenance notice, dialogs, receiver page). */
const MAINT_SAVE_NOTE = 'The station\'s next configuration of the receiver (a profile change, a re-apply after drift, a recovery step) ' +
  'ends with SAVECONFIG, which also saves whatever you changed from the PC, for example output on COM2, MASK or MMP. ' +
  'Undo what should not stay before you end maintenance.';

async function endMaintenance(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'End receiver maintenance?', ok: 'End now',
    body: [h('p', null, 'The station talks to the receiver again and checks it as at start-up. ' +
      'Make sure nothing else is connected to the receiver\'s other port and working on it.'),
    msg('warn', MAINT_SAVE_NOTE)] });
  if (!ok) return false;
  const r = await act(b, () => api('DELETE', '/api/gnss/maintenance'));
  if (r !== undefined) toast('Receiver maintenance ends. The station checks the receiver now.', 'ok');
  return r !== undefined;
}

async function retryPendingWifi(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'Try the new Wi-Fi credentials again?', ok: 'Try again',
    body: [h('p', null, 'The station leaves its current network and tries the new credentials for up to 90 seconds. ' +
      'If they get no address, it comes back by itself.'),
    h('p', { class: 'muted' }, 'This page may lose contact with the station meanwhile, and the casters reconnect afterwards.')] });
  if (!ok) return false;
  const r = await act(b, () => api('POST', '/api/wifi/pending/retry'));
  if (r !== undefined) toast('Trying the new Wi-Fi credentials…', 'ok');
  return r !== undefined;
}

async function discardPendingWifi(e) {
  const b = e && e.currentTarget;
  const ok = await dialog({ title: 'Discard the new Wi-Fi credentials?', ok: 'Discard', okKind: 'danger solid',
    body: h('p', null, 'The station keeps using its current network and forgets the new credentials.') });
  if (!ok) return false;
  const r = await act(b, () => api('POST', '/api/wifi/pending/discard'));
  if (r !== undefined) toast('New Wi-Fi credentials discarded.', 'ok');
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
