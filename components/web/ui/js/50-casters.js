/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Casters: one card per slot with live state and the full connection
 * statistics, and the slot editor (also used by the setup wizard).
 *
 * Editor rules mirror the server's (PUT /api/casters/{n}); the server has
 * the last word. Passwords are write-only: an empty field keeps the stored
 * one while the host stays the same. Streaming to Onocoy and to an open
 * caster at the same time needs the user's explicit acknowledgement, which
 * the server asks for with 409 caster_policy / dup_stream_ack_required.
 */

const PRESETS = [
  { id: 'onocoy', label: 'Onocoy' },
  { id: 'rtk2go', label: 'RTK2go', host: 'rtk2go.com', port: 2101, ntrip_version: 1, backoff_profile: 'polite' },
  { id: 'centipede', label: 'Centipede', host: 'crtk.net', port: 2101, ntrip_version: 1, backoff_profile: 'standard' },
  { id: 'emlid', label: 'Emlid Caster', host: 'caster.emlid.com', port: 2101, ntrip_version: 1, backoff_profile: 'standard' },
  { id: 'rtkdirect', label: 'RTKDirect', host: 'ntrip.rtkdirect.com', port: 2101, ntrip_version: 2, backoff_profile: 'standard' },
  { id: 'custom', label: 'Other caster' },
];

const PRESET_NOTES = {
  onocoy: 'Credential username and password come from the Onocoy console (Reference Stations, NTRIP Credentials). Onocoy allows one stream per station.',
  rtk2go: 'Needs a mountpoint reservation on rtk2go.com, and the NTRIP version must match its style (Rev1 = 1.0, Rev2 = 2.0). ' +
    'Mountpoint: letters, digits, "-" and "_", starting with a letter; keep it under 20 characters. RTK2go bans more than one stream ' +
    'from the same public IP address, so other base stations on your network count too. Anyone may use RTK2go streams.',
  centipede: 'Register the mountpoint with Centipede first (up to 4 capital letters). The password is Centipede\'s shared community password (see its base-station guide). ' +
    'Set your precise coordinates (fixed base) before you enable it. Centipede streams are public.',
  emlid: 'Use the mountpoint and password from your Emlid Caster account.',
  rtkdirect: 'Use the login, password and mountpoint from your RTKDirect dashboard. NTRIP 2.0 works best there.',
  custom: '',
};

const DUP_TEXT = 'Onocoy bans duplicated streams, also when someone else re-sends your stream from an open caster. ' +
  'Streams on RTK2go and Centipede can be used by anyone, so a third party could hand yours to Onocoy as theirs. To lower the risk, ' +
  'use a private RTK2go reservation, and tell Onocoy support that you are the original provider of that stream.';

function hostIn(host, domain) {
  host = String(host || '').toLowerCase().replace(/\.$/, '');
  return host === domain || host.endsWith('.' + domain);
}

function isOnocoyHost(host) {
  return hostIn(host, 'onocoy.com');
}

function presetOf(host) {
  if (!host) return 'custom';
  if (isOnocoyHost(host)) return 'onocoy';
  if (hostIn(host, 'rtk2go.com')) return 'rtk2go';
  if (hostIn(host, 'crtk.net')) return 'centipede';
  if (hostIn(host, 'caster.emlid.com')) return 'emlid';
  if (hostIn(host, 'rtkdirect.com')) return 'rtkdirect';
  return 'custom';
}

/** Onocoy dashboard label: the hostname reduced to letters and digits. */
function defaultLabel(hostname) {
  const s = String(hostname || '').replace(/[^A-Za-z0-9]/g, '').slice(0, 47);
  return s || 'onocoydiy';
}

function saveCaster(slot, body) {
  return api('PUT', '/api/casters/' + slot, body);
}

/** The duplicate-stream warning with its acknowledgement box. */
function dupPanel(onAck) {
  const ack = checkbox('I understand the risk and want to stream to both.', false);
  const go = btn('Save with acknowledgement', () => onAck(go), 'primary', { disabled: true });
  ack.input.addEventListener('change', () => {
    go.disabled = !ack.input.checked;
  });
  return h('div', { class: 'msg warn', role: 'alert' }, h('strong', null, 'Duplicate-stream risk'), h('p', null, DUP_TEXT), ack.el,
    h('div', { class: 'form-actions' }, go));
}

