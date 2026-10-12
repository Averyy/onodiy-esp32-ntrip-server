/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Start-up decisions (GET /api/session), log in, the public status view and
 * the first-run setup wizard.
 *
 * Wizard order: the admin password (only over the station's own access
 * point), then Wi-Fi: once the station has joined the home network, its
 * setup network closes a few seconds later (the page counts down), the
 * phone goes back to its own Wi-Fi and logs in at http://<name>.local/.
 * There the setup continues: a reception check of the antenna (the live
 * view, with plain hints) before any caster is enabled, the Onocoy
 * credentials, optional extra casters. The station keeps the next step
 * (POST /api/setup/step), so the setup resumes after the move to the home
 * network, a reload or a new login; everything else stays in memory only
 * (captive-portal browsers may not keep browser storage).
 */

/* The station's own network, as the session names it. */
function apSsidText() {
  return (S.session && S.session.ap_ssid) || '';
}

function recoveryHint() {
  const ap = apSsidText();
  return 'Forgot the password? Hold the station\'s BOOT button for 10 seconds. That clears the password and opens the ' +
    'station\'s own Wi-Fi network' + (ap ? ' (' + ap + ')' : '') + ' for 15 minutes; join it, open http://192.168.4.1/ and set a new password.';
}

async function boot() {
  applyTheme();
  renderTopActions();
  let s;
  try {
    s = await api('GET', '/api/session', undefined, { quiet401: true, timeout: 8000 });
  } catch (e) {
    bare(h('div', { class: 'narrow' }, card('The station does not answer', msg('bad', errText(e)),
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
  if (!s.setup_done && s.settings_available !== false) {
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
  bare(h('div', { class: 'narrow' }, card('Set up this station',
    h('p', null, 'This station has no admin password yet. The first password can only be set over the station\'s own Wi-Fi network: ' +
      (ap ? 'join "' + ap + '"' : 'join the network whose name starts with "onocoydiy-"') + ' and open http://192.168.4.1/.'),
    h('p', { class: 'muted' }, 'If that network is not there, hold the BOOT button for 10 seconds; the station then opens it for 15 minutes.'),
    h('div', { class: 'form-actions' }, btn('Check again', boot, 'primary')))));
}

/* The restart overlay notes when the station went down for a restart this
 * page asked for: the login after it says so (once, within 10 minutes). */
function restartNote() {
  let at = 0;
  try {
    at = Number(sessionStorage.getItem('station.restartedAt')) || 0;
    sessionStorage.removeItem('station.restartedAt');
  } catch (e) {
    at = 0;
  }
  return at && Date.now() - at < 600000 ? 'The station restarted. Log in again.' : null;
}

function showLogin(note) {
  live.stop();
  S.publicView = false;
  const restarted = restartNote();
  note = note || restarted;
  const pw = pwInput({ autocomplete: 'current-password', id: 'login-pw' });
  const err = errBox();
  const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Log in');
  let lockUntil = 0;
  const tickLock = () => {
    const left = Math.ceil((lockUntil - Date.now()) / 1000);
    if (left > 0) {
      submit.disabled = true;
      submit.textContent = 'Wait ' + fmt.dur(left);
      setTimeout(tickLock, 1000);
    } else {
      submit.disabled = false;
      submit.textContent = 'Log in';
    }
  };
  const form = h('form', { onsubmit: async e => {
    e.preventDefault();
    if (!pw.value) {
      pw.focus();
      return;
    }
    err.hidden = true;
    try {
      await busy(submit, () => api('POST', '/api/session', { password: pw.value }, { quiet401: true }));
    } catch (ex) {
      pw.select();
      showErr(ex, err);
      if (ex instanceof ApiError && ex.code === 'rate_limited' && ex.retry_in_s) {
        lockUntil = Date.now() + ex.retry_in_s * 1000;
        tickLock();
      }
      return;
    }
    pw.value = '';
    boot();
  } },
  field('Admin password', pw).el,
  h('div', { class: 'form-actions' }, submit),
  err);
  const pub = S.session && S.session.public_status ?
    h('p', null, h('button', { type: 'button', class: 'btn ghost small', onclick: startPublic }, 'View the status without logging in')) : null;
  bare(h('div', { class: 'narrow' }, card('Log in', note ? msg('info', note) : null, form,
    h('p', { class: 'muted small foot' }, recoveryHint()), pub)));
  pw.focus();
}

/* ---- Password rules (8-64 bytes of UTF-8, no control characters) -------- */

function adminPasswordProblem(pw, repeat) {
  if (hasControl(pw)) return 'The password must not contain control characters.';
  const n = utf8Len(pw);
  if (n < 8) return 'Use at least 8 characters.';
  if (n > 64) return 'Use at most 64 bytes (fewer characters if they are not plain letters).';
  if (repeat !== undefined && pw !== repeat) return 'The two passwords differ.';
  return null;
}

/** Wi-Fi password: empty (open), 8-63 characters, or 64 hex digits. */
function wifiPasswordProblem(pw) {
  if (pw === '') return null;
  if (/^[0-9a-fA-F]{64}$/.test(pw)) return null;
  if (pw.length < 8 || pw.length > 63) return 'Wi-Fi passwords have 8 to 63 characters (or 64 hex digits).';
  return null;
}

/* ---- Wi-Fi picker (wizard and System page) ------------------------------ */

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
  const pw = pwInput({ autocomplete: 'off' });
  const pwField = field('Wi-Fi password', pw, ' ');
  const pwHint = pwField.el.querySelector('.hint');
  const scanInfo = h('span', { class: 'muted small' });
  const err = errBox();
  const scanBtn = btn('Scan again', () => scan(), 'small');

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
    scanInfo.textContent = sc.running ? 'Scanning…' : sc.age_s === null || sc.age_s === undefined ? 'No scan yet.' :
      items.length + ' networks, scanned ' + fmt.dur(sc.age_s) + ' ago.';
    mount(list, items.map(r => {
      const isSel = selected && !selected.other && selected.ssid === r.ssid;
      return h('button', { type: 'button', class: 'net' + (isSel ? ' sel' : ''), 'aria-pressed': isSel ? 'true' : 'false',
        onclick: () => pick({ ssid: r.ssid, secured: r.secured }) },
      h('span', { class: 'ssid' }, r.ssid),
      h('span', { class: 'meta' }, (r.secured ? '🔒 ' : 'open · ') + 'ch ' + r.channel),
      rssiView(r.rssi));
    }), h('button', { type: 'button', class: 'net' + (selected && selected.other ? ' sel' : ''),
      onclick: () => pick({ other: true }) }, h('span', { class: 'ssid' }, 'Other network…')));
    otherField.el.hidden = !(selected && selected.other);
    pwField.el.hidden = !selected || (!selected.other && !selected.secured);
    /* What an empty field means depends on the choice. */
    if (pwHint) {
      pwHint.textContent = !selected ? '' : selected.other ? 'Leave it empty for an open network.' :
        stored(selected) ? 'Leave it empty to keep the stored password.' : '8 to 63 characters (or 64 hex digits).';
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
        body: h('p', null, 'While the station scans, the casters stall for a few seconds.') });
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
        otherField.error('A network name has 1 to 32 bytes.');
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
        pwField.error('Enter the password of ' + ssid + '.');
        return null;
      }
      const out = { ssid: ssid };
      /* An unchanged network with an empty field keeps the stored password. */
      if (!(current && current.configured && ssid === current.ssid && p === '')) out.password = p;
      return out;
    },
  };
}

