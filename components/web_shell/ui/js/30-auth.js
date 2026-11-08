/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Start-up decisions (GET /api/session), the login, the public status view
 * and the first-run setup wizard.
 *
 * Shared session, login, password claim and Wi-Fi helpers. Products register
 * commissioning through the UI registry before startup.
 */

/* The setup hotspot (the station's own access point), as the session names it. */
function apSsidText() {
  return (S.session && S.session.ap_ssid) || '';
}

/* Settings' one line under the admin password (the login's Forgot password has the steps in full). */
function recoveryHint() {
  const ap = apSsidText();
  return 'Forgot it? Hold BOOT for 10s, then join the setup hotspot' + (ap ? ' ' + ap : '') + ' and open http://192.168.4.1/.';
}

/* Forgot the password: the two steps (the login's Forgot password dialog). */
function recoverySteps() {
  const ap = apSsidText();
  return h('ol', { class: 'howto' },
    h('li', null, 'Hold the BOOT button on the station for 10 seconds. This clears the password.'),
    h('li', null, 'Within 15 minutes, join the setup hotspot', ap ? [' ', h('strong', null, ap)] : null, ', open ',
      h('strong', null, 'http://192.168.4.1/'), ' and set a new password.'));
}

async function boot() {
  applyTheme();
  renderTopActions();
  let s;
  try {
    s = await api('GET', '/api/session', undefined, { quiet401: true, timeout: 8000 });
  } catch (e) {
    const lost = e instanceof ApiError && (e.code === 'network' || e.code === 'timeout');
    bare(h('div', { class: 'narrow' }, pageCard('The station is not answering',
      h('p', null, lost ? 'Check that this device is on the same network as the station.' : errText(e)),
      h('div', { class: 'form-actions' }, btn('Try again', boot, 'primary')))));
    return;
  }
  if (apiMismatch(s.api)) return;
  S.session = s;
  if (s.hostname) setBrand(s.hostname);
  if (!s.authenticated) {
    if (!s.admin_set) {
      if (s.claim_allowed) startWizard(true);
      else showNoAdmin();
      return;
    }
    if (s.public_status) {
      startPublic();
      return;
    }
    showLogin();
    return;
  }
  /* With the settings unavailable this boot, setup_done reads false and
   * cannot be stored: the wizard could never finish. The dashboard and its
   * notice explain the situation instead. */
  if (UI.commissioning && !s.setup_done && s.settings_available !== false) {
    startWizard(false);
    return;
  }
  startAuthed();
}

async function startAuthed() {
  S.publicView = false;
  try {
    await loadStatus();
  } catch (e) {
    if (e instanceof ApiError && e.status === 401) {
      showLogin();
      return;
    }
    /* live polling keeps trying */
  }
  live.topics = null;
  live.start();
  startShell();
}

async function startPublic() {
  S.publicView = true;
  try {
    await loadStatus();
  } catch (e) {
    if (e instanceof ApiError && e.status === 401) {
      S.publicView = false;
      showLogin();
      return;
    }
  }
  live.topics = null;
  live.start();
  startShell();
}

function showNoAdmin() {
  const ap = apSsidText();
  bare(h('div', { class: 'narrow' }, pageCard('Set up this station',
    h('p', null, 'This station has no admin password yet. Set it over the setup hotspot:'),
    h('ol', { class: 'howto' },
      h('li', null, ap ? ['Join the setup hotspot ', h('strong', null, ap), '.'] : UI.setupHint),
      h('li', null, 'Open ', h('strong', null, 'http://192.168.4.1/'), '.')),
    h('p', { class: 'muted small' }, 'Hotspot not there? Hold the BOOT button for 10 seconds; it opens for 15 minutes.'),
    h('div', { class: 'form-actions' }, btn('Check again', boot, 'primary')))));
}

/* The restart overlay notes when the station went down for a restart this
 * page asked for: the login after it says so (once, within 10 minutes). */
