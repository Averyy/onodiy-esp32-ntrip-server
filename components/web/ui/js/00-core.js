/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Core helpers: DOM building, formatting, the JSON client, browser storage
 * and a small event bus.
 *
 * The bundle (tools/ui_bundle.py) joins js/*.js in name order inside one
 * strict-mode function, so these names are shared by the later files and
 * nothing reaches the global object. The page is built from DOM nodes only:
 * text from the station (caster replies, SSIDs, log lines) is always set as
 * text, never parsed as HTML.
 */

const doc = document;
const API_VERSION = 1;

/* ---- DOM --------------------------------------------------------------- */

/* Marks elements that h() gave a listener (see patch()). */
const HAS_ON = '_hOn';

/**
 * Create an element. props: class, text, on<event> (listener), style
 * (object; set through CSSOM, which the Content-Security-Policy allows),
 * value/checked/disabled/hidden/selected (properties), anything else as an
 * attribute (true = empty attribute, null/false = left out).
 */
function h(tag, props, ...kids) {
  const el = doc.createElement(tag);
  if (props) {
    for (const k of Object.keys(props)) {
      const v = props[k];
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'text') el.textContent = String(v);
      else if (k.startsWith('on') && typeof v === 'function') {
        el.addEventListener(k.slice(2), v);
        el[HAS_ON] = true;
      }
      else if (k === 'style') Object.assign(el.style, v);
      else if (k === 'value') el.value = v;
      else if (k === 'checked' || k === 'disabled' || k === 'hidden' || k === 'selected' || k === 'required') el[k] = !!v;
      else el.setAttribute(k, v === true ? '' : String(v));
    }
  }
  add(el, kids);
  return el;
}

function add(el, kids) {
  for (const k of kids) {
    if (k === null || k === undefined || k === false) continue;
    if (Array.isArray(k)) add(el, k);
    else el.append(k instanceof Node ? k : String(k));
  }
  return el;
}

function clear(el) {
  el.textContent = '';
  return el;
}

function mount(el, ...kids) {
  clear(el);
  return add(el, kids);
}

/**
 * Make el's children match `kids` (built fresh with h()) while keeping the
 * nodes already there wherever the same tag sits at the same place: only
 * text and attributes that changed are written (inline style property by
 * property, see patchStyle). Keyboard focus, a text
 * selection and a screen reader's place survive a refresh this way, which
 * mount() (new nodes every time) destroys. Elements that carry a listener
 * are always taken fresh, so no handler with stale data survives. For views
 * without form controls (their live values are properties, not attributes).
 */
function patch(el, ...kids) {
  patchKids(el, add(doc.createDocumentFragment(), kids));
  return el;
}

/*
 * Inline style through CSSOM only: the Content-Security-Policy (style-src
 * 'self', no 'unsafe-inline') blocks a style attribute written with
 * setAttribute, but allows property writes, which is how h() sets it.
 */
function patchStyle(a, b) {
  const as = a.style, bs = b.style;
  if (!as || !bs) return;
  for (let i = as.length - 1; i >= 0; i--) {
    const p = as[i];
    if (!bs.getPropertyValue(p)) as.removeProperty(p);
  }
  for (let i = 0; i < bs.length; i++) {
    const p = bs[i];
    const v = bs.getPropertyValue(p), pr = bs.getPropertyPriority(p);
    if (as.getPropertyValue(p) !== v || as.getPropertyPriority(p) !== pr) as.setProperty(p, v, pr);
  }
}

