# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The caster editor (components/web/ui/js/50-casters.js) driven in Node: what its Save sends and what it shows.

The UI's modules are loaded the way the bundle joins them (every js/*.js but
the start-up in 99-main.js, in file-name order, inside one function) over a
small DOM: elements with children, attributes, listeners and the form
properties the editor reads (a select's value is its selected option's).
Each case opens the editor on a slot as GET /api/casters gives it, changes
the caster choice and the fields as a user does (with the "change" events
the page gets), submits the form and returns the body of the PUT it sends
(fetch is stubbed; nothing is stored), or (inspect) what the editor shows
where. Other cases build a caster card's parts (the duplicate-stream
warning, the connection details), the Status page and the no-receiver
panel, and run the setup's Onocoy step against scripted answers (events
bubble up through the parents there) and Logs' "Clear journal"
against the station's answers to it. The Receiver pages are built whole
against scripted answers (patch() works on this DOM too: children know
their parent and siblings, and a node appended elsewhere moves). Skipped
when Node.js is not installed.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from datetime import datetime
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
UI_JS = ROOT / "components" / "web" / "ui" / "js"

HARNESS = r"""
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const [dir, casesJson] = process.argv.slice(-2);   /* node -e: no script path in argv */
const files = fs.readdirSync(dir).filter(f => f.endsWith('.js') && f !== '99-main.js').sort();
const src = files.map(f => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n');
const noop = () => {};

class Node {
  get parentNode() { return this.parent || null; }
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
    this.classList = { add: noop, remove: noop, toggle: noop, contains: () => false };
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
  querySelectorAll() { return []; }
  closest() { return null; }
  contains(x) { for (const e of this.walk()) if (e === x) return true; return false; }
  *walk() { yield this; for (const c of this.kids) if (c instanceof El) yield* c.walk(); }
}

const sent = [];
const timers = [];   /* the delays asked of setTimeout (never run) */
/* routes(method, path, body) -> [status, json]: the station's answers for a case that needs them (else every
 * request succeeds with an empty caster). ids: the page's elements by id, the same one every time. */
let routes = null;
const ids = {};
const storage = { getItem: () => null, setItem: noop, removeItem: noop };
const ctx = {
  Node,
  document: { addEventListener: noop, removeEventListener: noop,
              getElementById: id => ids[id] || (ids[id] = new El('div')), createElement: t => new El(t),
              createElementNS: (ns, t) => new El(t), createTextNode: t => new Text(t), querySelector: () => new El('div'),
              createDocumentFragment: () => new El('#fragment'),
              querySelectorAll: () => [], documentElement: new El('html'), body: new El('body'), hidden: false,
              visibilityState: 'visible' },
  window: { addEventListener: noop, removeEventListener: noop,
            matchMedia: () => ({ matches: false, addEventListener: noop }), confirm: () => true,
            scrollTo: noop },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {}, history: { replaceState: noop },
  localStorage: storage, sessionStorage: storage,
  setTimeout: (fn, ms) => { timers.push(ms); return 0; }, clearTimeout: noop, setInterval: noop, clearInterval: noop,
  AbortController: globalThis.AbortController, TextEncoder: globalThis.TextEncoder, crypto: globalThis.crypto,
  console,
  fetch: async (p, init) => {
    const body = init.body ? JSON.parse(init.body) : null;
    sent.push({ path: p, method: init.method, body: body });
    const r = routes ? routes(init.method, p, body) : [200, { slot: 0, config: {} }];
    if (r === 'abort') {   /* no answer before the page's timeout */
      const e = new Error('aborted');
      e.name = 'AbortError';
      throw e;
    }
    return { ok: r[0] < 300, status: r[0], headers: { get: () => 'application/json' }, json: async () => r[1] };
  },
};
vm.createContext(ctx);
const ui = vm.runInContext('"use strict";\n(function () {\n' + src +
  '\nreturn { casterEditor, openCasterWarning, casterEvidence, casterSummary, turnCaster, onOffSwitch, stationState, adminDashboard, heroAlso,\n' +
  '  publicDashboard, noReceiverPanel, startWizard, manageCtl, eventCard, healthCard, chipCard, signalsHead, bandTable, kv,\n' +
  '  startLines, foldRows, diagReceiverCard, receiverPage, renderLive,\n' +
  '  settingsPage: (el, arg) => Views.settings.create(el, arg),\n' +
  '  setStatus: s => { S.status = s; }, setSession: s => { S.session = s; },\n' +
  '  setLive: (age, started, page) => { S.statusAt = age === null ? 0 : Date.now() - age * 1000; shell.started = started;\n' +
  '    shell.current = page === null ? null : { name: page, arg: \'\', view: {} }; } };\n})()',
  ctx);

/* The elements on screen: hidden ones and everything inside them left out. */
function* visible(e) {
  if (e.hidden) return;
  yield e;
  for (const c of e.kids) if (c instanceof El) yield* visible(c);
}

/* What the editor shows: its heading, the field names in order, a preset's host and port (text), the help line, the
 * hints, the text fields' values in order (host:port, mountpoint, username: those shown), how many fold-outs it
 * has (shown or not), whether a retry pace shows, the pace choices and the hints and lines that say waits. */
function inspect(root) {
  const vis = [...visible(root)];
  const text = e => e.textContent;
  const names = els => els.filter(e => e.tagName === 'LEGEND' || (e.tagName === 'SPAN' && e.className === 'lbl') ||
    (e.tagName === 'LABEL' && e.className === 'check')).map(e => (e.tagName === 'LABEL' ? text(e.kids[1].kids[0]) : text(e)));
  /* A preset's host and port, as text where Other server has its field. */
  const fixed = vis.find(e => e.className === 'fixed-value' && !e.kids.some(k => k.tagName === 'LABEL'));
  const help = vis.find(e => e.tagName === 'P' && e.className === 'muted small');
  const pace = vis.find(e => e.tagName === 'SELECT' && e.options.some(o => o.v === 'polite'));
  return {
    title: text(vis.find(e => e.tagName === 'H2')),
    fields: names(vis),
    fixed: fixed ? text(fixed) : null,
    help: help ? text(help) : null,
    hints: vis.filter(e => e.tagName === 'SMALL' && e.className === 'hint').map(text),
    values: vis.filter(e => e.tagName === 'INPUT' && e.attrs.maxlength && e.attrs.type !== 'password').map(e => e.value),
    details: [...root.walk()].filter(e => e.tagName === 'DETAILS').length,
    pace: vis.some(e => e.kids.some(k => k instanceof Text && k.data.indexOf('Retry pace') >= 0)),
    paces: pace ? pace.options.map(text) : null,
    waits: vis.filter(e => /\d\u00a0(s|min)\b/.test(text(e)) && (e.tagName === 'SMALL' || e.tagName === 'P')).map(text),
  };
}

/* The controls, found as the page shows them. */
function controls(root) {
  const all = [...root.walk()];
  const sel = all.filter(e => e.tagName === 'SELECT');
  const opts = s => s.options.map(o => o.v);
  return {
    caster: sel.find(s => opts(s).includes('custom')),
    pace: sel.find(s => opts(s).includes('polite')),
    host: all.find(e => e.tagName === 'INPUT' && e.attrs.inputmode === 'url'),
    mountpoint: all.find(e => e.tagName === 'INPUT' && e.attrs.maxlength === '47'),
    username: all.filter(e => e.tagName === 'INPUT' && e.attrs.maxlength === '47')[1],
    password: all.find(e => e.tagName === 'INPUT' && e.attrs.type === 'password'),
    title: all.find(e => e.tagName === 'H2'),
    errors: () => all.filter(e => e.tagName === 'SMALL' && e.className === 'err' && !e.hidden).map(e => e.textContent),
  };
}

/* The setup's Onocoy step while an open caster is on: save (refused: the duplicate-stream question), tick the box
 * (the input event goes up to the form as in a page), then "Save anyway". What shows after each, and the PUTs. */
async function wizardDup() {
  const settle = async () => {
    for (let i = 0; i < 30; i++) await new Promise(r => setImmediate(r));
  };
  routes = (m, p, body) => {
    if (p === '/api/wifi') return [200, { configured: true, online: true, ssid: 'Home', ap: {} }];
    if (p === '/api/system') return [200, { hostname: 'onocoydiy' }];
    if (p === '/api/casters' && m === 'GET') {
      return [200, { max_slots: 4, warnings: [], onocoy_preset: { host: 'servers.onocoy.com', port: 2101, tls_port: 2121, ntrip_version: 1 },
        slots: [{ slot: 0, config: { host: 'servers.onocoy.com', port: 2101, enabled: false, name: 'Onocoy', mountpoint: '', username: '',
          password_set: false, protected: true }, status: {} },
        { slot: 1, config: { host: 'rtk2go.com', port: 2101, enabled: true, name: 'RTK2go', mountpoint: 'BASE1', password_set: true },
          status: {} }] }];
    }
    if (p === '/api/casters/0' && m === 'PUT') {
      return body.dup_stream_ack ? [200, { slot: 0, config: body }] :
        [409, { error: { code: 'caster_policy', reason: 'dup_stream_ack_required', message: 'acknowledgement required' } }];
    }
    return [200, {}];
  };
  ui.setSession({ api: 1, authenticated: true, admin_set: true, setup_done: false, setup_step: 'onocoy', hostname: 'onocoydiy' });
  ui.startWizard(false);
  await settle();
  const page = () => [...visible(ids['view-body'])];
  const shows = t => page().some(e => e.kids.some(k => k instanceof Text && k.data === t));
  const inputs = page().filter(e => e.tagName === 'INPUT');
  const text = inputs.filter(e => e.attrs.maxlength === '47');
  text[0].value = 'MP1';
  text[1].value = 'user1';
  inputs.find(e => e.attrs.type === 'password').value = 'secret123';
  page().find(e => e.tagName === 'FORM').fire('submit');
  await settle();
  const r = { asked: shows('Duplicate-stream risk') };
  const box = page().find(e => e.tagName === 'INPUT' && e.attrs.type === 'checkbox');
  box.checked = true;
  box.fire('input', true);
  box.fire('change', true);
  await settle();
  r.kept = shows('Duplicate-stream risk');
  const go = page().find(e => e.tagName === 'BUTTON' && e.textContent === 'Save anyway');
  r.enabled = !!go && !go.disabled;
  if (go) go.fire('click', true);
  await settle();
  r.puts = sent.filter(x => x.method === 'PUT' && x.path === '/api/casters/0').map(x => !!x.body.dup_stream_ack);
  routes = null;
  return r;
}

/* Receiver setup from (autocfg, recovery) to the choice `to` (auto, no_restart: Automatic with its restart box
 * cleared, manual): the PUT bodies in the order sent; fail: the member whose request the station refuses (500). */
async function manageSave(c) {
  const settle = async () => {
    for (let i = 0; i < 30; i++) await new Promise(r => setImmediate(r));
  };
  sent.length = 0;
  routes = (m, p, body) => (c.fail && c.fail in body ? [500, { error: { code: 'internal', message: 'flash' } }] :
    [202, { accepted: true, saved: true, apply: 'unchanged' }]);
  const m = ui.manageCtl();
  m.update({ autocfg: c.autocfg, recovery: c.recovery, first_apply_held: false });
  const root = new El('div');
  root.append(...m.nodes);
  const all = [...root.walk()];
  const radios = all.filter(e => e.tagName === 'INPUT' && e.attrs.name === 'manage');
  const pick = c.to === 'no_restart' ? 'auto' : c.to;
  for (const r of radios) r.checked = r.value === pick;
  radios.find(r => r.value === pick).fire('change');
  const restart = all.find(e => e.tagName === 'INPUT' && e.attrs.type === 'checkbox');
  restart.checked = c.to !== 'no_restart';
  restart.fire('change');
  all.find(e => e.tagName === 'BUTTON' && e.textContent === 'Save').fire('click');
  await settle();
  routes = null;
  return sent.filter(x => x.method === 'PUT').map(x => x.body);
}

/* Logs' event card with two records: "Clear journal…", confirmed, answered with c.answer ([status, body]).
 * What it then shows: the toast (text, tone), the card's error line, whether it read the journal again, and
 * the time the DELETE may take before the page gives up on it. */
async function journalClear(c) {
  const settle = async () => {
    for (let i = 0; i < 40; i++) await new Promise(r => setImmediate(r));
  };
  const recs = [1, 2].map(n => ({ seq: n, kind: 'boot', a: 0, b: 0, uptime_s: n, unix: null }));
  let deleted = false;
  /* c.after: the journal GET /api/health reports once the DELETE went (as before when not given). */
  routes = (m, p) => {
    const j = deleted && c.after ? c.after : { available: true, oldest_seq: 1, newest_seq: 2 };
    if (p === '/api/health') return [200, { journal: j }];
    if (p.startsWith('/api/journal') && m === 'GET') return [200, { entries: j.newest_seq ? recs : [], cursor: 2, more: false }];
    if (p === '/api/journal' && m === 'DELETE') {
      deleted = true;
      return c.answer;
    }
    return [200, {}];
  };
  sent.length = 0;
  ids.toasts = new El('div');
  ids.modal = new El('div');
  const card = ui.eventCard();
  await settle();
  const wipe = [...card.el.walk()].find(e => e.tagName === 'BUTTON' && e.textContent === 'Clear journal…');
  const r = { shown: !!wipe && !wipe.hidden };
  wipe.fire('click');
  await settle();
  const form = [...ids.modal.walk()].find(e => e.tagName === 'FORM');
  const before = sent.length;
  timers.length = 0;
  form.fire('submit');
  await settle();
  const after = sent.slice(before);
  r.deletes = after.filter(x => x.method === 'DELETE' && x.path === '/api/journal').length;
  r.timeout = Math.max(0, ...timers);
  r.reread = after.some(x => x.method === 'GET' && x.path === '/api/health');
  r.toasts = ids.toasts.kids.map(t => [t.textContent, t.className]);
  const err = [...card.el.walk()].find(e => e.attrs.role === 'alert' && e.className.indexOf('msg') === 0);
  r.error = err && !err.hidden ? err.textContent : null;
  r.errorTone = err && !err.hidden ? err.className : null;
  r.rowsBefore = [...visible(card.el)].filter(e => e.tagName === 'TR').length;
  if (c.refresh) {
    /* Then Refresh, with the journal as c.after says. */
    deleted = true;
    [...card.el.walk()].find(e => e.tagName === 'BUTTON' && e.textContent === 'Refresh').fire('click');
    await settle();
  }
  /* What the list shows: its rows' count, or its one line. */
  const lines = [...visible(card.el)].filter(e => e.tagName === 'P' || (e.className || '').indexOf('msg warn') === 0)
    .map(e => e.textContent).filter(t => t);
  r.rows = [...visible(card.el)].filter(e => e.tagName === 'TR').length;
  r.lines = lines;
  r.wipe = !wipe.hidden;
  routes = null;
  return r;
}

/* Logs' event card, its filter changed while the first read still runs: the journal reads it sends. */
async function eventFilterEarly() {
  const settle = async () => {
    for (let i = 0; i < 40; i++) await new Promise(r => setImmediate(r));
  };
  const recs = [1, 2].map(n => ({ seq: n, kind: 'boot', a: 0, b: 0, uptime_s: n, unix: null }));
  routes = (m, p) => {
    if (p === '/api/health') return [200, { journal: { available: true, oldest_seq: 1, newest_seq: 2 } }];
    if (p.startsWith('/api/journal')) return [200, { entries: recs, cursor: 2, more: false }];
    return [200, {}];
  };
  sent.length = 0;
  const card = ui.eventCard();
  const sel = [...card.el.walk()].find(e => e.tagName === 'SELECT');
  sel.value = sel.options[1].v;
  sel.fire('change');
  await settle();
  routes = null;
  return { reads: sent.filter(x => x.path.startsWith('/api/journal')).map(x => x.path),
    rows: [...visible(card.el)].filter(e => e.tagName === 'TR').length };
}

/* Logs' Health card for (hl, status): the chip [text, class], the messages' titles and lines, the rows. */
function healthView(c) {
  const sec = ui.healthCard(c.hl, c.status);
  const v = [...visible(sec)];
  const chip = v.find(e => (e.className || '').indexOf('badge') === 0);
  const msgs = v.filter(e => (e.className || '').indexOf('msg') === 0)
    .map(m => [m.className, ...[...visible(m)].filter(e => e.tagName === 'STRONG' || (e.tagName === 'DIV' && e.className === 'small'))
      .map(e => e.textContent)]);
  const pairs = [];
  for (const dl of v.filter(e => e.tagName === 'DL')) {
    for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
  }
  const buttons = v.filter(e => (e.tagName === 'A' || e.tagName === 'BUTTON') && (e.className || '').indexOf('btn') === 0)
    .map(e => [e.textContent, e.attrs.href || e.attrs['data-act'] || null]);
  return { chip: [chip.textContent, chip.className], msgs: msgs, rows: pairs, buttons: buttons,
    none: v.some(e => e.tagName === 'P' && e.textContent === 'No alerts.') };
}

/* What a page shows (el): the cards' titles, the <details> anywhere in it, the small headings, the field names, the
 * buttons, the label/value rows, the paragraphs, the table rows (their cells' text) and the numbered list's items. */
function pageParts(el) {
  const v = [...visible(el)];
  const pairs = [];
  for (const dl of v.filter(e => e.tagName === 'DL')) {
    for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
  }
  const card = e => e.tagName === 'SECTION' && (' ' + e.className + ' ').indexOf(' card ') >= 0;
  return {
    details: [...el.walk()].filter(e => e.tagName === 'DETAILS').length,
    cards: v.filter(card).map(c => (([...visible(c)].find(e => e.tagName === 'H2') || {}).textContent || null)),
    subs: v.filter(e => e.tagName === 'H3').map(e => e.textContent),
    fields: v.filter(e => e.tagName === 'SPAN' && e.className === 'lbl').map(e => e.textContent),
    buttons: v.filter(e => e.tagName === 'BUTTON' || (e.tagName === 'A' && (e.className || '').indexOf('btn') === 0)).map(e => e.textContent),
    rows: pairs,
    lines: v.filter(e => e.tagName === 'P').map(e => e.textContent),
    trs: v.filter(e => e.tagName === 'TR').map(tr => tr.kids.map(k => k.textContent).join(' | ')),
    lis: v.filter(e => e.tagName === 'LI' && e.parent.tagName === 'OL').length,
    /* The checkboxes by their label; the notes beside a button (why it waits); the correction messages' list. */
    checks: v.filter(e => e.tagName === 'LABEL' && e.className === 'check').map(e => e.kids[1].kids[0].textContent),
    notes: v.filter(e => e.tagName === 'SPAN' && (e.className || '').indexOf('note') === 0).map(e => e.textContent),
    msgList: v.filter(e => e.tagName === 'LI' && e.parent.tagName === 'UL' && e.parent.className.indexOf('msglist') >= 0)
      .map(e => e.textContent),
    msgs: v.filter(e => (e.className || '').indexOf('msg ') === 0).map(m => [m.className.slice(4), m.textContent]),
    /* Amber text (the position casters get, a value that differs). */
    warn: v.filter(e => (' ' + e.className + ' ').indexOf(' t-warn ') >= 0).map(e => e.textContent),
  };
}

/* A Receiver page (c.arg: '' the page, 'settings' Receiver settings, 'live' the page with the sky as it is) for the
 * status c.status and the station's answers: c.gnss (GET /api/gnss), c.sig (its signals), c.test (the comparison),
 * c.live (the live view), c.preview (the held configuration's commands). What it shows (pageParts); with c.confirm,
 * also what the held configuration's dialog shows once "Configure the receiver now" is pressed. */
async function receiverPageView(c) {
  const settle = async () => {
    for (let i = 0; i < 60; i++) await new Promise(r => setImmediate(r));
  };
  routes = (m, p) => {
    const path = p.split('?')[0];
    if (path === '/api/gnss') return [200, c.gnss];
    if (path === '/api/gnss/signals') return [200, c.sig];
    if (path === '/api/gnss/signal-test') return [200, c.test || { state: 'idle', groups: null, results: null, outcome: null }];
    if (path === '/api/gnss/live') return [200, c.live || { window: { open: false, last_end: 'none' } }];
    if (path === '/api/gnss/apply/preview') return [200, c.preview || { steps: [] }];
    return [200, {}];
  };
  ui.setStatus(c.status);
  ids.modal = new El('div');
  const el = new El('div');
  const view = ui.receiverPage(el, c.arg);
  await settle();
  const r = pageParts(el);
  if (c.confirm) {
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === 'Configure the receiver now').fire('click');
    await settle();
    r.dialog = pageParts(ids.modal);
  }
  if (c.test2) {
    /* The comparison's next answer (c.test2) and a refresh: is its results box the same element (a focus on it stays), and
     * what does it show now. */
    const box = () => [...visible(el)].find(e => e.attrs && e.attrs['aria-label'] === 'Signal-group comparison results');
    const before = box();
    c.test = c.test2;
    view.reload();
    await settle();
    r.again = { same: !!before && box() === before, trs: pageParts(el).trs };
  }
  view.destroy();
  routes = null;
  ui.setStatus(null);
  return r;
}

/* Settings for the status c.status and the station's answers: c.wifi (GET /api/wifi), c.update (GET /api/update), c.flash
 * (GET /api/gnss/flash), c.staged (GET /api/upload/gnss). What it shows (pageParts); with c.click, also what it shows once
 * the first button of that name is pressed. */
async function settingsPageView(c) {
  const settle = async () => {
    for (let i = 0; i < 60; i++) await new Promise(r => setImmediate(r));
  };
  routes = (m, p) => {
    const path = p.split('?')[0];
    if (path === '/api/wifi') return [200, c.wifi];
    if (path === '/api/system') return [200, { hostname: 'onocoydiy', hostname_active: 'onocoydiy', public_status: false }];
    if (path === '/api/update') return [200, c.update];
    if (path === '/api/gnss/flash') return [200, c.flash || { journal: 'none', flash_available: false }];
    if (path === '/api/upload/gnss') return [200, c.staged || { staged: null }];
    return [200, {}];
  };
  ui.setStatus(c.status);
  ui.setSession({ api: 1, authenticated: true, admin_set: true, setup_done: true, ap_ssid: 'onocoydiy-1A2B' });
  const el = new El('div');
  const view = ui.settingsPage(el, '');
  await settle();
  const r = pageParts(el);
  if (c.click) {
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === c.click).fire('click');
    r.after = pageParts(el);
  }
  view.destroy();
  routes = null;
  ui.setStatus(null);
  return r;
}

/* Logs' event list for the journal c.recs: its table's class, and per row the When cell [tag, class, date, time] and the
 * other cells' text. */
async function eventRows(c) {
  const settle = async () => {
    for (let i = 0; i < 40; i++) await new Promise(r => setImmediate(r));
  };
  routes = (m, p) => {
    if (p === '/api/health') return [200, { journal: { available: true, oldest_seq: 1, newest_seq: c.recs.length } }];
    if (p.startsWith('/api/journal')) return [200, { entries: c.recs, cursor: c.recs.length, more: false }];
    return [200, {}];
  };
  const card = ui.eventCard();
  await settle();
  routes = null;
  const table = [...card.el.walk()].find(e => e.tagName === 'TABLE');
  const part = (td, cls) => {
    const x = [...td.walk()].find(e => e.className === cls);
    return x ? x.textContent : null;
  };
  return { cls: table.className, rows: [...table.walk()].filter(e => e.tagName === 'TR').map(tr => [tr.kids[0].tagName,
    tr.kids[0].className, part(tr.kids[0], 'd'), part(tr.kids[0], 'tm'), tr.kids.slice(1).map(k => k.textContent).join(' | ')]) };
}

/* The Receiver page's Signals card for c.sig (GET /api/gnss/signals) while corrections come (c.live), with c.master the
 * main antenna's signal group the receiver runs (GET /api/gnss; null while unknown): its headline, the
 * hints (tone, text), the words only assistive technology reads, and whether the band table shows. */
function signalsView(c) {
  const el = new El('div');
  el.append(...ui.signalsHead(c.sig, c.live, c.master === undefined ? null : c.master).filter(Boolean), ui.bandTable(c.sig.systems));
  const v = [...visible(el)];
  const big = v.find(e => e.className === 'big');
  const checks = v.filter(e => e.tagName === 'LI' && e.parent && (e.parent.className || '').indexOf('checks') === 0);
  const words = v.filter(e => e.className === 'sr-only').map(e => e.textContent);
  return { big: big ? big.textContent : null, checks: checks.map(li => [li.className, li.textContent]), words: words,
    table: v.some(e => e.tagName === 'TABLE'), lines: v.filter(e => e.tagName === 'UL').map(e => e.className) };
}

const out = [];
(async () => {
for (const c of JSON.parse(casesJson)) {
  if (c.fn === 'journal_clear') {
    out.push(await journalClear(c));
    continue;
  }
  if (c.fn === 'event_filter_early') {
    out.push(await eventFilterEarly());
    continue;
  }
  if (c.fn === 'health') {
    out.push(healthView(c));
    continue;
  }
  if (c.fn === 'signals') {
    out.push(signalsView(c));
    continue;
  }
  if (c.fn === 'rx_page') {
    out.push(await receiverPageView(c));
    continue;
  }
  if (c.fn === 'settings_page') {
    out.push(await settingsPageView(c));
    continue;
  }
  if (c.fn === 'event_rows') {
    out.push(await eventRows(c));
    continue;
  }
  if (c.fn === 'chip') {
    const dl = [...ui.chipCard(c.sys, c.status, c.wifi, c.net).walk()].find(e => e.tagName === 'DL');
    const pairs = [];
    for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
    out.push(pairs);
    continue;
  }
  if (c.fn === 'starts') {
    /* Logs' earlier starts: per row its cells' text, then its hover text. today: the day of `now` in the page's zone. */
    const day = new Date(c.now * 1000).toDateString();
    out.push(ui.startLines(c.starts, day).map(tr => [...tr.kids.map(td => td.textContent), tr.getAttribute('title')]));
    continue;
  }
  if (c.fn === 'fold') {
    /* foldRows over `count` rows ("row 0", ...): the tbody's rows in order, the fold row as
     * [its class, the cell's colspan, the disclosure's data-k, whether it is open, its summary]. */
    const rows = [...Array(c.count).keys()].map(i => { const tr = new El('tr'); tr.append('row ' + i); return tr; });
    out.push([ui.foldRows(rows, c.n, c.more, c.cols, c.open, c.k)].flat(2).map(tr => {
      if (tr.className !== 'fold') return tr.textContent;
      const td = tr.kids[0], d = td.kids[0];
      return [tr.className, td.getAttribute('colspan'), d.getAttribute('data-k'), d.hasAttribute('open'), d.kids[0].textContent];
    }));
    continue;
  }
  if (c.fn === 'kv') {
    /* Each value as its parts: text, and "|" where it may wrap (a <wbr>). */
    const dl = ui.kv(c.rows);
    const vals = [];
    for (let i = 1; i < dl.kids.length; i += 2) {
      vals.push(dl.kids[i].kids.map(k => (k instanceof Text ? k.data : k.tagName === 'WBR' ? '|' : k.textContent)).join(''));
    }
    out.push(vals);
    continue;
  }
  if (c.fn === 'plain_port') {
    /* The Onocoy card's "Use plain port" (TLS failing): clicked, the card rebuilt while the PUT waits, then after the
     * answer (c.answer); what each build shows. */
    const settle = async () => {
      for (let i = 0; i < 40; i++) await new Promise(r => setImmediate(r));
    };
    sent.length = 0;
    routes = (m, p, body) => (m === 'PUT' ? c.answer : [200, {}]);
    const cfg = Object.assign({ slot: 0 }, c.cfg);
    const st = { state: 'backoff', reason: 'tls', failures_by_class: { tls: 12 } };
    const button = () => {
      const root = new El('div');
      root.append(...ui.casterSummary(cfg, st, { port: 2101 }).filter(Boolean));
      return [...root.walk()].find(e => e.tagName === 'BUTTON' && e.textContent.indexOf('Use plain port') === 0) || null;
    };
    const build = () => {
      const b = button();
      return b ? [b.className, b.getAttribute('aria-disabled'), b.disabled] : null;
    };
    const b0 = button();
    const before = build();
    b0.fire('click');
    const during = build();
    await settle();
    const after = build();
    routes = null;
    out.push({ before: before, during: during, after: after, puts: sent.filter(x => x.method === 'PUT').map(x => x.body) });
    continue;
  }
  if (c.fn === 'card_lines') {
    /* An Onocoy card's lines under its address for a caster status c.st (TLS on): each line's text, the boxes too. */
    const cfg = Object.assign({ slot: 0 }, c.cfg);
    const parts = ui.casterSummary(cfg, c.st, { port: 2101 }).filter(Boolean).slice(2);
    out.push(parts.map(p => p.textContent));
    continue;
  }
  if (c.fn === 'wizard_dup') {
    out.push(await wizardDup());
    continue;
  }
  if (c.fn === 'manage') {
    out.push(await manageSave(c));
    continue;
  }
  if (c.fn === 'warning') {
    const w = ui.openCasterWarning(c.data, c.slot);
    out.push(w ? [...w.walk()].filter(e => e.tagName === 'STRONG').map(e => e.textContent).join('') : null);
    continue;
  }
  if (c.fn === 'card') {
    /* A card's state line (its words, and the class of its state word) and its address line. */
    const parts = ui.casterSummary(Object.assign({ slot: 0 }, c.cfg), c.st, { port: 2101 }).filter(Boolean);
    out.push({ line: parts[0].textContent, tone: parts[0].kids[0].className, address: parts[1].textContent });
    continue;
  }
  if (c.fn === 'dashboard') {
    /* The Status page as built from a status: the top card's title, line and short line, its "Also:" lines ([id, title,
     * text, tone]), its facts; the grid's class and its cards in order; per card (by title) the label/value rows, the
     * one-line casters, the bar, the chip [text, class], its links [text, href, accessible name] (head: in its title row),
     * its paragraphs, the large figure, the coloured parts [class, text], the parts kept on one line, the buttons and
     * whether it is wide. The top card's buttons as shown [text, href], the caster slots its line names, and every class
     * on the page that gives a colour (a tone, a tone's text, a C/N0 class). */
    const st = ui.stationState(c.status, c.admin);
    const kids = (c.admin ? ui.adminDashboard(c.status, st, c.open || []) : ui.publicDashboard(c.status, st, null)).filter(Boolean);
    const all = kids.flatMap(k => [...visible(k)]);
    /* A list's label/value pairs (the top card's facts: each pair in a div of its own). */
    const pairs = v => v.filter(e => e.tagName === 'DL').flatMap(dl => {
      const r = [], kids = dl.kids.flatMap(k => (k.tagName === 'DIV' ? k.kids : [k]));
      for (let i = 0; i + 1 < kids.length; i += 2) r.push([kids[i].textContent, kids[i + 1].textContent]);
      return r;
    });
    const cls = e => ' ' + (e.className || '') + ' ';
    const hero = all.find(e => e.tagName === 'SECTION' && e.className.indexOf('hero') >= 0);
    const hv = [...visible(hero)];
    const grid = kids.find(k => k !== hero);
    const cards = {}, order = [];
    for (const sec of all.filter(e => e.tagName === 'SECTION' && e !== hero)) {
      const v = [...visible(sec)];
      const bar = v.find(e => (e.className || '').indexOf('bar') === 0);
      const head = v.find(e => e.className === 'card-head');
      const chip = v.find(e => cls(e).indexOf(' badge ') >= 0);
      const big = v.find(e => cls(e).indexOf(' big ') >= 0);
      const title = v.find(e => e.tagName === 'H2').textContent;
      order.push(title);
      cards[title] = { rows: pairs(v),
        lines: v.filter(e => e.className === 'cline').map(e => e.kids[0].textContent),
        /* Each line's state word's class (t-ok, t-warn, t-bad; muted): in the part after the name, with its separator. */
        lineTones: v.filter(e => e.className === 'cline').map(e => e.kids[0].kids[2].kids[1].className),
        /* Each line's name as its parts: text, and "|" where it may wrap (a <wbr>). */
        names: v.filter(e => e.className === 'cline').map(e => e.kids[0].kids[0].kids.map(k => (k instanceof Text ? k.data :
          k.tagName === 'WBR' ? '|' : k.textContent)).join('')),
        bar: bar ? { cls: bar.className, role: bar.attrs.role, label: bar.attrs['aria-label'], now: bar.attrs['aria-valuenow'],
          text: bar.attrs['aria-valuetext'] || null, width: bar.kids[0].style.width } : null,
        chip: chip ? [chip.textContent, chip.className] : null,
        links: v.filter(e => e.tagName === 'A').map(e => [e.textContent, e.getAttribute('href'), e.getAttribute('aria-label'),
          !!head && head.contains(e)]),
        notes: v.filter(e => e.tagName === 'P').map(e => [e.textContent, e.className]),
        big: big ? [big.textContent, big.className] : null,
        tones: v.filter(e => / t-(ok|warn|bad) /.test(cls(e))).map(e => [/ t-(ok|warn|bad) /.exec(cls(e))[1], e.textContent]),
        nowrap: v.filter(e => cls(e).indexOf(' nowrap ') >= 0).map(e => e.textContent),
        buttons: v.filter(e => e.tagName === 'BUTTON' || (e.tagName === 'A' && cls(e).indexOf(' btn ') >= 0)).length,
        wide: cls(sec).indexOf(' wide ') >= 0,
        text: sec.textContent };
    }
    const plan = hv.find(e => e.tagName === 'P' && e.className === 'muted small');
    const factLinks = hv.filter(e => e.tagName === 'A' && e.parent && e.parent.tagName === 'DD')
      .map(e => [e.textContent, e.getAttribute('href'), e.getAttribute('aria-label')]);
    const also = (c.admin ? ui.heroAlso(c.status, st, c.open || []) : st.also).map(a => [a.id, a.title, a.text, a.tone]);
    const heroActs = hv.find(e => cls(e).indexOf(' hero-actions ') >= 0);
    const toneClasses = [...new Set(all.flatMap(e => (e.className || '').split(/\s+/))
      .filter(k => /^(t-)?(ok|warn|bad|info)$|^cnr-/.test(k)))].sort();
    out.push({ head: hv.find(e => e.tagName === 'H2').textContent, line: hv.find(e => e.className === 'line').textContent,
      plan: plan ? plan.textContent : null, facts: pairs(hv), factLinks: factLinks, cards: cards, order: order,
      grid: grid ? grid.className : null, also: also, key: st.key, tone: st.tone, subject: st.subject,
      actions: st.actions.map(a => [a.label, a.href || null]), named: st.named,
      heroButtons: heroActs ? heroActs.kids.map(e => [e.textContent, e.getAttribute('href')]) : [], toneClasses: toneClasses,
      heroAlsoShown: hv.filter(e => e.tagName === 'LI').map(e => [e.className, e.kids[1].textContent]) });
    continue;
  }
  if (c.fn === 'header') {
    /* The header's indicator for a status (c.age seconds old, null: none yet) as a visitor (c.admin false) or the
     * admin, on the page c.page ('' the Status page; default Casters): [the link's class, its word, the word's role, the
     * count, whether the count is hidden from assistive technology, the tooltip (on the box inside the link), name,
     * address], whether a second render kept the same word element (its live region), whether the indicator is hidden,
     * whether the link itself has a title (its description), and whether the page is marked as without contact. */
    const cls = new Set();
    ctx.document.body.classList = { add: k => cls.add(k), remove: k => cls.delete(k), contains: k => cls.has(k),
      toggle: (k, on) => (on ? cls.add(k) : cls.delete(k)) };
    ui.setSession({ authenticated: !!c.admin });
    ui.setStatus(c.status);
    ui.setLive(c.age, c.started !== false, c.page === undefined ? 'casters' : c.page);
    ids.live = new El('span');
    ui.renderLive();
    const a = ids.live.querySelector('a');
    if (!a) {
      out.push(null);
      continue;
    }
    const w = a.querySelector('.word'), n = a.querySelector('.age'), box = a.querySelector('.in');
    ui.renderLive();
    out.push([a.className, w.textContent, w.getAttribute('role'), n.textContent, n.getAttribute('aria-hidden'), box.getAttribute('title'),
      a.getAttribute('aria-label'), a.getAttribute('href'), ids.live.querySelector('a') === a && a.querySelector('.word') === w,
      ids.live.hidden, a.hasAttribute('title'), cls.has('no-contact')]);
    continue;
  }
  if (c.fn === 'norx') {
    /* The no-receiver panel (Receiver page, setup) for a status: its title, its line, whether "Check again" shows. */
    const p = ui.noReceiverPanel();
    p.update(c.status);
    const v = [...visible(p.el)];
    out.push([v.find(e => e.tagName === 'STRONG').textContent, v.find(e => e.tagName === 'DIV' && e.className === 'small').textContent,
      v.some(e => e.tagName === 'BUTTON')]);
    continue;
  }
  if (c.fn === 'rx_details') {
    /* Logs' Receiver card (c.preview: the held configuration's commands): every row as [label, text]; with c.full
     * what pageParts says of it. */
    const card = ui.diagReceiverCard(c.g, c.preview || null);
    const all = [...card.walk()];
    const pairs = [];
    for (const dl of all.filter(e => e.tagName === 'DL')) {
      for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
    }
    out.push(c.full ? pageParts(card) : pairs);
    continue;
  }
  if (c.fn === 'turn') {
    /* A card's Turn off or Turn on (turnCaster) against the station's answers to its PUTs (c.answers, in order): the PUT
     * bodies, whether the duplicate-stream dialog asked (then ticked and confirmed, or cancelled: c.confirm), what it
     * returned and the toasts. */
    const settle = async () => {
      for (let i = 0; i < 40; i++) await new Promise(r => setImmediate(r));
    };
    sent.length = 0;
    const answers = c.answers.slice();
    routes = m => (m === 'PUT' ? answers.shift() : [200, {}]);
    ids.modal = new El('div');
    ids.toasts = new El('div');
    const done = ui.turnCaster({ slot: 1, name: 'RTK2go', enabled: !c.on }, c.on, null);
    await settle();
    const dlg = () => [...visible(ids.modal)];
    const asked = dlg().some(e => e.tagName === 'H2' && e.textContent === 'Duplicate-stream risk');
    if (asked && c.confirm) {
      const box = dlg().find(e => e.tagName === 'INPUT' && e.attrs.type === 'checkbox');
      box.checked = true;
      box.fire('change');
      dlg().find(e => e.tagName === 'FORM').fire('submit');
    } else if (asked) {
      dlg().find(e => e.tagName === 'BUTTON' && e.textContent === 'Cancel').fire('click');
    }
    await settle();
    const saved = await done;
    routes = null;
    out.push({ puts: sent.filter(x => x.method === 'PUT').map(x => x.body), asked: asked, saved: saved,
      toasts: ids.toasts.kids.map(t => t.textContent) });
    continue;
  }
  if (c.fn === 'switch') {
    /* An on/off switch clicked twice, its save answering c.answers (true: saved) in turn: [aria-checked, aria-busy]
     * after each answer, and what onChange was asked. */
    const settle = async () => {
      for (let i = 0; i < 10; i++) await new Promise(r => setImmediate(r));
    };
    const asked = [], states = [], answers = c.answers.slice();
    const sw = ui.onOffSwitch(c.on, 'Stream to RTK2go', async on => {
      asked.push(on);
      return answers.shift();
    });
    for (let i = 0; i < c.answers.length; i++) {
      sw.fire('click');
      await settle();
      states.push([sw.getAttribute('aria-checked'), sw.getAttribute('aria-busy')]);
    }
    out.push({ role: sw.getAttribute('role'), name: sw.getAttribute('aria-label'), asked: asked, states: states });
    continue;
  }
  if (c.fn === 'evidence') {
    /* A server card's rows of what it said or did: [label, text] each, [] when there are none. */
    const dl = ui.casterEvidence(c.st);
    const pairs = [];
    for (let i = 0; dl && i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
    out.push(pairs);
    continue;
  }
  sent.length = 0;
  const ed = ui.casterEditor({ slot: c.slot, config: c.config }, Object.assign({ preset: {} }, c.opts || {}));
  /* Found again at each step: a change of server places the fields that server has. */
  for (const [what, v] of c.steps) {
    const f = controls(ed.el);
    f[what].value = v;
    f[what].fire(what === 'caster' || what === 'pace' ? 'change' : 'input');
  }
  const f = controls(ed.el);
  if (c.inspect) {
    out.push(inspect(ed.el));
    continue;
  }
  ed.el.fire('submit');
  const put = sent.find(s => s.method === 'PUT');
  out.push(put ? put.body : { errors: f.errors(), title: f.title.textContent });
}
process.stdout.write(JSON.stringify(out));
})();
"""