function restartNote() {
  let at = 0;
  try {
    at = Number(sessionStorage.getItem(storageKey('restartedAt'))) || 0;
    sessionStorage.removeItem(storageKey('restartedAt'));
  } catch (e) {
    at = 0;
  }
  return at && Date.now() - at < 600000 ? 'The station restarted. Login again.' : null;
}

/*
 * The login: the panel in the middle of the screen (app.css .login), the
 * password, "Remember me" (on by default: 30 days on this browser, kept
 * across restarts; off: a session that ends with the browser or after 30
 * min idle) and one line with Forgot password at the left (a dialog with the
 * steps) and Login at the right.
 */
function showLogin(note) {
  live.stop();
  S.publicView = false;
  const restarted = restartNote();
  note = note || restarted;
  const pw = pwInput({ autocomplete: 'current-password', id: 'login-pw' });
  const F = field('Admin password', pw);
  const remember = checkbox('Remember me', true);
  const err = errBox();
  const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Login');
  let lockUntil = 0;
  const tickLock = () => {
    const left = Math.ceil((lockUntil - Date.now()) / 1000);
    if (left > 0) {
      submit.disabled = true;
      submit.textContent = 'Wait ' + fmt.dur(left);
      setTimeout(tickLock, 1000);
    } else {
      submit.disabled = false;
      submit.textContent = 'Login';
    }
  };
  const forgot = h('button', { type: 'button', class: 'linkbtn', onclick: () => dialog({ title: 'Forgot password', body: recoverySteps(),
    ok: false, cancel: 'Close' }) }, 'Forgot password');
  const pub = S.session && S.session.public_status ?
    h('div', { class: 'form-actions' }, h('button', { type: 'button', class: 'btn', onclick: startPublic }, 'View status without login')) : null;
  const form = h('form', { onsubmit: async e => {
    e.preventDefault();
    if (!pw.value) {
      pw.focus();
      return;
    }
    err.hidden = true;
    F.error(null);
    try {
      await busy(submit, () => api('POST', '/api/session', { password: pw.value, remember: remember.input.checked }, { quiet401: true }));
    } catch (ex) {
      pw.select();
      /* A refused password or too many attempts: under the field (the button counts the wait down). */
      if (ex instanceof ApiError && ex.code === 'wrong_password') {
        F.error('Wrong password.');
      } else if (ex instanceof ApiError && ex.code === 'rate_limited') {
        F.error('Too many attempts.');
        if (ex.retry_in_s) {
          lockUntil = Date.now() + ex.retry_in_s * 1000;
          tickLock();
        }
      } else {
        showErr(ex, err);
      }
      return;
    }
    pw.value = '';
    boot();
  } },
  F.el, remember.el, err,
  h('div', { class: 'login-actions' }, forgot, submit));
  bare(h('div', { class: 'narrow login' }, pageCard('Login', note ? msg('info', note) : null, form, pub)));
  pw.focus();
}

/* ---- Password rules (8-64 bytes of UTF-8, no control characters) -------- */

/* The repeat does not match: the form marks the repeat field (the other problems mark the password). */
const PW_MISMATCH = 'The passwords do not match.';

function adminPasswordProblem(pw, repeat) {
  if (hasControl(pw)) return 'The password contains a character that is not allowed.';
  const n = utf8Len(pw);
  if (n < 8) return 'Use at least 8 characters.';
  if (n > 64) return 'Too long: use fewer characters.';
  if (repeat !== undefined && pw !== repeat) return PW_MISMATCH;
  return null;
}

/** Wi-Fi password: empty (open), 8-63 characters, or 64 hex digits. */
function wifiPasswordProblem(pw) {
  if (pw === '') return null;
  if (/^[0-9a-fA-F]{64}$/.test(pw)) return null;
  if (pw.length < 8 || pw.length > 63) return 'Wi-Fi passwords have 8 to 63 characters (or 64 hex digits).';
  return null;
}