function patchKids(cur, next) {
  let a = cur.firstChild;
  let b = next.firstChild;
  while (b) {
    const nb = b.nextSibling;
    if (a && a.nodeType === b.nodeType && a.nodeName === b.nodeName &&
        (a.nodeType !== 1 || (!a[HAS_ON] && !b[HAS_ON] && a.namespaceURI === b.namespaceURI))) {
      if (a.nodeType === 1) {
        for (const at of Array.from(a.attributes)) {
          if (at.name !== 'style' && !b.hasAttribute(at.name)) a.removeAttribute(at.name);
        }
        for (const at of Array.from(b.attributes)) {
          if (at.name !== 'style' && a.getAttribute(at.name) !== at.value) a.setAttribute(at.name, at.value);
        }
        patchStyle(a, b);
        patchKids(a, b);
      } else if (a.nodeValue !== b.nodeValue) {
        a.nodeValue = b.nodeValue;
      }
      a = a.nextSibling;
    } else if (a) {
      const na = a.nextSibling;
      cur.replaceChild(b, a);
      a = na;
    } else {
      cur.append(b);
    }
    b = nb;
  }
  while (a) {
    const na = a.nextSibling;
    a.remove();
    a = na;
  }
}

/**
 * True while the user works inside el: keyboard focus on an element in it
 * (focus a mouse click leaves on a button does not count), or a text
 * selection in it. Periodic refreshes that rebuild nodes wait meanwhile.
 */
function interacting(el) {
  const a = doc.activeElement;
  if (a && a !== doc.body && el.contains(a)) {
    let keyboard = true;
    try {
      keyboard = a.matches(':focus-visible');
    } catch (e) {
      /* no :focus-visible: count any focus */
    }
    if (keyboard || a.matches('input, select, textarea')) return true;
  }
  const sel = window.getSelection ? window.getSelection() : null;
  return !!(sel && !sel.isCollapsed && sel.rangeCount && el.contains(sel.getRangeAt(0).commonAncestorContainer));
}

function svg(tag, attrs, ...kids) {
  const el = doc.createElementNS('http://www.w3.org/2000/svg', tag);
  if (attrs) {
    for (const k of Object.keys(attrs)) {
      if (attrs[k] !== null && attrs[k] !== undefined) el.setAttribute(k, String(attrs[k]));
    }
  }
  return add(el, kids);
}

function card(title, ...kids) {
  return h('section', { class: 'card' }, title ? h('h2', null, title) : null, kids);
}

/** Definition list from [label, value] rows; rows that are null are skipped. */
function kv(rows) {
  const dl = h('dl', { class: 'kv' });
  for (const r of rows) {
    if (!r) continue;
    dl.append(h('dt', null, r[0]), h('dd', null, r[1] === null || r[1] === undefined || r[1] === '' ? '–' : r[1]));
  }
  return dl;
}

function badge(text, tone) {
  return h('span', { class: 'badge ' + (tone || '') }, text);
}

function msg(tone, ...kids) {
  return h('div', { class: 'msg ' + (tone || ''), role: tone === 'bad' ? 'alert' : null }, kids);
}

function btn(label, onclick, kind, attrs) {
  return h('button', Object.assign({ type: 'button', class: 'btn ' + (kind || ''), onclick: onclick }, attrs || {}), label);
}

function link(label, href, cls) {
  return h('a', { href: href, class: cls || null }, label);
}

/** Labelled input with hint and error line. Returns {el, input, error(msg)}. */
function field(label, input, hint) {
  const err = h('small', { class: 'err', hidden: true });
  const el = h('div', { class: 'field' },
    h('label', null, h('span', { class: 'lbl' }, label), input),
    hint ? h('small', { class: 'hint' }, hint) : null,
    err);
  return {
    el: el,
    input: input,
    error(text) {
      err.textContent = text || '';
      err.hidden = !text;
      input.classList.toggle('invalid', !!text);
      if (text) input.setAttribute('aria-invalid', 'true');
      else input.removeAttribute('aria-invalid');
    },
  };
}