def run_editor(cases: list[dict]) -> list[dict]:
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is not installed")
    res = subprocess.run([node, "-e", HARNESS, str(UI_JS), json.dumps(cases)], capture_output=True, text=True,
                         timeout=60)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def stored(host: str, pace: str, **kw) -> dict:
    """A stored slot as GET /api/casters gives it (password stored, enabled)."""
    cfg = {"enabled": True, "name": kw.pop("name", "Mine"), "host": host, "port": 2101, "ntrip_version": 1,
           "tls": False, "trust": "bundle", "ca_pem_set": False, "mountpoint": "MINE01", "username": "",
           "password_set": True, "label": "", "v2_raw_body": False, "backoff_profile": pace, "dup_stream_ack": False}
    cfg.update(kw)
    return cfg


# A new slot starts on RTK2go while another slot streams to Onocoy (as on the casters page).
ONOCOY_ELSEWHERE = {"hideOnocoy": True, "onocoyElsewhere": {"slot": 0, "ready": True, "enabled": True}}
# What a new host needs before Save (the stored password belongs to the old host).
OTHER = [("host", "caster.example.org"), ("mountpoint", "MINE01"), ("password", "secret")]
NEW = [("mountpoint", "MINE01"), ("password", "secret")]


def case(slot: int, config: dict | None, steps: list, opts: dict | None = None, inspect: bool = False) -> dict:
    return {"slot": slot, "config": config, "steps": steps, "opts": opts or {}, "inspect": inspect}


