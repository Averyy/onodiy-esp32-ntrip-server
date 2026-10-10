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

function wifiStatus(w) {
  const ap = w.ap || {};
  return [
    kv([
      ['Network', w.configured ? w.ssid : 'Not set up'],
      w.credentials_unreadable ? ['Stored credentials', h('span', { class: 't-bad' }, 'Unreadable')] : null,
      ['Connection', w.online ? 'Online' : w.associated ? 'Connected, no IP address yet' : 'Not connected'],
      w.online ? ['Address', w.ip] : ['Offline for', fmt.dur(w.offline_s)],
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
      ['Name', ap.ssid],
      ap.active ? ['Connected devices', fmt.num(ap.clients)] : null,
      ap.recovery_left_s ? ['Recovery closes in', fmt.dur(ap.recovery_left_s)] : null,
      ['Password', ap.password_set ? 'Set' : 'None (open)'],
    ]),
  ];
}

Views.system = {
  title: 'System',
  admin: true,
  create(el) {
    let wifi = null, sys = null, stopJoin = null;
    let dirty = false;
    const wifiCard = h('section', { class: 'card' });
    const wifiBody = h('div');
    const wifiBadge = badge('', '');
    const showWifi = w => {
      wifiBadge.className = 'badge ' + (w.online ? 'ok' : 'bad');
      wifiBadge.textContent = w.online ? 'Online' : 'Offline';
      mount(wifiBody, wifiStatus(w));
      showPending(w);
    };
    const joinBox = h('div');
    const pendingBox = h('div');
    let pendingKey = '';
    /* New credentials on trial or pending: rebuilt only when that changes, so its buttons stay put. */
    const showPending = w => {
      const key = [w.cred_trial, w.cred_pending, w.cred_reverted, w.pending_ssid, w.ssid].join('|');
      if (key === pendingKey) {
        const left = pendingBox.querySelector('.trial-left');
        if (left) left.textContent = fmt.dur(w.cred_trial_left_s);
        return;
      }
      pendingKey = key;
      if (w.cred_trial) {
        mount(pendingBox, msg('info', h('strong', null, 'Trying ' + (w.pending_ssid || 'the new network') + '. '),
          'If it gets no address within ', h('span', { class: 'trial-left' }, fmt.dur(w.cred_trial_left_s)),
          ', the station goes back to its previous network and keeps the new credentials as pending.'));
      } else if (w.cred_pending) {
        const retry = btn('Try again', async e => {
          if (await retryPendingWifi(e)) load().catch(() => {});
        }, 'small primary');
        const discard = btn('Discard', async e => {
          if (await discardPendingWifi(e)) load().catch(() => {});
        }, 'small');
        mount(pendingBox, msg('warn', h('strong', null, 'Pending credentials for ' + (w.pending_ssid || 'a network') + ' not confirmed. '),
          w.cred_reverted ? 'They got no address within 90 seconds: the station is back on the previous network (' + (w.ssid || 'unknown') + ').' :
            'They are tried first at the next start.',
          h('div', { class: 'form-actions' }, retry, discard)));
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
        watchJoin(v.ssid, r.persisted !== false, r.trial === true, wifi.ssid);
      } }, picker.el, err, h('div', { class: 'form-actions' }, go, btn('Cancel', () => {
        dirty = false;
        clear(joinBox);
        changeBtn.hidden = false;
      }, 'ghost'))));
    });

    function watchJoin(ssid, persisted, trial, prev) {
      const t0 = Date.now();
      const status = h('div', { class: 'stack' });
      mount(joinBox, h('div', { class: 'foot' }, status));
      const show = (...kids) => mount(status, kids);
      show(msg('info', 'Joining ' + ssid + '…' + (trial ? ' The station tries it for up to 90 seconds; without an address there it goes back to ' +
        (prev || 'the current network') + ' by itself, so this page may lose contact for that long.' : '')),
      persisted ? null : msg('warn', 'Settings storage unavailable: this network is used until the next restart only.'));
      if (stopJoin) stopJoin();
      stopJoin = poller(1000, async () => {
        const secs = (Date.now() - t0) / 1000;
        let w;
        try {
          w = await api('GET', '/api/wifi', undefined, { timeout: 3000 });
        } catch (e) {
          if (secs > 5) show(msg('warn', 'This page lost contact with the station. If it moved to another network, open it there: ' +
            'its address is in that router\'s device list, named ' + ((sys && sys.hostname_active) || 'onocoydiy') + '.'));
          return;
        }
        wifi = w;
        showWifi(w);
        if (trial && w.cred_reverted && w.pending_ssid === ssid && !w.cred_trial) {
          stopJoin();
          stopJoin = null;
          show(msg('warn', 'No address on ' + ssid + ' within 90 seconds: the station is back on ' + (w.ssid || 'the previous network') +
            '. The new credentials are kept as pending; try them again or discard them above.'));
          changeBtn.hidden = false;
          return;
        }
        if (w.online && w.ssid === ssid && !w.cred_trial && secs > 2) {
          stopJoin();
          stopJoin = null;
          const moved = w.ip && location.hostname !== w.ip && /^\d+\.\d+\.\d+\.\d+$/.test(location.hostname);
          show(msg('ok', 'Connected to ' + ssid + ' with address ' + w.ip + '.'),
            moved ? h('p', null, 'The station has a new address: ', h('a', { href: 'http://' + w.ip + '/' }, 'http://' + w.ip + '/')) : null);
          changeBtn.hidden = false;
        } else if (secs > 45 && !w.cred_trial) {
          show(msg('warn', 'Not connected to ' + ssid + ' yet. Last error: ' + wifiReason(w.last_disconnect_reason) + '. The station keeps trying.'));
          changeBtn.hidden = false;
        }
      });
    }

    const apPw = pwInput({ maxlength: 63, autocomplete: 'new-password' });
    const apErr = errBox();
    const apField = field('Access point password', apPw, '8 to 63 characters. Takes effect the next time the access point comes up.');
    /* Only the AP password: the station keeps its Wi-Fi connection. */
    const apBody = () => ({ ap_password: apPw.value });
    const apSet = btn('Set password', async () => {
      apField.error(null);
      const v = apPw.value;
      if (v.length < 8 || v.length > 63 || hasControl(v)) {
        apField.error('8 to 63 characters.');
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
      const fName = field('Station name', name, 'Letters, digits and "-". The name the station gives your router (DHCP) and the Onocoy dashboard label default.');
      const pub = checkbox('Public status page', sys.public_status,
        'Anyone on your network can see the station state, caster names and states, signals and the sky view without logging in. ' +
        'Never shown: hosts, mountpoints, users, addresses, versions or the antenna position.');
      const err = errBox();
      const save = h('button', { type: 'submit', class: 'btn primary' }, 'Save');
      mount(deviceCard,
        h('h2', null, 'Station'),
        sys.hostname_active && sys.hostname_active !== sys.hostname ? msg('info', 'Now named ' + sys.hostname_active + ' on the network; ' + sys.hostname + ' takes over at the next restart.') : null,
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
        h('p', { class: 'muted small foot' }, RECOVERY_HINT));
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
    const stop = poller(10000, () => (stopJoin || dirty ? Promise.resolve() : api('GET', '/api/wifi').then(w => {
      wifi = w;
      showWifi(w);
    })), true);
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
        if (stopJoin) stopJoin();
      },
      dirty: () => dirty,
      reload() {
        if (!dirty) load().catch(() => {});
      },
    };
  },
};