function textInput(value, attrs) {
  return h('input', Object.assign({ type: 'text', value: value === null || value === undefined ? '' : value,
    autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false' }, attrs || {}));
}

function numInput(value, attrs) {
  return h('input', Object.assign({ type: 'text', inputmode: 'decimal', value: value === null || value === undefined ? '' : String(value),
    autocomplete: 'off' }, attrs || {}));
}

function pwInput(attrs) {
  return h('input', Object.assign({ type: 'password', autocomplete: 'new-password', autocapitalize: 'off', spellcheck: 'false' }, attrs || {}));
}

function selectInput(options, value, attrs) {
  const s = h('select', attrs || null);
  for (const o of options) {
    s.append(h('option', { value: String(o[0]), selected: String(o[0]) === String(value) }, o[1]));
  }
  return s;
}

/** Checkbox row. Returns {el, input}. */
function checkbox(label, checked, hint, attrs) {
  const input = h('input', Object.assign({ type: 'checkbox', checked: !!checked }, attrs || {}));
  const el = h('label', { class: 'check' }, input,
    h('span', null, h('span', null, label), hint ? h('small', { class: 'hint' }, hint) : null));
  return { el: el, input: input };
}

/** Radio group. Returns {el, value(), set(v), onchange(fn)}. */
function radios(legend, name, options, value) {
  const inputs = [];
  const box = h('div', { class: 'choices' });
  for (const o of options) {
    const r = h('input', { type: 'radio', name: name, value: String(o[0]), checked: String(o[0]) === String(value) });
    inputs.push(r);
    box.append(h('label', { class: 'choice' }, r, o[1]));
  }
  const el = h('fieldset', { class: 'fieldset' }, h('legend', null, legend), box);
  return {
    el: el,
    value() {
      for (const r of inputs) if (r.checked) return r.value;
      return null;
    },
    set(v) {
      for (const r of inputs) r.checked = r.value === String(v);
    },
    onchange(fn) {
      for (const r of inputs) r.addEventListener('change', fn);
    },
  };
}

/** A bar for a fraction; label names it for assistive technology. */
function bar(fraction, tone, label) {
  const f = Math.max(0, Math.min(1, fraction || 0));
  return h('div', { class: 'bar ' + (tone || ''), role: 'progressbar', 'aria-label': label || 'Progress', 'aria-valuemin': 0,
    'aria-valuemax': 100, 'aria-valuenow': Math.round(f * 100) }, h('span', { style: { width: (f * 100).toFixed(1) + '%' } }));
}

/**
 * Table from header texts and rows of cells. opts.num: indexes of numeric
 * columns (right-aligned); opts.stack: on a phone (under 480 px) each row
 * becomes a list of "header: value" lines instead of a table that scrolls
 * sideways (every column stays readable).
 */
function table(head, rows, opts) {
  const numeric = (opts && opts.num) || [];
  const stack = !!(opts && opts.stack);
  return h('div', { class: 'scroll' }, h('table', { class: stack ? 'stack-sm' : null },
    h('thead', null, h('tr', null, head.map((t, i) => h('th', { class: numeric.indexOf(i) >= 0 ? 'num' : null }, t)))),
    h('tbody', null, rows.map(r => h('tr', null, r.map((c, i) =>
      h('td', { class: numeric.indexOf(i) >= 0 ? 'num' : null, 'data-label': stack ? head[i] : null },
        c === null || c === undefined ? '–' : c)))))));
}

/* ---- Formatting -------------------------------------------------------- */

const DASH = '–';
/* Between a number and its unit: a line never breaks there ("7 min 49 s" wraps only between the pairs). */
const NB = '\u00a0';

const fmt = {
  /** Duration in seconds, e.g. "7 min 49 s", "3 h 12 min". */
  dur(s) {
    if (s === null || s === undefined || isNaN(s)) return DASH;
    s = Math.max(0, Math.round(s));
    const d = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60;
    if (d) return d + NB + 'd ' + hh + NB + 'h';
    if (hh) return hh + NB + 'h ' + m + NB + 'min';
    if (m) return m + NB + 'min' + (ss ? ' ' + ss + NB + 's' : '');
    return ss + NB + 's';
  },
  /** Age or latency in milliseconds. */
  ms(v) {
    if (v === null || v === undefined || isNaN(v)) return DASH;
    if (v < 1000) return Math.round(v) + NB + 'ms';
    if (v < 60000) return (v / 1000).toFixed(v < 10000 ? 1 : 0) + NB + 's';
    return fmt.dur(v / 1000);
  },
  bytes(b) {
    if (b === null || b === undefined || isNaN(b)) return DASH;
    if (b < 1024) return b + NB + 'B';
    if (b < 1048576) return (b / 1024).toFixed(b < 10240 ? 1 : 0) + NB + 'KB';
    if (b < 1073741824) return (b / 1048576).toFixed(1) + NB + 'MB';
    return (b / 1073741824).toFixed(2) + NB + 'GB';
  },
  num(n) {
    return n === null || n === undefined || isNaN(n) ? DASH : Number(n).toLocaleString();
  },
  permille(p) {
    return p === null || p === undefined ? DASH : (p / 10).toFixed(1) + NB + '%';
  },
  pct(p) {
    return p === null || p === undefined ? DASH : p + NB + '%';
  },
  time(unix) {
    if (!unix) return DASH;
    return new Date(unix * 1000).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
  },
  deg(v) {
    return v === null || v === undefined ? DASH : Number(v).toFixed(9) + '°';
  },
  m(v, digits) {
    return v === null || v === undefined ? DASH : Number(v).toFixed(digits === undefined ? 4 : digits) + NB + 'm';
  },
  kbps(bitsPerS) {
    return bitsPerS === null || bitsPerS === undefined ? DASH : (bitsPerS / 1000).toFixed(1) + NB + 'kbit/s';
  },
  yesno(b) {
    return b === null || b === undefined ? DASH : b ? 'Yes' : 'No';
  },
  /** Seconds until the next 00:00 UTC, and that moment in local time. */
  utcMidnight() {
    const now = new Date();
    const next = Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() + 1);
    return { in_s: Math.round((next - now.getTime()) / 1000),
      local: new Date(next).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' }) };
  },
};

/** Text of an enum value through a label table; unknown values verbatim. */
function lbl(table, v) {
  if (v === null || v === undefined) return DASH;
  const t = table[v];
  if (t === undefined) return String(v);
  return Array.isArray(t) ? t[0] : t;
}

/** Bytes of a string in UTF-8. */
function utf8Len(s) {
  return new TextEncoder().encode(s).length;
}

function hasControl(s) {
  return /[\u0000-\u001f\u007f]/.test(s);
}

/* ---- Browser storage (per-browser conveniences only) ------------------- */

const store = {
  get(k, dflt) {
    try {
      const v = localStorage.getItem('station.' + k);
      return v === null ? dflt : JSON.parse(v);
    } catch (e) {
      return dflt;
    }
  },
  set(k, v) {
    try {
      if (v === null || v === undefined) localStorage.removeItem('station.' + k);
      else localStorage.setItem('station.' + k, JSON.stringify(v));
    } catch (e) {
      /* private mode, captive-portal browser: the setting lasts for this page only */
    }
  },
};

/* ---- Event bus --------------------------------------------------------- */

const bus = {
  map: {},
  on(type, fn) {
    (this.map[type] = this.map[type] || []).push(fn);
    return () => this.off(type, fn);
  },
  off(type, fn) {
    const l = this.map[type];
    if (l) {
      const i = l.indexOf(fn);
      if (i >= 0) l.splice(i, 1);
    }
  },
  emit(type, ...args) {
    for (const fn of (this.map[type] || []).slice()) {
      try {
        fn(...args);
      } catch (e) {
        console.error(e);
      }
    }
  },
};

/* ---- JSON client ------------------------------------------------------- */

/*
 * This tab's id (random, per page load), sent with every request that
 * changes something: the station's settings_changed event names the client
 * whose request caused it, so the page that saved knows the change was its
 * own and does not warn about changes made elsewhere.
 */
const CLIENT_ID = (() => {
  const b = new Uint8Array(8);
  try {
    crypto.getRandomValues(b);
  } catch (e) {
    for (let i = 0; i < b.length; i++) b[i] = Math.floor(Math.random() * 256);
  }
  return Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
})();

class ApiError extends Error {
  constructor(status, body) {
    const e = body || {};
    super(e.message || (status ? 'HTTP ' + status : 'no answer'));
    this.status = status;
    this.code = e.code || (status ? 'http_' + status : 'network');
    this.field = e.field || null;
    this.retry_in_s = e.retry_in_s === undefined ? null : e.retry_in_s;
    this.by = e.by || null;
    this.body = e;
  }
}

/**
 * Call the API. POST and PUT always send Content-Type: application/json
 * (the server's CSRF rule, also for an empty body). Resolves with the parsed
 * body; rejects with ApiError. A 401 also emits "unauthorized" unless
 * opts.quiet401.
 */
async function api(method, path, body, opts) {
  opts = opts || {};
  const init = { method: method, credentials: 'same-origin', cache: 'no-store', headers: { Accept: 'application/json' } };
  if (method === 'POST' || method === 'PUT') {
    init.headers['Content-Type'] = 'application/json';
    init.body = body === undefined ? '' : JSON.stringify(body);
  }
  if (method !== 'GET') init.headers['X-Client-Id'] = CLIENT_ID;
  let timer = 0;
  if (typeof AbortController === 'function') {
    const ctl = new AbortController();
    init.signal = ctl.signal;
    timer = setTimeout(() => ctl.abort(), opts.timeout || 15000);
  }
  let res;
  try {
    res = await fetch(path, init);
  } catch (e) {
    throw new ApiError(0, { code: e && e.name === 'AbortError' ? 'timeout' : 'network' });
  } finally {
    clearTimeout(timer);
  }
  let data = null;
  if ((res.headers.get('Content-Type') || '').indexOf('json') >= 0) {
    try {
      data = await res.json();
    } catch (e) {
      data = null;
    }
  }
  if (!res.ok) {
    const err = new ApiError(res.status, data && data.error);
    if (res.status === 401 && !opts.quiet401) bus.emit('unauthorized');
    throw err;
  }
  return data;
}

/**
 * Raw upload (application/octet-stream plus the X-Requested-With header the
 * server requires). onProgress(sent, total) while the body goes out;
 * headers: more request headers ({'X-Reinstall': '1'}).
 */
function upload(path, file, onProgress, headers) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', path);
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.setRequestHeader('X-Requested-With', 'onocoydiy');
    x.setRequestHeader('X-Client-Id', CLIENT_ID);
    for (const k of Object.keys(headers || {})) x.setRequestHeader(k, headers[k]);
    x.upload.onprogress = e => {
      if (onProgress && e.lengthComputable) onProgress(e.loaded, e.total);
    };
    x.onload = () => {
      let data = null;
      try {
        data = JSON.parse(x.responseText);
      } catch (e) {
        data = null;
      }
      if (x.status >= 200 && x.status < 300) {
        resolve(data);
        return;
      }
      if (x.status === 401) bus.emit('unauthorized');
      reject(new ApiError(x.status, data && data.error));
    };
    x.onerror = () => reject(new ApiError(0, { code: 'network' }));
    x.send(file);
  });
}