def test_other_caster_keeps_the_pace_the_owner_chose():
    """A preset whose pace the owner set before (Centipede, Emlid, RTKDirect) set nothing in this editor: switching to
    Other server keeps the owner's pace, stored or picked there."""
    out = run_editor([
        # The stored Centipede slot on Polite, switched to Other server.
        case(2, stored("crtk.net", "polite"), [("caster", "custom")] + OTHER),
        # The same with Emlid and RTKDirect.
        case(2, stored("caster.emlid.com", "polite"), [("caster", "custom")] + OTHER),
        case(2, stored("ntrip.rtkdirect.com", "polite", ntrip_version=2, username="me"), [("caster", "custom")] + OTHER),
        # A new slot: Other server with Polite picked, Centipede, then Other server again.
        case(1, None, [("caster", "custom"), ("pace", "polite"), ("caster", "centipede"), ("caster", "custom")] + OTHER,
             ONOCOY_ELSEWHERE),
        # The stored Emlid slot on Polite through RTK2go (which fixes polite) to Other: the stored pace comes back.
        case(2, stored("caster.emlid.com", "polite"), [("caster", "rtk2go"), ("caster", "custom")] + OTHER),
    ])
    assert [b.get("backoff_profile") for b in out] == ["polite"] * 5, out


def test_other_caster_drops_the_pace_a_preset_set():
    """RTK2go's polite (set by the preset, or fixed for a stored RTK2go slot) is not kept on Other server: back to the
    slot's stored pace where the owner set it, else standard."""
    out = run_editor([
        case(1, None, [("caster", "custom")] + OTHER, ONOCOY_ELSEWHERE),
        case(2, stored("rtk2go.com", "polite", name="RTK2go"), [("caster", "custom")] + OTHER),
        case(2, stored("my.caster.net", "polite"), [("caster", "rtk2go"), ("caster", "custom")] + OTHER),
        case(2, stored("my.caster.net", "standard"), [("caster", "rtk2go"), ("caster", "custom")] + OTHER),
        case(2, stored("crtk.net", "standard"), [("caster", "rtk2go"), ("caster", "custom")] + OTHER),
        # Onocoy has no pace of its own either: a new slot (on Onocoy) through RTK2go and Onocoy to Other.
        case(1, None, [("caster", "rtk2go"), ("caster", "onocoy"), ("caster", "custom")] + OTHER),
    ])
    assert [b.get("backoff_profile") for b in out] == ["standard", "standard", "polite", "standard", "standard",
                                                       "standard"], out


def test_presets_with_a_pace_and_unchanged_slots():
    out = run_editor([
        case(1, None, NEW, ONOCOY_ELSEWHERE),                                       # a new slot on RTK2go
        case(1, None, [("caster", "centipede")] + NEW, ONOCOY_ELSEWHERE),           # Centipede's standard
        case(2, stored("my.caster.net", "polite"), [("caster", "centipede")] + NEW),  # a preset sets its own
        case(2, stored("crtk.net", "polite"), []),                                  # nothing changed
        case(2, stored("my.caster.net", "polite"), []),
    ])
    assert [b.get("backoff_profile") for b in out] == ["polite", "standard", "standard", "polite", "polite"], out


def test_host_and_port_are_one_field_for_other_server():
    """Other server takes its host and port in one field ("host:port"; a host alone uses 2101) and says what is wrong
    with either under it; a stored server shows both in it. A new server's heading is "New server"."""
    out = run_editor([
        case(1, None, [("caster", "custom"), ("host", "caster.example.org:2105"), ("mountpoint", "MINE01"),
                       ("password", "secret")], ONOCOY_ELSEWHERE),
        case(1, None, [("caster", "custom"), ("host", "caster.example.org"), ("mountpoint", "MINE01"),
                       ("password", "secret")], ONOCOY_ELSEWHERE),
        case(1, None, [("caster", "custom"), ("host", "bad host:2101")] + NEW, ONOCOY_ELSEWHERE),
        case(1, None, [("caster", "custom"), ("host", "caster.example.org:99999")] + NEW, ONOCOY_ELSEWHERE),
        case(1, None, [("caster", "centipede"), ("password", "secret")], ONOCOY_ELSEWHERE),
        case(2, stored("my.caster.net", "standard", port=2102), [], inspect=True),
    ])
    assert (out[0]["host"], out[0]["port"]) == ("caster.example.org", 2105)
    assert (out[1]["host"], out[1]["port"]) == ("caster.example.org", 2101)
    assert out[2]["errors"] == ["A host name or an IPv4 address, at most 63 characters."], out[2]
    assert out[3]["errors"] == ["After the colon, a port from 1 to 65535."], out[3]
    assert out[4]["errors"] == ["Enter the mountpoint."], out[4]
    assert out[2]["title"] == out[4]["title"] == "New server"
    assert out[5]["values"][0] == "my.caster.net:2102" and out[5]["fixed"] is None


def test_the_name_follows_the_server():
    """No name field: a server is named after its preset, or its host for Other server; a stored name stays while the
    server (and Other server's host) does."""
    out = run_editor([
        case(1, None, [("caster", "custom")] + OTHER, ONOCOY_ELSEWHERE),
        case(1, None, NEW, ONOCOY_ELSEWHERE),
        case(2, stored("my.caster.net", "standard", name="Cabin"), [("mountpoint", "CABIN2")]),
        case(2, stored("my.caster.net", "standard", name="Cabin"), [("host", "other.example.org:2101"), ("password", "x1")]),
        case(2, stored("my.caster.net", "standard", name="Cabin"), [("caster", "rtk2go")] + NEW),
        case(0, ONOCOY_SET_UP, [("password", "secret123")]),
    ])
    assert [b["name"] for b in out] == ["caster.example.org", "RTK2go", "Cabin", "other.example.org", "RTK2go", "Onocoy"]
    # Nothing the form no longer shows is sent: no chunked-encoding choice, no removal of the stored password.
    assert not any("v2_raw_body" in b or "clear_password" in b for b in out), out


def test_a_save_keeps_whether_the_server_is_on():
    """The form has no on/off box: a save keeps the stored value (the card's switch turns a server off and on), a new
    server is saved turned on, and an Onocoy slot that gets set up is turned on too (setting it up means using it), so
    it needs its username and password. A server that stays off needs nothing it would need to stream."""
    pending = stored("servers.onocoy.com", "fast", name="Onocoy", enabled=False, mountpoint="", username="",
                     password_set=False)
    out = run_editor([
        case(1, None, NEW, ONOCOY_ELSEWHERE),
        case(2, stored("crtk.net", "standard", enabled=False), [("mountpoint", "BASE02")]),
        case(2, stored("crtk.net", "standard"), [("mountpoint", "BASE02")]),
        case(0, pending, [("mountpoint", "MP0TEST1"), ("password", "secret123")]),
        case(2, stored("my.caster.net", "standard", enabled=False, password_set=False, mountpoint=""), []),
    ])
    assert [b.get("enabled") for b in out] == [True, False, True, None, False], out
    assert out[3]["errors"] == ["Enter the username from the Onocoy console."], out[3]


ONOCOY_SET_UP = stored("servers.onocoy.com", "fast", name="Onocoy", mountpoint="MP0TEST1", username="user0test")
# Another slot holds Onocoy, not set up: a new slot may still be switched to Onocoy.
ONOCOY_NOT_SET_UP_ELSEWHERE = {"onocoyElsewhere": {"slot": 0, "ready": False, "enabled": False}}


def test_onocoy_asks_for_its_username_and_password_and_a_name():
    """Onocoy's form: its server as text (no input that looks editable), a mountpoint that is any name (the station's,
    as the field takes it, proposed when there is none), the console's username and password. Nothing else: no
    NTRIP version, no TLS, no retry pace, no fold-out. A test slot shows its own server."""
    out = run_editor([
        case(0, ONOCOY_SET_UP, [], inspect=True),
        case(0, None, [], {"hostname": "onocoy diy!"}, inspect=True),
        case(1, None, [("caster", "onocoy")], dict(ONOCOY_NOT_SET_UP_ELSEWHERE, hostname="onocoydiy"), inspect=True),
        case(0, dict(ONOCOY_SET_UP, tls=True, port=2121), [], inspect=True),
        case(0, stored("10.0.2.2", "fast", name="Onocoy", port=2111, mountpoint="MP0TEST1", username="user0test",
                       test_onocoy=True), [], inspect=True),
    ])
    edit, new, switched, tls, test = out
    assert edit["title"] == "Edit Onocoy" and new["title"] == switched["title"] == "New server"
    for o in out:
        assert o["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Username", "Password"], o
        assert o["pace"] is False and o["details"] == 0 and o["waits"] == [], o
        assert o["hints"] == ["Any name; shows only in your Onocoy dashboard."], o
        assert o["help"] == "The Onocoy console gives the username and password, under NTRIP Credentials. Onocoy's guide ↗", o
    assert edit["fixed"] == new["fixed"] == switched["fixed"] == "servers.onocoy.com:2101"
    assert tls["fixed"] == "servers.onocoy.com:2121" and test["fixed"] == "10.0.2.2:2111"
    assert edit["values"][:2] == ["MP0TEST1", "user0test"]
    assert new["values"][:2] == ["onocoydiy", ""] and switched["values"][:2] == ["onocoydiy", ""]


def test_onocoy_takes_the_stations_ntrip_version_until_set_up():
    """No NTRIP version on Onocoy's form: a new Onocoy slot, or one not set up yet, takes the station's (onocoy_preset:
    with 2.0 the chosen mountpoint reaches the dashboard); a slot that is set up keeps its own. The proposed mountpoint
    is saved like a typed one, and with the username and password that is all Onocoy needs."""
    v2 = {"host": "servers.onocoy.com", "port": 2101, "tls_port": 2121, "ntrip_version": 2}
    pending = stored("servers.onocoy.com", "fast", name="Onocoy", enabled=False, mountpoint="", username="",
                     password_set=False, ntrip_version=1)
    creds = [("username", "user0test"), ("password", "secret123")]
    out = run_editor([
        case(0, None, creds, {"preset": v2, "hostname": "onocoydiy"}),
        case(0, pending, creds, {"preset": v2, "hostname": "onocoydiy"}),
        case(0, dict(ONOCOY_SET_UP, ntrip_version=1), [], {"preset": v2}),
        case(1, None, [("caster", "onocoy")] + creds, dict(ONOCOY_NOT_SET_UP_ELSEWHERE, preset=v2, hostname="base-7")),
    ])
    assert [b["ntrip_version"] for b in out] == [2, 2, 1, 2], out
    assert [b["mountpoint"] for b in out] == ["onocoydiy", "onocoydiy", "MP0TEST1", "base-7"], out
    assert out[0]["enabled"] is True and out[0]["host"] == "servers.onocoy.com" and out[0]["backoff_profile"] == "fast"


def test_presets_ask_only_for_what_they_use():
    """A preset asks for its mountpoint, its password and a username only where it uses one (NTRIP 2.0), with its host
    and port as text; its other settings are its own: the stored ones while it stays chosen, the preset's once chosen.
    Other server alone adds one row: NTRIP version, TLS (the certificate check once it is on) and the retry pace with
    its waits, as the station runs them (ntrip_proto.c). No fold-out anywhere."""
    out = run_editor([
        case(1, None, [], ONOCOY_ELSEWHERE, inspect=True),
        case(2, stored("crtk.net", "standard", ntrip_version=2, username="me"), [], inspect=True),
        case(2, stored("caster.emlid.com", "standard", tls=True, trust="custom_ca"), [], inspect=True),
        case(2, stored("caster.emlid.com", "standard", tls=True, trust="custom_ca"), []),
        case(2, stored("caster.emlid.com", "standard", tls=True, trust="custom_ca"), [("caster", "rtkdirect")] + NEW + [
            ("username", "me")]),
        case(1, None, [("caster", "custom")], ONOCOY_ELSEWHERE, inspect=True),
        case(1, None, [("caster", "custom"), ("pace", "polite")], ONOCOY_ELSEWHERE, inspect=True),
        case(2, stored("my.caster.net", "standard", tls=True), [], inspect=True),
    ])
    rtk2go, centipede, emlid, emlid_saved, rtkdirect, custom, polite, custom_tls = out
    for o in (rtk2go, centipede, emlid, custom, polite, custom_tls):
        assert o["details"] == 0, o
    assert rtk2go["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Password"] and rtk2go["pace"] is False
    assert rtk2go["fixed"] == "rtk2go.com:2101"
    assert rtk2go["help"] == "The mountpoint and password of your reservation. One base per internet connection. " \
                             "RTK2go's reservation page ↗"
    assert centipede["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Username", "Password"], centipede
    assert emlid["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Password"], emlid
    # Kept as stored while it stays Emlid; RTKDirect chosen: its own (NTRIP 2.0, no TLS, its port).
    assert (emlid_saved["tls"], emlid_saved["trust"], emlid_saved["ntrip_version"]) == (True, "custom_ca", 1), emlid_saved
    assert (rtkdirect["host"], rtkdirect["tls"], rtkdirect["ntrip_version"], rtkdirect["username"]) == (
        "ntrip.rtkdirect.com", False, 2, "me"), rtkdirect
    assert custom["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Password", "NTRIP version", "TLS",
                                "Encrypted", "Retry pace"], custom
    assert custom["help"] == "Enter what your server gives you for a base."
    assert custom["paces"] == ["Standard (2 s to 1 min)", "Polite (10 s to 5 min)"]
    assert custom["waits"] == ["Refused logins: 1, 5, 15, then 30 min."], custom["waits"]
    assert polite["waits"] == ["Refused logins: 5, 15, 30, then 60 min; after 20 failed connections in a row, one try an "
                               "hour."], polite["waits"]
    assert custom_tls["fields"][-4:] == ["TLS", "Encrypted", "Certificate check", "Retry pace"], custom_tls


def test_every_server_has_one_help_line():
    """Every server type shows one line of help with its own guide (Other server: none), whether it is set up or not."""
    out = run_editor([
        case(0, None, [], inspect=True),
        case(0, ONOCOY_SET_UP, [], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [("caster", "centipede")], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [("caster", "emlid")], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [("caster", "rtkdirect")], inspect=True),
    ])
    guides = ["Onocoy's guide", "Onocoy's guide", "RTK2go's reservation page", "Centipede's guide", "Emlid's guide",
              "RTKDirect's site"]
    for o, g in zip(out, guides):
        assert o["help"].endswith(" " + g + " ↗") and o["help"].count(".") >= 1, o["help"]


def test_turn_off_and_on_from_the_card():
    """A card's switch sends `enabled` alone (the rest stays as stored). Turning an open server on next to Onocoy asks
    about the duplicate stream in a dialog: confirmed, the same request with the acknowledgement; cancelled, nothing
    more."""
    dup = [409, {"error": {"code": "caster_policy", "reason": "dup_stream_ack_required", "message": "acknowledgement required"}}]
    ok = [200, {"slot": 1, "config": {"enabled": True}, "status": {}}]
    off, on, confirmed, cancelled = run_editor([
        {"fn": "turn", "on": False, "answers": [ok]},
        {"fn": "turn", "on": True, "answers": [ok]},
        {"fn": "turn", "on": True, "answers": [dup, ok], "confirm": True},
        {"fn": "turn", "on": True, "answers": [dup], "confirm": False},
    ])
    assert off["puts"] == [{"enabled": False}] and not off["asked"] and off["saved"]
    assert off["toasts"] == ["RTK2go is turned off."]
    assert on["puts"] == [{"enabled": True}] and on["toasts"] == ["RTK2go is turned on."]
    assert confirmed["asked"] and confirmed["puts"] == [{"enabled": True}, {"enabled": True, "dup_stream_ack": True}]
    assert confirmed["saved"] and confirmed["toasts"] == ["RTK2go is turned on."]
    assert cancelled["asked"] and cancelled["puts"] == [{"enabled": True}] and cancelled["saved"] is None
    assert cancelled["toasts"] == []


def test_the_switch_flips_back_when_not_saved():
    """The card's on/off switch (role switch, named for the server) shows the new state at once and goes back when
    the save fails or is cancelled."""
    (r,) = run_editor([{"fn": "switch", "on": True, "answers": [False, True, True]}])
    assert r["role"] == "switch" and r["name"] == "Stream to RTK2go"
    assert r["asked"] == [False, False, True]
    assert r["states"] == [["true", None], ["false", None], ["true", None]]


def test_duplicate_stream_warning_sits_on_the_open_casters_card():
    """GET /api/casters names the open slots streaming next to Onocoy: the warning goes on those cards only."""
    data = {"slots": [{"slot": 0, "config": ONOCOY_SET_UP}, {"slot": 1, "config": stored("rtk2go.com", "polite", name="RTK2go")},
                      {"slot": 2, "config": stored("crtk.net", "standard", name="My base")}, {"slot": 3, "config": None}],
            "warnings": [{"id": "onocoy_open_caster", "onocoy_slots": [0], "open_slots": [1, 2], "rtk2go": True}]}
    calm = dict(data, warnings=[])
    cases = [{"fn": "warning", "data": d, "slot": n} for d in (data, calm) for n in range(4)]
    out = run_editor(cases)
    assert out[:4] == [None, "Onocoy and RTK2go are both on", "Onocoy and Centipede are both on", None], out
    assert out[4:] == [None] * 4


def evidence(st: dict) -> dict:
    return {"fn": "evidence", "st": st}


QUIET = {"state": "streaming", "state_age_s": 600, "last_status_line": "ICY 200 OK", "last_status_code": 200,
         "epochs_dropped_today": 0, "epochs_sent_today": 600, "lag_ms_now": 40, "max_lag_ms": 1000,
         "bytes_sent_today": 1200000, "resolved_ip": "203.0.113.7", "dns_cached_used": 0, "login_rejects": 0,
         "login_blocks": 0, "kicked_sessions": 0, "last_session_text": "", "last_err": None, "data_rate_permille": None,
         "failures_by_class": {k: 0 for k in ("dns", "connect", "tls", "timeout", "remote_close", "write_stall", "keepalive",
                                              "auth", "mount_busy", "config", "banned", "login_blocked", "in_use",
                                              "replaced", "local")}}


def test_server_cards_say_what_went_wrong_and_nothing_else():
    """A server's card shows what it said or did when it refuses or drops the station, on the card itself (no fold):
    each row only when it has something to say, so a server that streams well shows none, not even its old failures.
    The trivia of the old connection details (state since, lag, reconnects, the address, the data rate the Status page
    judges) is gone."""
    quiet, full, refused, rate, past = run_editor([
        evidence(QUIET),
        evidence(dict(QUIET, state="backoff", login_rejects=1, login_blocks=1, kicked_sessions=4, data_rate_permille=974,
                      last_status_line="HTTP/1.1 401 Unauthorized", last_status_code=401,
                      last_session_text="ERROR - Bad Password", last_err="ESP_FAIL",
                      failures_by_class=dict(QUIET["failures_by_class"], timeout=3, dns=1))),
        # NTRIP 1.0's refusal has no status code.
        evidence(dict(QUIET, state="auth_failed", last_status_line="ERROR - Bad Password", last_status_code=0)),
        evidence(dict(QUIET, data_rate_permille=974)),
        evidence(dict(QUIET, failures_by_class=dict(QUIET["failures_by_class"], timeout=3))),
    ])
    assert quiet == [] and rate == [] and past == []
    # The firmware's error code ("ESP_FAIL") is never a row: the causes say it in words.
    assert full == [["Last reply", "HTTP/1.1 401 Unauthorized"], ["Server said", "ERROR - Bad Password"],
                    ["Login blocks", "1 of 3"], ["Sessions taken over", "3 of 3"],
                    ["Failures", "DNS lookup failed (1), Timed out (3)"]], full
    assert refused == [["Last reply", "ERROR - Bad Password"]]


