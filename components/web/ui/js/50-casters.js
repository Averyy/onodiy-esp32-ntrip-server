/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * NTRIP: one card per server (its name, Edit and an on/off switch that
 * saves at once, its state and traffic in one line, what it said when it
 * refuses or drops the station), one card to add a server while slots are free,
 * and the editor (the setup's Onocoy step uses its Onocoy rules). The code
 * keeps the API's word for a server: caster.
 *
 * Editor rules mirror the station's (PUT /api/casters/{n}), which has the
 * last word; byte limits count UTF-8 bytes. Passwords are write-only: an
 * empty field keeps the stored one while the host stays. Onocoy and an open
 * server at once need the user's acknowledgement (409 caster_policy /
 * dup_stream_ack_required); while both stream, the open server's card
 * carries a standing warning (GET /api/casters warnings) that blocks nothing.
 *
 * The form asks only what the chosen server needs (one line of help with
 * its guide, PRESET_INFO): for Onocoy the console's username and password
 * and a mountpoint that is any name (the station's proposed); a slot the
 * login guard stopped shows why and a "Try again" that warns first. A test
 * Onocoy slot (bench and QEMU builds) counts as Onocoy at its own host. A
 * save keeps whether the server is on; its card's switch turns it off and
 * on.
 *
 * One Onocoy slot per station: a new slot starts on RTK2go while another
 * slot has an Onocoy host, and says so once Onocoy is chosen; the Onocoy
 * choice is hidden while another enabled slot streams to Onocoy. A slot
 * saved without a host and not enabled is empty again, stored without a name.
 *
 * After the servers, the card "Local server": the station's own corrections
 * for the owner's rover on the home network (GET/PUT /api/local-caster; the
 * page never calls it a caster or a server).
 */

const PRESETS = [
  { id: 'onocoy', label: 'Onocoy' },
  { id: 'rtk2go', label: 'RTK2go', host: 'rtk2go.com', port: 2101, ntrip_version: 1, backoff_profile: 'polite' },
  { id: 'centipede', label: 'Centipede', host: 'crtk.net', port: 2101, ntrip_version: 1, backoff_profile: 'standard' },
  { id: 'emlid', label: 'Emlid Caster', host: 'caster.emlid.com', port: 2101, ntrip_version: 1, backoff_profile: 'standard' },
  { id: 'rtkdirect', label: 'RTKDirect', host: 'ntrip.rtkdirect.com', port: 2101, ntrip_version: 2, backoff_profile: 'standard' },
  { id: 'custom', label: 'Other server' },
];

/*
 * Per server: one line on what to enter, from that server's documentation,
 * and its own guide for the rest (Other server: none).
 */
const PRESET_INFO = {
  onocoy: {
    help: 'The Onocoy console gives the username and password, under NTRIP Credentials.',
    guide: ['Onocoy\'s guide', 'https://docs.onocoy.com/documentation/3.-become-a-miner/3.-connect-your-station-to-onocoy'],
  },
  rtk2go: {
    help: 'The mountpoint and password of your reservation. One base per internet connection.',
    guide: ['RTK2go\'s reservation page', 'http://rtk2go.com/base-station-reservation/'],
  },
  centipede: {
    help: 'A mountpoint of capital letters no other base uses; the password is Centipede\'s shared one.',
    guide: ['Centipede\'s guide', 'https://docs.centipede-rtk.org/connect-base/'],
  },
  emlid: {
    help: 'The mount point and password Emlid Caster shows for your base.',
    guide: ['Emlid\'s guide', 'https://docs.emlid.com/emlid-caster/base-connection-to-mount-point/'],
  },
  rtkdirect: {
    help: 'The login (as the username), password and mount point of your hotspot.',
    guide: ['RTKDirect\'s site', 'https://cloud.rtkdirect.com/'],
  },
  custom: { help: 'Enter what your server gives you for a base.', guide: null },
};

/** The server's help line and its guide (fills `el`, or a new line). */
function presetHelp(id, el) {
  const p = PRESET_INFO[id] || PRESET_INFO.custom;
  return mount(el || h('p', { class: 'muted small' }), p.help, p.guide ? [' ', extLink(p.guide[0], p.guide[1])] : null);
}

/** Onocoy's fixed server, as a line of text ("Server: servers.onocoy.com:2101"), not as inputs that look editable. */
function serverLine(el, host, port) {
  return mount(el || h('p', { class: 'muted small' }), 'Server: ', h('strong', { class: 'nowrap' }, host + ':' + port));
}

/** The soft note for a typed Onocoy password that breaks the console's rule ('' when it follows it or nothing is typed). */
function onocoyPwNote(pw) {
  if (!pw || (pw.length >= 8 && /[A-Za-z]/.test(pw) && /[0-9]/.test(pw))) return '';
  return 'Onocoy passwords have at least 8 characters with a letter and a digit. Check that you copied all of it.';
}

/* Onocoy's rule on the same stream submitted more than once (its FAQ, linked). */
const ONOCOY_FAQ = 'https://docs.onocoy.com/documentation/8.-faq-troubleshooting/frequently-asked-questions';

const DUP_TEXT = 'Streams on RTK2go and Centipede are public, so anyone could submit this station\'s stream to Onocoy as well. ' +
  'Onocoy\'s FAQ says the same stream submitted more than once, even by another user, leads to a ban.';

function dupText() {
  return h('p', null, DUP_TEXT + ' ', extLink('Onocoy\'s FAQ', ONOCOY_FAQ));
}

/** The open slots the duplicate-stream warning names (GET /api/casters warnings: Onocoy and an open server both on). */
function openCasterSlots(d) {
  const w = ((d && d.warnings) || []).find(x => x.id === 'onocoy_open_caster');
  return w ? (w.open_slots || []).slice() : [];
}

/**
 * The standing warning on the card of an open server that streams while
 * Onocoy does (slot in openCasterSlots(d)), or null. Blocks nothing.
 */
function openCasterWarning(d, slot) {
  if (openCasterSlots(d).indexOf(slot) < 0) return null;
  const cfg = ((d.slots || []).find(x => x.slot === slot) || {}).config || {};
  const kind = presetOf(cfg.host);
  const other = kind === 'rtk2go' ? 'RTK2go' : kind === 'centipede' ? 'Centipede' : cfg.name || 'this server';
  return msg('warn', h('strong', null, 'Onocoy and ' + other + ' are both on'), dupText(),
    h('p', { class: 'small' }, 'Both keep streaming. Turn one off to avoid the risk.'));
}

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
  if (m.charAt(0) === 'A') return 'RTK2go does not accept new mountpoints that start with "A".';
  if (m.length < 4 || m.length > 20) return 'RTK2go mountpoints have 4 to 20 characters.';
  return null;
}

const RETRY_TEXT = 'Make sure no other device streams with this credential and that it matches the Onocoy console. ' +
  'Every failed login counts toward Onocoy\'s login block.';