/* ---- Errors in the user's words ----------------------------------------- */

const ERR = {
  network: 'The station did not answer. Check that this device is still on the same network as the station.',
  timeout: 'The station did not answer in time.',
  bad_json: 'The station could not read the request.',
  invalid_field: 'A value is not accepted',
  confirm_required: 'This action needs a confirmation.',
  bad_request: 'The station refused the request.',
  invalid_image: 'This file is not a valid firmware image for this chip.',
  size_mismatch: 'The upload ended early. Try again.',
  unauthorized: 'Your session ended. Log in again.',
  forbidden: 'The station refused the request from this page.',
  wrong_password: 'Wrong password.',
  not_on_setup_ap: 'The first admin password can only be set over the station\'s own Wi-Fi network.',
  not_found: 'Not found.',
  nothing_staged: 'No uploaded receiver package is waiting.',
  no_journal: 'There is no receiver update session.',
  no_coredump: 'There is no core dump.',
  nothing_pending: 'No new Wi-Fi credentials are waiting (they were confirmed or discarded meanwhile).',
  method_not_allowed: 'The station does not support this action.',
  busy: 'The station is busy with another job. Try again in a moment.',
  journal_open: 'A receiver update session is open. Finish or clear it first.',
  not_waiting: 'The receiver update is still running.',
  app_trial: 'The new app version is still on trial.',
  app_update: 'An app update is running.',
  safe_mode: 'Not possible in safe mode.',
  not_in_safe_mode: 'The station is not in safe mode.',
  receiver_not_streaming: 'The receiver must be streaming first.',
  unsupported_model: 'Not possible with this receiver model.',
  package_pinned: 'A receiver package is held for an open update session.',
  manifest_changed: 'The offered version changed since this page loaded. Check the new offer and try again.',
  not_offered: 'That version is not offered (any more).',
  rolled_back_ack_required: 'This version was rolled back on this station before.',
  reboot_inhibited: 'A restart is blocked right now',
  settings_unavailable: 'The settings storage is unavailable this boot, so nothing can be saved.',
  no_position_yet: 'There is no surveyed position yet.',
  caster_policy: 'This caster setup is not allowed.',
  sha256_mismatch: 'The SHA-256 does not match the uploaded package.',
  downgrade: 'That build is older than the receiver\'s current firmware.',
  no_rollback_image: 'There is no previous app version to go back to.',
  admin_already_set: 'An admin password is already set.',
  admin_not_set: 'No admin password is set yet. Set one over the station\'s own Wi-Fi network.',
  receiver_maintenance: 'Receiver maintenance is on: nothing is sent to the receiver. End it first.',
  not_in_maintenance: 'Receiver maintenance is not on (it may have ended meanwhile).',
  live_view_closed: 'The live view is not open (it may have closed meanwhile).',
  length_required: 'The upload needs a known size.',
  too_large: 'Too large for the station.',
  unsupported_media_type: 'The station refused the request type.',
  bad_host: 'Open the station by its IP address or its hostname.',
  not_this_project: 'This image is not firmware for this station.',
  same_image: 'This is the version already running.',
  not_no_receiver: 'The station is not looking for a receiver right now (it found one, or is busy with it).',
  rate_limited: 'Too many attempts.',
  internal: 'The station hit an internal error',
  not_available_yet: 'Receiver firmware updates are not enabled in this build yet.',
  checks_disabled: 'This build cannot check for updates. Uploading a file still works.',
  too_many_clients: 'Too many pages are open on the station.',
};