const DNS_OR_IP = /^(?=.{1,63}$)[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)*\.?$/;

/**
 * Slot editor. entry = {slot, config}. opts: preset (onocoy_preset),
 * hostname, hideOnocoy, onSaved(entry), onCancel(). Returns {el, dirty()}.
 */
function casterEditor(entry, opts) {
  const slot = entry.slot;
  const cfg = Object.assign({ enabled: false, name: '', host: '', port: 2101, ntrip_version: 1, tls: false, trust: 'bundle',
    ca_pem_set: false, mountpoint: '', username: '', password_set: false, label: '', v2_raw_body: false,
    backoff_profile: 'standard', dup_stream_ack: false }, entry.config || {});
  const preset = Object.assign({ host: 'servers.onocoy.com', port: 2101, tls_port: 2121, ntrip_version: 1 }, opts.preset || {});
  const isNew = !cfg.host;
  let kind = isNew ? (opts.hideOnocoy ? 'rtk2go' : 'onocoy') : presetOf(cfg.host);
  let dirty = false;

  const choices = PRESETS.filter(p => !(p.id === 'onocoy' && opts.hideOnocoy && kind !== 'onocoy'));
  const presetSel = selectInput(choices.map(p => [p.id, p.label]), kind);
  const enabled = checkbox('Enabled', isNew ? true : cfg.enabled, 'Only enabled casters connect.');
  const name = textInput(cfg.name, { maxlength: 23 });
  const host = textInput(cfg.host, { maxlength: 63, inputmode: 'url' });
  const port = numInput(cfg.port, { inputmode: 'numeric', maxlength: 5 });
  const version = radios('NTRIP version', 'ntripv' + slot, [[1, 'NTRIP 1.0'], [2, 'NTRIP 2.0']], cfg.ntrip_version);
  const tls = checkbox('Use TLS (encrypted connection)', cfg.tls);
  const trust = selectInput([['bundle', L.trust.bundle], ['custom_ca', L.trust.custom_ca], ['insecure', L.trust.insecure]], cfg.trust);
  const mp = textInput(cfg.mountpoint, { maxlength: 47 });
  const user = textInput(cfg.username, { maxlength: 47 });
  const pw = pwInput({ maxlength: 63, placeholder: cfg.password_set ? 'Stored (leave empty to keep)' : '' });
  const clearPw = checkbox('Remove the stored password', false);
  const label = textInput(cfg.label, { maxlength: 47 });
  const backoff = selectInput([['standard', 'Standard'], ['polite', 'Polite (slower retries)']], cfg.backoff_profile === 'polite' ? 'polite' : 'standard');
  const raw = checkbox('Send NTRIP 2.0 data without chunked encoding', cfg.v2_raw_body, 'Only for casters that need it; most expect chunks.');

  const F = {
    name: field('Name', name, 'For this page only.'),
    host: field('Host', host),
    port: field('Port', port),
    mountpoint: field('Mountpoint', mp),
    username: field('Username', user),
    password: field('Password', pw),
    label: field('Dashboard label', label, 'NTRIP 2.0 only. Letters and digits; it shows in your Onocoy dashboard.'),
    trust: field('Certificate check', trust),
    backoff_profile: field('Retry pace', backoff, 'Polite waits longer between attempts; some casters ban fast reconnects.'),
  };
  const note = h('p', { class: 'muted small' });
  const tlsNote = h('small', { class: 'hint' });
  const forcedPace = h('p', { class: 'muted small' });
  const caBox = h('div', { class: 'stack' });
  const err = errBox();
  const dupBox = h('div');
  const saveBtn = h('button', { type: 'submit', class: 'btn primary' }, 'Save');

  /* Custom CA certificate (stored at once, separate from Save). */
  const pem = h('textarea', { rows: 6, placeholder: '-----BEGIN CERTIFICATE-----', spellcheck: 'false' });
  const caState = h('span', { class: 'muted small' });
  const caErr = errBox();
  let caSet = cfg.ca_pem_set;
  const showCa = () => {
    caState.textContent = caSet ? 'A CA certificate is stored for this slot.' : 'No CA certificate stored yet.';
  };
  const storeCa = btn('Store certificate', () => {
    const text = pem.value.trim();
    if (text.indexOf('-----BEGIN CERTIFICATE-----') < 0 || utf8Len(text) > 4096) {
      caErr.hidden = false;
      mount(caErr, 'Paste one or more PEM certificates (at most 4 KB).');
      return;
    }
    act(storeCa, async () => {
      const r = await api('PUT', '/api/casters/' + slot + '/ca', { pem: text + '\n' });
      caSet = !!(r && r.config && r.config.ca_pem_set);
      pem.value = '';
      showCa();
      toast('Certificate stored. The slot reconnects.', 'ok');
    }, caErr);
  }, 'small');
  const removeCa = btn('Remove', () => act(removeCa, async () => {
    await api('DELETE', '/api/casters/' + slot + '/ca');
    caSet = false;
    showCa();
  }, caErr), 'small danger');
  showCa();
  add(caBox, [field('CA certificate (PEM)', pem, 'The certificate of the authority that signed the caster\'s certificate.').el,
    h('div', { class: 'row' }, storeCa, removeCa, caState), caErr]);

  const onocoy = () => kind === 'onocoy';
  const v2 = () => version.value() === '2';

  function sync() {
    const ono = onocoy();
    host.disabled = ono;
    port.disabled = ono;
    F.mountpoint.el.querySelector('.lbl').textContent = ono ? 'Credential username' : 'Mountpoint';
    F.username.el.hidden = ono || !v2();
    F.label.el.hidden = !(ono && v2());
    if (ono && v2() && !label.value) label.value = defaultLabel(opts.hostname);
    F.trust.el.hidden = !tls.input.checked || ono;
    caBox.hidden = !(tls.input.checked && !ono && trust.value === 'custom_ca');
    tlsNote.textContent = !tls.input.checked ? 'Without TLS the password travels unencrypted, as is normal for NTRIP on port 2101.' :
      ono ? 'Experimental: Onocoy does not document uploads over TLS (port ' + preset.tls_port + ') and may stop them at any time.' :
        trust.value === 'insecure' ? 'Insecure: the caster\'s certificate is not checked.' : '';
    const pace = ono ? 'fast' : kind === 'rtk2go' ? 'polite' : null;
    F.backoff_profile.el.hidden = !!pace;
    forcedPace.hidden = !pace;
    forcedPace.textContent = pace ? 'Retry pace: ' + lbl(L.backoff, pace) + ' (fixed for ' + (ono ? 'Onocoy' : 'RTK2go') + ').' : '';
    raw.el.hidden = !v2() || ono;
    clearPw.el.hidden = !cfg.password_set;
    note.textContent = PRESET_NOTES[kind] || '';
  }

  function applyPreset() {
    const prev = kind;
    kind = presetSel.value;
    const p = PRESETS.find(x => x.id === kind) || {};
    const prevLabel = (PRESETS.find(x => x.id === prev) || {}).label;
    if (!name.value || name.value === prevLabel) name.value = kind === 'custom' ? '' : p.label;
    if (kind === 'onocoy') {
      host.value = preset.host;
      port.value = String(tls.input.checked ? preset.tls_port : preset.port);
      version.set(preset.ntrip_version || 1);
    } else if (p.host) {
      host.value = p.host;
      port.value = String(p.port);
      version.set(p.ntrip_version);
      tls.input.checked = false;
      if (p.backoff_profile === 'polite' || p.backoff_profile === 'standard') backoff.value = p.backoff_profile;
    } else if (prev !== 'custom') {
      host.value = '';
      port.value = '2101';
    }
    sync();
  }

  presetSel.addEventListener('change', () => {
    dirty = true;
    applyPreset();
  });
  tls.input.addEventListener('change', () => {
    if (onocoy()) port.value = String(tls.input.checked ? preset.tls_port : preset.port);
    sync();
  });
  trust.addEventListener('change', sync);
  version.onchange(sync);

  function collect() {
    for (const k of Object.keys(F)) F[k].error(null);
    const ono = onocoy();
    const v = v2();
    const on = enabled.input.checked;
    const hostVal = ono ? preset.host : host.value.trim();
    const portVal = Number(port.value);
    const hostChanged = hostVal.toLowerCase() !== String(cfg.host || '').toLowerCase();
    let bad = null;
    const fail = (k, text) => {
      F[k].error(text);
      if (!bad) bad = F[k].input;
    };
    if (hasControl(name.value) || name.value.length > 23) fail('name', 'At most 23 characters.');
    if (!hostVal && on) fail('host', 'Enter the caster\'s host name.');
    else if (hostVal && !DNS_OR_IP.test(hostVal)) fail('host', 'A host name or an IPv4 address, at most 63 characters.');
    if (!(Number.isInteger(portVal) && portVal >= 1 && portVal <= 65535)) fail('port', 'A port from 1 to 65535.');
    const mpVal = mp.value.trim();
    if (!mpVal && on) fail('mountpoint', ono ? 'Enter the credential username.' : 'Enter the mountpoint.');
    else if (mpVal && !/^[A-Za-z0-9._-]{1,47}$/.test(mpVal)) fail('mountpoint', 'Letters, digits, ".", "_" and "-" only, at most 47.');
    else if (kind === 'rtk2go' && mpVal && !/^[A-Za-z]/.test(mpVal)) fail('mountpoint', 'RTK2go mountpoints start with a letter.');
    const userVal = user.value;
    if (!ono && v) {
      if (userVal.indexOf(':') >= 0 || userVal.length > 47 || hasControl(userVal)) fail('username', 'At most 47 characters, without ":".');
      else if (on && !userVal) fail('username', 'NTRIP 2.0 needs a username.');
    }
    const pwVal = pw.value;
    const keepsPw = cfg.password_set && !hostChanged && !clearPw.input.checked;
    if (pwVal.length > 63 || hasControl(pwVal)) fail('password', 'At most 63 characters.');
    else if (!v && /\s/.test(pwVal)) fail('password', 'NTRIP 1.0 passwords cannot contain spaces.');
    else if (on && !pwVal && !keepsPw && (!v || ono)) fail('password', cfg.password_set && hostChanged ?
      'The host changed: enter the password for the new host.' : 'Enter the password.');
    const labelVal = label.value.trim();
    if (ono && v && labelVal && !/^[A-Za-z0-9]{1,47}$/.test(labelVal)) fail('label', 'Letters and digits only, at most 47.');
    if (bad) {
      bad.focus();
      return null;
    }
    const body = {
      enabled: on,
      name: name.value.trim() || (PRESETS.find(x => x.id === kind) || {}).label || '',
      host: hostVal,
      port: portVal,
      ntrip_version: v ? 2 : 1,
      tls: tls.input.checked,
      mountpoint: mpVal,
      backoff_profile: ono ? 'fast' : kind === 'rtk2go' ? 'polite' : backoff.value,
    };
    if (kind === 'custom' && body.name === '') body.name = hostVal.slice(0, 23);
    if (tls.input.checked) body.trust = ono ? 'bundle' : trust.value;
    if (!ono && v) body.username = userVal;
    if (ono && v) body.label = labelVal || defaultLabel(opts.hostname);
    if (v && !ono) body.v2_raw_body = raw.input.checked;
    if (pwVal) body.password = pwVal;
    else if (clearPw.input.checked) body.clear_password = true;
    return body;
  }

  async function save(ack, button) {
    const body = collect();
    if (!body) return;
    if (ack) body.dup_stream_ack = true;
    err.hidden = true;
    try {
      const r = await busy(button || saveBtn, () => saveCaster(slot, body));
      dirty = false;
      pw.value = '';
      clear(dupBox);
      toast('Saved. The caster reconnects with the new settings.', 'ok');
      if (opts.onSaved) opts.onSaved(r);
    } catch (e) {
      if (e instanceof ApiError && e.code === 'caster_policy' && e.body.reason === 'dup_stream_ack_required') {
        mount(dupBox, dupPanel(b => save(true, b)));
        dupBox.scrollIntoView({ block: 'nearest' });
        return;
      }
      if (e instanceof ApiError && e.code === 'invalid_field' && F[e.field]) {
        F[e.field].error('The station did not accept this value.');
        F[e.field].input.focus();
        return;
      }
      showErr(e, err);
    }
  }

  const form = h('form', { class: 'stack', onsubmit: e => {
    e.preventDefault();
    save(false);
  }, oninput: () => {
    dirty = true;
  } },
  field('Caster', presetSel).el,
  note,
  enabled.el,
  F.name.el,
  h('div', { class: 'cols c2' }, F.host.el, F.port.el),
  version.el,
  F.mountpoint.el,
  F.username.el,
  F.password.el,
  clearPw.el,
  F.label.el,
  h('div', { class: 'field' }, tls.el, tlsNote),
  F.trust.el,
  caBox,
  h('details', { class: 'more' }, h('summary', null, 'More options'), h('div', { class: 'stack' }, F.backoff_profile.el, forcedPace, raw.el)),
  cfg.dup_stream_ack ? h('p', { class: 'muted small' }, 'The duplicate-stream risk was acknowledged for this slot.') : null,
  dupBox,
  err,
  h('div', { class: 'form-actions' }, saveBtn, opts.onCancel ? btn('Cancel', () => opts.onCancel(), 'ghost') : null));

  if (isNew) applyPreset();
  else sync();
  return { el: form, dirty: () => dirty };
}