/** The user's retry of a stopped (or waiting) slot, after the warning. */
async function retryCaster(slot) {
  const ok = await dialog({ title: 'Try again', ok: 'Try once now', body: h('p', null, RETRY_TEXT),
    onOk: () => api('POST', '/api/casters/' + slot + '/retry', {}) });
  if (ok) toast('The station tries once now.', 'ok');
  return ok;
}

/** A station sentence ("waiting for the receiver…") as a line of its own. */
function capFirst(t) {
  t = String(t || '');
  return t.charAt(0).toUpperCase() + t.slice(1);
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
  const go = btn('Save anyway', () => onAck(go), 'primary', { disabled: true });
  ack.input.addEventListener('change', () => {
    go.disabled = !ack.input.checked;
  });
  return h('div', { class: 'msg warn', role: 'alert' }, h('strong', null, 'Duplicate-stream risk'), dupText(), ack.el,
    h('div', { class: 'form-actions' }, go));
}

const DNS_OR_IP = /^(?=.{1,63}$)[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)*\.?$/;

/* Mountpoint and username as the station takes them (NTRIP 1.0 sends the Onocoy username as the mountpoint). */
const MOUNT_RE = /^[A-Za-z0-9._-]{1,47}$/;

/** True when an Onocoy slot is set up: a mountpoint (a name of the owner's), the console's username and a password. */
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
    out.mountpoint = v.mountpoint ? 'Letters, digits, ".", "_" and "-" only, at most 47.' : 'Enter any name, such as the station\'s.';
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
  return capFirst(m) + (/[.!?]$/.test(m) ? '' : '.');
}

/** Where Onocoy is, for the editor of another slot switched to Onocoy (onocoyElsewhere: {slot, ready, enabled}). */
function onocoyElsewhereText(o) {
  if (!o.ready) return 'Onocoy is already in your list but not set up yet. Open it to add your credentials.';
  if (!o.enabled) return 'Onocoy is already set up here and turned off. Open it to turn it on.';
  return 'Another server here already streams to Onocoy, and Onocoy allows one stream per station.';
}

/*
 * The retry paces Other server offers, as the station runs them (ntrip_proto.c: k_transient, k_auth,
 * NTRIP_POLITE_HOURLY_AFTER; every wait ±20 %): [the choice with its waits after a failed or dropped connection, its
 * waits after a refused login]. A preset's pace is its own (Onocoy: the station's fast one; RTK2go: polite).
 */
const PACE = {
  standard: [lbl(L.backoff, 'standard') + ' (2s to 1min)', 'Refused logins: 1, 5, 15, then 30min.'],
  polite: [lbl(L.backoff, 'polite') + ' (10s to 5min)',
    'Refused logins: 5, 15, 30, then 60min; after 20 failed connections in a row, one try an hour.'],
};

/** The mountpoint the editor proposes for Onocoy: the station's name as the field takes it (Onocoy shows it only to you). */
function onocoyMountName(hostname) {
  return String(hostname || '').replace(/[^A-Za-z0-9._-]/g, '').slice(0, 47);
}

/** "host:port" (or a host alone: port 2101) as {host, port}; port NaN when what follows the colon is not a port. */
function splitHostPort(text) {
  const t = String(text || '').trim();
  const i = t.lastIndexOf(':');
  if (i < 0) return { host: t, port: 2101 };
  const p = t.slice(i + 1).trim();
  return { host: t.slice(0, i).trim(), port: /^\d{1,5}$/.test(p) ? Number(p) : NaN };
}

/**
 * NTRIP server editor. entry = {slot, config}. opts: preset (onocoy_preset),
 * hostname (the Onocoy mountpoint it proposes), hideOnocoy (no Onocoy
 * choice), onocoyElsewhere ({slot, ready, enabled} of another Onocoy slot),
 * openSlot(n), onSaved(entry), onCancel(). Returns {el, dirty()}.
 *
 * Only what the chosen server needs: its type with one line of help, then
 * host and port (a preset's as text), mountpoint, username (Onocoy and
 * NTRIP 2.0) and password, two columns from 560 px; Other server adds one
 * row: NTRIP version, TLS and the retry pace. A preset's other settings are
 * its own: the stored ones while it stays chosen, the preset's once chosen
 * (Onocoy's NTRIP version from the station's onocoy_preset). The name
 * follows the server (the preset's, or the host); a save keeps whether the
 * server is on, but a new server and an Onocoy slot that gets set up are
 * saved turned on.
 */
