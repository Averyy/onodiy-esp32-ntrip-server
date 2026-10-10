/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Start-up decisions (GET /api/session), log in, the public status view and
 * the first-run setup wizard.
 *
 * Wizard order follows the station: admin password (only over the station's
 * own access point), Onocoy credentials, optional extra casters, Wi-Fi last
 * (joining it ends the setup access point). It keeps its state in memory
 * only: captive-portal browsers may not keep browser storage.
 */

const RECOVERY_HINT = 'Forgot the password? Hold the station\'s BOOT button for 10 seconds. That clears the password and opens the ' +
  'station\'s own Wi-Fi network for 15 minutes; join it and set a new password.';

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
  S.session = s;
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
  if (!s.setup_done) {
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
  bare(h('div', { class: 'narrow' }, card('Set up this station',
    h('p', null, 'This station has no admin password yet. The first password can only be set over the station\'s own Wi-Fi network: ' +
      'join the network whose name starts with "onocoydiy-" and open http://192.168.4.1/.'),
    h('p', { class: 'muted' }, 'If that network is not there, hold the BOOT button for 10 seconds; the station then opens it for 15 minutes.'),
    h('div', { class: 'form-actions' }, btn('Check again', boot, 'primary')))));
}

function showLogin(note) {
  live.stop();
  S.publicView = false;
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
    h('p', { class: 'muted small foot' }, RECOVERY_HINT), pub)));
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
  const pwField = field('Wi-Fi password', pw, 'Leave it empty for an open network.');
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
      const out = { ssid: ssid };
      /* An unchanged network with an empty field keeps the stored password. */
      if (!(current && current.configured && ssid === current.ssid && p === '')) out.password = p;
      return out;
    },
  };
}

/* ---- Wizard ------------------------------------------------------------- */

function startWizard(claim) {
  live.stop();
  const steps = claim ? ['password', 'onocoy', 'casters', 'wifi'] : ['onocoy', 'casters', 'wifi'];
  const W = { casters: null, wifi: null, system: null, joined: null };

  function frame(step, title, ...body) {
    const idx = steps.indexOf(step);
    const head = idx >= 0 ? h('div', { class: 'wiz-head' },
      h('span', null, 'Setup · step ' + (idx + 1) + ' of ' + steps.length),
      h('div', { class: 'dots', 'aria-hidden': 'true' }, steps.map((s, i) => h('i', { class: i < idx ? 'done' : i === idx ? 'now' : null })))) : null;
    bare(h('div', { class: 'narrow' }, head, h('section', { class: 'card' }, h('h1', null, title), body)));
  }

  function next(step) {
    const i = steps.indexOf(step);
    go(steps[i + 1] || 'connect');
  }

  async function loadBasics() {
    if (!W.system) {
      try {
        W.system = await api('GET', '/api/system');
        setBrand(W.system.hostname);
      } catch (e) {
        W.system = { hostname: 'onocoydiy' };
      }
    }
    W.casters = await api('GET', '/api/casters');
  }

  async function go(step) {
    try {
      if (step === 'password') return stepPassword();
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

  function skipAll() {
    return h('p', { class: 'muted small foot' }, 'Experienced? ', h('button', { type: 'button', class: 'btn ghost small', onclick: async e => {
      const ok = await act(e.currentTarget, () => api('POST', '/api/setup/done'));
      if (ok !== undefined) boot();
    } }, 'Skip the rest of the setup'));
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

  function stepWifi() {
    const w = W.wifi;
    const picker = wifiPicker(w, { warnScan: false });
    const err = errBox();
    const submit = h('button', { type: 'submit', class: 'btn primary' }, 'Join this network');
    const keep = w.configured && w.online ? btn('Keep ' + w.ssid, async e => {
      const ok = await act(e.currentTarget, () => api('POST', '/api/setup/done'), err);
      if (ok !== undefined) boot();
    }) : null;
    frame('wifi', 'Wi-Fi',
      h('p', null, 'Last step: the Wi-Fi network the station uses to reach the casters. Joining it ends the setup network, so it comes last.'),
      w.configured ? msg('info', 'Now: ' + w.ssid + (w.online ? ' (connected, ' + w.ip + ')' : ' (not connected)')) : null,
      h('form', { onsubmit: async e => {
        e.preventDefault();
        const v = picker.value();
        if (!v) return;
        const r = await act(submit, async () => {
          await api('POST', '/api/setup/done');
          return api('PUT', '/api/wifi', v);
        }, err);
        if (r === undefined) return;
        W.joined = { ssid: v.ssid, persisted: r.persisted !== false, since: Date.now() };
        go('connect');
      } }, picker.el, err, h('div', { class: 'form-actions' }, submit, keep)));
  }

  function stepConnect() {
    const joined = W.joined || { ssid: W.wifi ? W.wifi.ssid : '', since: Date.now() };
    const host = (W.system && W.system.hostname) || 'onocoydiy';
    const status = h('div', { class: 'stack' });
    frame('connect', 'Joining ' + joined.ssid, status);
    let fails = 0, stopped = false, shown = '';
    /* Re-render only when the situation changes, so buttons stay put. */
    const show = (mode, ...kids) => {
      if (mode !== shown) {
        shown = mode;
        mount(status, kids);
      }
    };
    show('wait', h('div', { class: 'spinner' }), h('p', null, 'The station is joining ' + joined.ssid + '. This takes a few seconds.'));
    const loop = async () => {
      if (stopped) return;
      let w = null;
      try {
        w = await api('GET', '/api/wifi', undefined, { timeout: 3000, quiet401: true });
        fails = 0;
      } catch (e) {
        fails++;
      }
      const secs = Math.round((Date.now() - joined.since) / 1000);
      if (w && w.online && w.ip) {
        stopped = true;
        const addr = 'http://' + w.ip + '/';
        show('ok', msg('ok', h('strong', null, 'Connected.'), ' The station is now on ' + joined.ssid + '.'),
          kv([['Address', h('a', { href: addr }, addr)], ['Name', 'http://' + host + '.local/']]),
          h('p', null, 'Join ' + joined.ssid + ' with this device and open one of the addresses above. ' +
            'The setup network closes about a minute after the station connected.'),
          joined.persisted ? null : msg('warn', 'The settings storage is unavailable: the station forgets this network at the next restart.'),
          h('div', { class: 'form-actions' }, h('a', { class: 'btn primary', href: addr }, 'Open ' + addr), btn('Stay on this page', () => boot(), 'ghost')));
        return;
      }
      if (fails >= 3) {
        show('lost', msg('info', h('strong', null, 'This device lost the setup network.'),
          ' That is normal once the station joins ' + joined.ssid + ': its access point moves to that network\'s channel and closes a minute later.'),
        h('p', null, 'Join ' + joined.ssid + ' with this device and open http://' + host + '.local/ (or find the station\'s address in your router). ' +
          'If the station could not join, its setup network comes back: join it again and open http://192.168.4.1/.'),
        h('div', { class: 'spinner' }));
      } else if (w && secs > 40 && (w.connect_failures > 0 || w.last_disconnect_reason)) {
        show('failing:' + w.last_disconnect_reason, msg('warn', h('strong', null, 'Not connected yet.'), ' Last error: ' + wifiReason(w.last_disconnect_reason) + '.'),
          h('p', null, 'The station keeps trying. Check the network name and password.'),
          h('div', { class: 'form-actions' }, btn('Change Wi-Fi settings', () => {
            stopped = true;
            go('wifi');
          }, 'primary')));
      }
      setTimeout(loop, 1000);
    };
    loop();
  }

  go(steps[0]);
}
