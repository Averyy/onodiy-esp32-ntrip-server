/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Start-up decisions (GET /api/session), the login, the public status view
 * and the first-run setup wizard.
 *
 * Wizard: the admin password (over the station's own access point, the
 * setup hotspot), Wi-Fi (the hotspot closes once the station has joined;
 * the phone logs in again at http://<name>.local/), the antenna check,
 * Onocoy, and a last screen with Onocoy's live state and the next steps in
 * its console. The station keeps the next step (POST /api/setup/step: wifi, reception,
 * onocoy, casters = the last screen), so a reload or a login resumes there,
 * never past Wi-Fi while the station has none; the rest stays in memory
 * (captive-portal browsers may not keep storage).
 */

/* The setup hotspot (the station's own access point), as the session names it. */
function apSsidText() {
  return (S.session && S.session.ap_ssid) || '';
}

/* Settings' one line under the admin password (the login's Forgot password has the steps in full). */
function recoveryHint() {
  const ap = apSsidText();
  return 'Forgot it? Hold BOOT for 10 s, then join the setup hotspot' + (ap ? ' ' + ap : '') + ' and open http://192.168.4.1/.';
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
  bare(h('div', { class: 'narrow' }, pageCard('Set up this station',
    h('p', null, 'This station has no admin password yet. Set it over the setup hotspot:'),
    h('ol', { class: 'howto' },
      h('li', null, ap ? ['Join the setup hotspot ', h('strong', null, ap), '.'] : 'Join the setup hotspot (its name starts with onodiy-).'),
      h('li', null, 'Open ', h('strong', null, 'http://192.168.4.1/'), '.')),
    h('p', { class: 'muted small' }, 'Hotspot not there? Hold the BOOT button for 10 seconds; it opens for 15 minutes.'),
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
  const pw = pwInput({ autocomplete: 'off' });
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
      onclick: () => pick({ other: true }) }, h('span', { class: 'ssid' }, 'Other network…')));
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
        body: h('p', null, 'Streaming pauses for a few seconds while the station scans.') });
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

/* ---- Wizard ------------------------------------------------------------- */

/* What the step after the Wi-Fi is called in the instructions. */
const WIZ_NEXT = { reception: 'the antenna check', onocoy: 'the Onocoy step', casters: 'the last step' };

/* The setup's last screen: the next steps in the Onocoy console (Onocoy's guide). */
const ONOCOY_NEXT = ['Connect a wallet (top right).', 'Wait while Onocoy validates the station: 24 to 36 hours.',
  'Then press Register on this station\'s credential under NTRIP Credentials.', 'Rewards follow a day later: claim them with Claim Rewards.'];
const ONOCOY_REWARDS_GUIDE = 'https://docs.onocoy.com/documentation/3.-become-a-miner/4.-receive-rewards';