/* ---- Wi-Fi picker (wizard and Settings) ---------------------------------- */

/**
 * Network list from the cached scan plus "Other network". opts.warnScan:
 * confirm before scanning (a scan stalls the casters while connected).
 * Returns {el, value() -> {ssid, password} | null (with errors shown), setWifi(w)}.
 */
function wifiPicker(wifi, opts) {
  opts = opts || {};
  let selected = null;   /* {ssid, secured} or {other: true} */
  let current = wifi;
  const list = h('div', { class: 'nets' });
  const other = textInput('', { maxlength: 32, placeholder: 'Network name' });
  const otherField = field('Network name', other, 'For a hidden network: type its exact name.');
  const pw = pwInput({ autocomplete: 'off', 'data-reveal': null });
  const pwField = field('Wi-Fi password', pw, ' ');
  const pwHint = pwField.el.querySelector('.hint');
  const scanInfo = h('span', { class: 'muted small' });
  const err = errBox();
  const scanBtn = btn('Scan', () => scan(), 'small');

  function nets() {
    const res = current && current.scan && current.scan.results ? current.scan.results : [];
    const best = {};
    for (const r of res) {
      if (!r.ssid) continue;
      if (!best[r.ssid] || r.rssi > best[r.ssid].rssi) best[r.ssid] = r;
    }
    return Object.keys(best).map(k => best[k]).sort((a, b) => b.rssi - a.rssi);
  }

  /* The network the station has stored (its password can be kept). */
  function stored(sel) {
    return !!(sel && !sel.other && current && current.configured && sel.ssid === current.ssid);
  }

  function render() {
    const items = nets();
    const sc = current && current.scan ? current.scan : {};
    const never = sc.age_s === null || sc.age_s === undefined;
    scanInfo.textContent = sc.running ? 'Scanning…' : never ? 'Not scanned yet.' :
      items.length + (items.length === 1 ? ' network' : ' networks') + ' found ' + fmt.dur(sc.age_s) + ' ago.';
    /* The saved network, chosen at the start, has its row even when the last scan did not see it. */
    if (stored(selected) && !items.some(r => r.ssid === selected.ssid)) items.unshift({ ssid: selected.ssid, secured: true, rssi: null, saved: true });
    scanBtn.textContent = never && !sc.running ? 'Scan' : 'Scan again';
    mount(list, items.map(r => {
      const isSel = selected && !selected.other && selected.ssid === r.ssid;
      return h('button', { type: 'button', class: 'net' + (isSel ? ' sel' : ''), 'aria-pressed': isSel ? 'true' : 'false',
        onclick: () => pick({ ssid: r.ssid, secured: r.secured }) },
      h('span', { class: 'ssid' }, r.ssid),
      r.saved ? h('span', { class: 'meta' }, 'Saved') : r.secured ? null : h('span', { class: 'meta' }, 'Open'),
      r.saved ? null : rssiView(r.rssi, true));
    }), h('button', { type: 'button', class: 'net' + (selected && selected.other ? ' sel' : ''),
      onclick: () => pick({ other: true }) }, h('span', { class: 'ssid' }, 'Other network')));
    otherField.el.hidden = !(selected && selected.other);
    pwField.el.hidden = !selected || (!selected.other && !selected.secured);
    /* What an empty field means depends on the choice. */
    if (pwHint) {
      pwHint.textContent = !selected ? '' : selected.other ? 'Leave empty for an open network.' :
        stored(selected) ? 'Leave empty to keep the saved password.' : '';
      pwHint.hidden = !pwHint.textContent;
    }
  }

  function pick(sel) {
    selected = sel;
    pw.value = '';
    pwField.error(null);
    otherField.error(null);
    render();
    (sel.other ? other : pw).focus();
  }

  async function scan() {
    if (opts.warnScan && current && current.online) {
      const ok = await dialog({ title: 'Scan for networks?', ok: 'Scan',
        body: h('p', null, UI.scanWarning) });
      if (!ok) return;
    }
    await act(scanBtn, async () => {
      await api('POST', '/api/wifi/scan');
      scanInfo.textContent = 'Scanning…';
      const t0 = Date.now();
      await sleep(1500);
      while (Date.now() - t0 < 20000) {
        current = await api('GET', '/api/wifi');
        if (current.scan && !current.scan.running) break;
        await sleep(1000);
      }
      render();
    }, err);
  }

  if (wifi && wifi.ssid && wifi.configured) selected = { ssid: wifi.ssid, secured: true };
  render();

  return {
    el: h('div', null, h('div', { class: 'spread' }, h('span', { class: 'lbl' }, 'Network'), h('span', { class: 'row' }, scanInfo, scanBtn)),
      list, otherField.el, pwField.el, err),
    value() {
      otherField.error(null);
      pwField.error(null);
      if (!selected) {
        err.hidden = false;
        mount(err, 'Choose a network.');
        return null;
      }
      err.hidden = true;
      const ssid = selected.other ? other.value : selected.ssid;
      const bytes = utf8Len(ssid);
      if (bytes < 1 || bytes > 32) {
        otherField.error(bytes < 1 ? 'Enter the network name.' : 'That network name is too long.');
        return null;
      }
      const p = pwField.el.hidden ? '' : pw.value;
      const problem = wifiPasswordProblem(p);
      if (problem) {
        pwField.error(problem);
        return null;
      }
      /* A secured network from the list needs its password, unless it is the stored one. */
      if (!selected.other && selected.secured && p === '' && !stored(selected)) {
        pwField.error('Enter the password for ' + ssid + '.');
        return null;
      }
      const out = { ssid: ssid };
      /* An unchanged network with an empty field keeps the stored password. */
      if (!(current && current.configured && ssid === current.ssid && p === '')) out.password = p;
      return out;
    },
  };
}