function casterEditor(entry, opts) {
  const slot = entry.slot;
  const cfg = Object.assign({ enabled: false, name: '', host: '', port: 2101, ntrip_version: 1, tls: false, trust: 'bundle',
    ca_pem_set: false, mountpoint: '', username: '', password_set: false, label: '', v2_raw_body: false,
    backoff_profile: 'standard', dup_stream_ack: false }, entry.config || {});
  /* Onocoy's server and NTRIP version as the station gives them (onocoy_preset); 1.0 only from a station that gives none. */
  const preset = Object.assign({ host: 'servers.onocoy.com', port: 2101, tls_port: 2121, ntrip_version: 1 }, opts.preset || {});
  const isNew = !cfg.host;
  /* A test Onocoy slot (bench and QEMU builds) counts as Onocoy at its own host and port. */
  const testSlot = !!cfg.test_onocoy;
  const elsewhere = opts.onocoyElsewhere || null;
  let kind = isNew ? (opts.hideOnocoy || elsewhere ? 'rtk2go' : 'onocoy') : isOnocoySlot(cfg) ? 'onocoy' : presetOf(cfg.host);
  const storedKind = kind;
  let dirty = false;
  /* An Onocoy slot that is not set up yet: setting it up means using it. */
  const noCreds = isOnocoySlot(cfg) && !onocoyReady(cfg);
  const on = isNew || noCreds || !!cfg.enabled;
  const labelOf = id => (PRESETS.find(x => x.id === id) || {}).label || '';
  /* A preset's host and port: the stored ones until another server is chosen. */
  let fixedHost = cfg.host, fixedPort = cfg.port;

  const choices = PRESETS.filter(p => !(p.id === 'onocoy' && opts.hideOnocoy && kind !== 'onocoy'));
  const presetSel = selectInput(choices.map(p => [p.id, p.label]), kind);
  const hostPort = textInput(kind === 'custom' ? cfg.host + ':' + cfg.port : '', { maxlength: 69, inputmode: 'url',
    placeholder: 'host:port' });
  /* Not set up yet: Onocoy's NTRIP version from the station (2.0 sends the mountpoint the owner chose). */
  const version = selectInput([[1, 'NTRIP 1.0'], [2, 'NTRIP 2.0']], noCreds ? preset.ntrip_version : cfg.ntrip_version);
  const tls = checkbox('Encrypted', cfg.tls);
  const trust = selectInput([['bundle', L.trust.bundle], ['custom_ca', L.trust.custom_ca], ['insecure', L.trust.insecure]], cfg.trust);
  /* An Onocoy slot stored in the older layout (no username) holds the old credential in `mountpoint`, not a name of
   * the owner's: it starts with the proposed name, as an empty one does. */
  const oldLayout = isOnocoySlot(cfg) && !cfg.username;
  const mountName = onocoyMountName(opts.hostname);
  const mp = textInput(oldLayout ? '' : cfg.mountpoint, { maxlength: 47 });
  const user = textInput(cfg.username, { maxlength: 47 });
  const pw = pwInput({ maxlength: 63, placeholder: cfg.password_set ? 'Stored (leave empty to keep)' : '' });
  const backoff = selectInput([['standard', PACE.standard[0]], ['polite', PACE.polite[0]]], cfg.backoff_profile === 'polite' ? 'polite' : 'standard');
  /* The retry pace across server switches. presetPace: a preset set the pace (in this editor, or RTK2go's fixed
   * polite of a stored RTK2go slot), not the owner. ownPace: the owner's pace to go back to on Other server: the
   * stored one where the owner set it (Other server, or a preset kept from before), or what they picked here; else
   * standard. */
  let presetPace = !isNew && kind === 'rtk2go';
  let ownPace = !isNew && kind !== 'rtk2go' && kind !== 'onocoy' ? backoff.value : 'standard';

  const F = {
    host: field('Host and port', hostPort),
    mountpoint: field('Mountpoint', mp, 'Any name; shows only in your Onocoy dashboard.'),
    username: field('Username', user),
    password: field('Password', pw),
    ntrip_version: field('NTRIP version', version),
    trust: field('Certificate check', trust),
    backoff_profile: field('Retry pace', backoff, PACE.standard[1]),
  };
  F.port = F.host;   /* a refused port is the same field's */
  const mpHint = F.mountpoint.el.querySelector('.hint');
  const paceHint = F.backoff_profile.el.querySelector('.hint');
  /* A preset's host and port: text where Other server has its field. */
  /* A preset's host and port: a field that cannot be edited, so the form reads alike for every server. */
  const fixedText = h('input', { type: 'text', disabled: true });
  const fixedField = h('div', { class: 'field' }, h('label', null, h('span', { class: 'lbl' }, 'Host and port'), fixedText));
  /* TLS, and its certificate check while it is on. */
  const tlsField = h('div', { class: 'field' }, h('span', { class: 'lbl' }, 'TLS'), h('div', { class: 'fixed-value' }, tls.el),
    F.trust.el);
  const title = h('h2');
  const help = h('p', { class: 'muted small' });
  /* A note while the typed password breaks Onocoy's console rule (saving stays possible). */
  const pwNote = h('p', { class: 'small t-warn', role: 'status', hidden: true });
  const showPwNote = () => {
    pwNote.textContent = kind === 'onocoy' ? onocoyPwNote(pw.value) : '';
    pwNote.hidden = !pwNote.textContent;
  };
  pw.addEventListener('input', showPwNote);
  const elsewhereNote = elsewhere ? h('div', { class: 'msg info', role: 'note' }, h('p', null, onocoyElsewhereText(elsewhere)),
    opts.openSlot ? h('div', { class: 'form-actions' }, btn('Open Onocoy', () => opts.openSlot(elsewhere.slot), 'small')) : null) : null;
  const testNote = testSlot ? h('p', { class: 'small' }, badge('Test Onocoy slot (bench build)', 'warn'),
    ' Counts as Onocoy at this host, for a local test server.') : null;
  /* Other server's own settings: its retry pace beside the password, NTRIP version and TLS in a row under them (TLS's
   * certificate check under it). */
  const own = h('div', { class: 'cols c2' }, F.ntrip_version.el, tlsField);
  const caBox = h('div', { class: 'stack' });
  const ackNote = cfg.dup_stream_ack ? h('p', { class: 'muted small' }, 'You accepted the duplicate-stream risk for this server.') : null;
  const err = errBox();
  const dupBox = h('div');
  const saveBtn = h('button', { type: 'submit', class: 'btn primary' }, 'Save');
  const actions = h('div', { class: 'form-actions' }, saveBtn, opts.onCancel ? btn('Cancel', () => opts.onCancel(), 'ghost') : null);

  /* Custom CA certificate (stored at once, separate from Save). */
  const pem = h('textarea', { rows: 6, placeholder: '-----BEGIN CERTIFICATE-----', spellcheck: 'false' });
  const caState = h('span', { class: 'muted small' });
  const caErr = errBox();
  let caSet = cfg.ca_pem_set;
  const showCa = () => {
    caState.textContent = caSet ? 'A CA certificate is stored for this server.' : 'No CA certificate stored yet.';
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
      toast('Certificate stored. The station reconnects.', 'ok');
    }, caErr);
  }, 'small');
  const removeCa = btn('Remove', () => act(removeCa, async () => {
    await api('DELETE', '/api/casters/' + slot + '/ca');
    caSet = false;
    showCa();
  }, caErr), 'small danger');
  showCa();
  add(caBox, [field('CA certificate (PEM)', pem, 'The certificate of the authority that signed the server\'s certificate.').el,
    h('div', { class: 'row' }, storeCa, removeCa, caState), caErr]);

  const onocoy = () => kind === 'onocoy';
  const v2 = () => version.value === '2';

  const form = h('form', { class: 'stack', onsubmit: e => {
    e.preventDefault();
    save(false);
  }, oninput: () => {
    dirty = true;
  } });
  /* Onocoy already elsewhere: that comes first, before how to set up a new one. */
  mount(form, h('div', { class: 'card-head' }, title), h('div', { class: 'cols c2' }, field('NTRIP server', presetSel).el), help,
    elsewhereNote, testNote,
    h('div', { class: 'cols c2' }, F.host.el, fixedField, F.mountpoint.el, F.username.el, F.password.el, F.backoff_profile.el), pwNote,
    own, caBox, ackNote, dupBox, err, actions);

  function sync() {
    const ono = onocoy(), custom = kind === 'custom';
    presetHelp(kind, help);
    F.host.el.hidden = !custom;
    fixedField.hidden = custom;
    fixedText.value = fixedHost + ':' + fixedPort;
    mpHint.hidden = !ono;
    F.username.el.hidden = !ono && !v2();
    own.hidden = !custom;
    F.backoff_profile.el.hidden = !custom;
    paceHint.textContent = PACE[backoff.value === 'polite' ? 'polite' : 'standard'][1];
    F.trust.el.hidden = !custom || !tls.input.checked;
    caBox.hidden = F.trust.el.hidden || trust.value !== 'custom_ca';
    /* Where Onocoy is, while Onocoy is the choice. */
    if (elsewhereNote) elsewhereNote.hidden = !ono;
    showPwNote();
  }

  function applyPreset() {
    const prev = kind;
    kind = presetSel.value;
    const p = PRESETS.find(x => x.id === kind) || {};
    /* A preset with a pace of its own sets it. One without (Other server; Onocoy, whose pace the save fixes) never
     * keeps a pace a preset set (RTK2go's polite): back to the owner's. A pace the owner set stays. */
    if (p.backoff_profile === 'polite' || p.backoff_profile === 'standard') {
      backoff.value = p.backoff_profile;
      presetPace = true;
    } else if (presetPace) {
      backoff.value = ownPace;
      presetPace = false;
    }
    if (kind === 'onocoy') {
      fixedHost = testSlot ? cfg.host : preset.host;
      fixedPort = testSlot ? cfg.port : preset.port;
      version.value = String(preset.ntrip_version);
      tls.input.checked = false;
    } else if (p.host) {
      fixedHost = p.host;
      fixedPort = p.port;
      version.value = String(p.ntrip_version);
      tls.input.checked = false;
    } else if (prev !== 'custom') {
      hostPort.value = '';
    }
    /* The proposed Onocoy mountpoint goes with Onocoy; an empty one is proposed. */
    if (prev === 'onocoy' && mp.value === mountName) mp.value = '';
    if (kind === 'onocoy' && !mp.value) mp.value = mountName;
    sync();
  }

  presetSel.addEventListener('change', () => {
    dirty = true;
    /* A duplicate-stream question was about the server chosen before. */
    clear(dupBox);
    saveBtn.hidden = false;
    applyPreset();
  });
  backoff.addEventListener('change', () => {
    ownPace = backoff.value;
    presetPace = false;
    sync();
  });
  tls.input.addEventListener('change', sync);
  trust.addEventListener('change', sync);
  version.addEventListener('change', sync);

  function collect() {
    for (const k of Object.keys(F)) F[k].error(null);
    const ono = onocoy(), custom = kind === 'custom';
    const v = v2();
    const typed = splitHostPort(hostPort.value);
    const hostVal = custom ? typed.host : fixedHost;
    const portVal = custom ? typed.port : fixedPort;
    const hostChanged = hostVal.toLowerCase() !== String(cfg.host || '').toLowerCase();
    let bad = null;
    const fail = (k, text) => {
      F[k].error(text);
      if (!bad) bad = F[k].input;
    };
    /* Byte limits: the station stores UTF-8, so accents and symbols take more than one byte. */
    if (!hostVal && on) fail('host', 'Enter the server\'s host name and port, as host:port.');
    else if (hostVal && !DNS_OR_IP.test(hostVal)) fail('host', 'A host name or an IPv4 address, at most 63 characters.');
    else if (hostVal && !(Number.isInteger(portVal) && portVal >= 1 && portVal <= 65535)) fail('host', 'After the colon, a port from 1 to 65535.');
    const mpVal = mp.value.trim();
    const userVal = ono ? user.value.trim() : user.value;
    const pwVal = pw.value;
    const keepsPw = cfg.password_set && !hostChanged;
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
        if (userVal.indexOf(':') >= 0) fail('username', 'A username cannot contain ":".');
        else if (hasControl(userVal)) fail('username', 'No control characters.');
        else if (utf8Len(userVal) > 47) fail('username', 'Too long: use a shorter username.');
        else if (on && !userVal) fail('username', 'NTRIP 2.0 needs a username.');
      }
      if (hasControl(pwVal)) fail('password', 'No control characters.');
      else if (utf8Len(pwVal) > 63) fail('password', 'Too long: use a shorter password.');
      else if (!v && /\s/.test(pwVal)) fail('password', 'NTRIP 1.0 passwords cannot contain spaces.');
      else if (on && !pwVal && !keepsPw && !v) fail('password', cfg.password_set && hostChanged ?
        'The host changed: enter the password for the new host.' : 'Enter the password.');
    }
    if (bad) {
      bad.focus();
      return null;
    }
    /* No host and not enabled: the slot is empty again, and an empty slot has no name. The name follows the server:
     * the stored one while it stays (Other server: and its host), else the preset's or the host. */
    const emptied = !hostVal && !on;
    const keepName = cfg.name && kind === storedKind && !(custom && hostChanged);
    const body = {
      enabled: on,
      name: emptied ? '' : keepName ? cfg.name : ono ? 'Onocoy' : custom ? hostVal.slice(0, 23) : labelOf(kind),
      host: hostVal,
      port: portVal,
      ntrip_version: v ? 2 : 1,
      tls: tls.input.checked,
      mountpoint: mpVal,
      backoff_profile: ono ? 'fast' : kind === 'rtk2go' ? 'polite' : backoff.value,
    };
    if (tls.input.checked) body.trust = custom ? trust.value : cfg.trust;
    if (ono || v) body.username = userVal;
    if (pwVal) body.password = pwVal;
    if (testSlot) body.test_onocoy = ono;   /* switched to another server: no longer a test Onocoy slot */
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
      saveBtn.hidden = false;
      toast(on ? 'Saved. The station connects with the new settings.' : 'Saved.', 'ok');
      if (opts.onSaved) opts.onSaved(r);
    } catch (e) {
      if (e instanceof ApiError && e.code === 'caster_policy' && e.body.reason === 'dup_stream_ack_required') {
        /* One save button on screen: the panel's "Save anyway". */
        mount(dupBox, dupPanel(b => save(true, b)));
        saveBtn.hidden = true;
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

  title.textContent = isNew ? 'New server' : 'Edit ' + (cfg.name || labelOf(kind) || 'slot ' + (slot + 1));
  if (isNew) applyPreset();
  else {
    if (kind === 'onocoy' && !mp.value) mp.value = mountName;
    sync();
  }
  return { el: form, dirty: () => dirty };
}

/* ---- NTRIP page ----------------------------------------------------------- */

/** A stopped slot: why, what to do, and the retry. */
function stopPanel(c, st) {
  const why = L.stop[st.stop_reason];
  const text = Array.isArray(why) ? why[1] : 'The station stopped this server and makes no attempt until you act.';
  return msg('bad', h('strong', null, 'Stopped: ' + lbl(L.stop, st.stop_reason)), h('p', null, text),
    h('div', { class: 'form-actions' }, btn('Try again', () => retryCaster(c.slot), 'small primary')));
}

/**
 * A server's one detail line (its state line says the state): why it stopped
 * (opts.stopped: the page shows that in its stop panel instead), the login
 * guard's count or the station's hint (not while waiting for data: the state
 * line says so), else the last failure (opts.unreached: the page says that in its
 * not-reached line; opts.tls: the card's plain-port box says the TLS
 * failures, so neither a TLS hint nor a TLS failure here). null when there
 * is nothing to add.
 */
function casterDetail(st, opts) {
  opts = opts || {};
  if (st.state === 'stopped') {
    return opts.stopped && st.stop_reason && st.stop_reason !== 'none' ?
      h('div', { class: 't-bad small' }, 'Stopped: ' + lbl(L.stop, st.stop_reason)) : null;
  }
  const waiting = st.state !== 'streaming';
  const refused = waiting && st.login_rejects > 0 ? 'Onocoy refused the login (' + st.login_rejects + ' of 3)' : '';
  const hint = st.state === 'wait_data' || (opts.tls && /^TLS\b/.test(st.hint || '')) ? '' : st.hint;
  const text = hint || refused;
  if (text) return h('div', { class: 'hintline' }, capFirst(text));
  /* The last failure, unless the state line says the same ("Login refused"). */
  const fail = waiting && st.reason && st.reason !== 'none' && !(opts.tls && st.reason === 'tls') ? lbl(L.fail, st.reason) : '';
  if (fail && fail !== lbl(L.caster, st.state) && !(opts.unreached && st.unreached_s)) {
    return h('div', { class: 't-' + (casterTone(st.state) || 'warn') + ' small' }, fail);
  }
  return null;
}

/*
 * What a server said or did when it refuses or drops the station, each row
 * only when there is something to say (none while all is well): its last
 * reply when that was no OK, a line it sent during a session, the login
 * guard's blocks and taken-over sessions (the third of either stops an
 * Onocoy slot) and, while it does not stream, the failures by cause since the
 * station started (noTls: not TLS, which the card's plain-port box counts).
 * The state, the stop, a refused login and "not reached" are in the card's
 * lines already; the Onocoy day is on the Status page.
 */
function casterEvidence(st, noTls) {
  const fb = st.state === 'streaming' ? {} : st.failures_by_class || {};
  const causes = Object.keys(fb).filter(k => fb[k] > 0 && !(noTls && k === 'tls')).map(k => lbl(L.fail, k) + ' (' + fmt.num(fb[k]) + ')');
  const code = st.last_status_code;
  const rows = [
    st.last_status_line && !(code >= 200 && code < 300) ? ['Last reply', st.last_status_line] : null,
    st.last_session_text ? ['Server said', st.last_session_text] : null,
    st.login_blocks ? ['Login blocks', fmt.num(st.login_blocks) + ' of 3'] : null,
    st.kicked_sessions ? ['Sessions taken over', Math.min(st.kicked_sessions, 3) + ' of 3'] : null,
    causes.length ? ['Failures', causes.join(', ')] : null,
  ].filter(Boolean);
  return rows.length ? kv(rows) : null;
}

/*
 * Slots whose switch to the plain port runs ('busy') or is saved ('done'),
 * kept across the card's rebuild every second: the button keeps its spinner
 * and the keyboard focus (aria-disabled, not disabled) until the answer, and
 * goes once saved.
 */
const PLAIN_SWITCH = {};

/*
 * A server's line under its name: its state in a word, in its colour, and
 * what goes with it: while it streams, for how long, its rate (the last
 * 10 s) and what it sent since the station started; while it waits, its
 * next try. Each separator stays with the part after it.
 */
function casterLine(c, st) {
  if (!c.enabled) return h('div', { class: 'cstate muted' }, lbl(L.caster, 'disabled'));
  const tone = casterTone(st.state);
  const parts = [];
  if (st.state === 'streaming') {
    const r = slotRate(c.slot, 10);
    if (r !== null) parts.push(fmt.kbps(r));
    if (st.bytes_sent) parts.push(fmt.bytes(st.bytes_sent) + ' sent');
  } else if (st.next_retry_s) {
    parts.push('next try in ' + fmt.dur(st.next_retry_s));
  }
  return h('div', { class: 'cstate' },
    h('span', { class: tone ? 't-' + tone : 'muted' }, lbl(L.caster, st.state) +
      (st.state === 'streaming' && st.connected_s ? ' for ' + fmt.dur(st.connected_s) : '')),
    parts.map(t => [' ', h('span', { class: 'nowrap muted' }, '· ' + t)]));
}

function casterSummary(c, st, preset) {
  const ono = isOnocoySlot(c);
  const avg = c.backoff_profile === 'polite' && st.state === 'streaming' ? slotRate(c.slot, 60) : null;
  if (!c.tls) delete PLAIN_SWITCH[c.slot];
  /* TLS to Onocoy keeps failing: one box says it, with the count and the switch to the plain port. */
  const plain = ono && c.tls && PLAIN_SWITCH[c.slot] !== 'done' && st.failures_by_class && st.failures_by_class.tls >= 10 &&
    st.state !== 'streaming';
  const out = [
    casterLine(c, st),
    /* Where it streams: host, port and mountpoint; a test slot says so here. */
    h('div', { class: 'muted small' }, c.host + ':' + c.port + (c.mountpoint ? '/' + c.mountpoint : '') +
      (c.test_onocoy ? ' · Test Onocoy slot (bench build)' : '')),
    st.state === 'stopped' ? stopPanel(c, st) : null,
    casterDetail(st, { unreached: true, tls: plain }),
    /* Public servers ban fast streams: a warning only when this one gets close. */
    avg !== null && avg > 28000 ? h('div', { class: 'small t-warn' }, 'Average over 60s: ' + fmt.kbps(avg) +
      '. That is close to the limits of public servers; consider MSM4 messages instead of MSM7.') : null,
    st.unreached_s ? h('div', { class: 'hintline' }, 'Server not reached for ' + fmt.dur(st.unreached_s) +
      (st.reason && st.reason !== 'none' ? ': ' + lbl(L.fail, st.reason) : '') + '.') : null,
    casterEvidence(st, plain),
  ];
  if (plain) {
    const waiting = PLAIN_SWITCH[c.slot] === 'busy';
    const sw = btn('Use plain port ' + preset.port, async () => {
      if (PLAIN_SWITCH[c.slot]) return;
      PLAIN_SWITCH[c.slot] = 'busy';
      sw.classList.add('busy');
      sw.setAttribute('aria-disabled', 'true');
      const body = { tls: false, port: preset.port };
      let ok = false;
      try {
        await saveCaster(c.slot, body);
        ok = true;
      } catch (e) {
        if (e instanceof ApiError && e.code === 'caster_policy' && e.body.reason === 'dup_stream_ack_required') {
          /* This slot needs the acknowledgement itself (it may have been given
           * on the other slot only): ask for it here. A dialog, since this
           * card is rebuilt every second. */
          ok = await dialog({ title: 'Duplicate-stream risk', ok: 'Save anyway', body: dupText(),
            check: 'I understand the risk and want to stream to both.',
            onOk: () => saveCaster(c.slot, Object.assign({ dup_stream_ack: true }, body)) });
        } else {
          showErr(e);
        }
      }
      if (ok) PLAIN_SWITCH[c.slot] = 'done';
      else delete PLAIN_SWITCH[c.slot];
      sw.classList.remove('busy');
      sw.removeAttribute('aria-disabled');
      if (ok) toast('Switched to the plain port.', 'ok');
    }, 'small' + (waiting ? ' busy' : ''), { 'aria-disabled': waiting ? 'true' : null });
    out.push(msg('warn', h('p', null, 'TLS to Onocoy failed ' + st.failures_by_class.tls + ' times. Onocoy documents uploads on the plain port only.'),
      h('div', { class: 'form-actions' }, sw)));
  }
  return out;
}

/**
 * An on/off switch (role switch, named by label) that saves at once:
 * onChange(on) resolves true once saved, else it flips back. While it saves
 * it says so (aria-busy) and ignores clicks, keeping the keyboard focus.
 */
function onOffSwitch(on, label, onChange) {
  const b = h('button', { type: 'button', class: 'switch', role: 'switch', 'aria-checked': on ? 'true' : 'false', 'aria-label': label });
  b.addEventListener('click', async () => {
    if (b.getAttribute('aria-busy') === 'true') return;
    const next = b.getAttribute('aria-checked') !== 'true';
    b.setAttribute('aria-checked', String(next));
    b.setAttribute('aria-busy', 'true');
    const ok = await onChange(next);
    b.removeAttribute('aria-busy');
    if (!ok) b.setAttribute('aria-checked', String(!next));
  });
  return b;
}

/**
 * Turn a server off or on (its card's switch): PUT with `enabled` only (the
 * rest stays as stored). On next to Onocoy, an open server needs the
 * duplicate-stream acknowledgement, as a save does: a dialog asks for it.
 * slot: its number (the entry's, not in its config); c: its config. The slot's
 * entry as the station stored it, or null when nothing changed.
 */
async function turnCaster(slot, c, on) {
  const put = body => saveCaster(slot, body);
  let saved = null;
  try {
    saved = await put({ enabled: on });
  } catch (e) {
    if (!(on && e instanceof ApiError && e.code === 'caster_policy' && e.body.reason === 'dup_stream_ack_required')) {
      showErr(e);
      return null;
    }
    const ok = await dialog({ title: 'Duplicate-stream risk', ok: 'Turn on anyway', body: dupText(),
      check: 'I understand the risk and want to stream to both.',
      onOk: async () => (saved = await put({ enabled: true, dup_stream_ack: true })) });
    if (!ok) return null;
  }
  toast((c.name || 'The server') + (on ? ' is turned on.' : ' is turned off.'), 'ok');
  return saved;
}

/* ---- Your rover ----------------------------------------------------------- */

/* Its fields as the station takes them, with the station's words (local_caster.h). */
const ROVER_RULE = {
  mountpoint: [/^[A-Za-z0-9._-]{1,47}$/, '1 to 47 letters, digits, dots, hyphens or underscores.'],
  username: [/^[A-Za-z0-9._-]{1,31}$/, '1 to 31 letters, digits, dots, hyphens or underscores.'],
  password: [/^[!-~]{8,63}$/, '8 to 63 characters, letters, digits and symbols, no spaces.'],
};

/** Where a rover finds the station: its name on the home network (<mdns_host>.local), its IP address. */
function roverHosts(net) {
  net = net || {};
  return [net.mdns_host ? net.mdns_host + '.local' : null, net.ip || null].filter(Boolean);
}

/* A value to type into the rover's NTRIP client: a tap selects all of it, for a copy. */
const copyable = v => h('code', { class: 'copy' }, breakable(String(v)));

/*
 * Your rover's card under its head (d: GET /api/local-caster, s: the
 * status). Off: what it does, in one line. On, always in full: its state,
 * a warning while the receiver sends no corrections, what to type into the
 * rover, one line per rover connected, the limits, and what was refused
 * since the start. Patched in place (nothing in it has a listener), so a
 * value selected for a copy stays selected.
 */
function roverSummary(d, s) {
  const c = d.config, st = d.status || {}, port = st.port || 2101;
  const hosts = roverHosts(s && s.network);
  if (!c.enabled) {
    return [h('p', { class: 'muted' }, 'Stream location data locally so a rover or device can use it on your network without internet.')];
  }
  const list = st.clients || [], n = list.length, rf = st.refused || {}, wrong = (rf.login || 0) + (rf.throttled || 0);
  const line = st.state === 'error' ? ['bad', 'Not listening on port ' + port + ' (error ' + st.listen_errno + '). ' +
    'Trying again every 10s.'] :
    st.state === 'wait_network' ? ['', 'Waiting for Wi-Fi. Rovers connect over your home network, not the setup hotspot.'] :
    n ? ['ok', plural(n, 'rover', 'rovers') + ' connected'] : ['', lbl(L.rover, st.state === 'off' ? 'starting' : st.state)];
  /* A throttled login is a wrong password too (the rover's own retries). */
  const refused = [wrong && plural(wrong, 'wrong password', 'wrong passwords'), rf.not_local && rf.not_local + ' from outside your network',
    rf.full && rf.full + ' while full', rf.bad_request && plural(rf.bad_request, 'bad request', 'bad requests')].filter(Boolean);
  const rx = s && s.receiver;
  const oneRover = r => h('div', null, breakable(r.addr || DASH), ['NTRIP ' + (r.ntrip_version || 1) + '.0',
    'for ' + fmt.dur(r.connected_s), fmt.bytes(r.bytes_sent) + ' sent',
    r.epochs_dropped && plural(r.epochs_dropped, 'epoch', 'epochs') + ' dropped (slow link)']
    .filter(Boolean).map(t => [' ', h('span', { class: 'nowrap muted' }, '· ' + t)]));
  return [
    h('div', { class: 'cstate' }, h('span', { class: line[0] ? 't-' + line[0] : 'muted' }, line[1])),
    rx && !(typeof rx.rtcm_age_ms === 'number' && rx.rtcm_age_ms <= 3000) ?
      msg('warn', 'No corrections from the receiver right now: connected rovers get nothing.') : null,
    kv([['Host', hosts.length ? hosts.map((x, i) => [i ? ' or ' : '', copyable(x)]) : DASH], ['Port', copyable(port)],
      ['Mountpoint', copyable(c.mountpoint)], ['Username', copyable(c.username)], ['Password', c.password_set ? 'Set' : 'Not set'],
      ['NTRIP version', '1.0 or 2.0']]),
    list.map(oneRover),
    h('p', { class: 'muted small' }, 'At most ' + (st.max_clients || 2) + ' rovers at once. ' +
      'Only devices on your home network can connect.'),
    refused.length ? h('p', { class: 'muted small' }, 'Refused since the station started: ' + refused.join(', ') + '.') : null,
  ];
}

/**
 * Your rover's editor, in its card as a server's is: mountpoint, username
 * and password, each saying what it is and its default; the password is
 * write-only (empty keeps the stored one). opts: turnOn (opened by the
 * switch with no password stored: it says so, and Save turns it on),
 * hostname (the default mountpoint), onSaved(GET body), onCancel().
 * Returns {el, dirty(), focusPassword()}.
 */
function roverEditor(cfg, opts) {
  let dirty = false;
  const mp = textInput(cfg.mountpoint, { maxlength: 47 });
  const user = textInput(cfg.username, { maxlength: 31 });
  const pw = pwInput({ maxlength: 63, placeholder: cfg.password_set ? 'Stored, leave empty to keep' : '' });
  const F = {
    mountpoint: field('Mountpoint', mp, 'The name your rover asks for. Default: the station\'s name' +
      (opts.hostname ? ' (' + opts.hostname + ')' : '') + '. Letters, digits, dots, hyphens or underscores.'),
    username: field('Username', user, 'Default: rover.'),
    password: field('Password', pw, '8 to 63 characters, no spaces. No default: you choose it.'),
  };
  const err = errBox();
  const saveBtn = h('button', { type: 'submit', class: 'btn primary' }, opts.turnOn ? 'Save and turn on' : 'Save');
  const form = h('form', { class: 'stack', onsubmit: e => {
    e.preventDefault();
    save();
  }, oninput: () => {
    dirty = true;
  } }, h('div', { class: 'card-head' }, h('h2', null, 'Local server')), opts.turnOn ? h('p', null, 'Choose a password first.') : null,
  h('div', { class: 'cols c3' }, F.mountpoint.el, F.username.el, F.password.el), err,
  h('div', { class: 'form-actions' }, saveBtn, btn('Cancel', () => opts.onCancel(), 'ghost')));

  async function save() {
    const body = { mountpoint: mp.value.trim(), username: user.value.trim() };
    if (pw.value) body.password = pw.value;
    if (opts.turnOn) body.enabled = true;
    let bad = null;
    for (const k of Object.keys(F)) {
      const v = body[k];
      const why = v ? (ROVER_RULE[k][0].test(v) ? null : ROVER_RULE[k][1]) :
        k !== 'password' ? ROVER_RULE[k][1] : opts.turnOn && !cfg.password_set ? 'Choose a password.' : null;
      F[k].error(why);
      if (why && !bad) bad = F[k].input;
    }
    if (bad) {
      bad.focus();
      return;
    }
    err.hidden = true;
    try {
      const d = await busy(saveBtn, () => api('PUT', '/api/local-caster', body));
      dirty = false;
      pw.value = '';
      toast(opts.turnOn ? 'Saved. The local server is on.' : 'Saved.', 'ok');
      opts.onSaved(d);
    } catch (e) {
      if (e instanceof ApiError && e.code === 'invalid_field' && F[e.field]) {
        F[e.field].error(ruleText(e));
        F[e.field].input.focus();
        return;
      }
      showErr(e, err);
    }
  }
  return { el: form, dirty: () => dirty, focusPassword: () => pw.focus({ preventScroll: true }) };
}

Views.ntrip = {
  title: 'NTRIP',
  showTitle: true,
  admin: true,
  create(el, arg) {
    let data = null;
    let editing = null, editor = null;
    const cards = {};   /* slot -> card element (servers only; a free slot's editor opens in the add card) */
    const refs = {};    /* slot -> {badge, live} for in-place updates */
    let addCard = null;
    let stopPoll = null, gone = false;
    /* Your rover: GET /api/local-caster (null: none to show, safe mode), its card, its live part, its editor. */
    let rover = null, roverCard = null, roverLive = null, roverEd = null;

    const liveOf = slot => {
      const l = S.status && S.status.casters ? S.status.casters.find(c => c.slot === slot) : null;
      return l || {};
    };
    const statusOf = s => Object.assign({}, s.status || {}, liveOf(s.slot));
    /* A free slot: no server in it (no host, not enabled). Safe mode loads no config: every slot keeps its card. */
    const isFree = s => !!s.config && !s.config.host && !s.config.enabled;
    const freeSlots = () => data.slots.filter(isFree);
    const safeMode = () => data.slots.length > 0 && data.slots.every(s => s.config === null);

    function cardBody(s) {
      const c = s.config;
      delete refs[s.slot];
      if (editing === s.slot && editor) return [editor.el];
      const st = statusOf(s);
      if (!c) {
        return [cardHead('Slot ' + (s.slot + 1), badge(lbl(L.caster, st.state), casterTone(st.state))),
          h('p', { class: 'muted' }, 'Not loaded (safe mode).')];
      }
      if (isOnocoySlot(c) && !onocoyReady(c)) {
        return [cardHead(c.name || 'Onocoy', badge('Not set up', '')),
          h('p', null, 'Add the username and password from the Onocoy console to start streaming.'),
          st.state === 'streaming' ? msg('info', 'It streams with credentials stored by an earlier version. Enter the username ' +
            'and password from the Onocoy console to finish the setup.') : null,
          /* The main button, unless another server's editor (and its Save) is open. */
          h('div', { class: 'form-actions' }, btn('Add your Onocoy credentials', () => edit(s), editing === null ? 'primary' : ''))];
      }
      const r = { live: h('div', { class: 'caster-live fit-kv' }) };
      refs[s.slot] = r;
      fill(s);
      const name = c.name || 'Slot ' + (s.slot + 1);
      const sw = onOffSwitch(!!c.enabled, 'Stream to ' + name, async on => {
        const saved = await turnCaster(s.slot, c, on);
        if (!saved) return false;
        if (gone) return true;
        data.slots = data.slots.map(x => (x.slot === saved.slot ? saved : x));
        renderSlot(s.slot);
        /* The keyboard focus stays on the switch, rebuilt with its card. */
        const next = cards[s.slot] && cards[s.slot].querySelector('.switch');
        if (next) next.focus();
        load().catch(() => {});
        return true;
      });
      return [
        h('div', { class: 'card-head switch-head' }, h('h2', null, name),
          btn('Edit', () => edit(s), 'small', { 'aria-label': 'Edit ' + name }), sw),
        r.live,
        /* Onocoy and this open server both stream: the risk, on this server's own card. */
        openCasterWarning(data, s.slot),
      ];
    }

    /* The card after the servers, laid out as a server's: its title and how many slots are free, one line, and "Add a
     * server" (its editor opens in place). The main button only while there is no server yet: otherwise a server's own
     * action (or an editor's Save) is the one. */
    function addBody() {
      const free = freeSlots();
      if (editor && free.some(x => x.slot === editing)) return [editor.el];
      /* Not set up while no server besides Onocoy is; then how many slots are free of those Onocoy does not use. */
      const ono = data.slots.filter(x => !isFree(x) && x.config && isOnocoySlot(x.config)).length;
      const others = data.slots.filter(x => !isFree(x) && !(x.config && isOnocoySlot(x.config))).length;
      const head = cardHead('More servers', badge(!others ? 'Not set up' : free.length + ' of ' + (data.max_slots - ono) + ' slots free', ''));
      if (!free.length) return [head, h('p', null, 'Every slot is in use. Edit a server to replace it.')];
      return [head, h('p', null, 'Stream to another NTRIP server as well.'),
        h('div', { class: 'form-actions' }, btn('Add a server', () => edit(free[0]), free.length === data.slots.length ? 'primary' : ''))];
    }

    /* The add card: the whole row. */
    function fillAdd() {
      mount(addCard, addBody());
    }

    /* Refresh the live parts of a card without touching its switch and buttons. */
    function fill(s) {
      const r = refs[s.slot];
      if (!r) return;
      const st = statusOf(s);
      /* Rebuilt every second: a focused button ("Try again") hands the focus to its successor. */
      const a = doc.activeElement, had = r.live.contains(a) && a.textContent;
      mount(r.live, casterSummary(Object.assign({ slot: s.slot }, s.config), st, data.onocoy_preset || {}));
      const next = had && Array.from(r.live.querySelectorAll('button')).find(b => b.textContent === had);
      if (next) next.focus();
    }

    function edit(s) {
      if (editor && editor.dirty() && !window.confirm('Discard the changes to the other server?')) return;
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
          render();
          load().catch(() => {});
        },
        onCancel: () => {
          editing = null;
          editor = null;
          renderSlot(s.slot);
          setupCards();
        },
      });
      if (prev !== null && prev !== s.slot) renderSlot(prev);
      renderSlot(s.slot);
      setupCards();
      const target = cards[s.slot] || addCard;
      if (target) target.scrollIntoView({ block: 'start', behavior: 'smooth' });
    }

    /* Your rover's card: its name, Edit and the switch (on with no password stored opens the editor first), then its
     * live part; or its editor. */
    function roverBody() {
      if (roverEd) return [roverEd.el];
      roverLive = h('div', { class: 'caster-live fit-kv' });
      fillRover();
      return [h('div', { class: 'card-head switch-head' }, h('h2', null, 'Local server'),
        btn('Edit', () => editRover(false), 'small', { 'aria-label': 'Edit your rover' }),
        onOffSwitch(!!rover.config.enabled, 'Local server', turnRover)), roverLive];
    }

    function fillRover() {
      if (roverLive && rover && !roverEd) patch(roverLive, roverSummary(rover, S.status));
    }

    function renderRover() {
      if (roverCard && rover) mount(roverCard, roverBody());
      else render();
    }

    function editRover(turnOn) {
      const done = d => {
        roverEd = null;
        if (d && d.config) rover = d;
        renderRover();
      };
      roverEd = roverEditor(rover.config, { turnOn: turnOn, hostname: S.status && S.status.device ? S.status.device.hostname : '',
        onSaved: done, onCancel: () => done(null) });
      renderRover();
      if (roverCard) roverCard.scrollIntoView({ block: 'start', behavior: 'smooth' });
      /* Opened by the switch: the password is what it waits for. */
      if (turnOn && roverEd) roverEd.focusPassword();
    }

    async function turnRover(on) {
      if (on && !rover.config.password_set) {
        editRover(true);
        return false;
      }
      try {
        const d = await api('PUT', '/api/local-caster', { enabled: on });
        if (d && d.config) rover = d;
      } catch (e) {
        if (on && e instanceof ApiError && e.code === 'invalid_field' && e.field === 'password') {
          editRover(true);
          return false;
        }
        showErr(e);
        /* The station may have stored it all the same (500 internal: stored, but the caster could not start):
         * the card shows what it has now, the switch included. */
        try {
          const d = await api('GET', '/api/local-caster');
          if (d && d.config) {
            rover = d;
            renderRover();
          }
        } catch (e2) { /* the card stays as it was */ }
        return !!rover && rover.config.enabled === on;
      }
      toast(on ? 'The local server is on.' : 'The local server is off.', 'ok');
      fillRover();
      return true;
    }

    /* Redraw the cards of Onocoy slots not set up (their button is the main one only while no editor is open). */
    function setupCards() {
      for (const x of data.slots) {
        if (x.slot !== editing && x.config && isOnocoySlot(x.config) && !onocoyReady(x.config) && cards[x.slot]) renderSlot(x.slot);
      }
    }

    /* Redraw the card that shows slot n; a slot that became free or taken changes the list of cards. */
    function renderSlot(n) {
      const s = data.slots.find(x => x.slot === n);
      if (!s) return;
      if (isFree(s) ? cards[n] : !cards[n]) {
        render();
        return;
      }
      if (isFree(s)) {
        if (addCard) fillAdd();
      } else {
        mount(cards[n], cardBody(s));
      }
      sizeCards();
    }

    /* Two server cards side by side on a wide screen; the whole row for an open editor, and for a card that would
     * otherwise sit alone on its row (one server, or the last of an odd count). */
    function sizeCards() {
      const rest = data.slots.filter(x => !isFree(x) && x.slot !== editing && cards[x.slot]);
      const alone = rest.length % 2 ? rest[rest.length - 1].slot : null;
      for (const k of Object.keys(cards)) cards[k].classList.toggle('wide', Number(k) === editing || Number(k) === alone);
    }

    /* The page's heading (the view's title) says "NTRIP"; then one card per server (sizeCards), the add card and the
     * local server's, the whole row. Safe mode: the page-wide
     * notice says it, each card says "Not loaded", and there is no add card and no rover card. */
    function render() {
      for (const k of Object.keys(cards)) delete cards[k];
      addCard = safeMode() ? null : h('section', { class: 'card wide' });
      if (addCard) fillAdd();
      roverCard = rover ? h('section', { class: 'card wide' }, roverBody()) : null;
      mount(el, h('div', { class: 'grid' }, data.slots.filter(s => !isFree(s)).map(s =>
        (cards[s.slot] = h('section', { class: 'card' }, cardBody(s)))), addCard, roverCard));
      sizeCards();
    }

    async function load() {
      /* Your rover's part fails alone: a station without it (404) has no card; another failure keeps what it showed. */
      const [d, r] = await Promise.all([api('GET', '/api/casters'),
        api('GET', '/api/local-caster').catch(e => (e instanceof ApiError && e.status === 404 ? null : rover))]);
      const old = data, oldRover = rover;
      data = d;
      rover = r && r.config ? r : null;
      if (!rover) roverEd = null;
      if (!old || old.slots.length !== d.slots.length || !oldRover !== !rover) {
        render();
        return;
      }
      if (rover && !roverEd) {
        if (JSON.stringify(oldRover.config) === JSON.stringify(rover.config)) fillRover();
        else renderRover();
      }
      /* The cards whose duplicate-stream warning comes or goes are rebuilt. */
      const warned = JSON.stringify(old.warnings || []) === JSON.stringify(d.warnings || []) ? [] :
        openCasterSlots(old).concat(openCasterSlots(d));
      for (const s of d.slots) {
        if (s.slot === editing) continue;
        const before = old.slots.find(x => x.slot === s.slot);
        if (refs[s.slot] && before && JSON.stringify(before.config) === JSON.stringify(s.config) && warned.indexOf(s.slot) < 0) fill(s);
        else renderSlot(s.slot);
      }
    }

    mount(el, h('p', { class: 'loading' }, 'Loading servers…'));
    load().then(() => {
      if (gone) return;
      stopPoll = poller(5000, () => (editing === null ? load() : Promise.resolve()), true);
      if (arg === 'onocoy') {
        /* From Status or the setup ("Set up", "Turn on"): Onocoy set up and off shows its card, the focus on its switch;
         * else its editor (or a free slot's, in the add card). */
        history.replaceState(null, '', '#/ntrip');
        if (shell.current) shell.current.arg = '';
        const target = data.slots.find(x => x.config && isOnocoySlot(x.config)) || freeSlots()[0];
        const off = target && target.config.enabled === false && onocoyReady(target.config) && cards[target.slot];
        if (off) {
          off.scrollIntoView({ block: 'start', behavior: 'smooth' });
          const b = off.querySelector('.switch');
          if (b) b.focus();
        } else if (target) {
          edit(target);
        }
      }
    }, e => mount(el, msg('bad', errText(e)), btn('Try again', () => openView('ntrip', ''), 'primary')));

    return {
      update() {
        if (!data) return;
        for (const s of data.slots) if (s.slot !== editing) fill(s);
        fillRover();
      },
      destroy() {
        gone = true;
        if (stopPoll) stopPoll();
      },
      dirty: () => !!(editor && editor.dirty()) || !!(roverEd && roverEd.dirty()),
      reload() {
        if (editing === null) load().catch(() => {});
      },
    };
  },
};
