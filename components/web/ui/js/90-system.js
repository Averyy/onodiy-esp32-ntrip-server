/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Settings (#/settings): Network (the Wi-Fi, the station's name and public
 * status page, the setup hotspot), beside it Updates (70-updates.js, with
 * receiver maintenance and the manual installs), and under both the Danger
 * zone (restart, safe mode, the admin password, factory reset); every form
 * behind its button. #/settings/wifi, /updates, /session and /restart open
 * it at that card.
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

/* The setup hotspot's chip: [text, tone]; open without a password, a warning while it is on. */
function apChip(ap) {
  if (!ap.active) return ['Off', ''];
  return ap.password_set ? ['On', 'info'] : ['On, no password', 'warn'];
}

/* The station's address rows (stationAddressRows) in this page's words. */
function addressRows(net, prefix) {
  const names = { Name: 'Web address', Address: 'IP address' };
  return stationAddressRows(net).map(r => [r[0] ? (prefix || '') + (names[r[0]] || r[0]) : '', r[1]]);
}

/* The Wi-Fi in a few rows, the signal with its dBm while it is known; the counters behind it and the MAC address are in Logs. */
function wifiStatus(w) {
  return kv([
    ['Wi-Fi', w.configured ? w.ssid : 'Not set up'],
    w.credentials_unreadable ? ['Saved Wi-Fi settings', h('span', { class: 't-bad' }, 'Unreadable: choose the network again')] : null,
    w.online && rssiKnown(w.rssi) ? ['Signal', rssiView(w.rssi)] : null,
    ...(w.online ? addressRows(w) : [['Offline for', fmt.dur(w.offline_s)]]),
    w.retry_in_s && !w.online ? ['Next attempt in', fmt.dur(w.retry_in_s)] : null,
    ['Internet', lbl(L.upstream, w.upstream)],
  ]);
}

/* The setup hotspot (the station's access point) in a few rows: its name and password, while it is open why, where and who. */
function apStatus(w) {
  const ap = w.ap || {};
  const close = typeof ap.close_in_s === 'number' ? ap.close_in_s : ap.recovery_left_s || null;
  return kv([
    ['Network name', ap.ssid],
    ['Password', ap.password_set ? 'Set' : h('span', { class: 't-warn' }, 'None: anyone nearby can join')],
    ap.active ? ['Open because', (ap.reasons || []).map(r => lbl(L.apReason, r)).join(', ') || DASH] : null,
    ap.active ? ['Address on it', h('a', { href: 'http://192.168.4.1/' }, 'http://192.168.4.1/')] : null,
    ap.active && w.mdns_host ? ['Web address on it', h('a', { href: 'http://' + w.mdns_host + '.local/' }, 'http://' + w.mdns_host + '.local/')] : null,
    ap.active ? ['Connected devices', fmt.num(ap.clients)] : null,
    close ? ['Closes in', fmt.dur(close)] : null,
  ]);
}

/* True while an Onocoy server is set up and on: a restart then costs Onocoy offline time. */
function onocoyStreams(st) {
  return ((st && st.casters) || []).some(c => c.protected && c.enabled && c.onocoy_ready !== false);
}

