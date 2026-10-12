/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * System: Wi-Fi and the access point password, station name and public
 * status, the admin password, appearance, restart, safe mode and factory
 * reset.
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
  return 'Only plain ASCII: letters, digits, spaces and the usual symbols (no accented letters or emoji).';
}

/* How long the station tries new Wi-Fi credentials before it goes back to the previous ones. */
const TRIAL_S = 90;

/* "the new password for Home" when only the password changes, else "the new credentials for Cafe". */
function newCredsText(w, ssid) {
  const p = w.pending_ssid || 'the new network';
  return ssid && p === ssid ? 'the new password for ' + p : 'the new credentials for ' + p;
}

function wifiStatus(w) {
  const ap = w.ap || {};
  return [
    kv([
      ['Network', w.configured ? w.ssid : 'Not set up'],
      w.credentials_unreadable ? ['Stored credentials', h('span', { class: 't-bad' }, 'Unreadable')] : null,
      ['Connection', w.online ? 'Online' : w.associated ? 'Connected, no IP address yet' : 'Not connected'],
      ...(w.online ? stationAddressRows(w) : [['Offline for', fmt.dur(w.offline_s)]]),
      w.associated ? ['Signal', rssiView(w.rssi)] : null,
      w.associated ? ['Channel', String(w.channel)] : null,
      ['Internet', lbl(L.upstream, w.upstream) + (w.probe_age_s !== null && w.probe_age_s !== undefined ? ' (checked ' + fmt.dur(w.probe_age_s) + ' ago)' : '')],
      ['Clock synced', fmt.yesno(w.time_synced)],
      ['Reconnects / failures', fmt.num(w.reconnects) + ' / ' + fmt.num(w.connect_failures)],
      w.retry_in_s ? ['Next attempt in', fmt.dur(w.retry_in_s)] : null,
      ['Last disconnect', wifiReason(w.last_disconnect_reason)],
      w.fallback_dns ? ['DNS', 'Using the fallback servers'] : null,
      w.associated && !w.online && w.assoc_no_ip_s ? ['Without an address for', fmt.dur(w.assoc_no_ip_s)] : null,
      w.ladder && (w.ladder.triggers || []).length ? ['Network recovery', 'running: ' + w.ladder.triggers.map(t => lbl(L.trigger, t)).join(', ')] : null,
    ]),
    h('h3', { class: 'section-title' }, 'Station access point'),
    kv([
      ['State', ap.active ? 'On (' + (ap.reasons || []).map(r => lbl(L.apReason, r)).join(', ') + ')' : 'Off'],
      ['Network name', ap.ssid],
      ap.active ? ['Page on it', h('a', { href: 'http://192.168.4.1/' }, 'http://192.168.4.1/')] : null,
      ap.active && w.mdns_host ? ['Name on it', h('a', { href: 'http://' + w.mdns_host + '.local/' }, 'http://' + w.mdns_host + '.local/')] : null,
      ap.active ? ['Connected devices', fmt.num(ap.clients)] : null,
      ap.active && typeof ap.close_in_s === 'number' ? ['Closes in', fmt.dur(ap.close_in_s)] : null,
      ap.recovery_left_s && typeof ap.close_in_s !== 'number' ? ['Recovery closes in', fmt.dur(ap.recovery_left_s)] : null,
      ['Password', ap.password_set ? 'Set' : 'None (open)'],
    ]),
  ];
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
    const showWifi = w => {
      wifi = w;
      wifiAt = Date.now();
      wifiBadge.className = 'badge ' + (w.online ? 'ok' : 'bad');
      wifiBadge.textContent = w.online ? 'Online' : 'Offline';
      mount(wifiBody, wifiStatus(w));
      showPending(w);
    };
    const joinBox = h('div');
    const pendingBox = h('div');
    let pendingKey = '';
    let joinPrev = '';   /* the network before a change made on this page */
    /*
     * Seconds left of a credential trial: the last GET /api/wifi's
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
    /* New credentials on trial or pending: rebuilt only when that changes, so its buttons stay put. */
    const showPending = w => {
      const key = [w.cred_trial, w.cred_pending, w.cred_reverted, w.cred_reverted_why, w.pending_ssid, w.ssid].join('|');
      if (key === pendingKey) {
        tickTrial();
        return;
      }
      pendingKey = key;
      if (w.cred_trial) {
        /* No buttons while a trial runs: it ends by itself, either way. */
        mount(pendingBox, msg('info', h('strong', null, 'Trying ' + newCredsText(w, joinPrev) + ': '),
          trialLeftEl(), '. Without an address there the station goes back to its previous network ' +
          'and keeps the new credentials as pending.'));
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
        }, 'small primary');
        const discard = btn('Discard', async e => {
          if (await discardPendingWifi(e)) {
            /* Gone at once; the reload confirms it. */
            pendingKey = '';
            clear(pendingBox);
            load().catch(() => {});
          }
        }, 'small');
        let text;
        if (w.cred_reverted && w.cred_reverted_why === 'auth_failed') {
          text = [h('strong', null, (w.pending_ssid || 'The network') + ' refused the new password: '), 'the station is back on its previous credentials' +
            (w.ssid ? ' (' + w.ssid + ')' : '') + '. The new ones stay pending.'];
        } else if (w.cred_reverted) {
          text = [h('strong', null, 'No address with ' + newCredsText(w, w.ssid) + ' within 90 seconds: '), 'the station is back on its previous network (' +
            (w.ssid || 'unknown') + '). The new credentials stay pending.'];
        } else {
          text = [h('strong', null, 'Not confirmed yet: ' + newCredsText(w, w.ssid) + '. '), 'They become the station\'s once they get an address.'];
        }
        mount(pendingBox, msg('warn', text, h('div', { class: 'form-actions' }, retry, discard)));
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
      mount(joinBox, h('form', { class: 'stack foot', oninput: () => {
        dirty = true;
      }, onsubmit: async e => {
        e.preventDefault();
        const v = picker.value();
        if (!v) return;
        const ap = (wifi.ap || {}).ssid || 'its setup network';
        const ok = await dialog({ title: 'Join ' + v.ssid + '?', ok: 'Join',
          body: [h('p', null, 'Changing Wi-Fi drops every caster connection for a moment.'),
            h('p', { class: 'muted' }, 'If the station cannot join, it opens its own access point (' + ap + ') again so you can fix it.')] });
        if (!ok) return;
        const r = await act(go, () => api('PUT', '/api/wifi', v), err);
        if (r === undefined) return;
        dirty = false;
        joinPrev = wifi.ssid || '';
        pendingKey = '';
        watchJoin(v.ssid, r.persisted !== false, r.trial === true, wifi.ssid);
      } }, picker.el, err, h('div', { class: 'form-actions' }, go, btn('Cancel', () => {
        dirty = false;
        clear(joinBox);
        changeBtn.hidden = false;
      }, 'ghost'))));
    });

    /*
     * Follow a change of network made on this page (or a retry of pending
     * credentials). trial: the PUT's own word that the station tries the new
     * credentials for 90 s and goes back to `prev` without an address there;
     * the page then reports either end of that trial and nothing before it:
     * "Connected" only once the new credentials are the station's (no longer
     * pending), never while the station reports them refused (cred_reverted
     * for this network), even when the previous credentials are for the same
     * network. While the station is out of reach the countdown goes on here.
     */
    function watchJoin(ssid, persisted, trial, prev) {
      const t0 = Date.now();
      const status = h('div', { class: 'stack' });
      const storeNote = persisted ? null : msg('warn', 'Settings storage unavailable: this network is used until the next restart only.');
      mount(joinBox, h('div', { class: 'foot' }, status));
      /* One message at a time, rebuilt only when it changes (key). */
      let shown = '';
      const say = (key, box) => {
        if (key === shown) return;
        shown = key;
        mount(status, box, storeNote);
      };
      const what = ssid === prev ? 'the new password for ' + ssid : ssid;
      const back = prev || 'the current network';
      /* Until the station reports the trial (with its countdown), what it does in words. */
      say('start', msg('info', trial ? 'Trying ' + what + '… The station tries it for up to ' + TRIAL_S + ' seconds; without an address ' +
        'there it goes back to ' + back + ' by itself, so this page may lose contact for that long.' : 'Joining ' + ssid + '…'));
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
            const where = name ? [h('a', { href: name }, name), ', or find its address in that router\'s device list (named ' + dhcp + ').'] :
              'find its address in that router\'s device list (named ' + dhcp + ').';
            if (trial && wifi && wifi.cred_trial) {
              say('lost-trial', msg('warn', h('strong', null, 'This page lost contact with the station while it tries ' + what + ': '), trialLeftEl(),
                '. If ' + ssid + ' gives it an address, it stays there: open it on that network, ', where,
                ' Otherwise it goes back to ' + back + ' by itself and this page finds it again.'));
            } else {
              say('lost', msg('warn', 'This page lost contact with the station. If it moved to another network, open it there: ', where));
            }
          }
          tickTrial();
          return;
        }
        showWifi(w);
        const mine = w.pending_ssid === ssid;
        if (mine && w.cred_reverted && !w.cred_trial) {
          /* The trial ended without an address: only that is said. */
          say('reverted', msg('warn', (w.cred_reverted_why === 'auth_failed' ? ssid + ' refused the new password: the station is back on its ' +
            'previous credentials' + (w.ssid ? ' (' + w.ssid + ')' : '') + '.' : 'No address on ' + ssid + ' within ' + TRIAL_S +
            ' seconds: the station is back on ' + (w.ssid || 'the previous network') + '.') +
            ' The new credentials are kept as pending; try them again or discard them above.'));
          done();
        } else if (w.cred_trial) {
          say('trial', msg('info', h('strong', null, 'Trying ' + what + ': '), trialLeftEl(),
            '. Without an address there the station goes back to ' + back + ' by itself, so this page may lose contact until then.'));
        } else if (w.online && w.ssid === ssid && !(mine && w.cred_pending) && (trial || secs > 2)) {
          /* The new credentials are the station's: connected with them. */
          const moved = w.ip && location.hostname !== w.ip && /^\d+\.\d+\.\d+\.\d+$/.test(location.hostname);
          say('connected', [msg('ok', 'Connected to ' + ssid + '.'), kv(stationAddressRows(w)),
            moved ? h('p', null, 'The station has a new address: open it above.') : null]);
          done();
        } else if (secs > 45 && !trial) {
          const why = wifiReason(w.last_disconnect_reason);
          say('slow|' + why, msg('warn', 'Not connected to ' + ssid + ' yet. Last error: ' + why + '. The station keeps trying.'));
          changeBtn.hidden = false;
        } else if (shown === 'lost' || shown === 'lost-trial') {
          say('back', msg('info', (trial ? 'Trying ' + what : 'Joining ' + ssid) + '…'));
        }
      });
    }

    const apPw = pwInput({ maxlength: 63, autocomplete: 'new-password' });
    const apErr = errBox();
    const apField = field('Access point password', apPw, '8 to 63 characters (plain ASCII). Takes effect the next time the access point comes up.');
    /* Only the AP password: the station keeps its Wi-Fi connection. */
    const apBody = () => ({ ap_password: apPw.value });
    const apSet = btn('Set password', async () => {
      apField.error(null);
      const v = apPw.value;
      const problem = apPasswordProblem(v);
      if (problem) {
        apField.error(problem);
        return;
      }
      const r = await act(apSet, () => api('PUT', '/api/wifi', apBody()), apErr);
      if (r !== undefined) {
        apPw.value = '';
        toast('Access point password set.', 'ok');
        load();
      }
    }, 'small');
    const apClear = btn('Remove password', async () => {
      const ok = await dialog({ title: 'Remove the access point password?', ok: 'Remove', okKind: 'danger solid',
        body: msg('warn', 'The station\'s own access point (first setup, Wi-Fi trouble, recovery) is then open: anyone nearby can join it while it is up.') });
      if (!ok) return;
      apPw.value = '';
      const r = await act(apClear, () => api('PUT', '/api/wifi', apBody()), apErr);
      if (r !== undefined) {
        toast('Access point password removed.', 'ok');
        load();
      }
    }, 'small danger');

    function renderWifi() {
      showWifi(wifi);
      mount(wifiCard,
        h('h2', null, 'Wi-Fi', wifiBadge),
        wifiBody,
        pendingBox,
        h('div', { class: 'form-actions' }, changeBtn),
        joinBox,
        h('h3', { class: 'section-title' }, 'Access point password'),
        apField.el,
        h('div', { class: 'row foot' }, apSet, wifi.ap && wifi.ap.password_set ? apClear : null),
        apErr);
    }

    /* ---- Device ---- */
    function renderDevice() {
      const name = textInput(sys.hostname, { maxlength: 32 });
      /* The .local name answers only while the mDNS responder runs (mdns_host set). */
      const active = wifi && wifi.mdns_host;
      const ipUrl = wifi && wifi.online && wifi.ip ? 'http://' + wifi.ip + '/' : null;
      const reach = active ? 'Other devices on your network open http://' + active + '.local/' + (ipUrl ? ' (or ' + ipUrl + ')' : '') + '.' :
        ipUrl ? 'Other devices on your network open ' + ipUrl + ' (the .local name does not answer right now).' :
          'The .local name does not answer right now.';
      const fName = field('Station name', name, 'Letters, digits and "-". ' + reach +
        ' It is also the name the station gives your router (DHCP) and the Onocoy dashboard label default.');
      const pub = checkbox('Public status page', sys.public_status,
        'Anyone on your network can see the station state, caster names and states, signals and the sky view without logging in. ' +
        'Never shown: hosts, mountpoints, users, addresses, versions or the antenna position.');
      const err = errBox();
      const save = h('button', { type: 'submit', class: 'btn primary' }, 'Save');
      mount(deviceCard,
        h('h2', null, 'Station'),
        sys.hostname_active && sys.hostname_active !== sys.hostname ? msg('info', (active ? 'Reachable as http://' + active + '.local/ until the next restart, then http://' :
          'The new name takes effect at the next restart: http://') + sys.hostname + '.local/.') : null,
        h('form', { class: 'stack', oninput: () => {
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
        h('form', { class: 'stack', onsubmit: async e => {
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
            (problem.indexOf('differ') >= 0 ? f3 : f2).error(problem);
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
      mount(aboutCard, h('h2', null, 'About'), kv([
        ['Firmware', sys.version],
        ['Project', sys.project],
        ['ESP-IDF', sys.idf],
        ['Chip', [c.model, c.revision].filter(Boolean).join(' ')],
        ['Flash / PSRAM', (c.flash_mb ? c.flash_mb + ' MB' : DASH) + ' / ' + (c.psram_mb ? c.psram_mb + ' MB' : DASH)],
        ['MAC address', sys.mac],
        ['Settings storage', sys.settings_available === false ? h('span', { class: 't-bad' }, 'Unavailable this boot') : 'OK'],
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
          body: h('p', null, 'The casters reconnect after the restart, usually within a minute. That time counts toward today\'s Onocoy offline budget.') });
        if (!ok) return;
        const r = await act(restart, () => api('POST', '/api/system/reboot'), err);
        if (r !== undefined) showRestarting();
      });
      const leaveSafe = btn('Leave safe mode', async () => {
        const ok = await dialog({ title: 'Leave safe mode?', ok: 'Restart normally',
          body: h('p', null, 'The station restarts in normal mode with its crash count cleared. If the cause is still there, it may crash again and return to safe mode.') });
        if (!ok) return;
        const r = await act(leaveSafe, () => api('POST', '/api/system/safe-mode/clear'), err);
        if (r !== undefined) showRestarting();
      }, 'primary');
      const reset = btn('Factory reset…', async () => {
        const apName = wifi && wifi.ap && wifi.ap.ssid ? wifi.ap.ssid : 'the station\'s setup network';
        const ok = await dialog({
          title: 'Factory reset?',
          ok: 'Erase and restart',
          okKind: 'danger solid',
          typed: 'RESET',
          body: [
            h('p', null, 'Erased: Wi-Fi, the admin password, all casters, the receiver profile and the locked position, update settings and page settings.'),
            h('p', { class: 'muted' }, 'Kept: a receiver update in progress, the last receiver identity, app trial and rollback records, the start history and the availability record.'),
            msg('warn', 'The station then starts first setup with an open access point (' + apName + '). Join it and open http://192.168.4.1/ to set it up again.'),
          ],
        });
        if (!ok) return;
        const r = await act(reset, () => api('POST', '/api/system/factory-reset', { confirm: true }), err);
        if (r !== undefined) {
          showRestarting('Factory reset', 'The station erases its settings and restarts into first setup. Join ' + apName +
            ' and open http://192.168.4.1/ to set it up again.');
        }
      }, 'danger');
      mount(maintCard, h('h2', null, 'Maintenance'),
        sm.active ? msg('bad', h('strong', null, 'Safe mode. '), 'The station restarted after ' + (sm.crashes || 'repeated') +
          ' crashes. Receiver and casters are off; Wi-Fi, this page and updates work.' +
          (sm.retry_in_s ? ' Automatic retry of normal operation in ' + fmt.dur(sm.retry_in_s) + '.' : ''),
        h('div', { class: 'form-actions' }, leaveSafe)) : null,
        err,
        h('div', { class: 'form-actions' }, restart, reset));
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
      mount(el, h('div', { class: 'grid' }, wifiCard, deviceCard, pwCard, lookCard, aboutCard, maintCard));
      renderPassword();
      renderLook();
    }, e => mount(el, msg('bad', errText(e)), btn('Try again', () => openView('system', ''), 'primary')));
    const stop = poller(10000, () => (stopJoin || dirty ? Promise.resolve() : api('GET', '/api/wifi').then(showWifi)), true);
    /* A trial's countdown between the polls. */
    const tick = setInterval(tickTrial, 1000);
    let lastSafe = null;

    return {
      update(st) {
        const active = !!(st && st.safe_mode && st.safe_mode.active);
        if (sys && active !== lastSafe) {
          lastSafe = active;
          renderMaint();
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