/* Request fields as the forms label them, for "<label>: <rule>" refusals. */
const FIELD_LABEL = {
  name: 'Name', host: 'Host', port: 'Port', mountpoint: 'Mountpoint', username: 'Username', password: 'Password',
  label: 'Dashboard label', trust: 'Certificate check', backoff_profile: 'Retry pace', ntrip_version: 'NTRIP version',
  ssid: 'Network name', ap_password: 'Access point password', hostname: 'Station name', duration_s: 'Duration',
  minutes: 'Duration', pem: 'CA certificate', new: 'New password', current: 'Current password',
  expected_fw: 'Expected firmware', station_id: 'Station ID', com1_baud: 'Receiver link speed',
  survey_in_s: 'Survey-in time', lat_deg: 'Latitude', lon_deg: 'Longitude', height_m: 'Height',
  signal_group_master: 'Signal group (main antenna)', signal_group_slave: 'Signal group (second antenna)',
  antenna_model: 'Antenna model', antenna_serial: 'Antenna serial number', antenna_setup_id: 'Antenna setup ID',
  rtcm: 'RTCM messages', recovery: 'Receiver recovery', step: 'Setup step',
};

/** The generic message of a code (the server's own default, not a rule). */
const GENERIC_INVALID = 'A field has an invalid value';