# ---- The server cards' state and address lines; the Status page's receiver rows and server lines ----

def test_server_card_says_its_state_traffic_and_address_in_two_lines():
    """Under its name and switch, a card says its state in a word in its colour with what goes with it (streaming:
    for how long, the rate and what it sent; waiting: the next try; off: just that), then where it streams (host,
    port and mountpoint, not the protocol); a test slot says so in that line."""
    out = run_editor([
        {"fn": "card", "cfg": stored("rtk2go.com", "polite", ntrip_version=2, tls=True),
         "st": {"state": "streaming", "connected_s": 7200, "bytes_sent": 140200000}},
        {"fn": "card", "cfg": stored("10.0.2.2", "fast", port=2111, mountpoint="MP0TEST1", test_onocoy=True),
         "st": {"state": "stopped", "stop_reason": "rejected"}},
        {"fn": "card", "cfg": stored("servers.onocoy.com", "fast", mountpoint=""), "st": {"state": "wait_data"}},
        {"fn": "card", "cfg": stored("my.caster.net", "standard"), "st": {"state": "auth_failed", "next_retry_s": 240}},
        {"fn": "card", "cfg": stored("my.caster.net", "standard", enabled=False), "st": {"state": "disabled"}},
    ])
    assert out[0] == {"line": "Streaming for 2 h · 133.7 MB sent", "tone": "t-ok", "address": "rtk2go.com:2101/MINE01"}
    assert out[1] == {"line": "Stopped: needs you", "tone": "t-bad",
                      "address": "10.0.2.2:2111/MP0TEST1 · Test Onocoy slot (bench build)"}
    assert out[2] == {"line": "Waiting for the receiver", "tone": "t-warn", "address": "servers.onocoy.com:2101"}
    assert out[3]["line"] == "Login refused · next try in 4 min" and out[3]["tone"] == "t-bad"
    assert out[4]["line"] == "Off"


def st_caster(slot: int, name: str, state: str, **kw) -> dict:
    """A caster of GET /api/status (Onocoy set up and counting)."""
    c = {"slot": slot, "name": name, "empty": False, "enabled": state != "disabled", "protected": name == "Onocoy",
         "host": name.lower() + ".example", "state": state, "reason": "none", "next_retry_s": 0, "stop_reason": "none",
         "onocoy_ready": name == "Onocoy", "today_online_s": 600, "today_offline_s": 0, "hint": ""}
    if name == "Onocoy":
        c.update({"streak_ok": True, "budget_s": 473, "budget_left_s": 463, "data_rate_permille": 1000, "connected_s": 600})
    c.update(kw)
    return c


def over(s: dict, **kw) -> dict:
    """s with each section's fields overridden (a dict merges into the section, anything else replaces it)."""
    s = json.loads(json.dumps(s))
    for k, v in kw.items():
        s[k] = {**s[k], **v} if isinstance(v, dict) and isinstance(s.get(k), dict) else v
    return s


def st_status(**kw) -> dict:
    s = {"api": 1, "public": False, "device": {"hostname": "onocoydiy", "settings_available": True},
         "safe_mode": {"active": False}, "network": {"online": True},
         "receiver": {"state": "streaming", "model": "UM982", "rtcm_age_ms": 180, "journal": "none", "best_sys": "galileo",
                      "best_bands": 4, "glonass": True, "rtcm1230_supported": True, "position_locked": False,
                      "surveying": True, "survey_window_left_s": 300},
         "casters": [st_caster(0, "Onocoy", "streaming")],
         "health": {"level": "ok", "last_step": "none", "recovery_mode": "full"},
         "alerts": [], "availability": {"protected_down_today_s": 0, "budget_left_s": 463}}
    return over(s, **kw)


# ---- The Status page: the always-full dashboard ----

RTK2GO = dict(today_online_s=43190, today_offline_s=9)
# Onocoy and RTK2go set up and streaming; Wi-Fi "Home" at -61 dBm; v0.3.0, up to date; 28 satellites, locked.
BASE = st_status(uptime_s=86400, device={"version": "v0.3.0", "dev_build": False},
                 network={"online": True, "ssid": "Home", "rssi": -61, "upstream": "ok", "cred_trial": False},
                 receiver={"satellites": 28, "position_locked": True, "profile_applied": True, "profile_acknowledged": True,
                           "position_known": True, "surveying": False, "survey_unfinished": False, "survey_window_left_s": 0,
                           "identity_confirmed": True, "rx_maintenance": False, "uart_closed": False},
                 casters=[st_caster(0, "Onocoy", "streaming"), st_caster(1, "RTK2go", "streaming", **RTK2GO)],
                 update={"app_available": False, "app_version": "v0.3.0", "app_reverted_version": "",
                         "app_reverted_in_stage1": False, "gnss_available": False})
NO_DAY = {k: None for k in ("streak_ok", "budget_s", "budget_left_s", "data_rate_permille")}
ONO_SETUP = st_caster(0, "Onocoy", "disabled", enabled=False, onocoy_ready=False, today_online_s=0, **NO_DAY)
ONO_OFF = st_caster(0, "Onocoy", "disabled", enabled=False, today_online_s=0, **NO_DAY)
RTK = st_caster(1, "RTK2go", "streaming", **RTK2GO)
FEW = {"best_bands": 3, "best_sys": "gps"}
NO_RTCM = {"rtcm_age_ms": None, "satellites": None, "best_bands": 0, "best_sys": None, "glonass": False,
           "position_locked": False, "surveying": True, "survey_unfinished": True}
WAITING = [st_caster(0, "Onocoy", "wait_data"), st_caster(1, "RTK2go", "wait_data", **RTK2GO)]
SAFE_SLOT = {"name": None, "enabled": False, "protected": False, "onocoy_ready": False, "state": "disabled"}