/* ---- Caster page ---------------------------------------------------------- */

function casterSummary(c, st, preset) {
  const tone = casterTone(st.state);
  const ono = isOnocoyHost(c.host);
  const metrics = [];
  if (st.state === 'streaming') {
    metrics.push(['Connected', fmt.dur(st.connected_s)]);
    metrics.push(['Lag', fmt.ms(st.lag_ms_now)]);
    const r = slotRate(c.slot, 10);
    if (r !== null) metrics.push(['Rate', fmt.kbps(r)]);
  } else {
    metrics.push(['For', fmt.dur(st.state_age_s)]);
    if (st.next_retry_s) metrics.push(['Retry in', fmt.dur(st.next_retry_s)]);
  }
  const on = st.today_online_s || 0, off = st.today_offline_s || 0;
  if (on + off > 0) metrics.push(['Today online', fmt.dur(on)]);
  if (off) metrics.push(['Today offline', fmt.dur(off)]);
  if (st.epochs_dropped_today) metrics.push(['Epochs dropped today', fmt.num(st.epochs_dropped_today)]);
  const avg = c.backoff_profile === 'polite' && st.state === 'streaming' ? slotRate(c.slot, 60) : null;
  const out = [
    h('div', { class: 'muted small' }, [c.host + ':' + c.port, 'NTRIP ' + (c.ntrip_version === 2 ? '2.0' : '1.0'), c.tls ? 'TLS' : null,
      !ono && c.mountpoint ? '/' + c.mountpoint : null, c.enabled ? null : 'off'].filter(Boolean).join(' · ')),
    st.reason && st.reason !== 'none' && st.state !== 'streaming' ? h('div', { class: 't-' + (tone || 'warn') + ' small' }, lbl(L.fail, st.reason)) : null,
    h('div', { class: 'metrics' }, metrics.map(m => h('span', null, m[0] + ' ', h('b', null, m[1])))),
    st.hint ? h('div', { class: 'hintline' }, st.hint) : null,
    st.unreached_s ? h('div', { class: 'hintline' }, 'Caster not reached for ' + fmt.dur(st.unreached_s) +
      ' (name lookup or connection failing before the caster answered).') : null,
  ];
  if (avg !== null) {
    out.push(h('div', { class: 'small ' + (avg > 28000 ? 't-warn' : 'muted') }, 'Average over 60 s: ' + fmt.kbps(avg) +
      (avg > 28000 ? '. That is close to the limits of public casters; consider MSM4 messages instead of MSM7.' : '')));
  }
  if (ono && c.tls && st.failures_by_class && st.failures_by_class.tls >= 10 && st.state !== 'streaming') {
    const sw = btn('Use plain port ' + preset.port, async () => {
      const body = { tls: false, port: preset.port };
      try {
        await busy(sw, () => saveCaster(c.slot, body));
      } catch (e) {
        if (e instanceof ApiError && e.code === 'caster_policy' && e.body.reason === 'dup_stream_ack_required') {
          /* This slot needs the acknowledgement itself (it may have been given
           * on the other slot only): ask for it here. A dialog, since this
           * card is rebuilt every second. */
          const ok = await dialog({ title: 'Duplicate-stream risk', ok: 'Save with acknowledgement', body: h('p', null, DUP_TEXT),
            check: 'I understand the risk and want to stream to both.',
            onOk: () => saveCaster(c.slot, Object.assign({ dup_stream_ack: true }, body)) });
          if (ok) toast('Switched to the plain port.', 'ok');
          return;
        }
        showErr(e);
        return;
      }
      toast('Switched to the plain port.', 'ok');
    }, 'small');
    out.push(msg('warn', 'TLS to Onocoy failed ' + st.failures_by_class.tls + ' times. Onocoy documents uploads on the plain port only.', h('div', { class: 'form-actions' }, sw)));
  }
  return out;
}

