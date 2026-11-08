/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
const fs = require('fs'), vm = require('vm'), assert = require('assert/strict');
const [bundlePath, scenarioPath, optionsJson] = process.argv.slice(2);
const options = JSON.parse(optionsJson);
const noop = () => {};
class Node {
  get isConnected() { return !!this.root || !!(this.parent && this.parent.isConnected); }
  get parentNode() { return this.parent || null; }
  get parentElement() { return this.parent || null; }
  get nextSibling() {
    const k = this.parent ? this.parent.kids : [];
    const i = k.indexOf(this);
    return i >= 0 && i + 1 < k.length ? k[i + 1] : null;
  }
  remove() {
    if (this.parent) this.parent.kids = this.parent.kids.filter(k => k !== this);
    this.parent = null;
  }
}
class Text extends Node {
  constructor(t) { super(); this.nodeType = 3; this.data = t; }
  get nodeName() { return '#text'; }
  get nodeValue() { return this.data; }
  set nodeValue(v) { this.data = String(v); }
  get textContent() { return this.data; }
}
class El extends Node {
  constructor(tag) {
    super();
    this.nodeType = 1; this.tagName = String(tag).toUpperCase(); this.kids = []; this.attrs = {}; this.on = {};
    this.style = {}; this.dataset = {}; this.className = ''; this.hidden = false; this.disabled = false;
    this.checked = false; this.selected = false; this.readOnly = false; this.v = '';
    const classes = () => new Set(this.className.split(/\s+/).filter(Boolean));
    this.classList = {
      add: k => { const c = classes(); c.add(k); this.className = [...c].join(' '); },
      remove: k => { const c = classes(); c.delete(k); this.className = [...c].join(' '); },
      contains: k => classes().has(k),
      toggle: (k, on) => { const c = classes(); if (on === undefined) on = !c.has(k);
        if (on) c.add(k); else c.delete(k); this.className = [...c].join(' '); return on; },
    };
  }
  append(...kids) {
    for (const k of kids) {
      const n = k instanceof Node ? k : new Text(String(k));
      if (n.parent) n.remove();
      n.parent = this;
      this.kids.push(n);
    }
  }
  appendChild(k) { this.append(k); return k; }
  prepend(...kids) {
    const nodes = kids.map(k => (k instanceof Node ? k : new Text(String(k))));
    for (const n of nodes) {
      if (n.parent) n.remove();
      n.parent = this;
    }
    this.kids.unshift(...nodes);
  }
  replaceChildren(...kids) { this.kids = []; this.append(...kids); }
  replaceChild(n, old) {
    if (n.parent) n.remove();
    const i = this.kids.indexOf(old);
    n.parent = this;
    if (i < 0) this.kids.push(n);
    else {
      this.kids[i] = n;
      old.parent = null;
    }
    return old;
  }
  get nodeName() { return this.tagName; }
  get firstChild() { return this.kids[0] || null; }
  get children() { return this.kids.filter(k => k instanceof El); }
  get attributes() { return Object.keys(this.attrs).map(name => ({ name: name, value: this.attrs[name] })); }
  addEventListener(type, fn) { (this.on[type] = this.on[type] || []).push(fn); }
  removeEventListener() {}
  /* bubble: the event goes up through the parents too, as an input or click event does in a page. */
  fire(type, bubble) {
    const ev = { type, target: this, preventDefault: noop, stopPropagation: noop };
    for (let e = this; e; e = bubble ? e.parent : null) for (const fn of e.on[type] || []) fn(ev);
  }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; }
  hasAttribute(k) { return k in this.attrs; }
  get textContent() { return this.kids.map(c => c.textContent).join(''); }
  set textContent(t) {
    for (const k of this.kids) k.parent = null;
    this.kids = t === '' || t === null || t === undefined ? [] : [new Text(String(t))];
    for (const k of this.kids) k.parent = this;
  }
  get options() { return this.kids.filter(c => c.tagName === 'OPTION'); }
  get value() {
    if (this.tagName !== 'SELECT') return this.v;
    const o = this.options.find(x => x.selected) || this.options[0];
    return o ? o.v : '';
  }
  set value(x) {
    if (this.tagName === 'SELECT') for (const o of this.options) o.selected = o.v === String(x);
    else this.v = String(x);
  }
  focus() {}
  blur() {}
  scrollIntoView() {}
  /* A plain ".class" or "tag" finds the first such element inside; any other selector finds nothing. */
  querySelector(sel) {
    const cls = /^\.([\w-]+)$/.exec(sel), tag = /^([a-z]+)$/.exec(sel);
    for (const e of this.walk()) {
      if (e === this) continue;
      if (cls && (' ' + e.className + ' ').indexOf(' ' + cls[1] + ' ') >= 0) return e;
      if (tag && e.tagName === tag[1].toUpperCase()) return e;
    }
    return null;
  }
  /* A plain "tag" finds every such element inside; any other selector finds nothing. */
  querySelectorAll(sel) {
    const tag = /^([a-z]+)$/.exec(sel);
    return tag ? [...this.walk()].filter(e => e !== this && e.tagName === tag[1].toUpperCase()) : [];
  }
  closest() { return null; }
  contains(x) { for (const e of this.walk()) if (e === x) return true; return false; }
  *walk() { yield this; for (const c of this.kids) if (c instanceof El) yield* c.walk(); }
}