const systemPart = {
  create(arg) {
    let wifi = null, sys = null, stopJoin = null;
    let wifiAt = 0;   /* when `wifi` arrived */
    let joinDirty = false, devDirty = false;
    const wifiBody = h('div');
    const wifiBadge = badge('', '');
    const apBody = h('div');
    const apBadge = badge('', '');
    const showWifi = w => {
      wifi = w;
      wifiAt = Date.now();
      const chip = wifiChip(w);
      wifiBadge.className = 'badge ' + chip[1];
      wifiBadge.textContent = chip[0];
      mount(wifiBody, wifiStatus(w));
      const ap = apChip(w.ap || {});
      apBadge.className = 'badge ' + ap[1];
      apBadge.textContent = ap[0];
      mount(apBody, apStatus(w));
      apBox.hidden = false;
      apOpen.textContent = (w.ap || {}).password_set ? 'Change password' : 'Set a password';
      apClear.hidden = !(w.ap || {}).password_set;
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
      for (const t of netCard.querySelectorAll('.trial-left')) {
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

    /* ---- Wi-Fi ---- */
    const changeBtn = btn('Change network', () => {
      changeBtn.hidden = true;
      const picker = wifiPicker(wifi, { warnScan: true });
      const err = errBox();
      const go = h('button', { type: 'submit', class: 'btn primary' }, 'Join');
      mount(joinBox, h('form', { class: 'subcard', oninput: () => {
        joinDirty = true;
      }, onsubmit: async e => {
        e.preventDefault();
        const v = picker.value();
        if (!v) return;
        const ap = (wifi.ap || {}).ssid;
        const ok = await dialog({ title: 'Join ' + v.ssid + '?', ok: 'Join',
          body: [h('p', null, 'Servers drop for a moment while the station changes networks.'),
            h('p', { class: 'muted' }, wifi.configured ? 'If it gets no address there within ' + TRIAL_S + ' seconds, it goes back to ' +
              (wifi.ssid || 'the current network') + ' by itself.' : 'If it cannot join, the setup hotspot' + (ap ? ' (' + ap + ')' : '') +
              ' opens again so you can fix it.')] });
        if (!ok) return;
        const r = await act(go, () => api('PUT', '/api/wifi', v), err);
        if (r === undefined) return;
        joinDirty = false;
        joinPrev = wifi.ssid || '';
        pendingKey = '';
        watchJoin(v.ssid, r.persisted !== false, r.trial === true, wifi.ssid);
      } }, picker.el, err, h('div', { class: 'form-actions' }, btn('Cancel', () => {
        joinDirty = false;
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

    /* ---- The setup hotspot, in the Network card: its password form behind one button ---- */
    const apPw = pwInput({ maxlength: 63, autocomplete: 'new-password' });
    const apErr = errBox();
    const apField = field('New password', apPw, '8 to 63 characters, no accents or emoji. Used the next time it opens.');
    /* Only the hotspot's password: the station keeps its Wi-Fi connection. */
    const apPut = () => api('PUT', '/api/wifi', { ap_password: apPw.value });
    const apSet = h('button', { type: 'submit', class: 'btn primary' }, 'Set password');
    const apShut = () => {
      apPw.value = '';
      apField.error(null);
      apErr.hidden = true;
      apForm.hidden = true;
      apOpen.hidden = false;
    };
    const apOpen = btn('Change password', () => {
      apForm.hidden = false;
      apOpen.hidden = true;
      apPw.focus();
    });
    const apClear = btn('Remove password', async () => {
      const ok = await dialog({ title: 'Remove the password?', ok: 'Remove', okKind: 'danger solid',
        body: h('p', null, 'Anyone nearby can then join the setup hotspot while it is open.') });
      if (!ok) return;
      apPw.value = '';
      const r = await act(apClear, apPut, apErr);
      if (r !== undefined) {
        apShut();
        toast('Password removed.', 'ok');
        load();
      }
    }, 'danger');
    const apForm = h('form', { hidden: true, onsubmit: async e => {
      e.preventDefault();
      apField.error(null);
      const problem = apPasswordProblem(apPw.value);
      if (problem) {
        apField.error(problem);
        return;
      }
      const r = await act(apSet, apPut, apErr);
      if (r !== undefined) {
        apShut();
        toast('Password set.', 'ok');
        load();
      }
    } }, apField.el, apErr, h('div', { class: 'form-actions' }, btn('Cancel', () => {
      apShut();
      apOpen.focus();
    }), apClear, apSet));
    const apBox = h('div', { class: 'subcard sect', 'data-k': 'hotspot', hidden: true },
      h('div', { class: 'card-head' }, h('h3', null, 'Setup hotspot'), apBadge),
      h('p', { class: 'muted small' }, 'Opens for first setup, when the station cannot reach your Wi-Fi, and after BOOT is held.'),
      apBody, h('div', { class: 'form-actions' }, apOpen), apForm);

    /* ---- The station's name and public status page, in the Network card: its own Save once something changed ---- */
    const deviceBox = h('div', { class: 'subcard sect', hidden: true });
    const netCard = h('section', { class: 'card' }, cardHead('Network', wifiBadge), wifiBody, pendingBox,
      h('div', { class: 'form-actions' }, changeBtn), joinBox, deviceBox, apBox);
    function renderDevice() {
      const name = textInput(sys.hostname, { maxlength: 32 });
      const fName = field('Station name', name, 'Letters, digits and "-"; also its web address.');
      const pub = checkbox('Public status page', sys.public_status, 'Anyone on your network sees the station state without logging in.');
      const err = errBox();
      const save = h('button', { type: 'submit', class: 'btn primary', hidden: true }, 'Save');
      /* The .local name answers only while the mDNS responder runs (mdns_host set). */
      const active = wifi && wifi.mdns_host;
      mount(deviceBox,
        sys.hostname_active && sys.hostname_active !== sys.hostname ? msg('info', 'The new name takes effect at the next restart: http://' +
          sys.hostname + '.local/' + (active ? ' (until then http://' + active + '.local/).' : '.')) : null,
        h('form', { oninput: () => {
          devDirty = name.value.trim() !== sys.hostname || pub.input.checked !== !!sys.public_status;
          save.hidden = !devDirty;
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
          devDirty = false;
          sys = r;
          toast(r.reboot_required ? 'Saved. The new name takes effect after the next restart.' : 'Saved.', 'ok');
          renderDevice();
        } }, fName.el, pub.el, err, h('div', { class: 'form-actions' }, save)));
    }

    /* ---- The admin password, a row of the Danger zone: its form behind one button (kept across the card's rebuilds) ---- */
    const pwBox = h('div');
    function renderPassword() {
      const cur = pwInput({ autocomplete: 'current-password' });
      const nw = pwInput({ autocomplete: 'new-password' });
      const rep = pwInput({ autocomplete: 'new-password' });
      const f1 = field('Current password', cur), f2 = field('New password', nw, '8 to 64 characters.'), f3 = field('Repeat the new password', rep);
      const err = errBox();
      const save = h('button', { type: 'submit', class: 'btn primary' }, 'Change password');
      const shut = () => {
        cur.value = nw.value = rep.value = '';
        form.hidden = true;
        open.hidden = false;
      };
      const open = btn('Change password', () => {
        form.hidden = false;
        open.hidden = true;
        cur.focus();
      });
      const form = h('form', { hidden: true, onsubmit: async e => {
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
        shut();
        toast('Password changed. Other open sessions were logged out.', 'ok');
      } }, f1.el, f2.el, f3.el, err, h('div', { class: 'form-actions' }, btn('Cancel', () => {
        shut();
        open.focus();
      }), save));
      mount(pwBox, settingRow('Reset admin password', recoveryHint(), open), form);
    }

    /* ---- The Danger zone: restart, safe mode, the admin password, factory reset ---- */
    const maintCard = h('section', { class: 'card wide', hidden: true });
    function renderMaint() {
      const err = errBox();
      const sm = (S.status && S.status.safe_mode) || {};
      const restart = btn('Restart', async () => {
        const ok = await dialog({ title: 'Restart the station?', ok: 'Restart',
          body: [h('p', null, 'Servers reconnect within about a minute.'),
            onocoyStreams(S.status) ? h('p', { class: 'muted' }, 'That time counts toward today\'s Onocoy offline budget.') : null] });
        if (!ok) return;
        const r = await act(restart, () => api('POST', '/api/system/reboot'), err);
        if (r !== undefined) showRestarting();
      });
      const leaveSafe = btn('Leave safe mode', () => leaveSafeMode(leaveSafe, err), 'primary');
      const reset = btn('Factory reset…', async () => {
        const apName = wifi && wifi.ap && wifi.ap.ssid ? wifi.ap.ssid : 'the setup hotspot';
        const ok = await dialog({
          title: 'Factory reset?',
          ok: 'Erase and restart',
          okKind: 'danger solid',
          typed: 'RESET',
          body: [
            h('p', null, 'Erased: Wi-Fi, the admin password, all servers, the receiver profile and locked position, update and page settings.'),
            h('p', { class: 'muted' }, 'Kept: the event journal, the start history, the availability record and a receiver update in progress.'),
            msg('warn', 'Then the station opens its setup hotspot (' + apName + ') without a password for first setup: join it and open ' +
              'http://192.168.4.1/.'),
          ],
        });
        if (!ok) return;
        const r = await act(reset, () => api('POST', '/api/system/factory-reset', { confirm: true }), err);
        if (r !== undefined) {
          showRestarting('Resetting', 'The station erases its settings and restarts into first setup. Join ' + apName +
            ' and open http://192.168.4.1/.');
        }
      }, 'danger');
      mount(maintCard, h('h2', null, 'Danger zone'),
        sm.active ? msg('bad', h('strong', null, 'Safe mode'), h('div', { class: 'small' }, 'The receiver and servers are off after ' +
          (sm.crashes || 'repeated') + ' crashes.' + (sm.retry_in_s ? ' The station tries normal operation again in ' + inMinutes(sm.retry_in_s) + '.' : '')),
        h('div', { class: 'form-actions' }, leaveSafe)) : null,
        h('div', { class: 'rows' }, h('div', null, settingRow('Restart the station', 'Servers reconnect within about a minute.', restart)), pwBox,
          h('div', null, settingRow('Factory reset', 'Erases every setting and starts first setup again.', reset))),
        err);
    }

    async function load() {
      const r = await Promise.all([api('GET', '/api/wifi'), api('GET', '/api/system')]);
      sys = r[1];
      showWifi(r[0]);
      if (!devDirty) renderDevice();
      renderMaint();
    }

    mount(wifiBody, h('p', { class: 'loading' }, 'Loading…'));
    const ready = load().then(() => {
      deviceBox.hidden = maintCard.hidden = false;
      renderPassword();
    }, e => mount(netCard, msg('bad', errText(e)), h('div', { class: 'form-actions' }, btn('Try again', () => openView('settings', arg), 'primary'))));
    const stop = poller(10000, () => (stopJoin || joinDirty ? Promise.resolve() : api('GET', '/api/wifi').then(showWifi)), true);
    /* A trial's countdown between the polls. */
    const tick = setInterval(tickTrial, 1000);
    let lastSafe = null;
    /*
     * The Network card is the only place on this page for a trial or pending
     * settings (the shell shows no notice here), so a change the status
     * reports (a trial started in another tab, its end) is read at once
     * instead of at the next poll, which waits while a form is being edited.
     */
    const credKey = n => [n.cred_trial, n.cred_pending, n.cred_reverted, n.cred_reverted_why, n.pending_ssid].join('|');
    let credReadAt = 0;

    return {
      ready: ready,
      /* The Network card also at its old parts' names, the Danger zone at the admin password's. */
      cards: { wifi: netCard, network: netCard, station: netCard, restart: maintCard, password: maintCard },
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
      dirty: () => joinDirty || devDirty,
      reload() {
        if (!joinDirty && !devDirty) load().catch(() => {});
      },
    };
  },
};

Views.settings = {
  title: 'Settings',
  admin: true,
  create(el, arg) {
    const s = systemPart.create(arg), u = updatesPart.create(arg);
    const c = Object.assign({}, s.cards, u.cards);
    /* Network beside Updates (one column on a phone), the receiver update session and the Danger zone under both. */
    mount(el, h('div', { class: 'grid' }, c.wifi, c.updates, c.session, c.restart));
    /* Opened at a card (#/settings/restart): once both parts are in, so the place does not move. */
    Promise.all([u.ready, s.ready]).then(() => {
      const at = c[arg];
      if (at && at.isConnected && !at.hidden) at.scrollIntoView({ block: 'start' });
    });
    return {
      update(st) {
        s.update(st);
        u.update(st);
      },
      destroy() {
        u.destroy();
        s.destroy();
      },
      dirty: () => s.dirty(),
      reload() {
        u.reload();
        s.reload();
      },
    };
  },
};