# Every state of the page (admin), by name.
MATRIX = {
    "healthy": BASE,
    "quality": over(BASE, receiver=FEW, alerts=[{"id": "bands", "source": "health"}]),
    "quality_down": over(BASE, receiver=FEW, casters=[st_caster(0, "Onocoy", "streaming"),
                                                      st_caster(1, "RTK2go", "auth_failed", reason="auth", next_retry_s=30)]),
    "quality_down_alert": over(BASE, receiver=FEW, alerts=[{"id": "bands", "source": "health"}],
                               casters=[st_caster(0, "Onocoy", "streaming"), st_caster(1, "RTK2go", "auth_failed", reason="auth")]),
    "noglonass": over(BASE, receiver={"glonass": False}, casters=[st_caster(0, "Onocoy", "backoff", reason="timeout",
                                                                            next_retry_s=20), RTK]),
    "stall": over(BASE, receiver={"rtcm_age_ms": 6000}, health={"level": "recovering", "last_step": "link_restart"},
                  casters=WAITING),
    "noant": over(BASE, receiver=NO_RTCM, health={"level": "degraded", "recovery_mode": "alerts_only"},
                  alerts=[{"id": "no_rtcm", "source": "health"}], casters=WAITING),
    "norx": over(BASE, receiver={"state": "no_receiver", "responding": False, "next_detect_in_s": 23, "rtcm_age_ms": None},
                 alerts=[{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 23}], casters=WAITING),
    "uart": over(BASE, receiver={"state": "detecting", "uart_closed": True, "rtcm_age_ms": None}, casters=WAITING),
    "journal": over(BASE, receiver={"journal": "waiting", "state": "idle"}, casters=WAITING),
    "updating": over(BASE, receiver={"state": "flashing", "rtcm_age_ms": None}, casters=WAITING),
    "maint": over(BASE, receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 1500}),
    "maint_unchecked": over(BASE, receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 1500,
                                            "identity_confirmed": False, "position_locked": False}),
    "held": over(BASE, receiver={"first_apply_held": True, "rtcm_age_ms": None, "best_bands": 0, "position_locked": False,
                                 "position_known": False}, casters=WAITING),
    "starting": over(BASE, receiver={"state": "idle", "rtcm_age_ms": None}, casters=WAITING),
    "configuring": over(BASE, receiver={"state": "configuring"}, casters=WAITING),
    "survey": over(BASE, receiver={"position_locked": False, "surveying": True, "survey_window_left_s": 300}),
    "fixed_far": over(BASE, alerts=[{"id": "fixed_far", "source": "station", "horizontal_m": 120, "vertical_m": 3,
                                     "distance_m": 120, "height_only": False, "sea_level": False}]),
    "setup": over(BASE, casters=[ONO_SETUP, RTK], availability={"budget_left_s": None}),
    "refused": over(BASE, casters=[ONO_SETUP, st_caster(1, "RTK2go", "auth_failed", reason="auth", next_retry_s=240, **RTK2GO)],
                    availability={"budget_left_s": None}),
    "setup_rx": over(BASE, casters=[ONO_SETUP, st_caster(1, "RTK2go", "wait_data", **RTK2GO)], receiver=NO_RTCM,
                     alerts=[{"id": "no_rtcm", "source": "health"}], availability={"budget_left_s": None}),
    "onooff": over(BASE, casters=[ONO_OFF, RTK], availability={"budget_left_s": None}),
    "onooff_none": over(BASE, casters=[ONO_OFF, {"slot": 1, "name": "", "empty": True, "enabled": False, "state": "disabled"}],
                        availability={"budget_left_s": None}),
    "missed": over(BASE, casters=[st_caster(0, "Onocoy", "streaming", streak_ok=False, budget_left_s=0), RTK],
                   availability={"protected_down_today_s": 600, "budget_left_s": 0}),
    "stopped": over(BASE, casters=[st_caster(0, "Onocoy", "stopped", stop_reason="in_use"), RTK]),
    "stopped_banned": over(BASE, casters=[st_caster(0, "Onocoy", "stopped", stop_reason="in_use"),
                                          st_caster(1, "RTK2go", "banned", reason="banned", next_retry_s=3600, **RTK2GO)]),
    "partly": over(BASE, casters=[st_caster(0, "Onocoy", "streaming"),
                                  st_caster(1, "RTK2go", "auth_failed", reason="auth", next_retry_s=50, today_online_s=0,
                                            today_offline_s=20)]),
    "trial": over(BASE, network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin", "ssid": "Cabin",
                                 "rssi": -58}),
    "nonet": over(BASE, network={"upstream": "no_upstream"}, alerts=[{"id": "no_upstream", "source": "health"}],
                  casters=[st_caster(0, "Onocoy", "backoff", reason="dns", next_retry_s=20),
                           st_caster(1, "RTK2go", "backoff", reason="dns", next_retry_s=40, **RTK2GO)]),
    "settings": over(BASE, device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}],
                     casters=[st_caster(0, "Onocoy", "streaming"), st_caster(1, "RTK2go", "config_error", reason="config")]),
    "safe": over(BASE, safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle", "rtcm_age_ms": None},
                 casters=[{**st_caster(0, "Onocoy", "disabled"), **SAFE_SLOT, **NO_DAY},
                          {**st_caster(1, "RTK2go", "disabled"), **SAFE_SLOT}],
                 alerts=[{"id": "safe_mode", "source": "health"}], availability={"budget_left_s": None}),
    "update": over(BASE, update={"app_available": True, "app_version": "v0.4.0", "gnss_available": True, "gnss_build": 19152}),
    "rollback": over(BASE, update={"app_available": True, "app_version": "v0.4.0", "app_reverted_version": "v0.4.0",
                                   "app_reverted_in_stage1": True}),
    "devbuild": over(BASE, device={"version": "eb17ae3+dev", "dev_build": True}),
    "noclock": over(BASE, casters=[st_caster(0, "Onocoy", "streaming", budget_left_s=None), RTK],
                    availability={"budget_left_s": None}),
    "nocasters": over(BASE, casters=[{"slot": 0, "name": "", "empty": True, "enabled": False, "protected": False,
                                      "onocoy_ready": False, "state": "disabled"},
                                     {"slot": 1, "name": "", "empty": True, "enabled": False, "state": "disabled"}],
                      availability={"budget_left_s": None}),
    # The first seconds of a gap in the corrections: the station still calls the receiver streaming (up to 10 s).
    "rtcm7": over(BASE, receiver={"rtcm_age_ms": 7000}),
    # Settings cannot be saved (said first) while the receiver is missing or updating.
    "settings_norx": over(BASE, device={"settings_available": False}, receiver={"state": "no_receiver", "rtcm_age_ms": None},
                          alerts=[{"id": "settings_degraded", "source": "health"},
                                  {"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 23}],
                          casters=WAITING),
    "settings_flash": over(BASE, device={"settings_available": False}, receiver={"state": "flashing", "rtcm_age_ms": None},
                           alerts=[{"id": "settings_degraded", "source": "health"}], casters=WAITING),
    # No Onocoy slot, every caster turned off.
    "alloff": over(BASE, casters=[st_caster(0, "RTK2go", "disabled", onocoy_ready=False, **RTK2GO),
                                  st_caster(1, "Bench", "disabled")], availability={"budget_left_s": None}),
    # Onocoy not set up, RTK2go streaming, Bench refused.
    "setup_too": over(BASE, casters=[ONO_SETUP, RTK, st_caster(2, "Bench", "auth_failed", reason="auth", next_retry_s=90)],
                      availability={"budget_left_s": None}),
}
CARDS = ["Receiver", "Station", "Onocoy", "NTRIP"]


def dashboards(*statuses, admin: bool = True, open_slots: list | None = None) -> list[dict]:
    return run_editor([{"fn": "dashboard", "status": s, "admin": admin, "open": open_slots or []} for s in statuses])


def matrix() -> dict[str, dict]:
    return dict(zip(MATRIX, dashboards(*MATRIX.values())))


def rx_rows(o: dict) -> list[str]:
    return [v for _, v in o["cards"]["Receiver"]["rows"]]


def test_status_page_always_has_four_cards():
    """Whatever the state the admin sees the same four cards in the same places (never a card missing, never one
    taking the row): Receiver and Station, then Onocoy and NTRIP. Each has one link, in its head, whose name
    starts with its visible word, and no button: the top card has the page's buttons."""
    for name, o in matrix().items():
        assert o["order"] == CARDS, name
        assert o["grid"] == "grid", name
        for title in CARDS:
            card = o["cards"][title]
            assert not card["wide"], (name, title)
            assert card["buttons"] == 0, (name, title)
            assert len(card["links"]) == 1 and card["links"][0][3] is True, (name, title, card["links"])
            text, _, aria, _ = card["links"][0]
            assert aria.startswith(text), (name, title, card["links"])
        assert o["cards"]["Receiver"]["links"][0][:3] == ["Manage", "#/receiver", "Manage the receiver"]


def test_receiver_rows_in_every_state():
    """The Receiver card's four rows, in every state: dashes while the receiver's figures mean nothing; the two counts
    only while it delivers (so they never sit beside no corrections); the corrections' age; the position in a word."""
    out = matrix()
    for name, o in out.items():
        assert [r[0] for r in o["cards"]["Receiver"]["rows"]] == ["Satellites", "Bands", "Corrections", "Position"], name
    assert rx_rows(out["healthy"]) == ["28", "4 on Galileo", "Arriving", "Locked"]
    assert rx_rows(out["noant"]) == ["–", "–", "None yet", "Surveying, paused"]
    for name in ("norx", "uart", "safe", "journal", "updating", "starting"):
        assert rx_rows(out[name]) == ["–"] * 4, name
    # Settings cannot be saved, said first: a missing or updating receiver still dashes every row.
    for name in ("settings_norx", "settings_flash"):
        assert out[name]["key"] == "settings" and rx_rows(out[name]) == ["–"] * 4, name
    # A gap shorter than the station's 10 s: the receiver still streams, the corrections still count as arriving, as
    # the top card says (one freshness rule for the row and the counts).
    assert out["rtcm7"]["key"] == "streaming" and rx_rows(out["rtcm7"]) == ["28", "4 on Galileo", "Arriving", "Locked"]
    (gap,) = dashboards(over(BASE, receiver={"rtcm_age_ms": 12000, "state": "recovering"}, health={"level": "recovering"},
                             casters=WAITING))
    assert rx_rows(gap)[:3] == ["–", "–", "None for 12\u00a0s"]
    assert rx_rows(out["maint"]) == ["28", "4 on Galileo", "Arriving", "Locked"]
    assert rx_rows(out["maint_unchecked"]) == ["28", "4 on Galileo", "Arriving", "Not checked"]
    assert rx_rows(out["survey"]) == ["28", "4 on Galileo", "Arriving", "Surveying, 5 min left"]
    # The top card says the fixed position looks wrong: the row agrees in amber, without its sentence.
    assert out["fixed_far"]["head"] == "Fixed position looks wrong" and rx_rows(out["fixed_far"])[3] == "Fixed, check it"
    assert out["fixed_far"]["cards"]["Receiver"]["tones"] == [["warn", "Fixed, check it"]]
    # A stall: the fixture still has satellites, but nothing arrives.
    assert rx_rows(out["stall"])[:3] == ["–", "–", "None for 6 s"]
    assert rx_rows(out["configuring"])[:3] == ["–", "–", "Arriving"]
    assert rx_rows(out["held"])[:3] == ["–", "–", "None yet"]
    # An older firmware without the count: a dash.
    rx = dict(BASE["receiver"])
    del rx["satellites"]
    (old,) = dashboards({**BASE, "receiver": rx})
    assert rx_rows(old) == ["–", "4 on Galileo", "Arriving", "Locked"]


def test_bands_row_says_onocoys_need_once():
    """The Bands row marks too few bands while Onocoy counts, and says what Onocoy needs only when nothing else on the
    page says it (the top card's line, Onocoy's verdict line, the bands "Also:" line); without Onocoy it is plain."""
    few = MATRIX["quality"]
    few_no_alert = over(BASE, receiver=FEW)
    refused = over(BASE, receiver={"rtcm1230_supported": False})
    why_says = MATRIX["quality_down"]
    why_other = over(BASE, receiver=FEW, casters=[st_caster(0, "Onocoy", "backoff", reason="timeout"), RTK])
    no_gl = MATRIX["noglonass"]
    off = over(BASE, receiver=FEW, casters=[ONO_OFF, RTK])
    alert = MATRIX["quality_down_alert"]
    out = dashboards(few, few_no_alert, refused, why_says, why_other, no_gl, off, alert)
    bands = [dict(o["cards"]["Receiver"]["rows"])["Bands"] for o in out]
    tones = [o["cards"]["Receiver"]["tones"] for o in out]
    assert out[0]["head"] == "Too few bands for Onocoy" and bands[0] == "3 on GPS ✕" and tones[0] == [["warn", bands[0]]]
    assert bands[1] == "3 on GPS ✕"
    assert out[2]["key"] == "quality" and bands[2] == "4 on Galileo" and tones[2] == []
    # Onocoy's verdict line says it (another state on top).
    assert out[3]["key"] == "casters_down" and out[3]["cards"]["Onocoy"]["notes"][0][0].startswith("Only 3 bands")
    assert bands[3] == "3 on GPS ✕"
    assert out[4]["cards"]["Onocoy"]["notes"][0][0] == "Offline time is counting now."
    assert bands[4] == "3 on GPS ✕ · Onocoy needs 4"
    assert bands[5] == "4 on Galileo · no GLONASS" and tones[5] == [["warn", " · no GLONASS"]]
    assert out[6]["key"] == "streaming" and bands[6] == "3 on GPS" and tones[6] == []
    # The bands "Also:" line says it: neither the row nor Onocoy's verdict line again (At risk stays).
    assert "bands" in [a[0] for a in out[7]["also"]] and bands[7] == "3 on GPS ✕"
    assert out[7]["cards"]["Onocoy"]["chip"] == ["At risk", "badge warn"]
    assert not any(n[0].startswith("Only 3 bands") for n in out[7]["cards"]["Onocoy"]["notes"])


def test_casters_card_lists_counting_onocoy_first():
    """One line per caster set up: Onocoy first while it counts (no share: its own card has its day), left out when it
    is not set up or turned off; today's share for the others that are on. "No other servers." beside an Onocoy that is
    set up, else "No servers yet.", with Add (Manage when the top card's button adds one); when it shows in safe mode.
    The public form: names and states, no share, no link, and no card while it lists none."""
    swapped = over(BASE, casters=[st_caster(0, "RTK2go", "streaming", protected=False, onocoy_ready=False, **RTK2GO),
                                  st_caster(2, "Onocoy", "streaming"),
                                  st_caster(1, "Spare", "disabled", today_online_s=50)])
    no_slots = over(BASE, casters=[])
    public = {"api": 1, "public": True, "device": {"hostname": "onocoydiy"}, "safe_mode": {"active": False},
              "receiver": {"state": "streaming", "best_bands": 4},
              "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}, {"slot": 1, "name": "Bench", "state": "disabled"}],
              "health": {"level": "ok"}}
    public_safe = {"api": 1, "public": True, "device": {"hostname": "onocoydiy"}, "safe_mode": {"active": True},
                   "receiver": {"state": "idle", "best_bands": 0}, "casters": [], "health": {"level": "failed"}}
    names = ["healthy", "setup", "onooff", "onooff_none", "nocasters", "safe", "partly"]
    out = dashboards(*[MATRIX[n] for n in names], swapped, no_slots)
    pub = dashboards(public, public_safe, {**public, "casters": []}, admin=False)
    cas = {n: o["cards"]["NTRIP"] for n, o in zip(names + ["swapped", "no_slots"], out)}
    assert cas["healthy"]["lines"] == ["Onocoy · Streaming", "RTK2go · Streaming · 99.97 % today"]
    # Each separator stays with the part after it: a phone breaks before a separator, never after it.
    assert [" ".join(p.split()) for p in cas["healthy"]["nowrap"]] == ["· Streaming", "· Streaming", "· 99.97 % today"]
    assert cas["swapped"]["lines"] == ["Onocoy · Streaming", "RTK2go · Streaming · 99.97 % today",
                                       "Spare · Off"]
    assert cas["setup"]["lines"] == cas["onooff"]["lines"] == ["RTK2go · Streaming · 99.97 % today"]
    assert cas["partly"]["lines"][1] == "RTK2go · Login refused · 0 % today"
    assert cas["onooff_none"]["lines"] == [] and cas["onooff_none"]["notes"] == [["No other servers.", "muted"]]
    for n in ("nocasters", "no_slots"):
        assert cas[n]["lines"] == [] and cas[n]["notes"] == [["No servers yet.", "muted"]], n
    (unset,) = dashboards(over(BASE, casters=[ONO_SETUP], availability={"budget_left_s": None}))
    assert unset["cards"]["NTRIP"]["notes"] == [["No servers yet.", "muted"]]
    assert cas["onooff_none"]["links"][0][:3] == ["Add", "#/ntrip", "Add a server"]
    # No Onocoy slot either: the top card's button adds a caster, so the card's link is Manage.
    assert out[4]["actions"] == [["Add a server", "#/ntrip"]] and cas["nocasters"]["links"][0][0] == "Manage"
    # The top card's button adds a caster: the card's link is Manage.
    assert out[-1]["actions"] == [["Add a server", "#/ntrip"]] and cas["no_slots"]["links"][0][:3] == [
        "Manage", "#/ntrip", "Manage NTRIP servers"]
    assert cas["healthy"]["links"][0][:3] == ["Manage", "#/ntrip", "Manage NTRIP servers"]
    assert cas["safe"]["lines"] == [] and cas["safe"]["notes"] == [["Shows after safe mode.", "muted"]]
    assert pub[0]["cards"]["NTRIP"]["lines"] == ["Onocoy · Streaming", "Bench · Off"]
    assert pub[0]["cards"]["NTRIP"]["links"] == [] and pub[0]["cards"]["NTRIP"]["wide"]
    assert pub[1]["head"] == "Safe mode" and pub[1]["line"] == "The receiver and NTRIP servers are off."
    # Nothing listed (safe mode, none set up): no card; the top card says it.
    assert pub[1]["cards"] == {} and pub[1]["grid"] is None
    assert pub[2]["cards"] == {} and pub[2]["line"] == "The station is not streaming to any NTRIP server."


def test_station_card_rows():
    """Wi-Fi, how long the station runs and its firmware, in a few words; Update while the Updates page has something
    to do, unless the top card's button goes there."""
    net = lambda **kw: over(BASE, network=kw)
    upd = lambda **kw: over(BASE, update=kw)
    cases = [BASE, net(ssid=None, online=False, rssi=None), net(online=False, rssi=-70), net(online=False, rssi=None),
             net(rssi=None), MATRIX["trial"],
             upd(app_available=True, app_version="v0.4.0"), upd(gnss_available=True, gnss_build=19152),
             MATRIX["update"], MATRIX["rollback"],
             upd(app_available=True, app_version="v0.4.0", app_reverted_version="v0.4.0", app_reverted_in_stage1=True,
                 gnss_available=True),
             MATRIX["devbuild"], upd(app_version="v0.4.0"), upd(app_version=None), MATRIX["safe"],
             over(BASE, update={"app_available": True, "app_version": "v0.4.0"}, safe_mode={"active": True, "crashes": 3},
                  receiver={"state": "idle"})]
    out = dashboards(*cases)
    rows = [dict(o["cards"]["Station"]["rows"]) for o in out]
    links = [o["cards"]["Station"]["links"][0][:3] for o in out]
    wifi = [" ".join(r["Wi-Fi"].split()) for r in rows[:6]]
    # The signal in a word with its figure (a word alone means little), kept with its separator; never a bare word
    # without a reading. The network's name ends in an ellipsis where the line would wrap (app.css .wifi-line).
    assert wifi == ["Home · Good (\u221261 dBm)", "Not set up", "Home · no address yet", "Home · Offline", "Home",
                    "Good (\u221258 dBm)"]
    assert [" ".join(t.split()) for t in out[0]["cards"]["Station"]["nowrap"]] == ["· Good (\u221261 dBm)", "Good (\u221261 dBm)",
                                                                                  "· up to date"]
    # The firmware's parts too: a phone breaks before a separator.
    assert [" ".join(t.split()) for t in out[8]["cards"]["Station"]["nowrap"]][-2:] == ["· v0.4.0 available", "· receiver update"]
    assert rows[0]["Running for"] == "1 d"
    fw = [r["Firmware"] for r in rows[6:15]]
    assert rows[0]["Firmware"] == "v0.3.0 · up to date" and links[0] == ["Manage", "#/settings", "Manage station settings"]
    assert fw == ["v0.3.0 · v0.4.0 available", "v0.3.0 · receiver update available",
                  "v0.3.0 · v0.4.0 available · receiver update", "v0.3.0 · v0.4.0 rolled back",
                  "v0.3.0 · v0.4.0 rolled back · receiver update available",
                  "eb17ae3+dev", "v0.3.0", "v0.3.0", "v0.3.0 \u00b7 up to date"]
    assert all(lk == ["Update", "#/settings/updates", "Update the firmware"] for lk in links[6:11]), links
    assert links[11:15] == [["Manage", "#/settings", "Manage station settings"]] * 4
    # Safe mode: the top card's Updates button goes there already.
    assert out[15]["actions"][0] == ["Updates", "#/settings/updates"] and links[15][0] == "Manage"
    assert all(o["cards"]["Station"]["tones"] == [] for o in out)


def test_onocoy_today_forms():
    """Onocoy's day while it counts; otherwise one muted line on when the card shows (never a lone dash), with the link
    that sets Onocoy up or turns it on, Manage when the top card's button does that; in safe mode when it shows. Missed
    is amber, says it once: the chip, one line with the reset, the two rows (no figure, no bar), never red."""
    names = ["healthy", "setup", "onooff_none", "setup_rx", "onooff", "safe", "noclock", "missed", "nocasters", "refused",
             "alloff"]
    out = dict(zip(names, dashboards(*[MATRIX[n] for n in names])))
    ono = {n: o["cards"]["Onocoy"] for n, o in out.items()}
    assert ono["healthy"]["chip"] == ["On track", "badge ok"] and ono["healthy"]["big"][0].startswith("7 min 43 s")
    assert [r[0] for r in ono["healthy"]["rows"]] == ["Online today", "Offline today"]
    assert ono["healthy"]["links"][0][:3] == ["Manage", "#/ntrip", "Manage Onocoy"]
    for o in ono.values():
        assert all(n[0] not in ("", "–") for n in o["notes"]), o["notes"]
    shows = {n: o["notes"] for n, o in ono.items() if o["chip"] is None}
    assert shows == {"setup": [["Shows once Onocoy is set up.", "muted"]], "onooff_none": [["Shows while Onocoy is on.", "muted"]],
                     "setup_rx": [["Shows once Onocoy is set up.", "muted"]], "onooff": [["Shows while Onocoy is on.", "muted"]],
                     "safe": [["Shows after safe mode.", "muted"]], "nocasters": [["Shows once Onocoy is set up.", "muted"]],
                     "refused": [["Shows once Onocoy is set up.", "muted"]], "alloff": [["Shows once Onocoy is set up.", "muted"]]}
    assert all(ono[n]["rows"] == [] for n in shows)
    # The top card's button sets Onocoy up or turns it on: the card's link is Manage.
    assert out["setup"]["key"] == "onocoy_setup" and out["onooff_none"]["key"] == "no_caster"
    for n in ("setup", "onooff_none", "safe"):
        assert ono[n]["links"][0][:3] == ["Manage", "#/ntrip", "Manage Onocoy"], n
    assert out["setup_rx"]["key"] == "no_rtcm"
    for n in ("setup_rx", "nocasters", "refused", "alloff"):
        assert ono[n]["links"][0][:3] == ["Set up", "#/ntrip/onocoy", "Set up Onocoy"], n
    assert out["onooff"]["key"] == "streaming"
    assert ono["onooff"]["links"][0][:3] == ["Turn on", "#/ntrip/onocoy", "Turn on Onocoy"]
    assert ono["noclock"]["chip"] == ["On track", "badge ok"] and ono["noclock"]["big"] is None and ono["noclock"]["bar"] is None
    assert ono["noclock"]["notes"] == [["Shows once the station has the time (from GNSS or the internet).", "muted"]]
    assert [r[0] for r in ono["noclock"]["rows"]] == ["Online today", "Offline today"]
    m = ono["missed"]
    assert m["chip"] == ["Missed", "badge warn"] and len(m["notes"]) == 1 and m["notes"][0][1] == "small t-warn"
    assert re.fullmatch(r"Today's offline time is over the budget\. Resets at \d{1,2}:\d\d\s?([AP]M|[ap]\.m)\.", m["notes"][0][0]), m["notes"]
    assert m["big"] is None and m["bar"] is None and [r[0] for r in m["rows"]] == ["Online today", "Offline today"]
    assert m["tones"] == [["warn", m["notes"][0][0]]]
    # Missed with time still on the budget: no figure either.
    (still,) = dashboards(over(BASE, casters=[st_caster(0, "Onocoy", "streaming", streak_ok=False, budget_left_s=200), RTK],
                               availability={"protected_down_today_s": 10, "budget_left_s": 200}))
    assert still["cards"]["Onocoy"]["big"] is None and still["cards"]["Onocoy"]["bar"] is None


def test_card_colours_never_contradict_the_top_card():
    """No card is greener than the top card about the same thing: Receiver and Station are never green, the Receiver
    is amber only for Onocoy's quality needs and a fixed position the station finds far off; a caster's line is red or
    amber only for a caster the top card counts or lists in an "Also:" line, or for a wait under a top card that is not
    green, and neutral for one its line names (no second coloured statement); Onocoy's chip is green On track or amber
    At risk or Missed, never red, and never green while Onocoy does not stream."""
    for name, o in matrix().items():
        s = MATRIX[name]
        assert o["cards"]["Station"]["tones"] == [] and o["cards"]["Station"]["chip"] is None, name
        rx = o["cards"]["Receiver"]["tones"]
        assert all(t == "warn" for t, _ in rx), (name, rx)
        far = [t for t in rx if t[1] == "Fixed, check it"]
        assert not far or any(a["id"] == "fixed_far" for a in s["alerts"]), (name, rx)
        assert len(far) == len(rx) or o["cards"]["Onocoy"]["chip"] is not None, (name, rx)
        chip = o["cards"]["Onocoy"]["chip"]
        if chip:
            assert chip in (["On track", "badge ok"], ["At risk", "badge warn"], ["Missed", "badge warn"]), (name, chip)
            ono = next(c for c in s["casters"] if c.get("protected"))
            if chip[0] == "On track":
                assert ono["state"] == "streaming" and o["key"] not in ("rx_stopped", "no_rtcm", "no_satellites"), name
        also = {a[0] for a in o["also"]}
        by_name = {c["name"]: c for c in s["casters"] if not c.get("empty")}
        for line, cls in zip(o["cards"]["NTRIP"]["lines"], o["cards"]["NTRIP"]["lineTones"]):
            c = by_name[line.split(" · ")[0]]
            if c["slot"] in o["named"]:
                assert cls == "muted", (name, line, cls)
            if cls not in ("t-warn", "t-bad"):
                continue
            named = o["key"] == "casters_down" or c["slot"] == o["subject"] or "caster:%d" % c["slot"] in also
            waiting = c["state"] in ("wait_data", "wait_network", "connecting") and o["tone"] != "ok"
            assert named or waiting, (name, line, cls)
        if o["tone"] == "ok":
            assert all(c["state"] == "streaming" for c in s["casters"] if c.get("enabled") and c["state"] != "disabled"), name


def test_status_page_says_each_fact_once():
    """Each fact once: no top-card title or line inside a card, and no three words of a card in the top card's title,
    line or "Also:" lines; no facts under the top card at all (the NTRIP card lists the servers); the duplicate stream an
    "Also:" line; Onocoy not set up or off said once (the Onocoy card's line says when it shows, not that)."""
    out = matrix()
    norm = lambda t: re.sub(r"[^a-z0-9 ]", "", t.lower().replace(" ", " ").replace("·", " ")).split()
    for name, o in out.items():
        top = " " + " ".join(norm(" ".join([o["head"], o["line"]] + [a[1] + " " + a[2] for a in o["also"]]))) + " "
        for title, card in o["cards"].items():
            # A one-word title ("Streaming") is a caster's state word too; longer ones are never in a card.
            assert len(norm(o["head"])) < 2 or o["head"] not in card["text"], (name, title)
            assert not o["line"] or o["line"] not in card["text"], (name, title)
            # A caster line's state word is the top card's sentence in a word ("Waiting for the receiver" beside "The station
            # is listening for the receiver"): its name and share count, not the word.
            lines = [p for ln in card["lines"] for i, p in enumerate(ln.split(" \u00b7 ")) if i != 1]
            atoms = [v for r in card["rows"] for v in r] + lines + [n[0] for n in card["notes"]]
            atoms += [card["big"][0]] if card["big"] else []
            for atom in atoms:
                w = norm(atom)
                for i in range(len(w) - 2):
                    assert " " + " ".join(w[i:i + 3]) + " " not in top, (name, title, atom)
        assert o["facts"] == [], name
        # Onocoy off or not set up: said at most once, by the top card, never by an NTRIP line; the Onocoy card says
        # when it shows, in every such state.
        if name in ("setup", "refused", "setup_rx", "onooff", "onooff_none", "nocasters", "alloff", "setup_too"):
            ono = o["cards"]["Onocoy"]["notes"]
            shows = [n[0] for n in ono if n[0] in ("Shows once Onocoy is set up.", "Shows while Onocoy is on.")]
            said = [t for t in [o["head"], o["line"]] + [n[0] for c in o["cards"].values() for n in c["notes"]] +
                    o["cards"]["NTRIP"]["lines"]
                    if t not in shows and "Onocoy" in t and
                    re.search(r"not set up|turned off|\bOff\b|Onocoy is set up|Onocoy is on|Onocoy on", t)]
            assert len(said) <= 1 and len(shows) == 1 and ono == [[shows[0], "muted"]], (name, said, ono)
            assert not any("Onocoy" in ln for ln in o["cards"]["NTRIP"]["lines"]), name
    (dup,) = dashboards(BASE, open_slots=[1])
    assert dup["also"] == [["dup_stream", "Onocoy and RTK2go are both on", "A duplicate stream can get the Onocoy account banned.",
                            "warn"]]
    assert dup["heroAlsoShown"] == [["warn", "Also: Onocoy and RTK2go are both on. A duplicate stream can get the Onocoy "
                                             "account banned."]]
    assert "duplicate" not in dup["cards"]["NTRIP"]["text"].lower()
    # Three servers down: the line counts them, no fact counts them again (the NTRIP card lists each).
    (three,) = dashboards(over(BASE, casters=[st_caster(0, "Onocoy", "connecting"), st_caster(1, "RTK2go", "backoff", reason="dns"),
                                              st_caster(2, "Bench", "wait_network")]))
    assert three["line"] == "3 servers are not streaming." and three["facts"] == []


def test_top_card_names_every_failing_caster():
    """A caster on that a retry or the user must clear, which the top card's line does not name: an "Also:" line with
    why and the next try (the NTRIP card shows its word only). Not for a retry the no-internet alert explains, a
    wait, or while the line names or counts the casters down. Onocoy not set up while the casters that are on all
    fail: the failure is the top card (red, naming the caster and why), and the Onocoy card offers the setup."""
    backoff_nonet = over(BASE, alerts=[{"id": "no_upstream", "source": "health"}], receiver={"rtcm_age_ms": 30000},
                         health={"level": "recovering", "last_step": "link_restart"},
                         casters=[st_caster(0, "Onocoy", "wait_data"), st_caster(1, "RTK2go", "backoff", reason="dns")])
    waits = over(BASE, receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"},
                 casters=[st_caster(0, "Onocoy", "wait_data"), st_caster(1, "RTK2go", "wait_network")])
    refused, banned, partly, settings, too = dashboards(MATRIX["refused"], MATRIX["stopped_banned"], MATRIX["partly"],
                                                        MATRIX["settings"], MATRIX["setup_too"])
    nonet, wait = dashboards(backoff_nonet, waits)
    assert (refused["key"], refused["head"], refused["tone"]) == ("casters_down", "Not streaming", "bad")
    assert refused["line"] == "RTK2go is not streaming: login refused. Next try in 4\xa0min." and refused["subject"] == 1
    assert not any(a[0].startswith("caster:") for a in refused["also"]) and refused["heroButtons"] == []
    # The line names RTK2go: its row's word is neutral, not a second red statement.
    assert refused["cards"]["NTRIP"]["lineTones"] == ["muted"] and refused["cards"]["NTRIP"]["tones"] == []
    assert refused["cards"]["Onocoy"]["links"][0][:2] == ["Set up", "#/ntrip/onocoy"]
    # Something streams: Onocoy not set up stays on top, "too" is true, and the refused caster gets its "Also:" line.
    assert too["key"] == "onocoy_setup" and too["line"].endswith(" to stream to Onocoy too.")
    assert ["caster:2", "Bench not streaming", "Next try in 1\xa0min 30\xa0s.", "bad"] in too["also"]
    assert too["cards"]["NTRIP"]["tones"] == [["ok", "Streaming"], ["bad", "Login refused"]]
    assert banned["key"] == "stopped" and [a[0] for a in banned["also"]] == ["caster:1"]
    assert banned["also"][0][3] == "bad"
    assert settings["key"] == "settings" and "caster:1" in [a[0] for a in settings["also"]]
    assert not any(a[0].startswith("caster:") for a in nonet["also"]), nonet["also"]
    assert not any(a[0].startswith("caster:") for a in wait["also"]), wait["also"]
    assert partly["key"] == "casters_down" and not any(a[0].startswith("caster:") for a in partly["also"])


def test_closed_port_panel_says_so_without_cable_advice():
    """A closed receiver port: the panel says so, without the cable advice and without "Check again" (the station
    keeps reopening its port)."""
    out = run_editor([
        {"fn": "norx", "status": {"receiver": {"state": "no_receiver", "uart_closed": True}, "alerts": []}},
        {"fn": "norx", "status": {"receiver": {"state": "no_receiver", "next_detect_in_s": 90},
                                  "alerts": [{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 90}]}}])
    assert out[0] == ["Receiver port closed", "The station cannot open its port to the receiver and keeps trying.", False]
    assert out[1][0] == "No receiver found" and "cable" in out[1][1] and out[1][2] is True


def test_status_page_leaves_receiver_management_to_receiver_settings():
    """How the receiver is managed is set and said on Receiver settings only: the top card has no fact or Change link
    for it, whatever the choice."""
    me = st_status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                   alerts=[{"id": "no_rtcm", "source": "health"}], casters=[st_caster(0, "Onocoy", "wait_data")])
    out = run_editor([{"fn": "dashboard", "status": x, "admin": True}
                      for x in (me, st_status(health={"level": "ok", "recovery_mode": "no_reboot"}), st_status())])
    assert out[0]["head"] == "Receiver sends no corrections"
    for o in out:
        assert o["facts"] == [] and o["factLinks"] == [], o
        assert "management" not in json.dumps(o["heroAlsoShown"] + [o["head"], o["line"], o["plan"]]), o