const ids = {};
const element = id => {
  if (!ids[id]) { ids[id] = new El('div'); ids[id].root = true; }
  return ids[id];
};
const storageMap = new Map(Object.entries(options.storage || {}));
const sessionMap = new Map();
const storage = values => ({ getItem: k => values.get(k) ?? null,
  setItem: (k, v) => values.set(k, String(v)), removeItem: k => values.delete(k) });
const timers = new Map(), intervals = new Map();
let timerId = 0;
const session = Object.assign({ api: 1, authenticated: true, admin_set: true, setup_done: true,
  public_status: false, settings_available: true, hostname: 'counter', claim_allowed: true }, options.session);
const preferences = { settings_available: true, public_status: false, setup_step: 'network', setup_done: false };
const status = { api: 1, public: false, uptime_s: 12, product: { counter: 7 }, network: { online: true, ssid: null, ip: null } };
const env = { now: 1000000, sent: [], toasts: [], confirms: [], allowLeave: false,
  failSave: false, failLoad: false, failSession: false, session, preferences, status, timers, intervals, ids, storageMap, sessionMap,
  flush: async () => { for (let i = 0; i < 30; i++) await Promise.resolve(); },
  settle: () => { const due = [...timers].filter(([,t]) => t.ms === 300);
    for (const [id, t] of due) { timers.delete(id); t.fn(); } },
  text: id => element(id).textContent,
};
class Clock extends Date { static now() { return env.now; } }
class Socket {
  constructor() { this.readyState = 0; this.sent = []; }
  close() { this.readyState = 3; }
  send(message) { this.sent.push(JSON.parse(message)); }
}
const location = { pathname: '/', hash: '', host: 'counter.invalid', protocol: 'http:', reload: noop };
const ctx = {
  Node, env, assert, Date: Clock, WebSocket: Socket,
  document: { addEventListener: noop, removeEventListener: noop, getElementById: element,
    createElement: t => new El(t), createElementNS: (ns,t) => new El(t), createTextNode: t => new Text(t),
    createDocumentFragment: () => new El('#fragment'), querySelector: () => null, querySelectorAll: () => [],
    documentElement: new El('html'), body: new El('body'), hidden: false, visibilityState: 'visible' },
  window: { addEventListener: noop, removeEventListener: noop, scrollTo: noop,
    matchMedia: () => ({ matches: false, addEventListener: noop }),
    confirm: text => { env.confirms.push(text); return env.allowLeave; } },
  location, navigator: {}, history: { replaceState: (state, title, hash) => { location.hash = hash; } },
  localStorage: storage(storageMap), sessionStorage: storage(sessionMap),
  setTimeout: (fn, ms) => { const id = ++timerId; timers.set(id, { fn, ms }); return id; },
  clearTimeout: id => timers.delete(id),
  setInterval: (fn, ms) => { const id = ++timerId; intervals.set(id, { fn, ms }); return id; },
  clearInterval: id => intervals.delete(id),
  AbortController, TextEncoder, crypto: globalThis.crypto, console,
  fetch: async (path, init) => {
    const body = init.body ? JSON.parse(init.body) : null;
    env.sent.push({ path, method: init.method, body, headers: init.headers });
    let result;
    if (path === '/api/session') {
      if (env.failSession) return { ok: false, status: 503, headers: { get: () => 'application/json' },
        json: async () => ({ error: { code: 'unavailable' } }) };
      result = session;
    } else if (path === '/api/status') result = status;
    else if (path === '/api/setup/admin') {
      session.authenticated = session.admin_set = true;
      result = { authenticated: true, persisted: true };
    } else if (path === '/api/product/settings') {
      if (init.method === 'PUT') {
        if (env.failSave) return { ok: false, status: 500, headers: { get: () => 'application/json' },
          json: async () => ({ error: { code: 'internal' } }) };
        Object.assign(preferences, body);
      } else if (env.failLoad) return { ok: false, status: 500, headers: { get: () => 'application/json' },
        json: async () => ({ error: { code: 'internal' } }) };
      result = preferences;
    } else throw new Error('undeclared product request: ' + init.method + ' ' + path);
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ ...result }) };
  },
};
vm.createContext(ctx);
let source = fs.readFileSync(bundlePath, 'utf8');
assert(source.endsWith('})();\n'));
const scenario = fs.readFileSync(scenarioPath, 'utf8');
source = source.slice(0, -6) + `
// Test-only access, inserted after the complete production startup code.
globalThis.runScenario = async () => {
  await env.flush();
  toast = (text, tone) => env.toasts.push({text, tone});
  ${scenario}
};
})();
`;
vm.runInContext(source, ctx, { filename: bundlePath });
ctx.runScenario().then(value => process.stdout.write(JSON.stringify(value ?? true)), error => {
  console.error(error); process.exitCode = 1;
});