/** "<Label>: <rule>" for a 400 invalid_field (the rule the station gave, or a plain fallback). */
function fieldErrorText(e) {
  const label = FIELD_LABEL[e.field] || String(e.field).replace(/_/g, ' ');
  const rule = e.body && e.body.message && e.body.message !== GENERIC_INVALID ? e.body.message : 'the station did not accept this value';
  return label.charAt(0).toUpperCase() + label.slice(1) + ': ' + rule;
}

const POLICY = {
  second_onocoy: 'Another enabled slot already streams to Onocoy. Onocoy allows one stream per station.',
  second_rtk2go: 'Another enabled slot already streams to RTK2go. Use one RTK2go slot per station.',
  dup_stream_ack_required: 'Streaming to Onocoy and an open caster at the same time needs your acknowledgement.',
};

function errText(e) {
  if (!(e instanceof ApiError)) return 'This page hit an error: ' + (e && e.message ? e.message : String(e));
  let t = ERR[e.code] || e.message || ('Error ' + e.status);
  if (e.code === 'caster_policy' && POLICY[e.body.reason]) t = POLICY[e.body.reason];
  if (e.code === 'busy' && e.body.reason === 'receiver_session') {
    t = 'The station is working with the receiver right now (configuring or checking it). Offer maintenance again when that ends.';
  }
  if (e.code === 'busy' && e.body.reason === 'live_view_output') {
    t = 'The live view has the receiver\'s satellite positions turned on. The station turns them off first (the live view stays open ' +
      'without them): start maintenance again in a few seconds.';
  }
  if (e.code === 'invalid_field' && e.field) t = fieldErrorText(e);
  if (e.code === 'reboot_inhibited' && e.by) t += ' (' + e.by + ')';
  if (e.code === 'internal' && e.message) t += ' (' + e.message + ')';
  if (/[^.!?]$/.test(t)) t += '.';
  if (e.retry_in_s) t += ' Try again in ' + fmt.dur(e.retry_in_s) + '.';
  return t;
}