def test_header_shows_the_station_state_and_says_when_contact_is_lost():
    """The header's indicator: the Status page's state in a word, its dot in the top card's colour (the link's
    class), the full title as tooltip (on the box inside the link, so the link's name is not read again as its
    description) and in the name (after the word when the title does not start with it), a link to the Status page; a
    visitor sees the public form's. Without a status for 6 s: "No contact", grey, and the running count beside the word,
    outside its live region and hidden from screen readers, and the page is marked so (the Status page's top card goes
    grey too); "Connecting…" before the first. A second render keeps the elements (the live region announces a change
    once). It shows on the Status page too. Nothing before the app runs (login, setup)."""
    me = st_status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                   alerts=[{"id": "no_rtcm", "source": "health"}], casters=[st_caster(0, "Onocoy", "wait_data")])
    public = {"api": 1, "public": True, "device": {"hostname": "onocoydiy"}, "receiver": {"state": "detecting"},
              "casters": [{"slot": 0, "name": "Onocoy", "state": "wait_data"}], "health": {"level": "ok"}}
    out = run_editor([{"fn": "header", "status": me, "age": 1, "admin": True},
                      {"fn": "header", "status": st_status(), "age": 0, "admin": True},
                      {"fn": "header", "status": public, "age": 2, "admin": False},
                      {"fn": "header", "status": st_status(), "age": 9, "admin": True},
                      {"fn": "header", "status": st_status(), "age": 100, "admin": True},
                      {"fn": "header", "status": None, "age": None, "admin": True},
                      {"fn": "header", "status": st_status(), "age": 1, "admin": True, "started": False}] +
                     [{"fn": "header", "status": st_status(), "age": a, "admin": True} for a in (59, 3599, 9000, 3 * 86400 + 5)] +
                     [{"fn": "header", "status": me, "age": 1, "admin": True, "page": ""},
                      {"fn": "header", "status": public, "age": 2, "admin": False, "page": ""},
                      {"fn": "header", "status": me, "age": 9, "admin": True, "page": ""},
                      {"fn": "header", "status": None, "age": None, "admin": True, "page": ""}])
    shown = [False, False, False]   # not hidden, no title on the link itself, the page not marked as without contact
    assert out[0] == ["bad", "Not streaming", "status", "", "true", "Receiver sends no corrections",
                      "Not streaming: Receiver sends no corrections", "#/", True, *shown]
    assert out[1] == ["ok", "Streaming", "status", "", "true", "Streaming", "Streaming", "#/", True, *shown]
    assert out[2] == ["info", "Searching", "status", "", "true", "Looking for the receiver",
                      "Searching: Looking for the receiver", "#/", True, *shown]
    assert out[3] == ["", "No contact", "status", "9\u00a0s", "true", "No contact with the station for 9\u00a0s",
                      "No contact with the station", "#/", True, False, False, True]
    # Past a minute the count is its largest unit (it fits a phone's header); the title keeps the whole time.
    assert out[4][1] == "No contact" and out[4][3] == "1\u00a0min" and out[4][5].endswith("1\u00a0min 40\u00a0s")
    assert [o[3] for o in out[7:11]] == ["59\u00a0s", "59\u00a0min", "2\u00a0h", "3\u00a0d"]
    assert out[5] == ["", "Connecting…", "status", "", "true", "Connecting to the station", "Connecting to the station", "#/",
                      True, *shown]
    assert out[6] is None
    # The Status page (the admin's and a visitor's): the same as on every other page.
    assert out[11:] == [out[0], out[2], out[3], out[5]]


def test_no_contact_greys_every_colour_on_the_status_page():
    """Without contact (body.no-contact) the Status page shows the last state it had, so none of its colours may still
    claim it: its root is .status-page, and one rule turns every colour class the page draws (in every state, the
    admin's and a visitor's) to the grey of the header's dot: the tones (chips, bars, "Also:" marks, through --tone),
    the tone words and figures, the signals' ticks and C/N0."""
    css = re.sub(r"/\*.*?\*/", "", (UI_JS.parent / "app.css").read_text(), flags=re.S)
    rules = re.findall(r"(body\.no-contact \.status-page[^{]*)\{([^}]*)\}", css)
    covered = {c for sel, _ in rules for c in re.findall(r"\.([\w-]+)", sel)} - {"no-contact", "status-page"}
    decl = " ".join(d for _, d in rules)
    assert "--tone: var(--off)" in decl and "--tone-soft: var(--off-soft)" in decl and "color: var(--off)" in decl
    assert {"ok", "warn", "bad", "info", "t-ok", "t-warn", "t-bad", "cnr-weak", "cnr-ok", "cnr-good", "on"} <= covered
    assert ".status-page .bands td.on" in " ".join(sel for sel, _ in rules)
    assert "el.classList.add('status-page');" in (UI_JS / "40-dashboard.js").read_text()
    public = {"api": 1, "public": True, "device": {"hostname": "onocoydiy"}, "safe_mode": {"active": False},
              "receiver": {"state": "streaming", "best_bands": 4},
              "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}, {"slot": 1, "name": "Bench", "state": "backoff"}],
              "health": {"level": "ok"}}
    drawn = {k for o in list(matrix().values()) + dashboards(public, admin=False) for k in o["toneClasses"]}
    assert {"ok", "warn", "bad", "t-ok", "t-warn", "t-bad"} <= drawn
    assert drawn <= covered, drawn - covered


def test_top_card_buttons_never_repeat_a_cards_link():
    """A top-card button named after a page (Receiver, NTRIP) goes where a card's link in its head goes already: the
    page leaves it out. Real actions stay (Set up Onocoy, What to do, Check again, End now, Leave safe mode)."""
    out = matrix()
    for name, o in out.items():
        heads = {lk[1] for c in o["cards"].values() for lk in c["links"] if lk[3]}
        for label, href in o["heroButtons"]:
            assert not (label in ("Receiver", "NTRIP") and href in heads), (name, label, href)
    for name in ("quality", "partly", "nonet", "alloff", "refused"):
        assert out[name]["heroButtons"] == [], (name, out[name]["actions"], out[name]["heroButtons"])
    assert out["quality"]["actions"] == [["Receiver", "#/receiver"]]   # Logs' Overview keeps it (no cards there)
    assert out["setup"]["heroButtons"] == [["Set up Onocoy", "#/ntrip/onocoy"]]
    assert out["stopped"]["heroButtons"] == [["What to do", "#/ntrip"]]
    assert out["norx"]["heroButtons"] == [["Check again", None]]
    assert out["maint"]["heroButtons"] == [["End now", None]]
    assert out["safe"]["heroButtons"] == [["Updates", "#/settings/updates"], ["Leave safe mode", None]]
    assert out["onooff_none"]["heroButtons"] == [["Turn Onocoy on", "#/ntrip/onocoy"]]
    assert out["nocasters"]["heroButtons"] == [["Add a server", "#/ntrip"]]
def test_setup_onocoy_step_sends_the_duplicate_stream_answer():
    """The setup's Onocoy step while RTK2go is on: the station asks about the duplicate stream; ticking the box (its
    input event reaches the form) keeps the question with "Save anyway" enabled, and that sends the answer."""
    (r,) = run_editor([{"fn": "wizard_dup"}])
    assert r == {"asked": True, "kept": True, "enabled": True, "puts": [False, True]}, r


def test_receiver_setup_saves_its_two_settings_in_a_safe_order():
    """The station stores automatic setup and the recovery one after the other, so the choice sends one per request: the
    recovery first when automatic setup goes off, else automatic setup first. A refusal halfway never leaves automatic
    recovery on for a receiver the station does not configure. Automatic without its restart box is no_reboot."""
    out = run_editor([{"fn": "manage", "autocfg": True, "recovery": "full", "to": "manual"},
                      {"fn": "manage", "autocfg": False, "recovery": "alerts_only", "to": "auto"},
                      {"fn": "manage", "autocfg": True, "recovery": "full", "to": "no_restart"},
                      {"fn": "manage", "autocfg": True, "recovery": "full", "to": "manual", "fail": "autocfg"},
                      {"fn": "manage", "autocfg": False, "recovery": "alerts_only", "to": "auto", "fail": "recovery"}])
    assert out[0] == [{"recovery": "alerts_only"}, {"autocfg": False}]
    assert out[1] == [{"autocfg": True}, {"recovery": "full"}]
    assert out[2] == [{"recovery": "no_reboot"}]
    # Refused halfway: the recovery is already off (to Manual), automatic setup already on (to Automatic).
    assert out[3] == [{"recovery": "alerts_only"}, {"autocfg": False}]
    assert out[4] == [{"autocfg": True}, {"recovery": "full"}]


def journal_clear(status: int, body: dict) -> dict:
    return run_editor([{"fn": "journal_clear", "answer": [status, body]}])[0]


def internal(message: str) -> dict:
    return {"error": {"code": "internal", "message": message, "field": None, "retry_in_s": None, "by": None}}


def test_clear_journal_waits_for_the_erases_and_says_what_a_flash_error_left():
    # Cleared: the toast, the list read again; the request may take a minute (every sector is erased).
    r = journal_clear(200, {"ok": True})
    assert r["shown"] and r["deletes"] == 1 and r["timeout"] == 60000
    assert r["toasts"] == [["Event journal cleared.", "toast ok"]] and r["reread"] and r["error"] is None
    # The clear could not start: not cleared (a full journal may lose its oldest sector, as when it moves on), the
    # list stays as it is.
    r = journal_clear(500, internal("ESP_FAIL"))
    assert r["toasts"] == [] and not r["reread"]
    assert r["error"] == "Not cleared: the journal flash could not be written."
    # Cleared, but a sector could not be erased: its events may come back at the next start.
    r = journal_clear(500, internal("ESP_ERR_NOT_FINISHED"))
    assert r["toasts"] == [["Event journal cleared, but a flash error may bring some events back after a restart.",
                            "toast warn"]]
    assert r["reread"] and r["error"] is None
    # Another clear waits or runs, or an upload holds the station's worker: said so, nothing read again.
    busy = {"error": {"code": "busy", "message": "busy", "field": None, "retry_in_s": None, "by": None}}
    r = journal_clear(409, busy)
    assert r["toasts"] == [] and not r["reread"]
    assert r["error"] == "Another clear or an upload is running. Try again when it ends."
    # No usable journal (404): read again, the card says the partition is unusable and the button goes.
    gone = {"error": {"code": "not_found", "message": "no journal", "field": None, "retry_in_s": None, "by": None}}
    (r,) = run_editor([{"fn": "journal_clear", "answer": [404, gone], "after": {"available": False}}])
    assert r["reread"] and r["error"] is None and r["toasts"] == [] and not r["wipe"]
    assert r["lines"] == ["The journal partition is missing or unusable: events are not recorded."]
    # No answer within the minute: the clear runs on in the station, so the page says to look again, and Refresh
    # reads the journal from its start once it is cleared (not only what came after the records shown).
    (r,) = run_editor([{"fn": "journal_clear", "answer": "abort", "refresh": True,
                        "after": {"available": True, "oldest_seq": 0, "newest_seq": 0}}])
    assert r["error"] == "No answer within a minute. Press Refresh in a moment to see whether it cleared."
    assert r["errorTone"] == "msg warn" and r["toasts"] == [] and not r["reread"]
    assert r["rows"] == 0 and "No events recorded." in r["lines"]
    # The same Refresh with the journal as it was: the rows stay.
    (r,) = run_editor([{"fn": "journal_clear", "answer": [409, busy], "refresh": True}])
    assert r["rows"] == r["rowsBefore"] > 0 and "No events recorded." not in r["lines"]


def test_event_filter_changed_while_the_journal_loads():
    """A filter picked before the first read has answered pages from nothing: no read with a cursor that is not a
    number (the station refuses it), and the list comes when the first read does."""
    (r,) = run_editor([{"fn": "event_filter_early"}])
    assert r["reads"] and all(p.split("cursor=")[1].isdigit() for p in r["reads"]), r
    assert r["rows"] > 0


# ---- Status: the Onocoy card's bar; Logs: the Health card and Chip and Wi-Fi; Signals; long values; plain port ----

def test_onocoy_today_bar_is_the_share_of_the_allowance_left():
    """The bar under the offline time left fills with what is left of today's allowance, left / (left + offline
    counted against it): nearly full on a clean day, partly full at risk, empty once missed. Its colour is the
    verdict's; assistive technology reads it as a meter with the share left in words."""
    ono = lambda **kw: [st_caster(0, "Onocoy", "streaming", **kw)]
    cases = [st_status(availability={"protected_down_today_s": 10, "budget_left_s": 463}),
             st_status(availability={"protected_down_today_s": 22, "budget_left_s": 451}),
             st_status(availability={"protected_down_today_s": 300, "budget_left_s": 173}),
             st_status(casters=ono(streak_ok=False, budget_left_s=0), availability={"protected_down_today_s": 500, "budget_left_s": 0}),
             st_status(casters=ono(streak_ok=False, data_rate_permille=950), availability={"protected_down_today_s": 10,
                                                                                         "budget_left_s": 200})]
    out = run_editor([{"fn": "dashboard", "status": x, "admin": True} for x in cases])
    bars = [o["cards"]["Onocoy"]["bar"] for o in out]
    assert bars[0] == {"cls": "bar ok", "role": "meter", "label": "Today's offline time left", "now": "97",
                       "text": "97\u00a0% left", "width": "97.9%"}, bars[0]
    # The screenshots' "On track, 7 min 31 s left": nearly full, not nearly empty.
    assert bars[1]["width"] == "95.3%" and bars[1]["cls"] == "bar ok"
    assert bars[2]["width"] == "36.6%" and bars[2]["cls"] == "bar warn" and bars[2]["text"] == "36\u00a0% left"
    # Missed: no bar (the verdict and its line say it), whatever offline time the budget still shows.
    assert bars[3] is None and bars[4] is None


def health_hl(**kw) -> dict:
    hl = {"level": "ok", "last_step": "none", "recovery_mode": "full", "rtcm_gap_ms": 200, "link": {"open": True, "mode": "stream"}}
    hl.update(kw)
    return hl


def test_health_card_takes_the_worse_of_the_alerts_and_the_status_page():
    """The Health chip is the worse of the worst alert and the Status page's state. When the state is worse than
    every alert (Onocoy stopped beside a fragmented heap) its title and line come first, above the alerts; an alert the
    state says itself is not repeated. A closed receiver port never says "No receiver found" (its cable advice would
    mislead)."""
    heap = {"id": "heap_fragmented", "source": "health"}
    norx = {"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 200}
    stopped = st_status(casters=[st_caster(0, "Onocoy", "stopped", stop_reason="rejected")], health={"level": "degraded"},
                        alerts=[heap])
    closed = st_status(receiver={"state": "no_receiver", "uart_closed": True, "rtcm_age_ms": None, "next_detect_in_s": 200},
                       casters=[st_caster(0, "Onocoy", "wait_data")], alerts=[norx])
    missing = st_status(receiver={"state": "no_receiver", "rtcm_age_ms": None, "next_detect_in_s": 200},
                        casters=[st_caster(0, "Onocoy", "wait_data")], alerts=[norx])
    held = st_status(receiver={"first_apply_held": True}, alerts=[{"id": "rx_held", "source": "health"}])
    maint = st_status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600})
    out = run_editor([{"fn": "health", "hl": health_hl(level="degraded"), "status": stopped},
                      {"fn": "health", "hl": health_hl(level="degraded", link={"open": False, "mode": "stream"}), "status": closed},
                      {"fn": "health", "hl": health_hl(level="degraded"), "status": missing},
                      {"fn": "health", "hl": health_hl(level="degraded"), "status": st_status(alerts=[heap])},
                      {"fn": "health", "hl": health_hl(), "status": held},
                      {"fn": "health", "hl": health_hl(), "status": st_status()},
                      {"fn": "health", "hl": health_hl(), "status": maint}])
    stop, port, norx_, frag, hold, calm, busy = out
    assert stop["chip"] == ["Needs attention", "badge bad"]
    assert [m[:2] for m in stop["msgs"]] == [["msg bad", "Onocoy stopped"], ["msg warn", "Memory fragmented"]], stop["msgs"]
    # With the Status page's button (the page-wide notice that says the same is left out here).
    assert stop["buttons"] == [["What to do", "#/ntrip"]]
    assert port["chip"] == ["Needs attention", "badge bad"] and port["buttons"] == []
    assert port["msgs"] == [["msg bad", "Receiver port closed", "The station cannot open its port to the receiver and keeps trying."]]
    # Said once: no "Receiver port: Closed, reopening" row under the line that says it.
    assert "Receiver port" not in dict(port["rows"])
    assert "No receiver found" not in json.dumps(port) and "cable" not in json.dumps(port)
    # No receiver with its port open: the alert says it once (with its search), not the state as well.
    assert [m[1] for m in norx_["msgs"]] == ["No receiver found"] and norx_["chip"][1] == "badge bad"
    # Streaming with a fragmented heap: the alert only, amber.
    assert [m[:2] for m in frag["msgs"]] == [["msg warn", "Memory fragmented"]] and frag["chip"] == ["Needs attention", "badge warn"]
    # The held receiver: the state (amber) says what the blue alert says, once.
    assert [m[:2] for m in hold["msgs"]] == [["msg warn", "Receiver not configured yet"]] and hold["chip"][1] == "badge warn"
    assert calm["chip"] == ["Healthy", "badge ok"] and calm["msgs"] == [] and calm["none"]
    # Planned and passing: blue "Busy" with the state and its End now.
    assert busy["chip"] == ["Busy", "badge info"] and [m[1] for m in busy["msgs"]] == ["Receiver maintenance"]
    assert busy["buttons"] == [["End now", "end-maint"]]


def test_health_card_says_the_state_beside_an_alert_of_its_colour():
    """The Status page's state goes above the alerts while it is at least as bad as each of them. A stall (amber)
    beside the profile alert (amber): both, as the Status page says them. Safe mode: the state's line with when it
    tries again and its two buttons, its own alert not again. Settings storage: Restart, and no pointer to
    Logs on Logs. A Wi-Fi trial: the network and the time left, as the Status page says."""
    drift = {"id": "profile_drift", "source": "health"}
    stall = st_status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"},
                      alerts=[drift])
    safe = st_status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                     casters=[st_caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}])
    settings = st_status(device={"hostname": "onocoydiy", "settings_available": False},
                         alerts=[{"id": "settings_degraded", "source": "health"}])
    trial = st_status(network={"online": True, "cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"})
    out = run_editor([{"fn": "health", "hl": health_hl(level="recovering", last_step="link_restart"), "status": stall},
                      {"fn": "health", "hl": health_hl(level="failed"), "status": safe},
                      {"fn": "health", "hl": health_hl(level="failed"), "status": settings},
                      {"fn": "health", "hl": health_hl(), "status": trial}])
    stall_, safe_, settings_, trial_ = out
    assert stall_["chip"][1] == "badge warn"
    assert [m[:2] for m in stall_["msgs"]] == [["msg warn", "Receiver stopped sending data"],
                                              ["msg warn", "Output differs from the profile"]], stall_["msgs"]
    assert safe_["chip"][1] == "badge bad"
    assert safe_["msgs"] == [["msg bad", "Safe mode", "The receiver and NTRIP servers are off after 3 crashes. The station tries "
                              "normal operation again in 45\u00a0min."]], safe_["msgs"]
    assert safe_["buttons"] == [["Updates", "#/settings/updates"], ["Leave safe mode", "leave-safe"]]
    assert settings_["msgs"] == [["msg bad", "Settings cannot be saved",
                                  "The station runs, but changes are not saved this boot. Restart it."]], settings_["msgs"]
    assert settings_["buttons"] == [["Restart", "#/settings/restart"]]
    assert trial_["msgs"] == [["msg info", "Trying new Wi-Fi", "Trying Cabin: 1\u00a0min 20\u00a0s left. If it gets no address "
                               "there, the station goes back to the previous network by itself."]], trial_["msgs"]