function casterDetails(st) {
  const fb = st.failures_by_class || {};
  return [
    kv([
      ['State', lbl(L.caster, st.state) + (st.reason && st.reason !== 'none' ? ' (' + lbl(L.fail, st.reason) + ')' : '')],
      ['Connected since', fmt.time(st.connected_since_unix)],
      ['Connects / failures', fmt.num(st.connects) + ' / ' + fmt.num(st.failures)],
      ['Disconnects / restarts', fmt.num(st.disconnects) + ' / ' + fmt.num(st.restarts)],
      ['Last reply', st.last_status_line ? st.last_status_line + (st.last_status_code ? ' (' + st.last_status_code + ')' : '') : DASH],
      st.server_header ? ['Caster software', st.server_header] : null,
      ['Address', st.resolved_ip ? st.resolved_ip + (st.dns_cached_used ? ' (cached DNS used ' + st.dns_cached_used + '×)' : '') : DASH],
      st.unreached_s ? ['Not reached for', fmt.dur(st.unreached_s)] : null,
      st.tls_version ? ['TLS', st.tls_version + (st.tls_cipher ? ' · ' + st.tls_cipher : '')] : null,
      ['Sent', fmt.bytes(st.bytes_sent) + ' · ' + fmt.num(st.frames_sent) + ' frames · ' + fmt.num(st.epochs_sent) + ' epochs'],
      ['Epochs today', fmt.num(st.epochs_sent_today) + ' sent · ' + fmt.num(st.epochs_dropped_today) + ' dropped'],
      ['Dropped', fmt.num(st.frames_dropped) + ' frames · ' + fmt.bytes(st.bytes_dropped) + ' · ' + fmt.num(st.epochs_dropped) + ' epochs'],
      ['Lag now / limit', fmt.ms(st.lag_ms_now) + ' / ' + fmt.ms(st.max_lag_ms)],
      ['Lag drops / laps', fmt.num(st.lag_drops) + ' / ' + fmt.num(st.laps)],
      ['Skipped while idle', fmt.num(st.frames_skipped_idle)],
      ['Retry attempt', fmt.num(st.backoff_attempt)],
      st.graced_uptime_permille !== null && st.graced_uptime_permille !== undefined ?
        ['Graced uptime', fmt.permille(st.graced_uptime_permille) + (st.streak_ok ? ' (streak on track)' : ' (below 99.8 %)')] : null,
      st.data_rate_permille !== null && st.data_rate_permille !== undefined ? ['Data rate', fmt.permille(st.data_rate_permille)] : null,
      st.budget_left_s !== null && st.budget_left_s !== undefined ? ['Offline budget left', fmt.dur(st.budget_left_s)] : null,
      st.last_err ? ['Last error', st.last_err] : null,
    ]),
    h('h3', { class: 'section-title' }, 'Failures by cause'),
    table(['Cause', 'Count'], Object.keys(fb).map(k => [lbl(L.fail, k), fmt.num(fb[k])]), { num: [1] }),
  ];
}