/* ---- Wizard ------------------------------------------------------------- */

/* What the step after the Wi-Fi is called in the instructions. */
const WIZ_NEXT = { reception: 'the reception check', onocoy: 'the Onocoy step', casters: 'more casters' };

async function startWizard(claim) {
  live.stop();
  const sess = S.session || {};
  const W = { casters: null, wifi: null, system: null, joined: null };
  /* The claim (over the setup network): every step. Logged in: Wi-Fi only
   * while the phone is on the station's own network or the station has no
   * working Wi-Fi; the rest happens on the home network. */
  let steps = ['password', 'wifi', 'reception', 'onocoy', 'casters'];
  if (!claim) {
    let wifiFirst = !!sess.on_setup_ap;
    if (!wifiFirst) {
      try {
        W.wifi = await api('GET', '/api/wifi');
        wifiFirst = !W.wifi.configured || !W.wifi.online;
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) return;
        /* not known: the System page has the Wi-Fi settings */
      }
    }
    steps = (wifiFirst ? ['wifi'] : []).concat(['reception', 'onocoy', 'casters']);
  }

  function frame(step, title, ...body) {
    const idx = steps.indexOf(step);
    const head = idx >= 0 ? h('div', { class: 'wiz-head' },
      h('span', null, 'Setup · step ' + (idx + 1) + ' of ' + steps.length),
      h('div', { class: 'dots', 'aria-hidden': 'true' }, steps.map((s, i) => h('i', { class: i < idx ? 'done' : i === idx ? 'now' : null })))) : null;
    bare(h('div', { class: 'narrow' }, head, h('section', { class: 'card' }, h('h1', null, title), body)));
  }

  let stopCheck = null;   /* the reception check's polling, while it is shown */

  function after(step) {
    const i = steps.indexOf(step);
    return i >= 0 ? steps[i + 1] : steps[0];
  }

  /* The station keeps the next step, so the setup resumes on another network
   * or after a login. Not stored (settings unavailable this boot, no answer):
   * the order stays in this page's memory. false: the session ended. */
  async function saveStep(step) {
    try {
      await api('POST', '/api/setup/step', { step: step });
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return false;
    }
    return true;
  }

  async function next(step) {
    if (stopCheck) stopCheck();
    stopCheck = null;
    const following = after(step);
    if (!following) {
      finish(step);
      return;
    }
    if (await saveStep(following)) go(following);
  }

  /* The last step is done: setup_done, then the dashboard. */
  async function finish(step) {
    try {
      await setupDone();
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      frame(step, 'Setup', msg('bad', errText(e)), h('div', { class: 'form-actions' }, btn('Try again', () => finish(step), 'primary')));
      return;
    }
    boot();
  }

  async function loadBasics() {
    if (!W.system) {
      try {
        W.system = await api('GET', '/api/system');
        setBrand(W.system.hostname);
      } catch (e) {
        W.system = { hostname: (S.session && S.session.hostname) || 'onocoydiy' };
      }
    }
    W.casters = await api('GET', '/api/casters');
  }

  async function go(step) {
    try {
      if (step === 'password') return stepPassword();
      if (step === 'reception') return stepReception();
      if (step === 'onocoy' || step === 'casters') await loadBasics();
      if (step === 'onocoy') return stepOnocoy();
      if (step === 'casters') return stepCasters();
      if (step === 'wifi') {
        W.wifi = await api('GET', '/api/wifi');
        return stepWifi();
      }
      return stepConnect();
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return undefined;
      frame(step, 'Setup', msg('bad', errText(e)), h('div', { class: 'form-actions' }, btn('Try again', () => go(step), 'primary')));
    }
    return undefined;
  }

  /* Marks the setup finished. With the settings unavailable this boot the
   * flag cannot be stored (409 settings_unavailable); the setup goes on
   * regardless, and boot() then leaves the wizard for the dashboard. */
  async function setupDone() {
    try {
      await api('POST', '/api/setup/done');
    } catch (e) {
      if (!(e instanceof ApiError && e.code === 'settings_unavailable')) throw e;
    }
    return true;
  }

  function skipAll() {
    return h('p', { class: 'muted small foot' }, 'Experienced? ', h('button', { type: 'button', class: 'btn ghost small', onclick: async e => {
      const ok = await act(e.currentTarget, setupDone);
      if (ok !== undefined) {
        if (stopCheck) {
          stopCheck();
          stopCheck = null;
          api('DELETE', '/api/gnss/live').catch(() => {});   /* the reception check's view: it would close by itself */
        }
        boot();
      }
    } }, 'Skip the rest of the setup'));
  }

  /*
   * Reception check: opens the live view and shows plain hints about the
   * antenna's sky view before any caster streams. The guide values are in
   * RX_CHECK (66-live.js). Leaving the step closes the view again.
   */
  function stepReception() {
    const box = h('div', { class: 'stack' }, h('div', { class: 'spinner' }), h('p', { class: 'muted' }, 'Opening the live view…'));
    const err = errBox();
    const cont = btn('Continue', () => leave(cont), 'primary');
    const later = h('p', { class: 'muted small' });
    frame('reception', 'Reception check',
      h('p', null, 'Before the station streams to any caster, check what the antenna sees. Put it where it will stay, with a clear view of the sky, ' +
        'and watch the numbers settle for a minute or two. Moving the antenna shows its effect within seconds.'),
      box, err, later,
      h('div', { class: 'form-actions' }, cont),
      skipAll());

    let opened = false, stopped = false, closedShown = false;
    async function leave(b) {
      await busy(b, async () => {
        stopped = true;
        if (opened) {
          try {
            await api('DELETE', '/api/gnss/live');
          } catch (e) {
            /* closed already, or the station is busy: it closes by itself */
          }
        }
      });
      next('reception');
    }

    function render(d) {
      const t = d.totals;
      const r = receptionChecks(d);
      const w = d.window || {};
      if (r.waiting) {
        patch(box, h('div', { class: 'spinner' }), h('p', null, 'Waiting for the receiver\'s observations… (receiver: ' +
          lbl(L.station, d.receiver_state).toLowerCase() + ')'),
        h('p', { class: 'muted small' }, 'Right after power-on the receiver needs a minute or two to find satellites, and the station may still be configuring it.'));
      } else {
        patch(box, receptionList(r), liveTiles(t),
          h('div', { class: 'sky-wrap' }, liveSkyPlot(d.satellites || []), h('div', null, cnrLegend(), h('p', { class: 'muted small' }, SYS_KEY),
            liveCountsTable(t))));
      }
      cont.textContent = r.ok ? 'Continue' : 'Continue anyway';
      later.textContent = r.waiting || r.ok ? '' : 'The hints are rough guide values for a first check, not Onocoy\'s own scoring. ' +
        'You can open the live view any time later on the Signals page.';
      /* Built on the change only, so its button stays put between refreshes. */
      if (w.open === false && !closedShown) {
        closedShown = true;
        mount(err, 'The live view closed (' + (lbl(L.liveEnd, w.last_end) || 'time up') + '). ', btn('Open it again', reopen, 'small'));
        err.className = 'msg warn';
        err.hidden = false;
      } else if (w.open !== false && closedShown) {
        closedShown = false;
        err.hidden = true;
      }
    }

    async function reopen() {
      try {
        const d = await api('POST', '/api/gnss/live', {});
        opened = true;
        render(d);
      } catch (e) {
        closedShown = false;
        showErr(e, err);
      }
    }

    /* The step is gone when its box left the page: a lost session (login
     * screen, then a new wizard), "Skip the rest" while the first request
     * was still out, or anything else that replaced the page. Polling (which
     * keeps the view from closing as idle) ends then. */
    const gone = () => stopped || !box.isConnected;
    let myStop = null;
    const halt = () => {
      stopped = true;
      if (myStop) myStop();
      if (stopCheck === myStop) stopCheck = null;
    };
    (async () => {
      try {
        const d = await api('POST', '/api/gnss/live', {});
        if (gone()) {
          stopped = true;
          api('DELETE', '/api/gnss/live').catch(() => {});   /* nobody shows it: close it again */
          return;
        }
        render(d);
        opened = true;
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) return;
        if (gone()) return;
        patch(box, msg('warn', 'The live view could not be opened: ' + errText(e)),
          h('p', { class: 'muted small' }, 'You can check the reception later on the Signals page.'));
        cont.textContent = 'Continue';
        return;
      }
      myStop = poller(LIVE_POLL_MS, async () => {
        if (gone()) {
          halt();
          return;
        }
        const d = await api('GET', '/api/gnss/live');
        if (gone()) {
          halt();
          return;
        }
        render(d);
      }, true);
      stopCheck = myStop;
    })();
  }

  function stepPassword() {
    const pw = pwInput({ autocomplete: 'new-password' });
    const pw2 = pwInput({ autocomplete: 'new-password' });
    const f1 = field('Admin password', pw, '8 to 64 characters. You need it to change any setting later.');
    const f2 = field('Repeat the password', pw2);
    const err = errBox();
    const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Set password');
    frame('password', 'Welcome',
      h('p', null, 'This station is not set up yet. First choose the admin password for this page. There is no default password.'),
      h('form', { onsubmit: async e => {
        e.preventDefault();
        f1.error(null);
        f2.error(null);
        const problem = adminPasswordProblem(pw.value, pw2.value);
        if (problem) {
          (problem.indexOf('differ') >= 0 ? f2 : f1).error(problem);
          return;
        }
        const r = await act(submit, () => api('POST', '/api/setup/admin', { password: pw.value }), err);
        if (r === undefined) return;
        pw.value = '';
        pw2.value = '';
        S.session = await api('GET', '/api/session', undefined, { quiet401: true }).catch(() => S.session);
        if (r.persisted === false) toast('The settings storage is unavailable: the password holds until the next restart only.', 'warn');
        next('password');
      } }, f1.el, f2.el, err, h('div', { class: 'form-actions' }, submit)));
    pw.focus();
  }

  function stepOnocoy() {
    const data = W.casters;
    const preset = data.onocoy_preset || { host: 'servers.onocoy.com', port: 2101, ntrip_version: 1, backoff_profile: 'fast' };
    const slots = data.slots || [];
    if (slots.length && slots[0].config === null) {
      frame('onocoy', 'Onocoy', msg('warn', 'The casters cannot be set up in safe mode. Continue with Wi-Fi and set them up later.'),
        h('div', { class: 'form-actions' }, btn('Continue', () => next('onocoy'), 'primary')));
      return;
    }
    let slot = slots.find(s => s.config && isOnocoyHost(s.config.host));
    if (!slot) slot = slots.find(s => s.config && !s.config.host && !s.config.enabled);
    if (!slot) {
      frame('onocoy', 'Onocoy', msg('warn', 'Every caster slot is in use. Set up Onocoy later on the Casters page.'),
        h('div', { class: 'form-actions' }, btn('Continue', () => next('onocoy'), 'primary')));
      return;
    }
    const cfg = slot.config || {};
    const user = textInput(isOnocoyHost(cfg.host) ? cfg.mountpoint : '', { maxlength: 47 });
    const pw = pwInput({ placeholder: cfg.password_set ? 'Stored (leave empty to keep)' : '' });
    const fUser = field('Credential username', user, 'Generated by Onocoy when you create the credential.');
    const fPw = field('Credential password', pw, 'The password you chose for that credential.');
    const err = errBox();
    const dupBox = h('div');
    const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Save and continue');
    frame('onocoy', 'Onocoy',
      h('p', null, 'Onocoy rewards reference stations for their correction data. In the Onocoy console, open Reference Stations, ' +
        'then NTRIP Credentials, and add a credential: you pick the password and Onocoy generates the username. Enter both here.'),
      h('form', { onsubmit: async e => {
        e.preventDefault();
        fUser.error(null);
        fPw.error(null);
        const u = user.value.trim();
        if (!/^[A-Za-z0-9._-]{1,47}$/.test(u)) {
          fUser.error('Letters, digits, ".", "_" and "-" only, at most 47.');
          return;
        }
        if (!pw.value && !cfg.password_set) {
          fPw.error('Enter the credential password.');
          return;
        }
        if (/\s/.test(pw.value) || pw.value.length > 63) {
          fPw.error('At most 63 characters, without spaces.');
          return;
        }
        /* A slot that already points at Onocoy keeps its other settings (TLS, protocol). */
        const body = isOnocoyHost(cfg.host) ? { enabled: true, mountpoint: u } :
          { enabled: true, name: 'Onocoy', host: preset.host, port: preset.port, ntrip_version: preset.ntrip_version || 1,
            tls: false, trust: 'bundle', mountpoint: u, backoff_profile: 'fast' };
        if (pw.value) body.password = pw.value;
        const doSave = async (ack, button) => {
          if (ack) body.dup_stream_ack = true;
          err.hidden = true;
          try {
            await busy(button, () => saveCaster(slot.slot, body));
          } catch (ex) {
            if (ex instanceof ApiError && ex.code === 'caster_policy' && ex.body.reason === 'dup_stream_ack_required') {
              mount(dupBox, dupPanel(b => doSave(true, b)));
            } else {
              showErr(ex, err);
            }
            return;
          }
          pw.value = '';
          next('onocoy');
        };
        await doSave(false, submit);
      } }, fUser.el, fPw.el, dupBox, err, h('div', { class: 'form-actions' }, submit,
        btn('Skip for now', () => next('onocoy'), 'ghost'))),
      skipAll());
    user.focus();
  }

  function stepCasters() {
    const data = W.casters;
    const box = h('div', { class: 'stack' });
    const configured = (data.slots || []).filter(s => s.config && s.config.host);
    const free = (data.slots || []).filter(s => s.config && !s.config.host && !s.config.enabled);
    const editorBox = h('div');
    const render = () => {
      mount(box,
        configured.length ? h('div', { class: 'caster-list' }, configured.map(s => h('div', { class: 'caster' },
          h('div', { class: 'name' }, s.config.name || s.config.host), h('div', { class: 'muted small' },
            s.config.host + ':' + s.config.port + (s.config.enabled ? '' : ' · off'))))) : h('p', { class: 'muted' }, 'No caster set up yet.'),
        editorBox);
    };
    const addBtn = btn('Add a caster', () => {
      const target = free[0];
      if (!target) return;
      addBtn.hidden = true;
      mount(editorBox, casterEditor(target, {
        preset: data.onocoy_preset,
        hostname: W.system && W.system.hostname,
        hideOnocoy: configured.some(s => isOnocoyHost(s.config.host)),
        onSaved: async () => {
          W.casters = await api('GET', '/api/casters');
          stepCasters();
        },
        onCancel: () => {
          clear(editorBox);
          addBtn.hidden = false;
        },
      }).el);
    }, '', { hidden: !free.length });
    render();
    frame('casters', 'More casters (optional)',
      h('p', null, 'The station can stream to several casters at once, for example RTK2go or Centipede next to Onocoy. ' +
        'You can also do this later on the Casters page.'),
      box,
      h('div', { class: 'form-actions' }, addBtn, btn('Continue', () => next('casters'), 'primary')),
      skipAll());
  }

  /* The name the phone will open on the home network. */
  function localName(w) {
    const s = S.session || {};
    return ((w && w.mdns_host) || s.mdns_host || s.hostname || 'onocoydiy') + '.local';
  }

  function stepWifi() {
    const w = W.wifi;
    const s = S.session || {};
    const onAp = !!s.on_setup_ap;
    const following = after('wifi') || 'reception';
    const ap = (w.ap && w.ap.ssid) || s.ap_ssid || 'the setup network';
    const picker = wifiPicker(w, { warnScan: !onAp && !!w.online });
    const err = errBox();
    const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Join this network');
    const keep = w.configured && w.online ? btn('Keep ' + w.ssid, e => busy(e.currentTarget, () => next('wifi'))) : null;
    frame('wifi', 'Wi-Fi',
      h('p', null, 'Choose the Wi-Fi network the station uses to reach the casters.'),
      onAp ? h('p', null, 'When the station has joined your network, this setup network (' + ap + ') closes. Then connect this phone to your Wi-Fi and open ',
        h('strong', null, 'http://' + localName(w) + '/'), '. Log in with the admin password you just set; the setup continues there.') :
        h('p', null, 'The station leaves the network it uses now and joins the one you choose; this page then loses it. Open ',
          h('strong', null, 'http://' + localName(w) + '/'), ' on that network to continue the setup.'),
      w.configured ? msg('info', 'Now: ' + w.ssid + (w.online ? ' (connected, ' + w.ip + ')' : ' (not connected)')) : null,
      h('form', { onsubmit: async e => {
        e.preventDefault();
        const v = picker.value();
        if (!v) return;
        /* The step after the Wi-Fi first: the station keeps it while this page moves networks. */
        const r = await act(submit, async () => {
          await saveStep(following);
          return api('PUT', '/api/wifi', v);
        }, err);
        if (r === undefined) return;
        W.joined = { ssid: v.ssid, persisted: r.persisted !== false, applied: r.applied !== false, since: Date.now(), following: following };
        go('connect');
      } }, picker.el, err, h('div', { class: 'form-actions' }, submit, keep)));
  }

  /*
   * After PUT /api/wifi: polls GET /api/wifi every second. Connected and on
   * the setup network: the name and the address, the countdown to the
   * moment the setup network closes (ap.close_in_s, re-synced by every poll
   * and counted down here in between) and what to do on the home network.
   * Without a countdown (the page is not on the setup network, or the
   * build has none): Continue goes on in place.
   */
  function stepConnect() {
    const joined = W.joined || { ssid: W.wifi ? W.wifi.ssid : '', since: Date.now(), persisted: true, applied: true };
    const following = joined.following || after('wifi') || 'reception';
    const s = S.session || {};
    const onAp = !!s.on_setup_ap;
    const host = (W.system && W.system.hostname) || s.hostname || 'onocoydiy';
    const status = h('div', { class: 'stack' });
    frame('connect', 'Joining ' + joined.ssid, status);
    let fails = 0, stopped = false, shown = '', okNet = null, closeAt = 0, okSince = 0, ticker = 0;
    let leftEl = null, trialLeft = null;
    /* Re-render only when the situation changes, so buttons stay put. */
    const show = (mode, ...kids) => {
      if (mode !== shown) {
        shown = mode;
        mount(status, kids);
      }
    };
    const stop = () => {
      stopped = true;
      clearInterval(ticker);
    };
    const cont = () => {
      stop();
      go(following);
    };
    const apName = w => (w && w.ap && w.ap.ssid) || s.ap_ssid || 'the setup network';

    /* 1 join, 2 open, 3 log in: as text while the phone is still on the setup network, as links once it is closed. */
    function howTo(net, links) {
      const name = stationNameUrl(net), ip = stationIpUrl(net);
      const ref = u => (links ? h('a', { href: u }, u) : h('strong', null, u));
      const open = name && ip ? ['Open ', ref(name), ' (or ', ref(ip), ').'] : name ? ['Open ', ref(name), '.'] :
        ip ? ['Open ', ref(ip), '.'] : ['Find the station in your router\'s device list as ', h('strong', null, host), ' and open its address.'];
      return h('ol', { class: 'howto' },
        h('li', null, 'Connect this phone to ', h('strong', null, joined.ssid), '.'),
        h('li', null, open),
        h('li', null, 'Log in with the admin password; the setup continues with ' + (WIZ_NEXT[following] || 'the next step') + '.'));
    }

    function copyRow(net) {
      const url = stationUrl(net);
      if (!url) return null;
      const box = h('div', { class: 'copy-box' });
      return h('div', null, h('div', { class: 'form-actions' }, btn('Copy address', () => copyText(url, box), 'small')), box);
    }

    function closedView() {
      stop();
      show('closed', msg('info', h('strong', null, 'The setup network is closed.'), ' This phone goes back to its own Wi-Fi by itself, or join it now.'),
        howTo(okNet, true), copyRow(okNet),
        joined.persisted ? null : msg('warn', 'The settings storage is unavailable: the station forgets this network at the next restart.'));
    }

    function tick() {
      if (!leftEl || shown !== 'countdown') return;
      const left = Math.max(0, Math.ceil((closeAt - Date.now()) / 1000));
      leftEl.textContent = String(left);
      if (left === 0) closedView();
    }

    function okView(w) {
      okNet = w;
      if (!okSince) {
        okSince = Date.now();
        const title = status.parentElement && status.parentElement.querySelector('h1');
        if (title) title.textContent = 'Wi-Fi set up';
      }
      const ap = w.ap || {};
      const counting = onAp && ap.active && typeof ap.close_in_s === 'number';
      if (counting) {
        closeAt = Date.now() + ap.close_in_s * 1000;
        if (shown !== 'countdown') {
          leftEl = h('strong', { class: 'countdown-n' }, String(ap.close_in_s));
          show('countdown', msg('ok', h('strong', null, 'Connected to ' + joined.ssid + '.')),
            kv(stationAddressRows(w)),
            h('p', { class: 'countdown', role: 'status' }, 'The setup network ', h('strong', null, apName(w)), ' closes in ', leftEl, ' s.'),
            howTo(w, false), copyRow(w),
            joined.persisted ? null : msg('warn', 'The settings storage is unavailable: the station forgets this network at the next restart.'));
          clearInterval(ticker);
          ticker = setInterval(tick, 250);
        }
        tick();
        return;
      }
      if (shown === 'countdown' && !ap.active) {
        closedView();   /* closed before the count reached 0 */
        return;
      }
      /* On the setup network the countdown starts within a second of the
       * address; without one after a few seconds (or off that network) the
       * setup goes on from here. */
      if (onAp && ap.active && Date.now() - okSince < 3000) {
        show('ok-wait', msg('ok', h('strong', null, 'Connected to ' + joined.ssid + '.')), kv(stationAddressRows(w)), h('div', { class: 'spinner' }));
        return;
      }
      stop();
      show('ok', msg('ok', h('strong', null, 'Connected to ' + joined.ssid + '.')), kv(stationAddressRows(w)),
        joined.persisted ? null : msg('warn', 'The settings storage is unavailable: the station forgets this network at the next restart.'),
        h('div', { class: 'form-actions' }, btn('Continue', cont, 'primary')));
    }

    show('wait', h('div', { class: 'spinner' }), h('p', null, 'The station is joining ' + joined.ssid + '. This takes a few seconds.'));
    const loop = async () => {
      if (stopped || !status.isConnected) return;
      let w = null;
      try {
        w = await api('GET', '/api/wifi', undefined, { timeout: 3000, quiet401: true });
        fails = 0;
      } catch (e) {
        fails++;
      }
      if (stopped || !status.isConnected) return;
      if (okNet) {
        /* Connected already: only the countdown and the AP's end matter now. */
        if (w && w.online) okView(w);
        else if (!w && fails >= 2) closedView();
        if (!stopped) setTimeout(loop, 1000);
        return;
      }
      const secs = Math.round((Date.now() - joined.since) / 1000);
      if (w && joined.applied === false) {
        /* Stored, not used: this build keeps its link (the QEMU development build). */
        stop();
        show('stored', msg('info', h('strong', null, 'Saved.'), ' The station keeps its current connection (' + (w.ssid || 'unknown') + ').'),
          kv(stationAddressRows(w)), h('div', { class: 'form-actions' }, btn('Continue', cont, 'primary')));
        return;
      }
      /* The station switches about a second after PUT /api/wifi; until then
       * (and for a moment after) GET /api/wifi still describes the old
       * connection. Only the chosen network, past the switch, counts. */
      if (w && w.cred_reverted && w.pending_ssid === joined.ssid && !w.cred_trial) {
        /* Changed credentials that did not work: the station went back to its previous network. */
        const why = w.cred_reverted_why === 'auth_failed' ? joined.ssid + ' refused the new password: the station is back on its previous credentials.' :
          'No address on ' + joined.ssid + ' within 90 seconds: the station went back to ' + (w.ssid || 'its previous network') + '.';
        show('reverted:' + w.cred_reverted_why, msg('warn', h('strong', null, why), ' The new credentials stay pending.'),
          h('div', { class: 'form-actions' }, btn('Try them again', async e => {
            if (await retryPendingWifi(e)) {
              joined.since = Date.now();
              shown = '';
            }
          }, 'primary'), btn('Change Wi-Fi settings', () => {
            stop();
            go('wifi');
          })));
        setTimeout(loop, 1000);
        return;
      }
      if (w && w.cred_trial && w.pending_ssid === joined.ssid) {
        if (shown !== 'trial') {
          trialLeft = h('span', null, '');
          show('trial', h('div', { class: 'spinner' }), h('p', null, 'Trying the new credentials for ' + joined.ssid + ': ', trialLeft,
            ' left. Without an address there the station goes back to its previous network by itself.'));
        }
        trialLeft.textContent = fmt.dur(w.cred_trial_left_s);
      }
      if (w && w.online && w.ip && w.ssid === joined.ssid && !w.cred_trial && Date.now() - joined.since > 2000) {
        okView(w);
        if (!stopped) setTimeout(loop, 1000);
        return;
      }
      if (fails >= 3) {
        const name = 'http://' + localName(null) + '/';
        const ap = apName(W.wifi);
        show('lost', msg('info', h('strong', null, 'This phone lost the setup network.'),
          ' That is normal once the station joins ' + joined.ssid + ': its setup network moves to that network\'s channel and then closes.'),
        h('ol', { class: 'howto' },
          h('li', null, 'Connect this phone to ', h('strong', null, joined.ssid), '.'),
          h('li', null, 'Open ', h('a', { href: name }, name), ', or find the station in your router\'s device list as ', h('strong', null, host),
            ' and open its address.'),
          h('li', null, 'Log in with the admin password; the setup continues with ' + (WIZ_NEXT[following] || 'the next step') + '.')),
        h('p', { class: 'muted small' }, 'If the station could not join, ' + ap + ' comes back within a minute: join it and open http://192.168.4.1/.'),
        h('p', { class: 'muted small' }, 'On a guest network (devices there cannot see each other) the station cannot be reached from your phone: hold its BOOT ' +
          'button for 10 seconds. Its own network then opens for 15 minutes and the admin password is cleared; join it, set a new password and choose another network.'),
        h('div', { class: 'spinner' }));
      } else if (w && secs > 40 && !w.cred_trial && (w.connect_failures > 0 || w.last_disconnect_reason)) {
        show('failing:' + w.last_disconnect_reason, msg('warn', h('strong', null, 'Not connected yet.'), ' Last error: ' + wifiReason(w.last_disconnect_reason) + '.'),
          h('p', null, 'The station keeps trying. Check the network name and password.'),
          h('div', { class: 'form-actions' }, btn('Change Wi-Fi settings', () => {
            stop();
            go('wifi');
          }, 'primary')));
      }
      setTimeout(loop, 1000);
    };
    loop();
  }

  /* Resume where the station says the setup was (a login on the home network after the Wi-Fi step, a reload). */
  const resume = !claim && sess.setup_step && steps.indexOf(sess.setup_step) >= 0 ? sess.setup_step : steps[0];
  go(resume);
}
