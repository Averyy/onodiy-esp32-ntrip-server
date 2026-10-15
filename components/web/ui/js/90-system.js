/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * System: Wi-Fi, the station's own Wi-Fi (its access point), station name
 * and public status, the admin password, appearance, about, and restart,
 * safe mode and factory reset.
 */

function hostnameProblem(v) {
  if (!/^[A-Za-z0-9-]{1,32}$/.test(v)) return '1 to 32 letters, digits and "-".';
  if (v.charAt(0) === '-' || v.charAt(v.length - 1) === '-') return 'It cannot start or end with "-".';
  return null;
}

/* AP password: 8-63 printable ASCII characters (WPA2 passphrase, the station's rule). */
function apPasswordProblem(v) {
  if (/^[\x20-\x7e]{8,63}$/.test(v)) return null;
  if (v.length < 8 || v.length > 63) return '8 to 63 characters.';
  return 'Only plain letters, digits, spaces and symbols (no accents or emoji).';
}

/* How long the station tries new Wi-Fi settings before it goes back to the previous ones. */
const TRIAL_S = 90;

/* "the new password for Home" when only the password changes, else "Cafe". */
function newCredsText(w, ssid) {
  const p = w.pending_ssid || 'the new network';
  return ssid && p === ssid ? 'the new password for ' + p : p;
}

/* The Wi-Fi card's chip: [text, tone]. */
function wifiChip(w) {
  if (!w.configured) return ['Not set up', 'plain'];
  if (w.online) return ['Online', 'ok'];
  return w.associated ? ['Connecting', 'warn'] : ['Offline', 'bad'];
}

/* The station's address rows (stationAddressRows) in this page's words. */
function addressRows(net, prefix) {
  const names = { Name: 'Web address', Address: 'IP address' };
  return stationAddressRows(net).map(r => [r[0] ? (prefix || '') + (names[r[0]] || r[0]) : '', r[1]]);
}

/* What a normal user reads about the Wi-Fi; the rest (detailOpen: the disclosure as the user left it) under Details. */
function wifiStatus(w, detailOpen) {
  return [
    kv([
      ['Network', w.configured ? w.ssid : 'Not set up'],
      w.credentials_unreadable ? ['Saved Wi-Fi settings', h('span', { class: 't-bad' }, 'Unreadable: choose the network again')] : null,
      w.online && w.rssi < 0 ? ['Signal', rssiView(w.rssi, true)] : null,
      ...(w.online ? addressRows(w) : [['Offline for', fmt.dur(w.offline_s)]]),
      w.retry_in_s && !w.online ? ['Next attempt in', fmt.dur(w.retry_in_s)] : null,
      ['Internet', lbl(L.upstream, w.upstream)],
    ]),
    disclosure('Details', detailOpen, kv([
      w.associated ? ['Signal strength', w.rssi ? w.rssi + NB + 'dBm' : DASH] : null,
      w.associated ? ['Channel', String(w.channel)] : null,
      ['Clock synced', fmt.yesno(w.time_synced)],
      ['Reconnects / failures', fmt.num(w.reconnects) + ' / ' + fmt.num(w.connect_failures)],
      ['Last disconnect', wifiReason(w.last_disconnect_reason)],
      w.probe_age_s !== null && w.probe_age_s !== undefined ? ['Last internet check', fmt.dur(w.probe_age_s) + ' ago'] : null,
      w.fallback_dns ? ['DNS', 'Using the fallback servers'] : null,
      w.associated && !w.online && w.assoc_no_ip_s ? ['Without an address for', fmt.dur(w.assoc_no_ip_s)] : null,
      w.ladder && (w.ladder.triggers || []).length ? ['Network recovery', 'running: ' + w.ladder.triggers.map(t => lbl(L.trigger, t)).join(', ')] : null,
    ])),
  ];
}

/* The station's own Wi-Fi (its access point) in a few rows. */
function apStatus(w) {
  const ap = w.ap || {};
  const close = typeof ap.close_in_s === 'number' ? ap.close_in_s : ap.recovery_left_s || null;
  return kv([
    ['Network name', ap.ssid],
    ap.active ? ['Open because', (ap.reasons || []).map(r => lbl(L.apReason, r)).join(', ') || DASH] : null,
    ap.active ? ['Address on it', h('a', { href: 'http://192.168.4.1/' }, 'http://192.168.4.1/')] : null,
    ap.active && w.mdns_host ? ['Web address on it', h('a', { href: 'http://' + w.mdns_host + '.local/' }, 'http://' + w.mdns_host + '.local/')] : null,
    ap.active ? ['Connected devices', fmt.num(ap.clients)] : null,
    close ? ['Closes in', fmt.dur(close)] : null,
    ['Password', ap.password_set ? 'Set' : h('span', { class: 't-warn' }, 'None: anyone nearby can join')],
  ]);
}