def test_onocoy_card_says_a_tls_failure_once():
    """TLS to Onocoy failing again and again: the box with the count and the plain port says it, not a line above it
    as well (the last failure, or the station's hint that says the same), nor the failures row. Fewer failures: the
    line (the last failure) and the failures row (the count by cause)."""
    cfg = stored("servers.onocoy.com", "fast", name="Onocoy", tls=True, port=2121, mountpoint="MP0TEST1", username="u")
    st = {"state": "backoff", "reason": "tls", "failures_by_class": {"tls": 12}, "state_age_s": 30, "next_retry_s": 20}
    box = "TLS to Onocoy failed 12 times. Onocoy documents uploads on the plain port only.Use plain port 2101"
    out = run_editor([{"fn": "card_lines", "cfg": cfg, "st": st},
                      {"fn": "card_lines", "cfg": cfg, "st": {**st, "hint": "TLS keeps failing; plain port 2101 is an option"}},
                      {"fn": "card_lines", "cfg": cfg, "st": {**st, "reason": "timeout"}},
                      {"fn": "card_lines", "cfg": cfg, "st": {**st, "failures_by_class": {"tls": 3}}}])
    assert out[0] == [box] and out[1] == [box]
    # Another cause last time: that line stays.
    assert out[2] == ["Timed out", box]
    assert out[3] == ["TLS failed", "FailuresTLS failed (3)"]


def test_chip_card_names_the_access_point_and_the_transmit_power():
    """Chip and Wi-Fi: the network, its signal, the access point, its channel, the transmit power and whether the access
    point can steer the station in a row each, so no wrapped line starts with a dot on a phone; each only while known
    and online (QEMU's link has no access point). The moves to another access point this start, like the drops,
    whatever the link does now."""
    status = st_status(network={"online": True, "ssid": "Home", "rssi": -60})
    wifi = {"ssid": "Home", "online": True, "rssi": -60, "channel": 11, "bssid": "AA:BB:CC:DD:EE:FF", "tx_power_dbm": 19.5,
            "ap_steering": True, "roams": 2}
    qemu = dict(wifi, channel=None, bssid=None, tx_power_dbm=None, roams=0)
    del qemu["ap_steering"]   # never read in that build: left out
    off = st_status(network={"online": False, "ssid": "Home", "offline_s": 30})
    sys = {"chip": {"model": "ESP32-S3", "revision": "v0.2", "flash_mb": 16, "psram_mb": 8}, "idf": "v6.1", "mac": "11:22:33:44:55:66"}
    full, bare, gone = run_editor([{"fn": "chip", "sys": sys, "status": status, "wifi": wifi, "net": {}},
                                   {"fn": "chip", "sys": sys, "status": status, "wifi": qemu, "net": {}},
                                   {"fn": "chip", "sys": sys, "status": off, "wifi": wifi, "net": {}}])
    rows = dict(full)
    assert [r[0] for r in full] == ["Chip", "ESP-IDF", "Wi-Fi", "Signal", "Access point", "Channel", "Transmit power",
                                    "Access point can steer", "MAC address", "Moves to another access point"]
    assert rows["Wi-Fi"] == "Home" and rows["Signal"] == " Good (\u221260\u00a0dBm)"
    assert rows["Access point"] == "AA:BB:CC:DD:EE:FF" and rows["Channel"] == "11" and rows["Transmit power"] == "19.5\u00a0dBm"
    assert rows["Access point can steer"] == "Yes" and rows["Moves to another access point"] == "2 this start"
    assert not any("\u00b7" in v for _, v in full)
    assert [r[0] for r in bare] == ["Chip", "ESP-IDF", "Wi-Fi", "Signal", "MAC address"]
    assert gone == [r for r in gone if r[0] != "Signal"] and [r[0] for r in gone] == ["Chip", "ESP-IDF", "Wi-Fi", "MAC address",
                                                                                    "Moves to another access point"]
    assert dict(gone)["Wi-Fi"] == "Home, offline for 30\u00a0s"


def test_earlier_starts_say_when_what_and_which_version():
    """Logs' earlier starts, one line each: when it started (else how long it ran, else a dash), a word for the
    kind of start and the version; the full sentence only on hover, never in every row. How long the newest ran is This
    start's "Previous run", so never said again in its row; a start from another year says its year."""
    now = 1760090000   # Oct 10, 2025
    starts = [{"reset_class": "planned", "reset_reason": "sw", "planned_reason": "app_update", "boot_no": 2, "prev_uptime_s": 600,
               "version": "v0.3.0", "start_unix": None},
              {"reset_class": "power_on", "reset_reason": "poweron", "planned_reason": "none", "boot_no": 1, "prev_uptime_s": None,
               "version": "v0.2.9", "start_unix": now - 3 * 86400},
              {"reset_class": "crash", "reset_reason": "task_wdt", "planned_reason": "none", "boot_no": 2, "prev_uptime_s": 50,
               "version": "v0.2.9", "start_unix": None},
              {"reset_class": "power_on", "reset_reason": "poweron", "planned_reason": "none", "boot_no": 1, "prev_uptime_s": None,
               "version": "v0.2.8"},
              {"reset_class": "power_on", "reset_reason": "poweron", "planned_reason": "none", "boot_no": 1, "prev_uptime_s": None,
               "version": "v0.2.7", "start_unix": now - 400 * 86400}]
    (rows,) = run_editor([{"fn": "starts", "starts": starts, "now": now}])
    # The newest, not dated: a dash (how long it ran is This start's "Previous run").
    assert rows[0][:3] == ["\u2013", "Update", "v0.3.0"]
    # Dated: its time, a short date as the event list has it, without this year.
    assert rows[1][1:3] == ["Power-on", "v0.2.9"] and ":" in rows[1][0] and not rows[1][0].startswith("Ran")
    assert "2025" not in rows[1][0]
    # A power-on came after the crash (count 2, then 1): how long it ran is not known.
    assert rows[2][:3] == ["\u2013", "Crash", "v0.2.9"]
    # Not dated: how long it ran, the previous run of the start after it (their counts follow on: 1, then 2).
    assert rows[3][:3] == ["Ran 50\u00a0s", "Power-on", "v0.2.8"]
    # Over a year back (the history survives a factory reset): the year too, so it never reads as this year's.
    assert "2024" in rows[4][0] and ":" in rows[4][0] and rows[4][1:3] == ["Power-on", "v0.2.7"]
    assert rows[1][3] == "Started after power-on or a reset by its pin" and rows[2][3] == "Restarted after a crash (the task watchdog)"
    assert not any("Started" in c or "Restarted" in c for r in rows for c in r[:3])


def test_folded_rows_stay_in_their_table():
    """Rows past the first few (Logs' older starts, History's earlier days) come after a summary row of the same
    table, spanning its columns, so they keep the columns of the rows above; nothing to fold when they all fit."""
    fit, starts, days = run_editor([
        {"fn": "fold", "count": 5, "n": 5, "more": "Older starts", "cols": 3, "open": False, "k": "starts"},
        {"fn": "fold", "count": 7, "n": 5, "more": "Older starts", "cols": 3, "open": True, "k": "starts"},
        {"fn": "fold", "count": 10, "n": 8, "more": "Earlier days", "cols": 2, "open": False, "k": None}])
    assert fit == ["row %d" % i for i in range(5)]
    assert starts == ["row %d" % i for i in range(5)] + [["fold", "3", "starts", True, "Older starts"], "row 5", "row 6"]
    assert days == ["row %d" % i for i in range(8)] + [["fold", "2", None, False, "Earlier days"], "row 8", "row 9"]



def test_event_list_says_every_rows_date_in_a_column_of_one_width():
    """Logs' event list: the When column has one width (a fixed table layout), so the text after it starts at the same
    place in every row; each start's heading has its time in that column too, and every row says the date the same way
    (an estimated time with "about")."""
    t0 = int(datetime(2026, 10, 17, 13, 36).timestamp())
    recs = [{"seq": 1, "kind": "boot", "a": 0, "b": 0, "uptime_s": 1, "time_unix": None},
            {"seq": 2, "kind": "session", "a": -1, "b": 1, "uptime_s": 1, "time_unix": None},
            {"seq": 3, "kind": "rx_boot", "a": 1, "b": 1, "uptime_s": 30, "time_unix": None},
            {"seq": 4, "kind": "wifi_disconnect", "a": 8, "b": -70, "uptime_s": 60, "time_unix": t0},
            {"seq": 5, "kind": "wifi_up", "a": 1, "b": 6, "uptime_s": 66, "time_unix": t0 + 6}]
    (r,) = run_editor([{"fn": "event_rows", "recs": recs}])
    assert r["cls"].split() == ["events", "fixed"]
    head, start, drop, rx = r["rows"]
    assert head == ["TH", "when", None, None, "Event"]
    assert start[:2] == ["TH", "when"] and start[3].startswith("about ")
    assert start[4] == "Started after power-on or a reset by its pin"
    assert drop[:3] == ["TD", "when", start[2]] and not drop[3].startswith("about") and drop[4].startswith("Wi-Fi dropped")
    assert rx[:3] == ["TD", "when", start[2]] and rx[3].startswith("about ") and rx[4] == "Receiver at the start Kept running"


SET_WIFI = {"configured": True, "online": True, "associated": True, "ssid": "Home", "ip": "192.168.1.50", "rssi": -82,
            "upstream": "ok", "mdns_host": "onocoydiy",
            "ap": {"active": False, "ssid": "onocoydiy-1A2B", "reasons": [], "clients": 0, "password_set": True}}
SET_UPDATE = {"state": "idle", "checks_enabled": True, "last_check_unix": None, "last_check_result": "ok",
              "app": {"running": "v0.3.0", "latest": "v0.3.0", "available": False, "rollback_available": True,
                      "rolled_back": [], "trial": {"stage": "none"}},
              "gnss": {"model": "UM982", "running": "R4.10Build18975", "running_build": 18975, "latest": "R4.10Build18975",
                       "available": False, "blocked": "none", "flash_available": False}}
SET_FLASH = {"journal": "waiting",
             "record": {"state": "needs_user", "phase": "transferring", "sha256": "9ac25d1d359e5815b132", "size": 3027628,
                        "expected_fw": "R4.10Build19152", "pre_model": "UM982", "pre_fw": "R4.10Build18975", "transfers": 1,
                        "entry_attempts": 1, "last_acked_block": 1984, "failure": "transport", "changed_unix": 1760000000},
             "progress": {"phase": "needs_user", "bytes_sent": 2031616, "bytes_total": 3027628, "retries": 3, "transfers": 1,
                          "failure": "transport", "frames_acked": 1984, "frames_total": 2957, "frames_sent": 1990,
                          "entry_attempts": 1},
             "receiver_in_loader": False, "receiver_text": "Ready.",
             "actions": {"retry": {"allowed": True}, "restart_receiver": {"allowed": True}, "clear": {"allowed": True}},
             "last": None, "flash_available": True}