async function startWizard(claim) {
  live.stop();
  const sess = S.session || {};
  const W = { casters: null, wifi: null, system: null, joined: null };
  /* The claim (over the setup network): every step. After a login: Wi-Fi only
   * while the phone is on the station's own network or the station has no
   * working Wi-Fi; the rest happens on the home network. */
  let steps = ['password', 'wifi', 'reception', 'onocoy', 'casters'];
  /* The station has no working Wi-Fi (none stored, or not connected): the
   * setup cannot go past the Wi-Fi step, whatever step it stored. */
  let wifiDown = false;
  if (!claim) {
    try {
      W.wifi = await api('GET', '/api/wifi');
      wifiDown = !W.wifi.configured || !W.wifi.online;
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      /* not known: Settings has the Wi-Fi settings */
    }
    const wifiFirst = !!sess.on_setup_ap || wifiDown;
    steps = (wifiFirst ? ['wifi'] : []).concat(['reception', 'onocoy', 'casters']);
  }

  function frame(step, title, ...body) {
    const idx = steps.indexOf(step === 'connect' ? 'wifi' : step);
    const head = idx >= 0 ? h('div', { class: 'wiz-head' },
      h('span', null, 'Setup step ' + (idx + 1) + ' of ' + steps.length),
      h('div', { class: 'dots', 'aria-hidden': 'true' }, steps.map((s, i) => h('i', { class: i < idx ? 'done' : i === idx ? 'now' : null })))) : null;
    /* "Skip the rest" (skipAll) goes under the card. */
    const skip = body.filter(k => k && k.nodeType === 1 && k.classList.contains('wiz-skip'));
    bare(h('div', { class: 'narrow' }, head, pageCard(title, body.filter(k => skip.indexOf(k) < 0)), skip));
  }

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
    const following = after(step);
    if (!following) {
      finish(step);
      return;
    }
    if (await saveStep(following)) go(following);
  }

  /* The last step is done: setup_done, then the app at `hash` (the Status page, or NTRIP to add a server). */
  async function finish(step, hash) {
    try {
      await setupDone();
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      frame(step, 'Setup', msg('bad', errText(e)), h('div', { class: 'form-actions' }, btn('Try again', () => finish(step, hash), 'primary')));
      return;
    }
    if (hash) history.replaceState(null, '', hash);
    boot();
  }

  async function loadBasics() {
    if (!W.system) {
      try {
        W.system = await api('GET', '/api/system');
        setBrand(W.system.hostname);
      } catch (e) {
        W.system = { hostname: (S.session && S.session.hostname) || 'onodiy' };
      }
    }
    W.casters = await api('GET', '/api/casters');
  }

  async function go(step) {
    try {
      if (step === 'password') return stepPassword();
      if (step === 'reception') return stepReception();
      if (step === 'onocoy') {
        await loadBasics();
        return stepOnocoy();
      }
      if (step === 'casters') return stepDone();
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

  /* `guard` (optional): resolves true to go on (a step with unsaved input asks first). */
  function skipAll(guard) {
    return h('div', { class: 'wiz-skip' }, h('button', { type: 'button', class: 'btn ghost small', onclick: async e => {
      const b = e.currentTarget;
      if (guard && !(await guard())) return;
      const ok = await act(b, setupDone);
      /* The reception check's polling ends once its page is gone; a live
       * view it opened closes by itself after 120 s without a reader. */
      if (ok !== undefined) boot();
    } }, 'Skip the rest of the setup'));
  }

  /*
   * Antenna check: with no receiver (or while looking for it) the
   * no-receiver panel and nothing opened; once it streams the live view
   * with plain hints (RX_CHECK, 66-live.js), closed again on leaving. Going
   * on without the check is a secondary button.
   */
  function stepReception() {
    const box = h('div', { class: 'stack' }, h('div', { class: 'spinner' }), h('p', { class: 'muted' }, 'Checking the receiver…'));
    const err = errBox();
    const cont = btn('Continue', () => leave(cont), 'primary');
    const setCont = (text, main) => {
      cont.textContent = text;
      cont.className = 'btn' + (main ? ' primary' : '');
    };
    const note = h('p', { class: 'muted small', hidden: true });
    const later = h('p', { class: 'muted small' });
    frame('reception', 'Check the antenna',
      h('p', null, 'Put the antenna where it will stay, with a clear view of the sky. The checks below update within seconds.'),
      box, err, note,
      h('div', { class: 'form-actions' }, cont),
      later,
      skipAll());
    const setNote = t => {
      note.textContent = t;
      note.hidden = !t;
    };

    const nr = noReceiverPanel();
    let mode = '';             /* '', 'held', 'receiver' (no receiver, or looking for it), 'live' */
    let opened = false, stopped = false, closedShown = false;
    /* A build that holds the first configuration: what it would send, before the live view (once per visit). */
    let heldDone = false;
    const heldNext = () => {
      heldDone = true;
      mode = '';
      patch(box, h('div', { class: 'spinner' }), h('p', { class: 'muted' }, 'Starting the check…'));
      setCont('Continue', true);
      later.textContent = '';
      setNote('');
    };
    const held = heldPanel({ onLater: heldNext, onApplied: heldNext });
    let liveSince = 0, postAt = 0, closeIgnored = false;
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

    /* The step is gone when its box left the page: a lost session (login
     * screen, then a new wizard), "Skip the rest", or anything else that
     * replaced the page. Polling (which keeps the view from closing as
     * idle) ends then. */
    const gone = () => stopped || !box.isConnected;

    function showReceiverMissing(st) {
      if (mode !== 'receiver') {
        mode = 'receiver';
        mount(box, nr.el);
        err.hidden = true;
      }
      nr.update(st);
      setCont('Continue without the check', false);
      setNote('');
      later.textContent = 'You can check the antenna later on the Receiver page.';
    }

    function renderLive(d) {
      const t = d.totals;
      const r = receptionChecks(d);
      const w = d.window || {};
      if (r.waiting) {
        /* Streaming, but no satellites reported: after two minutes that is the antenna's business. */
        const long = liveSince && Date.now() - liveSince > 120000;
        patch(box, h('div', { class: 'spinner' }), h('p', null, 'Waiting for satellite data. Now: ' +
          lbl(L.station, d.receiver_state).toLowerCase() + '.'),
        long ? msg('warn', h('strong', null, 'No satellites reported for two minutes.'), h('div', { class: 'small' },
          'Check that the antenna cable is screwed on at both ends and that the antenna sees the open sky, not a roof or a wall.')) :
          h('p', { class: 'muted small' }, 'Right after power-on this takes a minute or two.'));
      } else {
        /* The sky view stays as the user left it (open or closed) across the refreshes. */
        const was = box.querySelector('details.sky-more');
        patch(box, receptionList(r),
          h('details', { class: 'more sky-more', open: was && was.open ? true : null }, h('summary', null, 'Sky view and satellite counts'),
            h('div', { class: 'stack' }, liveTiles(t),
              h('div', { class: 'sky-wrap' }, liveSkyPlot(d.satellites || []), h('div', null, cnrLegend(),
                h('p', { class: 'muted small' }, SYS_KEY), liveCountsTable(t))))));
      }
      setCont(r.ok ? 'Continue' : 'Continue anyway', true);
      setNote(r.waiting ? '' : 'These are rough guide values, not Onocoy\'s scoring. The sky is also on the Receiver page.');
      later.textContent = '';
      /* A 200 to our own start means open; a closed window read within 5 s
       * of it is a stale answer, ignored once. */
      if (w.open === false && Date.now() - postAt < 5000 && !closeIgnored) {
        closeIgnored = true;
        return;
      }
      /* Built on the change only, so its button stays put between refreshes. */
      if (w.open === false && !closedShown) {
        closedShown = true;
        mount(err, 'The sky view closed (' + (lbl(L.liveEnd, w.last_end) || 'time up') + '). ', btn('Open it again', openLive, 'small'));
        err.className = 'msg warn';
        err.hidden = false;
      } else if (w.open !== false && closedShown) {
        closedShown = false;
        err.hidden = true;
      }
    }

    async function openLive() {
      postAt = Date.now();
      closeIgnored = false;
      let d;
      try {
        d = await api('POST', '/api/gnss/live', {});
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) return;
        if (gone()) return;
        closedShown = false;
        if (mode !== 'live') {
          mode = 'failed';   /* not asked again every 2 s: the user goes on, or checks later on the Receiver page */
          patch(box, msg('warn', 'The sky view could not be opened: ' + errText(e)),
            h('p', { class: 'muted small' }, 'You can check the antenna later on the Receiver page.'));
          setCont('Continue', true);
        } else {
          showErr(e, err);
        }
        return;
      }
      opened = true;
      if (gone()) {
        api('DELETE', '/api/gnss/live').catch(() => {});   /* nobody shows it: close it again */
        return;
      }
      if (mode !== 'live') {
        mode = 'live';
        liveSince = Date.now();
      }
      renderLive(d);
    }

    async function tick() {
      if (gone() || mode === 'failed') return;
      if (mode === 'live') {
        const d = await api('GET', '/api/gnss/live');
        if (gone()) return;
        if (d.receiver_state === 'no_receiver' || d.receiver_state === 'detecting') {
          /* The receiver went away: say so (the view closes by itself). */
          const st = await api('GET', '/api/status');
          if (!gone()) showReceiverMissing(st);
          return;
        }
        renderLive(d);
        return;
      }
      const st = await api('GET', '/api/status');
      if (gone()) return;
      const rcv = st.receiver || {};
      if (rcv.first_apply_held && !heldDone && !rcv.rx_maintenance) {
        if (mode !== 'held') {
          mode = 'held';
          mount(box, held.el);
          held.load();
          err.hidden = true;
        }
        setCont('Continue without configuring', false);
        later.textContent = 'You can configure the receiver later on the Receiver page.';
        return;
      }
      if (rcv.state === 'no_receiver' || rcv.state === 'detecting' || (rcv.state === 'idle' && !rcv.rx_maintenance)) {
        showReceiverMissing(st);
      } else if (rcv.rx_maintenance) {
        mode = '';
        patch(box, msg('warn', 'Receiver maintenance is on, so the station sends the receiver nothing. Check the antenna after it ends.'));
        setCont('Continue without the check', false);
      } else {
        await openLive();
      }
    }

    let stopPoll = null;
    const loop = () => {
      if (gone()) {
        if (stopPoll) stopPoll();
        return Promise.resolve();
      }
      return tick();
    };
    stopPoll = poller(LIVE_POLL_MS, loop);
  }

  function stepPassword() {
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

  /* Onocoy: the server as text, a mountpoint name (any; the station's name to start with), the console's username and
   * the password chosen there, one line on sharing, the duplicate-stream question. */
  function stepOnocoy() {
    const data = W.casters;
    const preset = data.onocoy_preset || { host: 'servers.onocoy.com', port: 2101, ntrip_version: 1, backoff_profile: 'fast' };
    const slots = data.slots || [];
    if (slots.length && slots[0].config === null) {
      frame('onocoy', 'Connect to Onocoy', msg('warn', 'NTRIP servers cannot be set up in safe mode. Continue, and set up Onocoy later on the NTRIP page.'),
        h('div', { class: 'form-actions' }, btn('Continue', () => next('onocoy'), 'primary')));
      return;
    }
    let slot = slots.find(s => s.config && isOnocoySlot(s.config));
    if (!slot) slot = slots.find(s => s.config && !s.config.host && !s.config.enabled);
    if (!slot) {
      frame('onocoy', 'Connect to Onocoy', msg('warn', 'No free server slot. Set up Onocoy later on the NTRIP page.'),
        h('div', { class: 'form-actions' }, btn('Continue', () => next('onocoy'), 'primary')));
      return;
    }
    const cfg = slot.config || {};
    const ono = isOnocoySlot(cfg);
    /* A slot stored in the older layout (no username) holds the old credential in `mountpoint`: start empty. */
    const current = ono && cfg.username;
    /* In the console's words: the server (fixed, a line of text), mountpoint, username, password. The console makes
     * the username and the user picks the password there; the mountpoint is any name (Onocoy shows it only in its
     * dashboard), the station's name unless one is stored. A test Onocoy slot (bench and QEMU builds) keeps its own
     * host and port. */
    const name = String((W.system && W.system.hostname) || (S.session && S.session.hostname) || '').replace(/[^A-Za-z0-9._-]/g, '').slice(0, 47);
    const mpI = textInput(current && cfg.mountpoint ? cfg.mountpoint : name, { maxlength: 47 });
    const userI = textInput(current ? cfg.username : '', { maxlength: 47 });
    const pw = pwInput({ maxlength: 63, placeholder: ono && cfg.password_set ? 'Saved (leave empty to keep)' : '' });
    const F = { mountpoint: field('Mountpoint', mpI, 'Any name; shows only in your Onocoy dashboard.'), username: field('Username', userI),
      password: field('Password', pw) };
    /* Onocoy's password rule, checked softly: a note while the typed password breaks it, saving still allowed. */
    const pwNote = h('p', { class: 'small t-warn', role: 'status', hidden: true });
    pw.addEventListener('input', () => {
      pwNote.textContent = onocoyPwNote(pw.value);
      pwNote.hidden = !pwNote.textContent;
    });
    const err = errBox();
    const dupBox = h('div');
    const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Save and continue');
    frame('onocoy', 'Connect to Onocoy',
      h('p', null, 'In the Onocoy console, open Reference Stations, NTRIP Credentials and pick or add this station\'s credential. ' +
        'Enter its username and the password you chose. ', extLink('Onocoy\'s guide', PRESET_INFO.onocoy.guide[1])),
      /* A change to the values asks again; ticking the panel's own box is its answer, not a change. */
      h('form', { oninput: e => {
        if (dupBox.contains(e.target)) return;
        clear(dupBox);
        submit.hidden = false;
      }, onsubmit: async e => {
        e.preventDefault();
        for (const k of Object.keys(F)) F[k].error(null);
        const v = { mountpoint: mpI.value.trim(), username: userI.value.trim(), password: pw.value };
        const pr = onocoyProblems(v, true, ono && cfg.password_set);
        const bad = Object.keys(F).filter(k => pr[k]);
        if (bad.length) {
          for (const k of bad) F[k].error(pr[k]);
          F[bad[0]].input.focus();
          return;
        }
        /* A slot that already points at Onocoy keeps its other settings (TLS, protocol). */
        const body = ono ? { enabled: true, mountpoint: v.mountpoint, username: v.username } :
          { enabled: true, name: 'Onocoy', host: preset.host, port: preset.port, ntrip_version: preset.ntrip_version || 1,
            tls: false, trust: 'bundle', mountpoint: v.mountpoint, username: v.username, backoff_profile: 'fast' };
        if (v.password) body.password = v.password;
        const doSave = async (ack, button) => {
          if (ack) body.dup_stream_ack = true;
          err.hidden = true;
          try {
            await busy(button, () => saveCaster(slot.slot, body));
          } catch (ex) {
            if (ex instanceof ApiError && ex.code === 'caster_policy' && ex.body.reason === 'dup_stream_ack_required') {
              mount(dupBox, dupPanel(b => doSave(true, b)));
              submit.hidden = true;
            } else if (ex instanceof ApiError && ex.code === 'invalid_field' && F[ex.field]) {
              F[ex.field].error(ruleText(ex));
              F[ex.field].input.focus();
            } else {
              showErr(ex, err);
            }
            return;
          }
          pw.value = '';
          next('onocoy');
        };
        await doSave(false, submit);
      } }, serverLine(null, ono ? cfg.host : preset.host, ono ? cfg.port : preset.port),
      F.mountpoint.el, F.username.el, F.password.el, pwNote,
      h('p', { class: 'small t-warn' }, 'Never use a credential another device is streaming with.'),
      dupBox, err, h('div', { class: 'form-actions' }, btn('Skip for now', () => next('onocoy')), submit)),
      skipAll());
    (mpI.value ? userI : mpI).focus();
  }

  /*
   * The last screen (stored step "casters"): Onocoy's state as it happens
   * (onocoyNow), the next steps in the Onocoy console, a link to NTRIP and
   * "Go to Status"; both mark the setup done. GET /api/casters only when
   * Onocoy's state changes (the count of refused logins).
   */
  function stepDone() {
    const row = h('ul', { class: 'checks', 'aria-live': 'polite' });
    const nextBox = h('div', null, h('h2', { class: 'sub' }, 'Next, in the Onocoy console'),
      h('ol', { class: 'howto' }, ONOCOY_NEXT.map(t => h('li', null, t))), extLink('Onocoy\'s guide', ONOCOY_REWARDS_GUIDE));
    const add = h('a', { href: '#/ntrip', onclick: e => {
      e.preventDefault();
      finish('casters', '#/ntrip');
    } }, 'Add RTK2go or another server');
    const goStatus = btn('Go to Status', e => busy(e.currentTarget, () => finish('casters', '#/')), 'primary');
    frame('casters', 'Setup done', row, nextBox, h('p', null, add), h('div', { class: 'form-actions' }, goStatus));
    const icon = { ok: '✓', warn: '!', bad: '✕', info: '…' };
    let shown = '', rejects = null, seen = '';
    const gone = () => !row.isConnected;
    const back = async () => {
      if (await saveStep('onocoy')) go('onocoy');
    };
    const render = o => {
      const key = JSON.stringify(o);
      if (key === shown) return;
      shown = key;
      mount(row, h('li', { class: o.tone }, h('span', { class: 'ic', 'aria-hidden': 'true' }, icon[o.tone]),
        h('span', null, h('strong', null, o.head), o.text ? h('div', { class: 'muted small' }, o.text) : null,
          o.act ? h('div', { class: 'form-actions' }, btn(o.act === 'edit' ? 'Edit' : 'Set up Onocoy', back, 'small')) : null)));
      nextBox.hidden = o.key === 'setup' || o.key === 'safe_mode' || o.key === 'wait';
    };
    render({ key: 'wait', tone: 'info', head: 'Checking Onocoy…', text: '', act: null });
    let stop = null;
    stop = poller(LIVE_POLL_MS, async () => {
      if (gone()) {
        if (stop) stop();
        return;
      }
      const st = await api('GET', '/api/status');
      const c = onocoySlot(st);
      const now = c ? c.state + '|' + (c.hint || '') : '';
      if (c && c.state === 'auth_failed' && now !== seen) {
        try {
          const d = await api('GET', '/api/casters');
          const sl = (d.slots || []).find(x => x.slot === c.slot);
          rejects = sl && sl.status ? sl.status.login_rejects || null : null;
        } catch (e) {
          rejects = null;
        }
      }
      seen = now;
      if (!gone()) render(onocoyNow(st, rejects));
    });
  }

  /* The name the phone will open on the home network, http://<mdns_host>.local/,
   * or null while the station's mDNS responder does not run (the name would
   * not answer): then its address, or the router's device list, finds it. */
  function localUrl(w) {
    return stationNameUrl(w || S.session);
  }

  /* The DHCP name, as the router's device list shows the station. */
  function routerName() {
    return (W.system && W.system.hostname) || (S.session && S.session.hostname) || 'onodiy';
  }

  function stepWifi() {
    const w = W.wifi;
    const s = S.session || {};
    const onAp = !!s.on_setup_ap;
    const following = after('wifi') || 'reception';
    const picker = wifiPicker(w, { warnScan: !onAp && !!w.online });
    const err = errBox();
    const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Join this network');
    const keep = w.configured && w.online ? btn('Keep using ' + w.ssid, e => busy(e.currentTarget, () => next('wifi'))) : null;
    const url = localUrl(w);
    /* Without a name that answers: the address, which the next screen shows once the station has joined. */
    frame('wifi', 'Connect to Wi-Fi',
      h('p', null, 'Pick the network the station uses to reach the internet.'),
      onAp ? h('p', null, 'Once the station has joined it, the setup hotspot closes. Then connect this phone to the same Wi-Fi, open ',
        url ? h('strong', null, url) : 'the address the next screen shows', ' and login to continue.') :
        h('p', null, 'The station switches to the network you pick, and this page loses it. Then ',
          url ? ['open ', h('strong', null, url), ' on that network'] : ['find ', h('strong', null, routerName()),
            ' in your router\'s device list and open its address'], ' to continue.'),
      w.configured ? h('p', { class: 'muted small' }, w.online ? 'Now on ' + w.ssid + (w.ip ? ' (' + w.ip + ')' : '') + '.' :
        'Saved network: ' + w.ssid + ' (not connected).') : null,
      h('form', { onsubmit: async e => {
        e.preventDefault();
        const v = picker.value();
        if (!v) return;
        /* The step after the Wi-Fi first: the station keeps it while this page
         * moves networks. A join that was not sent puts the Wi-Fi step back. */
        let stepSaved = false;
        const r = await act(submit, async () => {
          stepSaved = await saveStep(following);
          return api('PUT', '/api/wifi', v);
        }, err);
        if (r === undefined) {
          if (stepSaved) saveStep('wifi');
          return;
        }
        W.joined = { ssid: v.ssid, persisted: r.persisted !== false, applied: r.applied !== false, since: Date.now(), following: following };
        go('connect');
      } }, picker.el, err, h('div', { class: 'form-actions' }, keep, submit)));
  }

  /*
   * After PUT /api/wifi: polls GET /api/wifi every second. Connected on the
   * setup network: name, address, the countdown to its closing (ap.close_in_s,
   * re-synced by each poll) and what to do next; without a countdown,
   * Continue goes on in place.
   */
  function stepConnect() {
    const joined = W.joined || { ssid: W.wifi ? W.wifi.ssid : '', since: Date.now(), persisted: true, applied: true };
    const following = joined.following || after('wifi') || 'reception';
    const s = S.session || {};
    const onAp = !!s.on_setup_ap;
    const host = (W.system && W.system.hostname) || s.hostname || 'onodiy';
    const status = h('div', { class: 'stack' });
    frame('connect', 'Joining ' + joined.ssid, status);
    let fails = 0, stopped = false, shown = '', okNet = null, closeAt = 0, okSince = 0, ticker = 0;
    let leftEl = null, trialLeft = null, stepBack = false, lastNet = null;
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
    const apName = w => (w && w.ap && w.ap.ssid) || s.ap_ssid || 'the setup hotspot';

    /* 1 join, 2 open, 3 login: as text while the phone is still on the setup network, as links once it is closed. */
    function howTo(net, links) {
      const name = stationNameUrl(net), ip = stationIpUrl(net);
      const ref = u => (links ? h('a', { href: u }, u) : h('strong', null, u));
      const open = name && ip ? ['Open ', ref(name), ' (or ', ref(ip), ').'] : name ? ['Open ', ref(name), '.'] :
        ip ? ['Open ', ref(ip), '.'] : ['Find the station in your router\'s device list as ', h('strong', null, host), ' and open its address.'];
      return h('ol', { class: 'howto' },
        h('li', null, 'Connect this phone to ', h('strong', null, joined.ssid), '.'),
        h('li', null, open),
        h('li', null, 'Login with the admin password; the setup continues with ' + (WIZ_NEXT[following] || 'the next step') + '.'));
    }

    function copyRow(net) {
      const url = stationUrl(net);
      if (!url) return null;
      const box = h('div', { class: 'copy-box' });
      return h('div', null, h('div', { class: 'form-actions' }, btn('Copy address', () => copyText(url, box), 'small')), box);
    }

    function closedView() {
      stop();
      show('closed', msg('info', h('strong', null, 'The setup hotspot is closed.'), ' This phone goes back to its usual Wi-Fi by itself, or join it now.'),
        howTo(okNet, true), copyRow(okNet),
        joined.persisted ? null : msg('warn', 'Settings cannot be saved right now: the station forgets this network when it restarts.'));
    }

    function tick() {
      if (!leftEl || shown !== 'countdown') return;
      const left = Math.max(0, Math.ceil((closeAt - Date.now()) / 1000));
      leftEl.textContent = String(left);
      if (left === 0) closedView();
    }

    const setTitle = text => {
      const title = status.parentElement && status.parentElement.querySelector('h1');
      if (title) title.textContent = text;
    };

    function okView(w) {
      okNet = w;
      if (!okSince) {
        okSince = Date.now();
        setTitle('Wi-Fi set up');
      }
      if (stepBack) {
        stepBack = false;
        saveStep(following);   /* joined after all: the setup goes on from the next step */
      }
      const ap = w.ap || {};
      const counting = onAp && ap.active && typeof ap.close_in_s === 'number';
      if (counting) {
        closeAt = Date.now() + ap.close_in_s * 1000;
        if (shown !== 'countdown') {
          leftEl = h('strong', { class: 'countdown-n' }, String(ap.close_in_s));
          show('countdown', msg('ok', h('strong', null, 'Connected to ' + joined.ssid + '.')),
            kv(stationAddressRows(w)),
            h('p', { class: 'countdown', role: 'status' }, 'The setup hotspot ', h('strong', null, apName(w)), ' closes in ', leftEl, ' s.'),
            howTo(w, false), copyRow(w),
            joined.persisted ? null : msg('warn', 'Settings cannot be saved right now: the station forgets this network when it restarts.'));
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
        joined.persisted ? null : msg('warn', 'Settings cannot be saved right now: the station forgets this network when it restarts.'),
        h('div', { class: 'form-actions' }, btn('Continue', cont, 'primary')));
    }

    show('wait', h('div', { class: 'spinner' }), h('p', null, 'The station is joining ' + joined.ssid + '. This takes a few seconds.'));
    const loop = async () => {
      if (stopped || !status.isConnected) return;
      let w = null;
      try {
        w = await api('GET', '/api/wifi', undefined, { timeout: 3000, quiet401: true });
        fails = 0;
        lastNet = w;
      } catch (e) {
        fails++;
      }
      if (stopped || !status.isConnected) return;
      if (okNet) {
        /* Connected already: only the countdown and the AP's end matter now. */
        if (w && w.online) {
          okView(w);
        } else if (w) {
          /* The station lost the network again (within the setup network's
           * last seconds): the setup network stays open as the fallback and
           * the countdown is void. Back to waiting for the join.
           * PROVISIONAL: to be confirmed on hardware (a home network that
           * drops the station within the 15 s). */
          clearInterval(ticker);
          ticker = 0;
          leftEl = null;
          okNet = null;
          okSince = 0;
          joined.since = Date.now();
          setTitle('Joining ' + joined.ssid);
          show('relost', msg('warn', h('strong', null, 'The station lost ' + joined.ssid + ' again.'),
            ' Its setup hotspot (' + apName(w) + ') stays open while it reconnects; stay on it.'), h('div', { class: 'spinner' }));
        } else if (fails >= 2) {
          closedView();
        }
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
        const back = ' The station is back on ' + (w.ssid || 'the previous network') + '.';
        const why = (w.cred_reverted_why === 'auth_failed' ? joined.ssid + ' refused the password.' :
          'No address on ' + joined.ssid + ' within 90 seconds.') + back;
        if (shown !== 'reverted:' + w.cred_reverted_why) {
          stepBack = true;
          saveStep('wifi');   /* a reload or a new login comes back here */
        }
        show('reverted:' + w.cred_reverted_why, msg('warn', why),
          h('div', { class: 'form-actions' }, btn('Change Wi-Fi settings', () => {
            stop();
            go('wifi');
          }), btn('Try again', async e => {
            if (await retryPendingWifi(e)) {
              joined.since = Date.now();
              shown = '';
            }
          }, 'primary')));
        setTimeout(loop, 1000);
        return;
      }
      if (w && w.cred_trial && w.pending_ssid === joined.ssid) {
        if (shown !== 'trial') {
          trialLeft = h('span', null, '');
          show('trial', h('div', { class: 'spinner' }), h('p', null, 'Trying ' + joined.ssid + ': ', trialLeft,
            ' left. If it gets no address, the station goes back to the previous network by itself.'));
        }
        trialLeft.textContent = fmt.dur(w.cred_trial_left_s);
      }
      if (w && w.online && w.ip && w.ssid === joined.ssid && !w.cred_trial && Date.now() - joined.since > 2000) {
        okView(w);
        if (!stopped) setTimeout(loop, 1000);
        return;
      }
      if (fails >= 3) {
        const name = localUrl(lastNet || W.wifi);
        const ap = apName(W.wifi);
        show('lost', msg('info', h('strong', null, 'This phone left the setup hotspot'),
          h('div', null, 'That is normal once the station has joined ' + joined.ssid + '.')),
        h('ol', { class: 'howto' },
          h('li', null, 'Connect this phone to ', h('strong', null, joined.ssid), '.'),
          name ? h('li', null, 'Open ', h('a', { href: name }, name), ', or find the station in your router\'s device list as ', h('strong', null, host),
            ' and open its address.') :
            h('li', null, 'Find the station in your router\'s device list as ', h('strong', null, host), ' and open its address.'),
          h('li', null, 'Login with the admin password; the setup continues with ' + (WIZ_NEXT[following] || 'the next step') + '.')),
        h('p', { class: 'muted small' }, 'If the station could not join, ' + ap + ' comes back within a minute: join it and open http://192.168.4.1/.'),
        h('p', { class: 'muted small' }, 'On a guest network your phone cannot reach the station. Hold its BOOT button for 10 seconds to open ' +
          'the setup hotspot again (this also clears the admin password), then pick another network.'),
        h('div', { class: 'spinner' }));
      } else if (w && secs > 40 && !w.cred_trial && (w.connect_failures > 0 || w.last_disconnect_reason)) {
        if (shown.indexOf('failing:') !== 0) {
          stepBack = true;
          saveStep('wifi');   /* a reload or a new login comes back here */
        }
        show('failing:' + w.last_disconnect_reason, msg('warn', h('strong', null, 'Not connected yet'),
          h('div', null, 'Last error: ' + wifiReason(w.last_disconnect_reason, true) + '. The station keeps trying. Check the network name and password.')),
          h('div', { class: 'form-actions' }, btn('Change Wi-Fi settings', () => {
            stop();
            go('wifi');
          }, 'primary')));
      }
      setTimeout(loop, 1000);
    };
    loop();
  }

  /* Resume where the station says (after a login or a reload), never past
   * Wi-Fi while the station has none: the next step is stored before the
   * join is sent, and a join that failed leaves it there. */
  let resume = !claim && sess.setup_step && steps.indexOf(sess.setup_step) >= 0 ? sess.setup_step : steps[0];
  if (!claim && wifiDown) resume = 'wifi';
  go(resume);
}