/* ---- Small async helpers ------------------------------------------------ */

/** Run fn with the button disabled and spinning. */
async function busy(button, fn) {
  if (button) {
    button.disabled = true;
    button.classList.add('busy');
  }
  try {
    return await fn();
  } finally {
    if (button) {
      button.disabled = false;
      button.classList.remove('busy');
    }
  }
}

/** busy() plus error display: in `out` (an element) or as a toast. Resolves undefined on error. */
async function act(button, fn, out) {
  if (out) out.hidden = true;
  try {
    return await busy(button, fn);
  } catch (e) {
    showErr(e, out);
    return undefined;
  }
}

function showErr(e, out) {
  if (!(e instanceof ApiError)) console.error(e);
  if (e instanceof ApiError && e.status === 401) return; /* the login screen takes over */
  const text = errText(e);
  if (out) {
    mount(out, text);
    out.className = 'msg bad';
    out.hidden = false;
  } else {
    toast(text, 'bad');
  }
}

/** Show a plain message in a box made by errBox(). */
function say(out, text, tone) {
  mount(out, text);
  out.className = 'msg ' + (tone || 'bad');
  out.hidden = false;
}

/** Empty message box for act(); hidden until something is shown. */
function errBox() {
  return h('div', { class: 'msg bad', role: 'alert', hidden: true });
}

/**
 * Repeat fn every `ms` while the page is visible, never overlapping
 * (later = wait one period before the first call). Returns a stop function.
 */
function poller(ms, fn, later) {
  let stopped = false, timer = 0;
  const loop = async () => {
    if (stopped) return;
    if (doc.visibilityState !== 'hidden') {
      try {
        await fn();
      } catch (e) {
        if (!(e instanceof ApiError)) console.error(e);
      }
    }
    if (!stopped) timer = setTimeout(loop, ms);
  };
  if (later) timer = setTimeout(loop, ms);
  else loop();
  return () => {
    stopped = true;
    clearTimeout(timer);
  };
}

function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}

function toast(text, tone) {
  const box = doc.getElementById('toasts');
  const t = h('div', { class: 'toast ' + (tone || 'info') }, text);
  box.append(t);
  setTimeout(() => t.remove(), tone === 'bad' ? 7000 : 4000);
}