def test_settings_is_network_updates_and_the_danger_zone_without_fold_outs():
    """Settings: Network (the Wi-Fi with its signal in words and dBm, Change network, the station's name and public status
    page, the setup hotspot with its name, password and its own password button), Updates (then Install manually: both
    files, the receiver from a PC and going back) and the Danger zone (restart, the admin password with the BOOT-button
    way back, factory reset). No fold-outs anywhere: the hotspot's password field comes with its button, the update
    session's record shows whole."""
    on = {**SET_WIFI, "online": False, "rssi": None, "offline_s": 420,
          "ap": {**SET_WIFI["ap"], "active": True, "reasons": ["fallback"], "clients": 1, "password_set": False}}
    off, hot, sess = run_editor([
        {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": SET_UPDATE, "click": "Change password"},
        {"fn": "settings_page", "status": st_status(), "wifi": on, "update": SET_UPDATE, "click": "Set a password"},
        {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": SET_UPDATE, "flash": SET_FLASH}])
    assert off["cards"] == ["Network", "Updates", "Danger zone"] and off["details"] == 0
    assert off["subs"] == ["Setup hotspot", "Install manually"]
    rows = dict(off["rows"])
    assert rows["Wi-Fi"] == "Home" and rows["Signal"] == " Weak (\u221282\u00a0dBm)"
    assert rows["Network name"] == "onocoydiy-1A2B" and rows["Password"] == "Set"
    assert off["buttons"] == ["Change network", "Change password", "Check now", "Choose file", "Choose file", "Start", "Go back…",
                              "Restart", "Change password", "Factory reset…"]
    assert off["lines"] == ["Opens for first setup, when the station cannot reach your Wi-Fi, and after BOOT is held."]
    # The hotspot's password: its field and buttons once its button is pressed (the admin password's stays as it is).
    assert off["after"]["fields"] == ["Station name", "New password"]
    assert off["after"]["buttons"][:4] == ["Change network", "Cancel", "Remove password", "Set password"]
    assert off["after"]["buttons"][-2] == "Change password"
    # Offline, the hotspot open without a password: no signal row (no word without its dBm), nothing to remove.
    assert "Signal" not in dict(hot["rows"]) and dict(hot["rows"])["Password"] == "None: anyone nearby can join"
    assert "Set a password" in hot["buttons"] and "Remove password" not in hot["after"]["buttons"]
    # The update session: what it recorded, shown whole, and the receiver's own text under its title.
    assert sess["cards"] == ["Network", "Updates", "Receiver update session", "Danger zone"] and sess["details"] == 0
    assert sess["subs"][-1] == "What the receiver sent"
    srows = dict(sess["rows"])
    assert srows["Package"].startswith("2.9") and srows["Transfers / loader attempts"] == "1 / 1" and srows["Blocks resent"] == "3"

SIG = {"best_sys": "galileo", "best_bands": 4, "e6_lost": False, "epoch_age_ms": 420, "rtcm1230_supported": True,
       "epochs": 900, "epoch_gaps": 0, "epoch_jumps": 0, "messages": [],
       "systems": [{"sys": "gps", "satellites": 11, "bands": ["l1", "l2", "l5"], "cnr_dbhz": {"l1": 45, "l2": 41, "l5": 44}},
                   {"sys": "glonass", "satellites": 9, "bands": ["l1", "l2"], "cnr_dbhz": {"l1": 45, "l2": 43}},
                   {"sys": "galileo", "satellites": 11, "bands": ["l1", "l2", "l5", "l6"],
                    "cnr_dbhz": {"l1": 41, "l2": 41, "l5": 41, "l6": 38}},
                   {"sys": "beidou", "satellites": 9, "bands": ["l1", "l2", "l5", "l6"],
                    "cnr_dbhz": {"l1": 45, "l2": 43, "l5": 41, "l6": 43}}]}


def test_signals_card_says_the_bands_once():
    """The Receiver page's Signals card: the best constellation and its bands in the headline only; the hints are one
    line each and only what the headline does not say; the band table stays. Short of 4 bands, one line says what
    Onocoy needs."""
    three = {**SIG, "best_sys": "gps", "best_bands": 3,
             "systems": [{**x, "bands": x["bands"][:3]} for x in SIG["systems"]]}
    good, few, idle = run_editor([{"fn": "signals", "live": True, "sig": SIG, "master": 7},
                                  {"fn": "signals", "live": True, "sig": three, "master": 7},
                                  {"fn": "signals", "live": False, "sig": SIG, "master": 7}])
    assert good["big"] == "Galileo \u00b7 4 bands" and good["table"] and good["lines"] == ["checks lines"]
    assert good["checks"] == [["ok", "\u2713Good: 40 satellites tracked"], ["ok", "\u2713Good: Strong signals (42\u00a0dB\u2011Hz)"]], good
    assert not any("band" in c[1] or "Galileo" in c[1] for c in good["checks"])
    assert good["words"] == ["Good: ", "Good: "]
    assert few["big"] == "GPS \u00b7 3 bands"
    assert few["checks"][-1] == ["warn", "!Check: Onocoy needs 4 bands: signal group 3 6 adds Galileo E6"], few
    # No corrections from the receiver: no verdicts, the headline and the table only.
    assert idle["checks"] == [] and idle["big"] == "Galileo \u00b7 4 bands" and idle["table"]
    # Group 3 6 applied (the Receiver card above says so) and still no Galileo E6: the antenna, not that group again.
    (applied,) = run_editor([{"fn": "signals", "live": True, "sig": three, "master": 3}])
    assert applied["checks"][-1] == ["warn", "!Check: Onocoy needs 4 bands: Galileo E6 is not tracked, check the antenna covers L6"]
    assert not any("3 6" in c[1] for c in applied["checks"])


def test_long_values_wrap_at_their_separators():
    """A long token in a label/value row wraps at its separators (a <wbr>, shown as "|"), never mid-word; short values
    and words are left alone. On a phone the wide cards' labels keep to 40 % of the row, as everywhere else."""
    (vals,) = run_editor([{"fn": "kv", "rows": [["Expected firmware", "R4.10Build26000"],
                                                ["Access point", "AA:BB:CC:DD:EE:FF"],
                                                ["Server", "servers.onocoy.com:2101"], ["Network", "Home"],
                                                ["Signal", "-61 dBm"], ["Note", "Remove the stored password"]]}])
    assert vals == ["R4.10|Build26000", "AA:|BB:|CC:|DD:|EE:|FF", "servers.|onocoy.|com:|2101", "Home", "-61 dBm",
                    "Remove the stored password"]
    css = (UI_JS.parent / "app.css").read_text()
    assert ".tight-kv .kv { grid-template-columns: minmax(7.5rem, min(13rem, 40%)) minmax(0, 1fr); }" in css
    # The Status page: a caster's name wraps at its separators; a long value without any (a 32-character network name)
    # wraps anywhere rather than widen its card.
    long = over(BASE, network={"ssid": "HomeNetworkWithAVeryLongName2024"},
                casters=[st_caster(0, "Onocoy", "streaming"),
                         st_caster(1, "MyVeryLongCasterName-Backup_Two", "streaming"),
                         st_caster(2, "", "streaming", host="caster.example-ntrip-service.org")])
    (o,) = dashboards(long)
    assert o["cards"]["NTRIP"]["names"] == ["Onocoy", "My|Very|Long|Caster|Name-|Backup_|Two", "caster.|example-|ntrip-|service.|org"]
    assert dict(o["cards"]["Station"]["rows"])["Wi-Fi"].startswith("HomeNetworkWithAVeryLongName2024 \u00b7 ")
    assert ".fit-kv .kv dd { overflow-wrap: anywhere; }" in css
    assert ".cline .name { font-weight: 650; overflow-wrap: anywhere; }" in css


def test_use_plain_port_keeps_its_spinner_and_focus_across_the_refresh():
    """The Onocoy card's "Use plain port" (TLS failing) is rebuilt every second: while its PUT waits the rebuilt
    button still shows the spinner and stays focusable (aria-disabled, never disabled); once saved the panel goes;
    refused, the button is back as it was."""
    cfg = stored("servers.onocoy.com", "fast", name="Onocoy", tls=True, port=2121, mountpoint="MP0TEST1", username="u")
    ok, bad = run_editor([{"fn": "plain_port", "cfg": {**cfg, "slot": 0}, "answer": [200, {"slot": 0, "config": {}}]},
                          {"fn": "plain_port", "cfg": {**cfg, "slot": 1}, "answer": [500, internal("ESP_FAIL")]}])
    assert ok["before"] == ["btn small", None, False]
    assert ok["during"] == ["btn small busy", "true", False]
    assert ok["after"] is None and ok["puts"] == [{"tls": False, "port": 2101}]
    assert bad["during"] == ["btn small busy", "true", False] and bad["after"] == ["btn small", None, False]


# A maintenance resumed with nothing kept of the receiver (GET /api/gnss).
MAINT_G = {"state": "idle", "maintenance": {"on": True}, "identity_confirmed": False, "responding": True,
           "receiver": {"valid": False}, "link": {"mode": "stream", "baud": 115200, "rx_bytes": 100, "rx_age_ms": 200},
           "position": {"source": "none"}, "arp": {"valid": False}}


def test_receiver_details_say_nothing_was_checked_in_a_maintenance_with_nothing_kept():
    """Logs' Receiver card in a maintenance resumed with nothing kept of the receiver: it is not checked, so the
    identity row reads as unchecked (a dash, like the fixed-position row), not a "No" that reads as a failed check.
    Outside maintenance, or with the identity kept, the row says Yes or No as before."""
    g = MAINT_G
    unchecked, kept, plain = run_editor([{"fn": "rx_details", "g": g},
                                         {"fn": "rx_details", "g": {**g, "identity_confirmed": True}},
                                         {"fn": "rx_details", "g": {**g, "maintenance": {"on": False}}}])
    assert dict(unchecked)["Identity confirmed"] == "\u2013" and dict(unchecked)["Answers"] == "Yes"
    assert dict(kept)["Identity confirmed"] == "Yes"
    assert dict(plain)["Identity confirmed"] == "No"


# ---- The Receiver pages: everything shown, no fold-outs ----

RX = {"valid": True, "model": "UM982", "firmware": "R4.10Build25164", "build": 25164, "serial": "TEST0001", "pn": "PN-TEST",
      "efuse_id": "EFUSE-TEST", "compile_date": "2025/06/13"}
# GET /api/gnss of a streaming station whose surveyed position is locked (a fixed base at it), as API.md shows it.
GNSS = {"state": "streaming", "receiver": RX, "responding": True, "restarting": False, "identity_confirmed": True, "alerts": [],
        "profile_applied": True, "profile_acknowledged": True, "rtcm_differs": False, "rtcm1230_supported": True,
        "config_failed": False, "config_retry_in_s": 0, "config_auto_retry": True, "detect_failures": 0,
        "link": {"baud": 115200, "mode": "stream", "rx_bytes": 1000, "rx_age_ms": 12, "rtcm_age_ms": 180, "open": True},
        "autocfg": True, "recovery": "full", "first_apply_held": False,
        "profile": {"signal_group_master": 3, "signal_group_slave": 6, "base_mode": "fixed", "survey_in_s": 0,
                    "fixed": {"lat_deg": 45.1, "lon_deg": -75.1, "height_m": 85.1}, "station_id": 1234, "com1_baud": 115200,
                    "rtcm": [{"msg": 1005, "period_s": 1}, {"msg": 1077, "period_s": 1}], "sat_view_output": False,
                    "antenna_model": "", "antenna_serial": "", "antenna_setup_id": 0},
        "rx_settings": {"valid": True, "age_s": 3600, "read_unix": None, "b1cb2a": "on", "mmp": "off", "mask_deg": 10.0,
                        "antenna": {"model": "", "serial": "", "setup_id": 0}},
        "apply_steps": {"antenna": {"sent": False, "ok": False, "reply": None},
                        "b1cb2a": {"sent": True, "ok": True, "reply": None}},
        "rtcm1033": {"valid": True, "station_id": 1234, "antenna": "", "setup_id": 0, "antenna_serial": "",
                     "receiver_type": "UM982", "receiver_fw": "R4.10Build25164", "receiver_serial": "TEST0001",
                     "age_ms": 400, "bad_frames": 0},
        "maintenance": {"on": False}, "rx_signal_group": {"master": 3, "slave": 6}, "rx_bands": {"state": "unknown"},
        "signal_group_refused": None,
        "position": {"locked": True, "source": "survey", "lat_deg": 45.1, "lon_deg": -75.1, "height_m": 85.1,
                     "sigma_m": 0.012, "locked_unix": 1759000000},
        "survey": {"active": False, "window_left_s": 0, "unfinished": False}, "fixed_check": {"state": "none"},
        "arp": {"valid": True, "station_id": 1234, "lat_deg": 45.1, "lon_deg": -75.1, "height_m": 85.1, "age_ms": 800,
                "offset_m": 0.002}}
SIG_MSGS = {**SIG, "messages": [{"msg": 1005, "count": 900, "frames": 900, "period_ms": 1000, "age_ms": 400, "expected_period_s": 1},
                                {"msg": 1077, "count": 900, "frames": 900, "period_ms": 1000, "age_ms": 400, "expected_period_s": 1}]}
# The receiver answers but sends nothing (no antenna): no corrections, the survey paused, no messages seen.
NOANT_ST = st_status(receiver={"rtcm_age_ms": None, "best_sys": None, "best_bands": 0, "responding": True},
                     health={"level": "failed", "recovery_mode": "alerts_only"}, alerts=[{"id": "no_rtcm", "source": "health"}],
                     casters=[st_caster(0, "Onocoy", "wait_data")])
NOANT_G = {**GNSS, "autocfg": False, "recovery": "alerts_only", "profile_applied": False,
           "profile": {**GNSS["profile"], "base_mode": "survey_in", "survey_in_s": 600, "fixed": None},
           "position": {"locked": False, "source": "none"}, "survey": {"active": False, "window_left_s": 0, "unfinished": True},
           "arp": {"valid": False}, "rtcm1033": {"valid": False, "bad_frames": 0}}
NOANT_SIG = {**SIG, "best_sys": None, "best_bands": 0, "epoch_age_ms": None, "systems": [],
             "messages": [{**m, "count": 0, "frames": 0, "period_ms": None, "age_ms": None} for m in SIG_MSGS["messages"]]}


def sgt_result(m: int, s: int, status: str, best: str, systems: list) -> dict:
    return {"group": {"master": m, "slave": s}, "status": status, "measured_s": 1800 if status == "done" else 640,
            "samples": 180, "epoch_gaps": 0, "best_sys": best, "best_bands": 4, "systems": systems}


GAL = {"sys": "galileo", "satellites": 10.9, "bands": ["l1", "l2", "l5", "l6"],
       "per_band": [{"band": b, "satellites": n, "cnr_dbhz": c, "signal_ids": [2]} for b, c, n in
                    (("l1", 44.6, 10.9), ("l2", 42.8, 10.9), ("l5", 44.7, 9.6), ("l6", 41.2, 8.25))]}
BDS = {"sys": "beidou", "satellites": 11.8, "bands": ["l1", "l2", "l5", "l6"],
       "per_band": [{"band": b, "satellites": 11.8, "cnr_dbhz": 43.0, "signal_ids": [2]} for b in ("l1", "l2", "l5", "l6")]}
SGT_RUN = {"state": "measuring", "groups": [{"master": 3, "slave": 6}, {"master": 7, "slave": 0}],
           "before": {"master": 3, "slave": 6}, "phase": 1, "phase_s": 1800, "settle_s": 60, "left_s": 1160,
           "decide_left_s": None, "running_group": {"master": 7, "slave": 0},
           "results": [sgt_result(3, 6, "done", "galileo", [GAL]), sgt_result(7, 0, "measuring", "beidou", [BDS])],
           "outcome": None, "kept": None, "reason": "", "ended_unix": None}
# A minute later: Galileo's L1 a little stronger in the second group's figures.
SGT_RUN2 = {**SGT_RUN, "left_s": 1100, "results": [sgt_result(3, 6, "done", "galileo", [{**GAL, "per_band": [
    {**GAL["per_band"][0], "cnr_dbhz": 45.0}, *GAL["per_band"][1:]]}]), sgt_result(7, 0, "measuring", "beidou", [BDS])]}
SGT_DONE = {**SGT_RUN, "state": "deciding", "phase": None, "left_s": None, "decide_left_s": 84000,
            "results": [sgt_result(3, 6, "done", "galileo", [GAL]), sgt_result(7, 0, "done", "beidou", [BDS])]}
LIVE = {"window": {"open": True, "left_s": 540, "max_min": 60, "default_min": 10, "sky": "on", "sky_why": "none", "last_end": "none"},
        "totals": {"satellites": 2, "tracked": 2, "with_pos": 2, "dropped": 0, "cnr_dbhz": 43, "msm_age_ms": 400,
                   "bands": {"l1": {"satellites": 2, "cnr_dbhz": 44}}, "systems": []},
        "satellites": [{"sys": "gps", "prn": 12, "cnr_dbhz": {"l1": 44}, "el_deg": 45, "az_deg": 210},
                       {"sys": "galileo", "prn": 5, "cnr_dbhz": {"l1": 41}, "el_deg": 30, "az_deg": 90}]}
PREVIEW = {"steps": [{"text": "VERSIONA", "kind": "query", "when": "always", "saved": False},
                     {"text": "CONFIG SIGNALGROUP 3 6", "kind": "write", "when": "if_differs", "saved": True},
                     {"text": "SAVECONFIG", "kind": "write", "when": "always", "saved": True}], "saveconfig": True}


def rx_page(arg: str, status: dict, gnss: dict, sig: dict, **kw) -> dict:
    return {"fn": "rx_page", "arg": arg, "status": status, "gnss": gnss, "sig": sig, **kw}


def test_receiver_pages_show_everything_without_fold_outs():
    """The Receiver page, its live address and Receiver settings, built whole in the states the other tests build
    (streaming with a locked position and four bands, no antenna, a maintenance resumed with nothing kept, the held
    first configuration, no receiver, a comparison running and one waiting for a choice): no <details> anywhere, nor in the held
    configuration's dialog. What the folds held is in plain view where it matters: the position in its own card,
    the messages in the Receiver card (once any arrived), every satellite while the sky is open, the profile's
    correction messages and advanced settings, the comparison's form while idle (only why while a start would be
    refused) and its results (each band's C/N0 inline) while it runs. With manual receiver setup no comparison card, nor
    the link to it, unless one still runs."""
    maint_st = st_status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600})
    norx_st = st_status(receiver={"state": "no_receiver", "rtcm_age_ms": None},
                        alerts=[{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 90}])
    held_g = {**GNSS, "first_apply_held": True, "autocfg": False, "profile_applied": False, "profile_acknowledged": False}
    run_st = st_status(receiver={"signal_test": {"state": "measuring", "phase": 1, "left_s": 1160}})
    done_st = st_status(receiver={"signal_test": {"state": "deciding", "decide_left_s": 84000}})
    cases = {
        "stream": rx_page("", st_status(), GNSS, SIG_MSGS),
        "live": rx_page("live", st_status(), GNSS, SIG_MSGS, live=LIVE),
        "noant": rx_page("", NOANT_ST, NOANT_G, NOANT_SIG),
        "maint": rx_page("", maint_st, {**GNSS, **MAINT_G, "profile": NOANT_G["profile"], "survey": {"active": False}}, NOANT_SIG),
        "norx": rx_page("", norx_st, {**GNSS, "state": "no_receiver"}, NOANT_SIG),
        "held": rx_page("", st_status(), held_g, SIG_MSGS, preview=PREVIEW, confirm=True),
        "settings": rx_page("settings", st_status(), GNSS, SIG_MSGS),
        "settings_noant": rx_page("settings", NOANT_ST, NOANT_G, NOANT_SIG),
        "noant_satview": rx_page("", NOANT_ST, {**NOANT_G, "profile": {**NOANT_G["profile"], "sat_view_output": True}}, NOANT_SIG),
        "no_restart": rx_page("settings", st_status(), {**GNSS, "recovery": "no_reboot"}, SIG_MSGS),
        "settings_unlocked": rx_page("settings", st_status(), {**GNSS, "position": {"locked": False, "source": "none"}}, SIG_MSGS),
        "manual_running": rx_page("settings", run_st, {**GNSS, "autocfg": False, "recovery": "alerts_only"}, SIG_MSGS,
                                  test=SGT_RUN),
        "running": rx_page("settings", run_st, GNSS, SIG_MSGS, test=SGT_RUN, test2=SGT_RUN2),
        "deciding": rx_page("settings", done_st, GNSS, SIG_MSGS, test=SGT_DONE),
    }
    out = dict(zip(cases, run_editor(list(cases.values()))))
    assert {k: v["details"] for k, v in out.items()} == {k: 0 for k in cases}
    assert out["held"]["dialog"]["details"] == 0

    stream = out["stream"]
    assert stream["cards"] == ["Receiver", "Position", "Signals"]
    rows = dict(stream["rows"])
    assert rows["Model"] == "UM982 \u00b7 R4.10Build25164" and rows["Source"] == "Survey-in"
    assert rows["Latitude"].startswith("45.1") and rows["Sent to servers"] == "This position"
    assert "Locked. If the antenna moves, survey again." in stream["lines"]
    assert "Messages" in stream["subs"] and any(r.startswith("1005 Station position") for r in stream["trs"])
    assert "Survey again" in stream["buttons"] and "Receiver settings" in stream["buttons"]
    # Logs' rows, not here.
    assert not {"Link", "Part number", "Chip ID", "Answers", "Identity confirmed", "Epochs"} & set(rows)

    # The sky as it is: every satellite listed under the table, nothing to open first.
    live = out["live"]
    assert any(r.startswith("G12") for r in live["trs"]) and any(r.startswith("E05") for r in live["trs"])
    assert not any("Show all" in b for b in live["buttons"])

    # Nothing known about the position but its line: a row of the Receiver card, no card of its own.
    noant = out["noant"]
    assert noant["cards"] == ["Receiver", "Signals"]
    assert dict(noant["rows"])["Position"] == "Surveying, paused while no corrections arrive."
    # Manual setup: the sky needs the profile's satellite positions output, and says so beside its button.
    assert noant["notes"] == ["Manual setup: turn on Satellite positions output in Receiver settings."]
    assert out["noant_satview"]["notes"] == [] and out["stream"]["notes"] == []
    # No message ever arrived: the card's line says so, no table of "Never".
    assert "Messages" not in noant["subs"] and not any(r.startswith("1005") for r in noant["trs"])

    # Not checked during maintenance and no position: the Receiver card alone says so.
    assert out["maint"]["cards"] == ["Receiver", "Signals"]
    # No receiver: its panel, no position (a stale fact), no messages and no signals.
    norx = out["norx"]
    assert norx["cards"] == ["Receiver"] and "Check again" in norx["buttons"] and "Messages" not in norx["subs"]
    assert not norx["rows"]

    # The held configuration's commands in one line (Logs lists them); a write sent only if the receiver differs
    # may change nothing, so "up to".
    held = out["held"]
    assert held["cards"][0] == "Receiver not configured yet" and held["lis"] == 0 and "Commands it will send" not in held["subs"]
    assert "3 commands; up to 2 change the receiver." in held["lines"]
    assert not any("commands" in t for t in held["dialog"]["lines"])

    settings = out["settings"]
    assert settings["cards"] == ["Position", "Receiver profile", "Receiver setup", "Compare two signal groups"]
    assert settings["subs"] == ["Signals", "Correction messages", "Antenna model", "Advanced"]
    assert {"Station ID", "Receiver link speed (COM1)", "Group A", "Group B", "Time per group"} <= set(settings["fields"])
    assert "Start comparison" in settings["buttons"] and "Apply again" in settings["buttons"]
    assert "Not sure? Compare two groups" in settings["buttons"]
    assert any(t.startswith("Tries two signal groups") for t in settings["lines"])
    # The messages listed without opening anything; the editor behind Edit.
    assert settings["msgList"] == ["1005 Station position", "1077 GPS observations (MSM7)"]
    assert "Message" not in settings["fields"] and "Antenna model (IGS name)" not in settings["fields"]
    # A start the station would refuse: why, beside Start, without the form.
    blocked = out["settings_unlocked"]
    assert "Group A" not in blocked["fields"] and "Start comparison" in blocked["buttons"]
    # Automatic setup: its restart box under it, ticked for full recovery (no_reboot: the same box cleared).
    restart = "Also restart the station if that doesn't help"
    assert restart in settings["checks"] and restart in out["no_restart"]["checks"]
    # Manual setup cannot start one: no card and no link to it; nor the restart box.
    manual = out["settings_noant"]
    assert restart not in manual["checks"]
    assert manual["cards"] == ["Position", "Receiver profile", "Receiver setup"]
    assert "Not sure? Compare two groups" not in manual["buttons"] and "Start comparison" not in manual["buttons"]
    # One that still runs stays, without a start form, with the way to it.
    still = out["manual_running"]
    assert still["cards"][-1] == "Compare two signal groups" and "Stop" in still["buttons"]
    assert "Not sure? Compare two groups" in still["buttons"] and "Group A" not in still["fields"]

    running = out["running"]
    assert "Group A" not in running["fields"] and not any(t.startswith("Tries two") for t in running["lines"])
    # Each band's C/N0 and its satellites inline; on a phone a cell wraps only between its parts, each ending with its dot.
    assert ("Galileo | 10.9 satellites\u00a0\u00b7 4 bandsL1 44.6 (10.9)\u00a0\u00b7 L2 42.8 (10.9)\u00a0\u00b7 "
            "L5 44.7 (9.6)\u00a0\u00b7 L6 41.2 (8.3) | \u2013") in running["trs"]
    assert "Per band: mean C/N0 in dB-Hz; in brackets, mean satellites." in running["lines"] and "Stop" in running["buttons"]
    # Its next answer patches the same results box (a keyboard focus on it stays) with the new figures.
    assert running["again"]["same"]
    assert any(r.startswith("Galileo | 10.9 satellites") and "L1 45.0 (10.9)" in r for r in running["again"]["trs"])
    deciding = out["deciding"]
    assert {"Keep 3 6 (Galileo E6)", "Keep 7 0 (BeiDou B2a)"} <= set(deciding["buttons"])


def test_receiver_pages_say_refusals_the_survey_and_a_stream_off_the_position():
    """What the folds held, in states the whole-page test does not build: the antenna and B1C/B2a steps the receiver
    refused, as messages under Corrections; while a survey runs, the position the receiver sends in the Position card
    with its source in a word and Lock offered; a locked position whose stream lies away from it, in amber."""
    refused_g = {**GNSS, "profile": {**GNSS["profile"], "antenna_model": "TESTANT1"},
                 "apply_steps": {"antenna": {"sent": True, "ok": False, "reply": "PARSING FAILED"},
                                 "b1cb2a": {"sent": True, "ok": False, "reply": None}}}
    survey_st = st_status(receiver={"position_locked": False, "surveying": True, "survey_window_left_s": 300})
    survey_g = {**GNSS, "profile": {**GNSS["profile"], "base_mode": "survey_in", "survey_in_s": 600, "fixed": None},
                "position": {"locked": False, "source": "none"},
                "survey": {"active": True, "window_left_s": 300, "unfinished": False},
                "arp": {"valid": True, "station_id": 1234, "lat_deg": 45.2, "lon_deg": -75.2, "height_m": 86.2, "age_ms": 700,
                        "offset_m": None}}
    offset_g = {**GNSS, "arp": {**GNSS["arp"], "offset_m": 1.32}}
    refused, survey, offset = run_editor([rx_page("settings", st_status(), refused_g, SIG_MSGS),
                                          rx_page("", survey_st, survey_g, SIG_MSGS),
                                          rx_page("", st_status(), offset_g, SIG_MSGS)])
    assert ["warn", "Antenna model rejected: PARSING FAILED."] in refused["msgs"]
    assert ["warn", "BeiDou B1C/B2a output rejected: no reason given."] in refused["msgs"]

    rows = dict(survey["rows"])
    assert survey["cards"] == ["Receiver", "Position", "Signals"]
    assert rows["Latitude"].startswith("45.2") and rows["Source"] == "Survey-in (running)"
    assert "Sent to servers" not in rows and "Locked on" not in rows
    assert "Lock this position" in survey["buttons"] and "Survey again" not in survey["buttons"]
    assert any(t.startswith("Surveying, about 5") for t in survey["lines"])

    assert dict(offset["rows"])["Sent to servers"] == "1.32\u00a0m from this position"
    assert "1.32\u00a0m from this position" in offset["warn"]


def test_logs_receiver_card_says_the_descriptor_once():
    """Logs' Receiver card: the descriptor casters get (1033) names the receiver and its antenna; where that is
    what the identity and the read-back above say, one row says it matches instead of repeating them. A field that
    differs shows every field, that one in amber; one that cannot be checked (nothing read back since the start) shows
    as it is. While the first configuration is held, the card lists the commands it would send."""
    differ = {**GNSS, "rtcm1033": {**GNSS["rtcm1033"], "antenna": "TESTANT1", "receiver_serial": "TEST0002"}}
    unread = {**GNSS, "rx_settings": {"valid": False}}
    held = {**GNSS, "first_apply_held": True}
    same, other, nothing_read, commands = run_editor([
        {"fn": "rx_details", "g": GNSS, "full": True}, {"fn": "rx_details", "g": differ, "full": True},
        {"fn": "rx_details", "g": unread, "full": True}, {"fn": "rx_details", "g": held, "preview": PREVIEW, "full": True}])
    rows = dict(same["rows"])
    assert rows["Descriptor (1033)"] == "Received 400\u00a0ms ago, matches the receiver"
    assert not {"Receiver serial", "Receiver type", "Receiver firmware", "Antenna", "Antenna serial", "Antenna setup ID"} & set(rows)
    assert rows["Serial number"] == "TEST0001" and "Commands the first configuration sends" not in same["subs"]

    rows = dict(other["rows"])
    assert rows["Descriptor (1033)"] == "Received 400\u00a0ms ago"
    assert rows["Receiver serial"] == "TEST0002 (differs)" and rows["Antenna"] == "TESTANT1 (differs)"
    assert rows["Receiver type"] == "UM982" and rows["Antenna setup ID"] == "0"
    assert other["warn"] == ["TEST0002 (differs)", "TESTANT1 (differs)"]

    rows = dict(nothing_read["rows"])
    assert rows["Descriptor (1033)"] == "Received 400\u00a0ms ago, matches the receiver"
    assert rows["Antenna"] == "none" and rows["Antenna setup ID"] == "0" and "Receiver serial" not in rows

    assert "Commands the first configuration sends" in commands["subs"] and commands["lis"] == 3