/* Default commissioning only claims the shared admin password, then opens Status.
 * A product may own its full commissioning flow through UI.commissioning. */
function startWizard(claim) {
  if (UI.commissioning) return UI.commissioning(claim);
  if (!claim) return startAuthed();
  live.stop();
  return claimPassword((step, title, ...body) => bare(h('div', { class: 'narrow' }, pageCard(title, ...body))),
    () => startAuthed());
}

function claimPassword(frame, next) {
  const pw = pwInput({ autocomplete: 'new-password' });
  const pw2 = pwInput({ autocomplete: 'new-password' });
  const f1 = field('Admin password', pw, 'At least 8 characters.');
  const f2 = field('Repeat password', pw2);
  const hint1 = f1.el.querySelector('.hint');
  const err = errBox();
  const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Set password');
  frame('password', 'Set an admin password',
    h('p', null, 'You need it to open this page and change settings. There is no default password.'),
    h('form', { onsubmit: async e => {
      e.preventDefault();
      f1.error(null);
      f2.error(null);
      const problem = adminPasswordProblem(pw.value, pw2.value);
      /* The rule's own words replace the hint while they show. */
      hint1.hidden = !!problem && problem !== PW_MISMATCH;
      if (problem) {
        (problem === PW_MISMATCH ? f2 : f1).error(problem);
        return;
      }
      const r = await act(submit, () => api('POST', '/api/setup/admin', { password: pw.value }), err);
      if (r === undefined) return;
      pw.value = '';
      pw2.value = '';
      S.session = await api('GET', '/api/session', undefined, { quiet401: true }).catch(() => S.session);
      if (r.persisted === false) toast('Settings cannot be saved right now: this password works until the next restart.', 'warn');
      next('password');
    } }, f1.el, f2.el, err, h('div', { class: 'form-actions' }, submit)));
  pw.focus();
}

