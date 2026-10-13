/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Casters: one card per slot with live state and the full connection
 * statistics, and the slot editor (also used by the setup wizard).
 *
 * Editor rules mirror the server's (PUT /api/casters/{n}); the server has
 * the last word. Byte limits are checked in UTF-8 bytes, as the station
 * stores them. Passwords are write-only: an empty field keeps the stored
 * one while the host stays the same. Streaming to Onocoy and to an open
 * caster at the same time needs the user's explicit acknowledgement, which
 * the server asks for with 409 caster_policy / dup_stream_ack_required.
 *
 * Onocoy slots ask for exactly what the Onocoy console gives: host and port
 * (fixed), mountpoint, username and password. A slot the station's Onocoy
 * login guard stopped shows why and a "Try again" that warns first (every
 * failed login counts toward Onocoy's block of the whole connection). A test
 * Onocoy slot (bench and QEMU builds) counts as Onocoy at its own host.
 *
 * One Onocoy slot per station: a new slot starts on RTK2go, not Onocoy,
 * while another slot has an Onocoy host (set up or not, on or off), and
 * says where that slot is; the Onocoy choice is hidden only while another
 * enabled slot streams to Onocoy (the server refuses a second one). A slot
 * saved without a host and not enabled is empty again: it is stored without
 * a name, so no page shows the caster it held before.
 */

const PRESETS = [
  { id: 'onocoy', label: 'Onocoy' },
  { id: 'rtk2go', label: 'RTK2go', host: 'rtk2go.com', port: 2101, ntrip_version: 1, backoff_profile: 'polite' },
  { id: 'centipede', label: 'Centipede', host: 'crtk.net', port: 2101, ntrip_version: 1, backoff_profile: 'standard' },
  { id: 'emlid', label: 'Emlid Caster', host: 'caster.emlid.com', port: 2101, ntrip_version: 1, backoff_profile: 'standard' },
  { id: 'rtkdirect', label: 'RTKDirect', host: 'ntrip.rtkdirect.com', port: 2101, ntrip_version: 2, backoff_profile: 'standard' },
  { id: 'custom', label: 'Other caster' },
];

/* Where the Onocoy console shows what the slot needs. */
const ONOCOY_CONSOLE = 'In the Onocoy console open Reference Stations, your station, NTRIP credentials: copy the mountpoint, username and password shown there.';

/* Onocoy's password rule, shown and checked softly (a note, never a refusal: credentials made before the rule still work). */
const ONOCOY_PW_RULE = 'Onocoy\'s console asks for a credential password of at least 8 characters with at least one letter and one digit. ' +
  'This station also needs it without spaces (NTRIP 1.0 sends it in the request line).';

const ONOCOY_ONE_DEVICE = 'Use a credential that no other device is streaming with. Two devices on one credential keep knocking each other off, ' +
  'and repeated failed logins can make Onocoy block logins from your internet connection for a while, which stops every station on that connection.';

/** The soft note for a typed Onocoy password that breaks the console's rule ('' when it follows it or nothing is typed). */
function onocoyPwNote(pw) {
  if (!pw || (pw.length >= 8 && /[A-Za-z]/.test(pw) && /[0-9]/.test(pw))) return '';
  return 'Onocoy\'s console requires at least 8 characters with a letter and a digit: check that you copied the whole password.';
}

const PRESET_NOTES = {
  onocoy: 'Onocoy allows one stream per station. ' + ONOCOY_ONE_DEVICE,
  rtk2go: 'Needs a mountpoint reservation on rtk2go.com. New reservations were paused in September 2026 and the notice was still up ' +
    'in October 2026; existing reservations are kept, so check the reservation page first. The reservation fixes the NTRIP version ' +
    '(Rev1 = 1.0, Rev2 = 2.0). Mountpoint: 4 to 20 letters, digits, "_" or "-", starting with a letter other than "A". ' +
    'RTK2go bans more than one stream from the same public IP address (other base stations on your network count too), more than ' +
    'one message per second per type, more than 35 kbit/s, and "more than 500Mb per day" without saying whether that means megabits ' +
    'or megabytes. This station\'s usual stream (MSM7, four constellations) is about 13 kbit/s, about 140 MB (1.1 Gbit) a day: under ' +
    '500 megabytes, over 500 megabits. Ask RTK2go if unsure. Anyone may use RTK2go streams.',
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

/** The slot counts as Onocoy: an Onocoy host, or a test Onocoy slot (bench and QEMU builds only). */
function isOnocoySlot(cfg) {
  return !!cfg && (isOnocoyHost(cfg.host) || !!cfg.test_onocoy);
}

/* RTK2go's rules for a new mountpoint, in the station's words (null when it follows them). */
function rtk2goMountProblem(m) {
  if (!/^[A-Za-z0-9_-]*$/.test(m)) return 'RTK2go mountpoints use letters, digits, "_" and "-" only (no ".").';
  if (!/^[A-Za-z]/.test(m)) return 'RTK2go mountpoints start with a letter.';
  if (m.charAt(0) === 'A') return 'RTK2go no longer accepts new mountpoints that start with "A".';
  if (m.length < 4 || m.length > 20) return 'RTK2go mountpoints have 4 to 20 characters.';
  return null;
}

const RETRY_TEXT = 'Try once now? Only after you made sure no other device streams with this credential and the credentials match the ' +
  'Onocoy console. Every failed login counts toward Onocoy\'s login block for your internet connection.';

/** The user's retry of a stopped (or waiting) slot, after the warning. */
async function retryCaster(slot) {
  const ok = await dialog({ title: 'Try again', ok: 'Try once now', body: h('p', null, RETRY_TEXT),
    onOk: () => api('POST', '/api/casters/' + slot + '/retry', {}) });
  if (ok) toast('The slot tries once now.', 'ok');
  return ok;
}

/** Bytes as decimal megabytes and megabits ("140.2 MB (1122 Mbit)"), as RTK2go's daily limit is worded. */
function volText(b) {
  if (b === null || b === undefined || isNaN(b)) return DASH;
  return (b / 1e6).toFixed(1) + NB + 'MB (' + Math.round(b * 8 / 1e6) + NB + 'Mbit)';
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

/* Mountpoint and username as the station takes them (NTRIP 1.0 sends the Onocoy username as the mountpoint). */
const MOUNT_RE = /^[A-Za-z0-9._-]{1,47}$/;

/** True when an Onocoy slot has what the console gives: mountpoint, username and a password. */
function onocoyReady(cfg) {
  return !!(cfg && isOnocoySlot(cfg) && cfg.mountpoint && cfg.username && cfg.password_set);
}

/**
 * The Onocoy fields' rules: {mountpoint, username, password} -> error text
 * per field (missing ones only when `on`; a stored password may stay).
 */
function onocoyProblems(v, on, pwStored) {
  const out = {};
  if (v.mountpoint ? !MOUNT_RE.test(v.mountpoint) : on) {
    out.mountpoint = v.mountpoint ? 'Letters, digits, ".", "_" and "-" only, at most 47.' : 'Enter the mountpoint from the Onocoy console.';
  }
  if (v.username ? !MOUNT_RE.test(v.username) : on) {
    out.username = v.username ? 'Letters, digits, ".", "_" and "-" only, at most 47.' : 'Enter the username from the Onocoy console.';
  }
  if (v.password ? (/\s/.test(v.password) || utf8Len(v.password) > 63 || hasControl(v.password)) : on && !pwStored) {
    out.password = v.password ? '1 to 63 characters, no spaces.' : 'Enter the password from the Onocoy console.';
  }
  return out;
}

/** A field refusal from the station in words: its rule when it gave one. */
function ruleText(e) {
  const m = e && e.body && e.body.message;
  if (!m || m === GENERIC_INVALID) return 'The station did not accept this value.';
  return m.charAt(0).toUpperCase() + m.slice(1) + (/[.!?]$/.test(m) ? '' : '.');
}

/** Where the Onocoy slot is, for the editor of another slot (onocoyElsewhere: {slot, ready, enabled}). */
function onocoyElsewhereText(o) {
  const n = 'slot ' + (o.slot + 1);
  if (!o.ready) return 'Onocoy uses ' + n + ', which is not set up yet: open it to add your credentials.';
  if (!o.enabled) return 'Onocoy is set up in ' + n + ' and turned off: open it to turn it on.';
  return 'Onocoy streams from ' + n + '. Onocoy allows one stream per station.';
}

/**
 * Slot editor. entry = {slot, config}. opts: preset (onocoy_preset),
 * hostname, hideOnocoy (another enabled slot streams to Onocoy: no Onocoy
 * choice), onocoyElsewhere ({slot, ready, enabled} of another slot with an
 * Onocoy host: a new slot starts on RTK2go and says where it is), openSlot(n)
 * (opens that slot's editor), onSaved(entry), onCancel(). Returns {el, dirty()}.
 */
function casterEditor(entry, opts) {
  const slot = entry.slot;
  const cfg = Object.assign({ enabled: false, name: '', host: '', port: 2101, ntrip_version: 1, tls: false, trust: 'bundle',
    ca_pem_set: false, mountpoint: '', username: '', password_set: false, label: '', v2_raw_body: false,
    backoff_profile: 'standard', dup_stream_ack: false }, entry.config || {});
  const preset = Object.assign({ host: 'servers.onocoy.com', port: 2101, tls_port: 2121, ntrip_version: 1 }, opts.preset || {});
  const isNew = !cfg.host;
  /* A test Onocoy slot (bench and QEMU builds) counts as Onocoy at its own host and port. */
  const testSlot = !!cfg.test_onocoy;
  const elsewhere = opts.onocoyElsewhere || null;
  let kind = isNew ? (opts.hideOnocoy || elsewhere ? 'rtk2go' : 'onocoy') : isOnocoySlot(cfg) ? 'onocoy' : presetOf(cfg.host);
  let dirty = false;
  /* An Onocoy slot without its credentials yet: setting it up means using it. */
  const noCreds = isOnocoySlot(cfg) && !onocoyReady(cfg);

  const choices = PRESETS.filter(p => !(p.id === 'onocoy' && opts.hideOnocoy && kind !== 'onocoy'));
  const presetSel = selectInput(choices.map(p => [p.id, p.label]), kind);
  const enabled = checkbox('Enabled', isNew || noCreds ? true : cfg.enabled, 'Only enabled casters connect.');
  const name = textInput(cfg.name, { maxlength: 23 });
  const host = textInput(cfg.host, { maxlength: 63, inputmode: 'url' });
  const port = numInput(cfg.port, { inputmode: 'numeric', maxlength: 5 });
  const version = radios('NTRIP version', 'ntripv' + slot, [[1, 'NTRIP 1.0'], [2, 'NTRIP 2.0']], cfg.ntrip_version);
  const versionNote = h('small', { class: 'hint' });
  version.el.append(versionNote);
  const tls = checkbox('Use TLS (encrypted connection)', cfg.tls);
  const trust = selectInput([['bundle', L.trust.bundle], ['custom_ca', L.trust.custom_ca], ['insecure', L.trust.insecure]], cfg.trust);
  /* An Onocoy slot stored in the older layout (no username) holds the old credential in `mountpoint`, not the
   * console's mountpoint: start empty, as the wizard does. */
  const oldLayout = isOnocoySlot(cfg) && !cfg.username;
  const mp = textInput(oldLayout ? '' : cfg.mountpoint, { maxlength: 47 });
  const user = textInput(cfg.username, { maxlength: 47 });
  const pw = pwInput({ maxlength: 63, placeholder: cfg.password_set ? 'Stored (leave empty to keep)' : '' });
  const clearPw = checkbox('Remove the stored password', false);
  const backoff = selectInput([['standard', 'Standard'], ['polite', 'Polite (slower retries)']], cfg.backoff_profile === 'polite' ? 'polite' : 'standard');
  /* The retry pace across caster switches. presetPace: a preset set the pace shown (in this editor, or RTK2go's fixed
   * polite of a stored RTK2go slot), not the owner. ownPace: the owner's pace to go back to: the stored one where the
   * owner sets it (Other caster, Centipede, Emlid, RTKDirect), or what they picked here; else standard. */
  let presetPace = !isNew && kind === 'rtk2go';
  let ownPace = !isNew && kind !== 'rtk2go' && kind !== 'onocoy' ? backoff.value : 'standard';
  const raw = checkbox('Send NTRIP 2.0 data without chunked encoding', cfg.v2_raw_body, 'Only for casters that need it; most expect chunks.');

  const F = {
    name: field('Name', name, 'For this page only. At most 23 bytes (fewer characters with accents or symbols).'),
    host: field('Host', host),
    port: field('Port', port),
    mountpoint: field('Mountpoint', mp),
    username: field('Username', user),
    password: field('Password', pw),
    trust: field('Certificate check', trust),
    backoff_profile: field('Retry pace', backoff, 'Polite waits longer between attempts; some casters ban fast reconnects.'),
  };
  /* What the console gives, its password rule, and a note while the typed password breaks that rule (saving stays possible). */
  const pwNote = h('p', { class: 'small t-warn', role: 'status', hidden: true });
  const onoNote = h('div', { class: 'stack' }, h('p', { class: 'small' }, ONOCOY_CONSOLE), h('p', { class: 'small' }, ONOCOY_PW_RULE));
  const showPwNote = () => {
    pwNote.textContent = kind === 'onocoy' ? onocoyPwNote(pw.value) : '';
    pwNote.hidden = !pwNote.textContent;
  };
  pw.addEventListener('input', showPwNote);
  const note = h('p', { class: 'muted small' });
  const elsewhereNote = elsewhere ? h('div', { class: 'msg info', role: 'note' }, onocoyElsewhereText(elsewhere),
    opts.openSlot ? h('div', { class: 'form-actions' }, btn('Open slot ' + (elsewhere.slot + 1), () => opts.openSlot(elsewhere.slot), 'small')) : null) : null;
  const tlsNote = h('small', { class: 'hint' });
  const forcedPace = h('p', { class: 'muted small' });
  const caBox = h('div', { class: 'stack' });
  const tlsBox = h('div', { class: 'field' }, tls.el, tlsNote);
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
    /* Onocoy's host and port are fixed: shown as the console shows them. */
    host.readOnly = ono;
    port.readOnly = ono;
    F.name.el.hidden = ono;
    F.username.el.hidden = !ono && !v2();
    onoNote.hidden = !ono;
    tlsBox.hidden = ono && !tls.input.checked;   /* Onocoy documents the plain port only */
    versionNote.textContent = ono ? (v2() ? 'NTRIP 2.0 sends the mountpoint and logs in with the username and password.' :
      'NTRIP 1.0 (the default) sends "SOURCE <password> /<username>": the username as the mountpoint, as Onocoy asks of NTRIP 1.0 sources.') :
      v2() ? 'NTRIP 2.0 logs in with the username and password.' : 'NTRIP 1.0 sends the password and the mountpoint in its first line.';
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
    /* Where Onocoy is: for a new slot, and for any slot switched to Onocoy here. */
    if (elsewhereNote) elsewhereNote.hidden = !(isNew || ono);
    showPwNote();
  }

  function applyPreset() {
    const prev = kind;
    kind = presetSel.value;
    const p = PRESETS.find(x => x.id === kind) || {};
    const prevLabel = (PRESETS.find(x => x.id === prev) || {}).label;
    /* A preset with a pace of its own sets it. One without (Other caster; Onocoy, whose pace the save fixes) never
     * keeps a pace a preset set (RTK2go's polite): back to the owner's. A pace the owner set stays. */
    if (p.backoff_profile === 'polite' || p.backoff_profile === 'standard') {
      backoff.value = p.backoff_profile;
      presetPace = true;
    } else if (presetPace) {
      backoff.value = ownPace;
      presetPace = false;
    }
    if (!name.value || name.value === prevLabel) name.value = kind === 'custom' ? '' : p.label;
    if (kind === 'onocoy') {
      host.value = testSlot ? cfg.host : preset.host;
      port.value = String(testSlot ? cfg.port : tls.input.checked ? preset.tls_port : preset.port);
      version.set(preset.ntrip_version || 1);
    } else if (p.host) {
      host.value = p.host;
      port.value = String(p.port);
      version.set(p.ntrip_version);
      tls.input.checked = false;
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
  backoff.addEventListener('change', () => {
    ownPace = backoff.value;
    presetPace = false;
  });
  tls.input.addEventListener('change', () => {
    if (onocoy() && !testSlot) port.value = String(tls.input.checked ? preset.tls_port : preset.port);
    sync();
  });
  trust.addEventListener('change', sync);
  version.onchange(sync);

  function collect() {
    for (const k of Object.keys(F)) F[k].error(null);
    const ono = onocoy();
    const v = v2();
    const on = enabled.input.checked;
    const hostVal = ono ? (testSlot ? cfg.host : preset.host) : host.value.trim();
    const portVal = ono ? (testSlot ? cfg.port : tls.input.checked ? preset.tls_port : preset.port) : Number(port.value);
    const hostChanged = hostVal.toLowerCase() !== String(cfg.host || '').toLowerCase();
    let bad = null;
    const fail = (k, text) => {
      F[k].error(text);
      if (!bad) bad = F[k].input;
    };
    /* Byte limits: the station stores UTF-8, so accents and symbols take more than one byte. */
    if (!ono && (hasControl(name.value) || utf8Len(name.value) > 23)) fail('name', 'At most 23 bytes (fewer characters with accents or symbols).');
    if (!hostVal && on) fail('host', 'Enter the caster\'s host name.');
    else if (hostVal && !DNS_OR_IP.test(hostVal)) fail('host', 'A host name or an IPv4 address, at most 63 characters.');
    if (!(Number.isInteger(portVal) && portVal >= 1 && portVal <= 65535)) fail('port', 'A port from 1 to 65535.');
    const mpVal = mp.value.trim();
    const userVal = ono ? user.value.trim() : user.value;
    const pwVal = pw.value;
    const keepsPw = cfg.password_set && !hostChanged && !clearPw.input.checked;
    if (ono) {
      const pr = onocoyProblems({ mountpoint: mpVal, username: userVal, password: pwVal }, on, keepsPw);
      for (const k of Object.keys(pr)) fail(k, pr[k]);
    } else {
      /* RTK2go's rules for new names: a slot stored before them keeps its mountpoint while host and name stay. */
      const rtk2goNew = hostIn(hostVal, 'rtk2go.com') && (hostChanged || mpVal !== (cfg.mountpoint || ''));
      /* RTK2go's rule first: the generic one allows "." and 47 characters, which RTK2go refuses. */
      if (!mpVal && on) fail('mountpoint', 'Enter the mountpoint.');
      else if (rtk2goNew && mpVal && rtk2goMountProblem(mpVal)) fail('mountpoint', rtk2goMountProblem(mpVal));
      else if (mpVal && !MOUNT_RE.test(mpVal)) fail('mountpoint', 'Letters, digits, ".", "_" and "-" only, at most 47.');
      if (v) {
        if (userVal.indexOf(':') >= 0 || utf8Len(userVal) > 47 || hasControl(userVal)) fail('username', 'At most 47 bytes, without ":".');
        else if (on && !userVal) fail('username', 'NTRIP 2.0 needs a username.');
      }
      if (utf8Len(pwVal) > 63 || hasControl(pwVal)) fail('password', 'At most 63 bytes.');
      else if (!v && /\s/.test(pwVal)) fail('password', 'NTRIP 1.0 passwords cannot contain spaces.');
      else if (on && !pwVal && !keepsPw && !v) fail('password', cfg.password_set && hostChanged ?
        'The host changed: enter the password for the new host.' : 'Enter the password.');
    }
    if (bad) {
      bad.focus();
      return null;
    }
    /* No host and not enabled: the slot is empty again, and an empty slot has no name (not the preset's). */
    const emptied = !hostVal && !on;
    const body = {
      enabled: on,
      name: emptied ? '' : ono ? (cfg.name && isOnocoySlot(cfg) ? cfg.name : 'Onocoy') :
        name.value.trim() || (kind === 'custom' ? hostVal.slice(0, 23) : (PRESETS.find(x => x.id === kind) || {}).label || ''),
      host: hostVal,
      port: portVal,
      ntrip_version: v ? 2 : 1,
      tls: tls.input.checked,
      mountpoint: mpVal,
      backoff_profile: ono ? 'fast' : kind === 'rtk2go' ? 'polite' : backoff.value,
    };
    if (tls.input.checked) body.trust = ono ? 'bundle' : trust.value;
    if (ono || v) body.username = userVal;
    if (v && !ono) body.v2_raw_body = raw.input.checked;
    if (pwVal) body.password = pwVal;
    else if (clearPw.input.checked) body.clear_password = true;
    if (testSlot) body.test_onocoy = ono;   /* switched to another caster: no longer a test Onocoy slot */
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
      if (e instanceof ApiError && e.code === 'invalid_field' && F[e.field] && !F[e.field].el.hidden) {
        F[e.field].error(ruleText(e));
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
  elsewhereNote,
  testSlot ? h('p', { class: 'small' }, badge('Test Onocoy slot (bench build)', 'warn'),
    ' Counts as Onocoy at this host, for a local test caster.') : null,
  note,
  enabled.el,
  F.name.el,
  h('div', { class: 'cols c2' }, F.host.el, F.port.el),
  F.mountpoint.el,
  F.username.el,
  F.password.el,
  pwNote,
  clearPw.el,
  onoNote,
  version.el,
  tlsBox,
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

/** A stopped slot: why, what to do, and the retry. */
function stopPanel(c, st) {
  const why = L.stop[st.stop_reason];
  const text = Array.isArray(why) ? why[1] : 'The station stopped this slot and makes no attempt until you act.';
  return msg('bad', h('strong', null, 'Stopped: ' + lbl(L.stop, st.stop_reason)), h('p', null, text),
    h('div', { class: 'form-actions' }, btn('Try again', () => retryCaster(c.slot), 'small primary')));
}

function casterSummary(c, st, preset) {
  const tone = casterTone(st.state);
  const ono = isOnocoySlot(c);
  const metrics = [];
  if (st.state === 'streaming') {
    metrics.push(['Connected', fmt.dur(st.connected_s)]);
    metrics.push(['Lag', fmt.ms(st.lag_ms_now)]);
    const r = slotRate(c.slot, 10);
    if (r !== null) metrics.push(['Rate', fmt.kbps(r)]);
  } else if (c.enabled) {
    metrics.push(['For', fmt.dur(st.state_age_s)]);
    if (st.next_retry_s) metrics.push(['Retry in', fmt.dur(st.next_retry_s)]);
  }
  const on = st.today_online_s || 0, off = st.today_offline_s || 0;
  if (on + off > 0) metrics.push(['Today online', fmt.dur(on)]);
  if (off) metrics.push(['Today offline', fmt.dur(off)]);
  /* Epoch counts live in RAM: today since the last start (online and offline above come from the ledger). */
  if (st.epochs_dropped_today) metrics.push(['Epochs dropped today (since the start)', fmt.num(st.epochs_dropped_today)]);
  const avg = c.backoff_profile === 'polite' && st.state === 'streaming' ? slotRate(c.slot, 60) : null;
  const stopped = st.state === 'stopped';
  /* The login guard counting: at the third refusal the slot stops (the station's hint says the same: shown once). */
  const refused = !stopped && st.login_rejects > 0 && st.state !== 'streaming' ?
    'Onocoy refused the login (' + st.login_rejects + ' of 3)' : '';
  const out = [
    h('div', { class: 'muted small' }, [c.host + ':' + c.port, 'NTRIP ' + (c.ntrip_version === 2 ? '2.0' : '1.0'), c.tls ? 'TLS' : null,
      c.mountpoint ? '/' + c.mountpoint : null].filter(Boolean).join(' · ')),
    stopped ? stopPanel(c, st) : null,
    st.reason && st.reason !== 'none' && st.state !== 'streaming' ? h('div', { class: 't-' + (tone || 'warn') + ' small' }, lbl(L.fail, st.reason)) : null,
    refused ? h('div', { class: 't-bad small' }, refused) : null,
    h('div', { class: 'metrics' }, metrics.map(m => h('span', null, m[0] + ' ', h('b', null, m[1])))),
    st.hint && !stopped && st.hint !== refused ? h('div', { class: 'hintline' }, st.hint) : null,
    st.unreached_s ? h('div', { class: 'hintline' }, 'Caster not reached for ' + fmt.dur(st.unreached_s) +
      ' (name lookup or connection failing before the caster answered).') : null,
  ];
  if (avg !== null) {
    out.push(h('div', { class: 'small ' + (avg > 28000 ? 't-warn' : 'muted') }, 'Average over 60 s: ' + fmt.kbps(avg) +
      (avg > 28000 ? '. That is close to the limits of public casters; consider MSM4 messages instead of MSM7.' : '')));
  }
  /* RTK2go and other polite casters count volume per day (RAM: since the last start). */
  if (c.backoff_profile === 'polite' && st.bytes_sent_today !== undefined && st.bytes_sent_today !== null) {
    out.push(h('div', { class: 'small muted' }, 'Sent today (since the start): ' + volText(st.bytes_sent_today)));
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
      st.stop_reason && st.stop_reason !== 'none' ? ['Stopped because', lbl(L.stop, st.stop_reason)] : null,
      ['Last reply', st.last_status_line ? st.last_status_line + (st.last_status_code ? ' (' + st.last_status_code + ')' : '') : DASH],
      st.last_session_text ? ['Caster said during a session', st.last_session_text] : null,
      st.login_rejects || st.login_blocks ? ['Refused logins / login blocks', fmt.num(st.login_rejects) + ' / ' + fmt.num(st.login_blocks) +
        ' (the third of either stops the slot)'] : null,
      st.kicked_sessions ? ['Sessions taken over (score)', Math.min(st.kicked_sessions, 3) + ' of 3'] : null,
      st.server_header ? ['Caster software', st.server_header] : null,
      ['Address', st.resolved_ip ? st.resolved_ip + (st.dns_cached_used ? ' (cached DNS used ' + st.dns_cached_used + '×)' : '') : DASH],
      st.unreached_s ? ['Not reached for', fmt.dur(st.unreached_s)] : null,
      st.tls_version ? ['TLS', st.tls_version + (st.tls_cipher ? ' · ' + st.tls_cipher : '')] : null,
      ['Sent', fmt.bytes(st.bytes_sent) + ' · ' + fmt.num(st.frames_sent) + ' frames · ' + fmt.num(st.epochs_sent) + ' epochs'],
      ['Epochs today (since the start)', fmt.num(st.epochs_sent_today) + ' sent · ' + fmt.num(st.epochs_dropped_today) + ' dropped'],
      st.bytes_sent_today !== undefined ? ['Sent today (since the start)', volText(st.bytes_sent_today)] : null,
      ['Dropped', fmt.num(st.frames_dropped) + ' frames · ' + fmt.bytes(st.bytes_dropped) + ' · ' + fmt.num(st.epochs_dropped) + ' epochs'],
      ['Lag now / limit', fmt.ms(st.lag_ms_now) + ' / ' + fmt.ms(st.max_lag_ms)],
      ['Lag drops / laps', fmt.num(st.lag_drops) + ' / ' + fmt.num(st.laps)],
      ['Skipped while idle', fmt.num(st.frames_skipped_idle)],
      ['Retry attempt', fmt.num(st.backoff_attempt)],
      /* Onocoy's day: so far today, never red while the streak holds (null while the slot is not set up and on). */
      st.graced_uptime_so_far_permille !== null && st.graced_uptime_so_far_permille !== undefined ?
        ['Graced uptime so far', soFarView(st.graced_uptime_so_far_permille, st.streak_ok)] : null,
      /* (online today + the 300 s grant) / 24 h: what the whole day reaches if nothing more is sent. */
      st.graced_uptime_permille !== null && st.graced_uptime_permille !== undefined ?
        ['Day covered so far (online + 5 min grant)', fmt.permille(st.graced_uptime_permille)] : null,
      st.data_rate_permille !== null && st.data_rate_permille !== undefined ? ['Data rate', fmt.permille(st.data_rate_permille)] : null,
      st.budget_s !== null && st.budget_s !== undefined ? ['Offline budget today', 'about ' + fmt.dur(st.budget_s)] : null,
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
  create(el, arg) {
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
      if (isOnocoySlot(c) && !onocoyReady(c)) {
        return [h('h2', null, c.name || 'Onocoy', badge('Not set up', '')),
          h('p', null, 'Onocoy not set up.'),
          h('p', { class: 'muted small' }, ONOCOY_CONSOLE),
          st.state === 'streaming' ? msg('info', 'It streams with credentials stored by an earlier version. Enter the three values ' +
            'from the Onocoy console to finish the setup.') : null,
          h('div', { class: 'form-actions' }, btn('Add your Onocoy credentials', () => edit(s), 'primary'))];
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
        h('h2', null, c.name || 'Slot ' + (s.slot + 1), c.enabled ? r.badge : badge('Off', ''),
          c.test_onocoy ? badge('Test Onocoy slot (bench build)', 'warn') : null),
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
      /* Another slot with an Onocoy host (the enabled one first): this one starts elsewhere and says where Onocoy is. */
      const onoSlots = data.slots.filter(x => x.slot !== s.slot && x.config && isOnocoySlot(x.config));
      const ono = onoSlots.find(x => x.config.enabled) || onoSlots[0];
      editor = casterEditor(s, {
        preset: data.onocoy_preset,
        hostname: S.status && S.status.device ? S.status.device.hostname : '',
        hideOnocoy: onoSlots.some(x => x.config.enabled),
        onocoyElsewhere: ono ? { slot: ono.slot, ready: onocoyReady(ono.config), enabled: !!ono.config.enabled } : null,
        openSlot: n => {
          const t = data.slots.find(x => x.slot === n);
          if (t) edit(t);
        },
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
      if (arg === 'onocoy') {
        /* From "Add your Onocoy credentials": the Onocoy slot's editor (or a free slot for it). */
        history.replaceState(null, '', '#/casters');
        if (shell.current) shell.current.arg = '';
        const target = data.slots.find(x => x.config && isOnocoySlot(x.config)) ||
          data.slots.find(x => x.config && !x.config.host && !x.config.enabled);
        if (target) edit(target);
      }
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