/* True while an Onocoy caster is set up and on: a restart then costs Onocoy offline time. */
function onocoyStreams(st) {
  return ((st && st.casters) || []).some(c => c.protected && c.enabled && c.onocoy_ready !== false);
}

Views.system = {
  title: 'System',
  admin: true,
  create(el) {
    let wifi = null, sys = null, stopJoin = null;
    let wifiAt = 0;   /* when `wifi` arrived */
    let dirty = false;
    const wifiCard = h('section', { class: 'card' });
    const wifiBody = h('div');
    const wifiBadge = badge('', '');
    const apCard = h('section', { class: 'card' });
    const apBody = h('div');
    const apBadge = badge('', '');
    const showWifi = w => {
      wifi = w;
      wifiAt = Date.now();
      const chip = wifiChip(w);
      wifiBadge.className = 'badge ' + chip[1];
      wifiBadge.textContent = chip[0];
      const was = wifiBody.querySelector('details');
      mount(wifiBody, wifiStatus(w, was && was.open));
      const ap = w.ap || {};
      apBadge.className = 'badge ' + (ap.active ? 'info' : '');
      apBadge.textContent = ap.active ? 'On' : 'Off';
      mount(apBody, apStatus(w));
      apClear.hidden = !ap.password_set;
      showPending(w);
    };
    const joinBox = h('div');
    const pendingBox = h('div');
    let pendingKey = '';
    let joinPrev = '';   /* the network before a change made on this page */
    /*
     * Seconds left of a settings trial: the last GET /api/wifi's
     * cred_trial_left_s, counted down here from when it arrived, so the
     * countdown goes on while the station is out of reach (during a trial it
     * usually is). Every .trial-left on this page shows it.
     */
    const trialLeftText = () => {
      const left = Math.max(0, Math.ceil(((wifi && wifi.cred_trial_left_s) || 0) - (Date.now() - wifiAt) / 1000));
      return left > 0 ? fmt.dur(left) + ' left' : 'ending now';
    };
    const tickTrial = () => {
      if (!wifi || !wifi.cred_trial) return;
      const text = trialLeftText();
      for (const t of el.querySelectorAll('.trial-left')) {
        if (t.textContent !== text) t.textContent = text;
      }
    };
    /* role timer: a screen reader does not read every second out. */
    const trialLeftEl = () => h('span', { class: 'trial-left', role: 'timer' }, trialLeftText());
    /* New settings on trial or pending: rebuilt only when that changes, so its buttons stay put. */
    const showPending = w => {
      const key = [w.cred_trial, w.cred_pending, w.cred_reverted, w.cred_reverted_why, w.pending_ssid, w.ssid].join('|');
      if (key === pendingKey) {
        tickTrial();
        return;
      }
      pendingKey = key;
      const back = 'The station is back on ' + (w.ssid || 'the previous network') + '. Try again, or discard the new settings.';
      if (w.cred_trial) {
        /* No buttons while a trial runs: it ends by itself, either way. */
        mount(pendingBox, msg('info', h('strong', null, 'Trying ' + newCredsText(w, joinPrev) + ': '), trialLeftEl(),
          h('div', { class: 'small' }, 'If it gets no address there, the station goes back to the previous network by itself.')));
      } else if (w.cred_pending) {
        const retry = btn('Try again', async e => {
          const ssid = w.pending_ssid, prev = w.ssid;
          if (await retryPendingWifi(e)) {
            /* The same trial as a change made here: follow it to its end. */
            joinPrev = prev || '';
            pendingKey = '';
            changeBtn.hidden = true;
            watchJoin(ssid, true, true, prev);
          }
        }, 'primary');
        const discard = btn('Discard', async e => {
          if (await discardPendingWifi(e)) {
            /* Gone at once; the reload confirms it. */
            pendingKey = '';
            clear(pendingBox);
            load().catch(() => {});
          }
        });
        const pend = w.pending_ssid || 'the new network';
        const title = w.cred_reverted && w.cred_reverted_why === 'auth_failed' ? pend + ' refused the new password.' :
          w.cred_reverted ? 'No address on ' + pend + '.' : 'New settings for ' + pend + ' not confirmed yet.';
        mount(pendingBox, msg('warn', h('strong', null, title), h('div', { class: 'small' },
          w.cred_reverted ? back : 'They take over once the station gets an address there.'),
        h('div', { class: 'form-actions' }, discard, retry)));
      } else {
        clear(pendingBox);
      }
    };
    const deviceCard = h('section', { class: 'card' });
    const pwCard = h('section', { class: 'card' });
    const aboutCard = h('section', { class: 'card' });
    const lookCard = h('section', { class: 'card' });
    const maintCard = h('section', { class: 'card wide' });

    /* ---- Wi-Fi ---- */
    const changeBtn = btn('Change network', () => {
      changeBtn.hidden = true;
      const picker = wifiPicker(wifi, { warnScan: true });
      const err = errBox();
      const go = h('button', { type: 'submit', class: 'btn primary' }, 'Join');
      mount(joinBox, h('form', { class: 'subcard', oninput: () => {
        dirty = true;
      }, onsubmit: async e => {
        e.preventDefault();
        const v = picker.value();
        if (!v) return;
        const ap = (wifi.ap || {}).ssid || 'its own Wi-Fi';
        const ok = await dialog({ title: 'Join ' + v.ssid + '?', ok: 'Join',
          body: [h('p', null, 'Casters drop for a moment while the station changes networks.'),
            h('p', { class: 'muted' }, wifi.configured ? 'If it gets no address there within ' + TRIAL_S + ' seconds, it goes back to ' +
              (wifi.ssid || 'the current network') + ' by itself.' : 'If it cannot join, its own Wi-Fi (' + ap + ') opens again so you can fix it.')] });
        if (!ok) return;
        const r = await act(go, () => api('PUT', '/api/wifi', v), err);
        if (r === undefined) return;
        dirty = false;
        joinPrev = wifi.ssid || '';
        pendingKey = '';
        watchJoin(v.ssid, r.persisted !== false, r.trial === true, wifi.ssid);
      } }, picker.el, err, h('div', { class: 'form-actions' }, btn('Cancel', () => {
        dirty = false;
        clear(joinBox);
        changeBtn.hidden = false;
      }), go)));
    });

    /*
     * Follow a change of network made on this page (or a retry of pending
     * settings). trial: the PUT's own word that the station tries the new
     * settings for 90 s and goes back to `prev` without an address there.
     * The box above (showPending, from GET /api/wifi) owns the trial's
     * countdown and the refused, no-address and not-confirmed messages with
     * their buttons; this line says only what that box cannot know: the
     * change before the station reports its trial, contact lost and back,
     * "Connected" (only once the new settings are the station's, no longer
     * pending, never while the station reports them refused, even when the
     * previous settings are for the same network) and a slow join without
     * a trial.
     */
    function watchJoin(ssid, persisted, trial, prev) {
      const t0 = Date.now();
      const status = h('div', { class: 'stack' });
      const storeNote = persisted ? null : msg('warn', 'Settings cannot be saved right now: this network is used until the next restart only.');
      mount(joinBox, h('div', { class: 'subcard' }, status));
      /* One message at a time, rebuilt only when it changes (key); null = nothing of its own (the box above speaks). */
      let shown = '';
      const say = (key, box) => {
        if (key === shown) return;
        shown = key;
        mount(status, box, storeNote);
      };
      const what = ssid === prev ? 'the new password for ' + ssid : ssid;
      const back = prev || 'the current network';
      /* Until the station reports the trial (with its countdown), what it does in words. */
      say('start', msg('info', trial ? 'Trying ' + what + '… If it gets no address there within ' + TRIAL_S + ' seconds, the station goes back to ' +
        back + ' by itself; this page may lose it meanwhile.' : 'Joining ' + ssid + '…'));
      if (stopJoin) stopJoin();
      const done = () => {
        stopJoin();
        stopJoin = null;
        changeBtn.hidden = false;
      };
      stopJoin = poller(1000, async () => {
        const secs = (Date.now() - t0) / 1000;
        let w;
        try {
          w = await api('GET', '/api/wifi', undefined, { timeout: 3000 });
        } catch (e) {
          if (secs > 5) {
            /* The .local name only while the station's mDNS responder runs (it answers nothing otherwise). */
            const name = stationNameUrl(wifi);
            const dhcp = (sys && sys.hostname_active) || 'onocoydiy';
            const where = name ? [h('a', { href: name }, name), ', or find ' + dhcp + ' in that router\'s device list.'] :
              'find ' + dhcp + ' in that router\'s device list.';
            if (trial && wifi && wifi.cred_trial) {
              /* The countdown stays in the box above (it keeps ticking without the station). */
              say('lost-trial', msg('warn', h('strong', null, 'This page lost the station while it tries ' + what + '.'),
                ' If ' + ssid + ' gives it an address, it stays there: open ', where,
                ' Otherwise it goes back to ' + back + ' by itself and this page finds it again.'));
            } else {
              say('lost', msg('warn', 'This page lost the station. If it moved to another network, open it there: ', where));
            }
          }
          tickTrial();
          return;
        }
        showWifi(w);
        const mine = w.pending_ssid === ssid;
        if (mine && w.cred_reverted && !w.cred_trial) {
          /* The trial ended without an address: the box above says why, with Try again and Discard. */
          clear(joinBox);
          done();
        } else if (w.cred_trial) {
          /* The box above shows the trial and its countdown. */
          say('trial', null);
        } else if (w.online && w.ssid === ssid && !(mine && w.cred_pending) && (trial || secs > 2)) {
          /* The new settings are the station's: connected with them. */
          const moved = w.ip && location.hostname !== w.ip && /^\d+\.\d+\.\d+\.\d+$/.test(location.hostname);
          say('connected', [msg('ok', 'Connected to ' + ssid + '.'), kv(addressRows(w)),
            moved ? h('p', null, 'The station has a new address: open it above.') : null]);
          done();
        } else if (secs > 45 && !trial) {
          const why = wifiReason(w.last_disconnect_reason);
          say('slow|' + why, msg('warn', 'Not connected to ' + ssid + ' yet (' + why + '). The station keeps trying.'));
          changeBtn.hidden = false;
        } else if (shown === 'lost' || shown === 'lost-trial') {
          say('back', msg('info', (trial ? 'Trying ' + what : 'Joining ' + ssid) + '…'));
        }
      });
    }

    function renderWifi() {
      showWifi(wifi);
      mount(wifiCard,
        cardHead('Wi-Fi', wifiBadge),
        wifiBody,
        pendingBox,
        h('div', { class: 'form-actions' }, changeBtn),
        joinBox);
    }

    /* ---- The station's own Wi-Fi ---- */
    const apPw = pwInput({ maxlength: 63, autocomplete: 'new-password' });
    const apErr = errBox();
    const apField = field('New password', apPw, '8 to 63 characters, no accents or emoji. Used the next time it opens.');
    /* Only the AP password: the station keeps its Wi-Fi connection. */
    const apPut = () => api('PUT', '/api/wifi', { ap_password: apPw.value });
    const apSet = h('button', { type: 'submit', class: 'btn primary' }, 'Set password');
    const apClear = btn('Remove password', async () => {
      const ok = await dialog({ title: 'Remove the password?', ok: 'Remove', okKind: 'danger solid',
        body: h('p', null, 'Anyone nearby can then join the station\'s own Wi-Fi while it is open.') });
      if (!ok) return;
      apPw.value = '';
      const r = await act(apClear, apPut, apErr);
      if (r !== undefined) {
        toast('Password removed.', 'ok');
        load();
      }
    }, 'danger');
    mount(apCard,
      cardHead('Station\'s own Wi-Fi', apBadge),
      h('p', { class: 'muted small' }, 'Opens for first setup, when the station cannot reach your Wi-Fi, and after the BOOT button is held.'),
      apBody,
      h('form', { class: 'subcard', onsubmit: async e => {
        e.preventDefault();
        apField.error(null);
        const problem = apPasswordProblem(apPw.value);
        if (problem) {
          apField.error(problem);
          return;
        }
        const r = await act(apSet, apPut, apErr);
        if (r !== undefined) {
          apPw.value = '';
          toast('Password set.', 'ok');
          load();
        }
      } }, apField.el, apErr, h('div', { class: 'form-actions' }, apClear, apSet)));

    /* ---- Station ---- */
    function renderDevice() {
      const name = textInput(sys.hostname, { maxlength: 32 });
      const fName = field('Station name', name, 'Letters, digits and "-". Also its web address (name.local) and its name in your router.');
      const pub = checkbox('Public status page', sys.public_status,
        'Anyone on your network sees the station state, caster names and signals without logging in. Caster addresses and logins, ' +
        'versions and the position stay hidden.');
      const err = errBox();
      const save = h('button', { type: 'submit', class: 'btn primary' }, 'Save');
      /* The .local name answers only while the mDNS responder runs (mdns_host set). */
      const active = wifi && wifi.mdns_host;
      mount(deviceCard,
        h('h2', null, 'Station'),
        sys.hostname_active && sys.hostname_active !== sys.hostname ? msg('info', 'The new name takes effect at the next restart: http://' +
          sys.hostname + '.local/' + (active ? ' (until then http://' + active + '.local/).' : '.')) : null,
        h('form', { oninput: () => {
          dirty = true;
        }, onsubmit: async e => {
          e.preventDefault();
          fName.error(null);
          const v = name.value.trim();
          const problem = hostnameProblem(v);
          if (problem) {
            fName.error(problem);
            return;
          }
          const r = await act(save, () => api('PUT', '/api/system', { hostname: v, public_status: pub.input.checked }), err);
          if (r === undefined) return;
          dirty = false;
          sys = r;
          toast(r.reboot_required ? 'Saved. The new name takes effect after the next restart.' : 'Saved.', 'ok');
          renderDevice();
          renderAbout();
        } }, fName.el, pub.el, err, h('div', { class: 'form-actions' }, save)));
    }

    /* ---- Admin password ---- */
    function renderPassword() {
      const cur = pwInput({ autocomplete: 'current-password' });
      const nw = pwInput({ autocomplete: 'new-password' });
      const rep = pwInput({ autocomplete: 'new-password' });
      const f1 = field('Current password', cur), f2 = field('New password', nw, '8 to 64 characters.'), f3 = field('Repeat the new password', rep);
      const err = errBox();
      const save = h('button', { type: 'submit', class: 'btn primary' }, 'Change password');
      mount(pwCard, h('h2', null, 'Admin password'),
        h('form', { onsubmit: async e => {
          e.preventDefault();
          f1.error(null);
          f2.error(null);
          f3.error(null);
          if (!cur.value) {
            f1.error('Enter the current password.');
            return;
          }
          const problem = adminPasswordProblem(nw.value, rep.value);
          if (problem) {
            (problem === PW_MISMATCH ? f3 : f2).error(problem);
            return;
          }
          const r = await act(save, () => api('PUT', '/api/admin-password', { current: cur.value, new: nw.value }), err);
          if (r === undefined) return;
          cur.value = nw.value = rep.value = '';
          toast('Password changed. Other open sessions were logged out.', 'ok');
        } }, f1.el, f2.el, f3.el, err, h('div', { class: 'form-actions' }, save)),
        h('p', { class: 'muted small foot' }, recoveryHint()));
    }

    /* ---- About, appearance ---- */
    function renderAbout() {
      const c = sys.chip || {};
      const mem = [c.flash_mb ? c.flash_mb + NB + 'MB flash' : null, c.psram_mb ? c.psram_mb + NB + 'MB PSRAM' : null].filter(Boolean).join(', ');
      mount(aboutCard, h('h2', null, 'About'), kv([
        ['Firmware', sys.version],
        ['Project', sys.project],
        ['ESP-IDF', sys.idf],
        ['Chip', [c.model, c.revision].filter(Boolean).join(' ')],
        ['Memory', mem],
        ['MAC address', sys.mac],
        ['Settings storage', sys.settings_available === false ? h('span', { class: 't-bad' }, 'Unavailable since this start') : 'Working'],
      ]));
    }

    function renderLook() {
      const theme = radios('Colours', 'theme', [['auto', 'Like the system'], ['light', 'Light'], ['dark', 'Dark']], store.get('theme', 'auto'));
      theme.onchange(() => setTheme(theme.value()));
      mount(lookCard, h('h2', null, 'Appearance'), theme.el, h('p', { class: 'muted small foot' }, 'Kept in this browser only.'));
    }

    /* ---- Restart, safe mode, factory reset ---- */
    function renderMaint() {
      const err = errBox();
      const sm = (S.status && S.status.safe_mode) || {};
      const restart = btn('Restart', async () => {
        const ok = await dialog({ title: 'Restart the station?', ok: 'Restart',
          body: [h('p', null, 'Casters reconnect within about a minute.'),
            onocoyStreams(S.status) ? h('p', { class: 'muted' }, 'That time counts toward today\'s Onocoy offline budget.') : null] });
        if (!ok) return;
        const r = await act(restart, () => api('POST', '/api/system/reboot'), err);
        if (r !== undefined) showRestarting();
      });
      const leaveSafe = btn('Leave safe mode', () => leaveSafeMode(leaveSafe, err), 'primary');
      const reset = btn('Factory reset…', async () => {
        const apName = wifi && wifi.ap && wifi.ap.ssid ? wifi.ap.ssid : 'the station\'s own Wi-Fi';
        const ok = await dialog({
          title: 'Factory reset?',
          ok: 'Erase and restart',
          okKind: 'danger solid',
          typed: 'RESET',
          body: [
            h('p', null, 'Erased: Wi-Fi, the admin password, all casters, the receiver profile and locked position, update and page settings.'),
            h('p', { class: 'muted' }, 'Kept: the event journal, the start history, the availability record and a receiver update in progress.'),
            msg('warn', 'Then the station starts first setup on its own open Wi-Fi (' + apName + '): join it and open http://192.168.4.1/.'),
          ],
        });
        if (!ok) return;
        const r = await act(reset, () => api('POST', '/api/system/factory-reset', { confirm: true }), err);
        if (r !== undefined) {
          showRestarting('Resetting', 'The station erases its settings and restarts into first setup. Join ' + apName +
            ' and open http://192.168.4.1/.');
        }
      }, 'danger');
      mount(maintCard, h('h2', null, 'Restart or reset'),
        sm.active ? msg('bad', h('strong', null, 'Safe mode'), h('div', { class: 'small' }, 'The receiver and casters are off after ' +
          (sm.crashes || 'repeated') + ' crashes.' + (sm.retry_in_s ? ' The station tries normal operation again in ' + inMinutes(sm.retry_in_s) + '.' : '')),
        h('div', { class: 'form-actions' }, leaveSafe)) : null,
        settingRow('Restart the station', 'Casters reconnect within about a minute.', restart),
        settingRow('Factory reset', 'Erases every setting and starts first setup again.', reset),
        err);
    }

    async function load() {
      const r = await Promise.all([api('GET', '/api/wifi'), api('GET', '/api/system')]);
      wifi = r[0];
      sys = r[1];
      renderWifi();
      if (!dirty) renderDevice();
      renderAbout();
      renderMaint();
    }

    mount(el, h('p', { class: 'loading' }, 'Loading…'));
    load().then(() => {
      mount(el, h('div', { class: 'grid' }, wifiCard, apCard, deviceCard, pwCard, lookCard, aboutCard, maintCard));
      renderPassword();
      renderLook();
    }, e => mount(el, msg('bad', errText(e)), h('div', { class: 'form-actions' }, btn('Try again', () => openView('system', ''), 'primary'))));
    const stop = poller(10000, () => (stopJoin || dirty ? Promise.resolve() : api('GET', '/api/wifi').then(showWifi)), true);
    /* A trial's countdown between the polls. */
    const tick = setInterval(tickTrial, 1000);
    let lastSafe = null;
    /*
     * The Wi-Fi card is the only place on this page for a trial or pending
     * settings (the shell shows no notice here), so a change the status
     * reports (a trial started in another tab, its end) is read at once
     * instead of at the next poll, which waits while a form is being edited.
     */
    const credKey = n => [n.cred_trial, n.cred_pending, n.cred_reverted, n.cred_reverted_why, n.pending_ssid].join('|');
    let credReadAt = 0;

    return {
      update(st) {
        const active = !!(st && st.safe_mode && st.safe_mode.active);
        if (sys && active !== lastSafe) {
          lastSafe = active;
          renderMaint();
        }
        const n = st && st.network;
        if (n && wifi && !stopJoin && credKey(n) !== credKey(wifi) && Date.now() - credReadAt > 3000) {
          credReadAt = Date.now();
          api('GET', '/api/wifi').then(w => {
            if (!stopJoin) showWifi(w);
          }, () => {});
        }
      },
      destroy() {
        stop();
        clearInterval(tick);
        if (stopJoin) stopJoin();
      },
      dirty: () => dirty,
      reload() {
        if (!dirty) load().catch(() => {});
      },
    };
  },
};