Views.casters = {
  title: 'Casters',
  admin: true,
  create(el) {
    let data = null;
    let editing = null, editor = null;
    const cards = {};   /* slot -> card element */
    const refs = {};    /* slot -> {badge, live, details} for in-place updates */
    const open = {};    /* slot -> details expanded */
    let stopPoll = null;

    const liveOf = slot => {
      const l = S.status && S.status.casters ? S.status.casters.find(c => c.slot === slot) : null;
      return l || {};
    };
    const statusOf = s => Object.assign({}, s.status || {}, liveOf(s.slot));

    function cardBody(s) {
      const c = s.config;
      delete refs[s.slot];
      if (editing === s.slot && editor) {
        return [h('h2', null, 'Slot ' + (s.slot + 1)), editor.el];
      }
      const st = statusOf(s);
      if (!c) {
        return [h('h2', null, 'Slot ' + (s.slot + 1), badge(lbl(L.caster, st.state), casterTone(st.state))),
          h('p', { class: 'muted' }, 'Not loaded (safe mode).')];
      }
      if (!c.host && !c.enabled) {
        return [h('h2', null, 'Slot ' + (s.slot + 1), badge('Empty', '')),
          h('div', { class: 'row' }, btn('Set up this slot', () => edit(s), 'primary'))];
      }
      const r = {
        badge: badge(lbl(L.caster, st.state), casterTone(st.state)),
        live: h('div'),
        details: h('div'),
      };
      refs[s.slot] = r;
      fill(s);
      const det = h('details', { class: 'more', open: !!open[s.slot] }, h('summary', null, 'Connection details'), r.details);
      det.addEventListener('toggle', () => {
        open[s.slot] = det.open;
      });
      return [
        h('h2', null, c.name || 'Slot ' + (s.slot + 1), c.protected ? badge('Onocoy', 'info plain') : null, r.badge),
        r.live,
        det,
        h('div', { class: 'form-actions' }, btn('Edit', () => edit(s))),
      ];
    }

    /* Refresh the live parts of a card without touching its buttons. */
    function fill(s) {
      const r = refs[s.slot];
      if (!r) return;
      const st = statusOf(s);
      r.badge.className = 'badge ' + casterTone(st.state);
      r.badge.textContent = lbl(L.caster, st.state);
      mount(r.live, casterSummary(Object.assign({ slot: s.slot }, s.config), st, data.onocoy_preset || {}));
      mount(r.details, casterDetails(st));
    }

    function edit(s) {
      if (editor && editor.dirty() && !window.confirm('Discard the changes to the other slot?')) return;
      const prev = editing;
      editing = s.slot;
      const others = data.slots.filter(x => x.slot !== s.slot && x.config && isOnocoyHost(x.config.host) && x.config.enabled);
      editor = casterEditor(s, {
        preset: data.onocoy_preset,
        hostname: S.status && S.status.device ? S.status.device.hostname : '',
        hideOnocoy: others.length > 0,
        onSaved: saved => {
          editing = null;
          editor = null;
          if (saved && saved.slot !== undefined) data.slots = data.slots.map(x => x.slot === saved.slot ? saved : x);
          renderSlot(s.slot);
          load().catch(() => {});
        },
        onCancel: () => {
          editing = null;
          editor = null;
          renderSlot(s.slot);
        },
      });
      if (prev !== null && prev !== s.slot) renderSlot(prev);
      renderSlot(s.slot);
      cards[s.slot].scrollIntoView({ block: 'start', behavior: 'smooth' });
    }

    function renderSlot(n) {
      const s = data.slots.find(x => x.slot === n);
      if (s && cards[n]) mount(cards[n], cardBody(s));
    }

    function render() {
      const safe = data.slots.length > 0 && data.slots.every(s => s.config === null);
      for (const k of Object.keys(cards)) delete cards[k];
      mount(el,
        h('section', { class: 'card' }, h('h2', null, 'Casters'),
          h('p', null, 'The station sends the same RTCM stream to every enabled caster, up to ' + data.max_slots +
            ' at a time. Onocoy takes one slot; the others are optional.'),
          safe ? msg('warn', 'Safe mode: the casters are off and cannot be changed until the station runs normally again.') : null),
        data.slots.map(s => (cards[s.slot] = h('section', { class: 'card' }, cardBody(s)))));
    }

    async function load() {
      const d = await api('GET', '/api/casters');
      const old = data;
      data = d;
      if (!old || old.slots.length !== d.slots.length) {
        render();
        return;
      }
      for (const s of d.slots) {
        if (s.slot === editing) continue;
        const before = old.slots.find(x => x.slot === s.slot);
        if (refs[s.slot] && before && JSON.stringify(before.config) === JSON.stringify(s.config)) fill(s);
        else renderSlot(s.slot);
      }
    }

    mount(el, h('p', { class: 'loading' }, 'Loading casters…'));
    load().then(() => {
      stopPoll = poller(5000, () => (editing === null ? load() : Promise.resolve()), true);
    }, e => mount(el, msg('bad', errText(e)), btn('Try again', () => openView('casters', ''), 'primary')));

    return {
      update() {
        if (!data) return;
        for (const s of data.slots) if (s.slot !== editing) fill(s);
      },
      destroy() {
        if (stopPoll) stopPoll();
      },
      dirty: () => !!(editor && editor.dirty()),
      reload() {
        if (editing === null) load().catch(() => {});
      },
    };
  },
};

