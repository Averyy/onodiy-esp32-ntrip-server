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
  /* A plain "tag" finds every such element inside; any other selector finds nothing. */
  querySelectorAll(sel) {
    const tag = /^([a-z]+)$/.exec(sel);
    return tag ? [...this.walk()].filter(e => e !== this && e.tagName === tag[1].toUpperCase()) : [];
  }
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
  '  stampOnocoyReport, onocoyReportCard, tickOnocoyReportRetry, reportClock: fn => { const old = Date.now; Date.now = fn; return old; },\n' +
  '  publicDashboard, noReceiverPanel, startWizard, manageCtl, eventCard, healthCard, chipCard, signalsHead, bandTable, kv,\n' +
  '  startLines, foldRows, diagReceiverCard, receiverPage, renderLive, roverSummary, roverEditor, noticeItems,\n' +
  '  settingsPage: (el, arg) => Views.settings.create(el, arg), ntripPage: (el, arg) => Views.ntrip.create(el, arg),\n' +
  '  setStatus: s => { S.status = s; }, setSession: s => { S.session = s; },\n' +
  '  setLive: (age, started, page) => { S.statusAt = age === null ? 0 : Date.now() - age * 1000; shell.started = started;\n' +
  '    shell.current = page === null ? null : { name: page, arg: \'\', view: {} }; } };\n})()',
  ctx);

/* The elements on screen: hidden ones and everything inside them left out; of a closed fold, its summary only. */
function* visible(e) {
  if (e.hidden) return;
  yield e;
  const shut = e.tagName === 'DETAILS' && !e.open && !('open' in e.attrs);
  for (const c of e.kids) if (c instanceof El && (!shut || c.tagName === 'SUMMARY')) yield* visible(c);
}

/* What the editor shows: its heading, the field names in order, a preset's host and port (text), the help line, the
 * hints, the text fields' values in order (host:port, mountpoint, username: those shown), how many fold-outs it
 * has (shown or not), whether a retry pace shows, the pace choices and the hints and lines that say waits. */
function inspect(root) {
  const vis = [...visible(root)];
  const text = e => e.textContent;
  const names = els => els.filter(e => e.tagName === 'LEGEND' || (e.tagName === 'SPAN' && e.className === 'lbl') ||
    (e.tagName === 'LABEL' && e.className === 'check')).map(e => (e.tagName === 'LABEL' ? text(e.kids[1].kids[0]) : text(e)));
  /* A preset's host and port, in a field that cannot be edited where Other server has its own. */
  const fixed = vis.find(e => e.tagName === 'INPUT' && e.disabled);
  const help = vis.find(e => e.tagName === 'P' && e.className === 'muted small');
  const pace = vis.find(e => e.tagName === 'SELECT' && e.options.some(o => o.v === 'polite'));
  return {
    title: text(vis.find(e => e.tagName === 'H2')),
    fields: names(vis),
    fixed: fixed ? fixed.value : null,
    help: help ? text(help) : null,
    hints: vis.filter(e => e.tagName === 'SMALL' && e.className === 'hint').map(text),
    values: vis.filter(e => e.tagName === 'INPUT' && e.attrs.maxlength && e.attrs.type !== 'password').map(e => e.value),
    details: [...root.walk()].filter(e => e.tagName === 'DETAILS').length,
    pace: vis.some(e => e.kids.some(k => k instanceof Text && k.data.indexOf('Retry pace') >= 0)),
    paces: pace ? pace.options.map(text) : null,
    waits: vis.filter(e => /\d(s|min)\b/.test(text(e)) && (e.tagName === 'SMALL' || e.tagName === 'P')).map(text),
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
    stats: all.find(e => e.tagName === 'INPUT' && e.attrs.type === 'checkbox' && e.parent.textContent.includes('Show Onocoy statistics')),
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
    if (p === '/api/system') return [200, { hostname: 'onodiy' }];
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
  ui.setSession({ api: 1, authenticated: true, admin_set: true, setup_done: false, setup_step: 'onocoy', hostname: 'onodiy' });
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
  const box = page().find(e => e.tagName === 'LABEL' && e.textContent.includes('I understand the risk'))
    .kids.find(e => e.tagName === 'INPUT');
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

/* Logs' event card with two records: "Clear journal", confirmed, answered with c.answer ([status, body]).
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
  const wipe = [...card.el.walk()].find(e => e.tagName === 'BUTTON' && e.textContent === 'Clear journal');
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
    /* The folds by their title, and whether a sky plot shows. */
    summaries: v.filter(e => e.tagName === 'SUMMARY').map(e => e.textContent),
    hints: v.filter(e => e.tagName === 'SMALL' && e.className === 'hint').map(e => e.textContent),
    sky: v.some(e => e.tagName === 'SVG' && (e.attrs.class || '').indexOf('sky') >= 0),
    cards: v.filter(card).map(c => (([...visible(c)].find(e => e.tagName === 'H2') || {}).textContent || null)),
    subs: v.filter(e => e.tagName === 'H3').map(e => e.textContent),
    fields: v.filter(e => e.tagName === 'SPAN' && e.className === 'lbl').map(e => e.textContent),
    buttons: v.filter(e => e.tagName === 'BUTTON' || (e.tagName === 'A' && (e.className || '').indexOf('btn') === 0)).map(e => e.textContent),
    rows: pairs,
    lines: v.filter(e => e.tagName === 'P').map(e => e.textContent),
    pres: v.filter(e => e.tagName === 'PRE').map(e => e.textContent),
    trs: v.filter(e => e.tagName === 'TR').map(tr => tr.kids.map(k => k.textContent).join(' | ')),
    lis: v.filter(e => e.tagName === 'LI' && e.parent.tagName === 'OL').length,
    /* The checkboxes by their label; the notes beside a button (why it waits); the correction messages' list. */
    checks: v.filter(e => e.tagName === 'LABEL' && e.className === 'check').map(e => e.kids[1].kids[0].textContent),
    notes: v.filter(e => e.tagName === 'SPAN' && (e.className || '').indexOf('note') === 0).map(e => e.textContent),
    msgList: v.filter(e => e.tagName === 'LI' && e.parent.tagName === 'UL' && e.parent.className.indexOf('msglist') >= 0)
      .map(e => e.textContent),
    msgs: v.filter(e => (e.className || '').indexOf('msg ') === 0).map(m => [m.className.slice(4), m.textContent]),
    /* The progress lines: [label, the bar's class]. */
    progress: v.filter(e => e.tagName === 'DIV' && e.className === 'small' && e.parent && e.parent.kids[1] &&
      (e.parent.kids[1].className || '').indexOf('bar') === 0).map(e => [e.textContent, e.parent.kids[1].className]),
    /* Amber text (the position casters get, a value that differs). */
    warn: v.filter(e => (' ' + e.className + ' ').indexOf(' t-warn ') >= 0).map(e => e.textContent),
    /* The setting rows: [title, text]. */
    settings: v.filter(e => e.className === 'setting-row').map(e => [e.kids[0].kids[0].textContent, e.kids[0].kids[1].textContent]),
    /* The radio choices by their name. */
    choices: v.filter(e => e.tagName === 'SPAN' && e.className === 'opt').map(e => e.kids[0].textContent),
  };
}

/* A Receiver page (c.arg: '' the page, 'settings' Receiver settings, 'live' the old address of the sky) for the
 * status c.status and the station's answers: c.gnss (GET /api/gnss), c.sig (its signals), c.test (the comparison),
 * c.live (the live view), c.preview (the held configuration's commands). What it shows (pageParts) and the methods it
 * sent to /api/gnss/live (live); with c.confirm, also what the held configuration's dialog shows once "Configure the
 * receiver now" is pressed; with c.openAdvanced, what Receiver settings shows with Advanced opened (adv), and whether
 * it is still open once the page rebuilt its forms; with c.badStationId, what a save with station ID 5000 does
 * (invalid). */
async function receiverPageView(c) {
  const settle = async () => {
    for (let i = 0; i < 60; i++) await new Promise(r => setImmediate(r));
  };
  const first = sent.length;
  routes = (m, p) => {
    const path = p.split('?')[0];
    if (path === '/api/gnss') return [200, c.gnss];
    if (path === '/api/gnss/signals') return [200, c.sig];
    if (path === '/api/gnss/signal-test') return [200, c.test || { state: 'idle', groups: null, results: null, outcome: null }];
    if (path === '/api/gnss/live') return [200, c.live || { window: { open: false, last_end: 'none' } }];
    if (path === '/api/gnss/apply/preview') return [200, c.preview || { steps: [] }];
    if (path === '/api/gnss/query') return m === 'POST' ? [202, { accepted: true }] : [200, c.query || { state: 'none' }];
    return [200, {}];
  };
  ui.setStatus(c.status);
  ids.modal = new El('div');
  const el = new El('div');
  const view = ui.receiverPage(el, c.arg);
  await settle();
  const r = pageParts(el);
  r.live = sent.slice(first).filter(x => x.path === '/api/gnss/live').map(x => x.method);
  if (c.openPosition) {
    const details = [...el.walk()].find(e => e.tagName === 'DETAILS' && e.kids[0].textContent === 'Position details');
    details.open = true;
    view.reload();
    await settle();
    r.position = pageParts(el);
  }
  if (c.confirm) {
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === 'Configure the receiver now').fire('click');
    await settle();
    r.dialog = pageParts(ids.modal);
  }
  const fold = () => [...el.walk()].find(e => e.tagName === 'DETAILS');
  if (c.openAdvanced) {
    const adv = fold();
    adv.open = true;
    adv.fire('toggle');
    r.adv = pageParts(el);
    view.reload();
    await settle();
    const again = fold();
    r.adv.rebuilt = again !== adv;
    r.adv.stillOpen = !!again.open;
  }
  if (c.badStationId) {
    const adv = fold();
    const lab = [...el.walk()].find(e => e.tagName === 'SPAN' && e.className === 'lbl' && e.textContent === 'Station ID');
    const input = lab.parent.kids.find(k => k.tagName === 'INPUT');
    let focused = false;
    input.focus = () => {
      focused = true;
    };
    input.value = '5000';
    input.fire('input', true);
    const n = sent.length;
    [...el.walk()].find(e => e.tagName === 'FORM' && [...e.walk()].includes(input)).fire('submit');
    await settle();
    r.invalid = { open: !!adv.open, focused: focused, puts: sent.slice(n).filter(x => x.method === 'PUT').length,
      errors: [...visible(el)].filter(e => e.tagName === 'SMALL' && e.className === 'err').map(e => e.textContent) };
  }
  if (c.pick) {
    /* c.pick: the list holding that option set to it: what the page shows then, and whether Ask is greyed out. */
    const sel = [...visible(el)].find(e => e.tagName === 'SELECT' && e.options.some(o => o.v === c.pick));
    sel.value = c.pick;
    sel.fire('change');
    await settle();
    r.picked = pageParts(el);
    r.picked.ask_disabled = !![...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === 'Ask').disabled;
  }
  if (c.press) {
    /* c.press: a button pressed (by its text), then c.confirm2 in the dialog it opens, if any: the requests that went out. */
    const n = sent.length;
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === c.press).fire('click');
    await settle();
    if (c.confirm2) {
      /* Its OK is the form's submit button: the dialog named c.confirm2 is submitted. */
      const ok = [...visible(ids.modal)].find(e => e.tagName === 'BUTTON' && e.textContent === c.confirm2);
      [...ids.modal.walk()].find(e => e.tagName === 'FORM' && [...e.walk()].includes(ok)).fire('submit');
      await settle();
    }
    r.pressed = sent.slice(n).filter(x => x.method !== 'GET').map(x => [x.method, x.path, x.body]);
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

/* The receiver wiring as GET /api/system gives it: the dev board's, nothing saved. */
const WIRING = { rx_gpio: 1, tx_gpio: 2, port: 1, saved: null, default: { rx_gpio: 1, tx_gpio: 2, port: 1 },
  pins: [1, 2, 4, 5, 17, 18] };

/* Settings for the status c.status and the station's answers: c.wifi (GET /api/wifi), c.update (GET /api/update), c.flash
 * (GET /api/gnss/flash), c.staged (GET /api/upload/gnss), c.wiring (GET /api/system's wiring, WIRING by default). What it
 * shows (pageParts); with c.click, also what it shows once the first button of that name is pressed. With c.wire, the
 * receiver wiring's dialog: what it shows (wire.dialog, its lists as [option, greyed out, chosen]), then with
 * c.wire.pick ([rx, tx, port]) chosen and saved (the answer c.wire.answer), the PUTs that went out and the page after.
 * With c.press, the dialog that button opens (dialog, with ok_disabled before and after its checkbox is ticked), then
 * confirmed: the POSTs that went out. */
async function settingsPageView(c) {
  const settle = async () => {
    for (let i = 0; i < 60; i++) await new Promise(r => setImmediate(r));
  };
  routes = (m, p, body) => {
    const path = p.split('?')[0];
    if (path === '/api/wifi') return [200, c.wifi];
    if (path === '/api/system' && m === 'PUT') return [200, c.wire.answer];
    if (path === '/api/system') {
      return [200, { hostname: 'onodiy', hostname_active: 'onodiy', public_status: false, wiring: c.wiring || WIRING }];
    }
    if (path === '/api/update') return [200, c.update];
    if (path === '/api/gnss/flash') return [200, c.flash || { journal: 'none', flash_available: false }];
    if (path === '/api/upload/gnss') return [200, c.staged || { staged: null }];
    return [200, {}];
  };
  ui.setStatus(c.status);
  ui.setSession({ api: 1, authenticated: true, admin_set: true, setup_done: true, ap_ssid: 'onodiy-1A2B' });
  const el = new El('div');
  const view = ui.settingsPage(el, '');
  await settle();
  const r = pageParts(el);
  if (c.click) {
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === c.click).fire('click');
    r.after = pageParts(el);
  }
  if (c.press) {
    ids.modal = new El('div');
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === c.press).fire('click');
    await settle();
    const dlg = () => [...visible(ids.modal)];
    const ok = () => dlg().find(e => e.tagName === 'BUTTON' && e.attrs.type === 'submit');
    r.dialog = pageParts(ids.modal);
    r.dialog.ok_disabled = !!ok().disabled;
    const box = dlg().find(e => e.tagName === 'INPUT' && e.attrs.type === 'checkbox');
    if (box) {
      box.checked = true;
      box.fire('change');
    }
    r.dialog.ok_disabled_ticked = !!ok().disabled;
    const n = sent.length;
    dlg().find(e => e.tagName === 'FORM').fire('submit');
    await settle();
    r.posts = sent.slice(n).filter(x => x.method === 'POST').map(x => [x.path, x.body]);
  }
  if (c.wire) {
    ids.modal = new El('div');
    [...visible(el)].find(e => e.tagName === 'BUTTON' && e.textContent === 'Edit pins').fire('click');
    await settle();
    const lists = () => [...visible(ids.modal)].filter(e => e.tagName === 'SELECT');
    const options = () => lists().map(s => s.options.map(o => [o.textContent, !!o.disabled, !!o.selected]));
    r.wire = { dialog: pageParts(ids.modal), lists: options() };
    if (c.wire.pick) {
      lists().forEach((s, i) => {
        s.value = String(c.wire.pick[i]);
        s.fire('change');
      });
      r.wire.picked = options();
      const n = sent.length;
      [...ids.modal.walk()].find(e => e.tagName === 'FORM').fire('submit');
      await settle();
      r.wire.puts = sent.slice(n).filter(x => x.method === 'PUT').map(x => [x.path, x.body]);
      r.wire.open = [...ids.modal.walk()].some(e => e.tagName === 'FORM');
      r.wire.answer_reboot = !!c.wire.answer.reboot_required;
      r.wire.after = pageParts(el);
    }
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

/* Your rover's card under its head for GET /api/local-caster c.d and the status c.status: what pageParts says of it,
 * the state line [its word's class, text], each rover's line and the values marked for a copy. */
function roverCard(c) {
  const root = new El('div');
  root.append(...[ui.roverSummary(c.d, c.status)].flat(4).filter(Boolean));
  const v = [...visible(root)];
  const st = v.find(e => e.className === 'cstate');
  return Object.assign(pageParts(root), { state: st ? [st.kids[0].className, st.textContent] : null,
    rovers: root.kids.filter(k => k.tagName === 'DIV' && !k.className).map(k => k.textContent),
    copy: v.filter(e => e.className === 'copy').map(e => e.textContent) });
}

/* Your rover's editor on the config c.cfg (c.turnOn: opened by the switch with no password stored), its fields set as
 * c.steps say, then Save against the station's answer c.answer (default: saved). What it shows first (pageParts, the
 * hints, the password's placeholder), then the PUT bodies, the field errors, its error box, whether it handed the
 * answer on, whether it still counts as changed, and the toasts. */
async function roverEdit(c) {
  const settle = async () => {
    for (let i = 0; i < 30; i++) await new Promise(r => setImmediate(r));
  };
  sent.length = 0;
  ids.toasts = new El('div');
  routes = (m, p, body) => c.answer || [200, { config: Object.assign({}, c.cfg, body, { password: undefined }), status: {} }];
  let saved = null;
  const ed = ui.roverEditor(c.cfg, { turnOn: !!c.turnOn, hostname: 'onodiy', onSaved: d => { saved = d; }, onCancel: () => {} });
  const all = [...ed.el.walk()];
  const inp = { mountpoint: all.find(e => e.tagName === 'INPUT' && e.attrs.maxlength === '47'),
    username: all.find(e => e.tagName === 'INPUT' && e.attrs.maxlength === '31'),
    password: all.find(e => e.tagName === 'INPUT' && e.attrs.type === 'password') };
  const view = Object.assign(pageParts(ed.el), { hints: [...visible(ed.el)].filter(e => e.tagName === 'SMALL' &&
    e.className === 'hint').map(e => e.textContent), placeholder: inp.password.getAttribute('placeholder') });
  for (const [k, v] of c.steps || []) {
    inp[k].value = v;
    ed.el.fire('input');
  }
  ed.el.fire('submit');
  await settle();
  routes = null;
  const box = all.find(e => e.attrs.role === 'alert' && (e.className || '').indexOf('msg') === 0);
  return { view: view, puts: sent.filter(x => x.method === 'PUT').map(x => x.body), saved: !!saved, dirty: ed.dirty(),
    errors: [...visible(ed.el)].filter(e => e.tagName === 'SMALL' && e.className === 'err').map(e => e.textContent),
    box: box && !box.hidden ? box.textContent : null, toasts: ids.toasts.kids.map(t => t.textContent) };
}

/* The NTRIP page against GET /api/casters c.casters and GET /api/local-caster c.rover ([status, body]); a PUT to
 * /api/local-caster answers c.put, by default the stored body with the request merged in. c.act: 'switch' clicks Your
 * rover's switch, 'edit' its Edit. The cards' titles, Your rover's card before and after (pageParts), its class, its
 * switch [checked, name] before and checked after, the PUT bodies, the toasts and whether the page counts as changed. */
async function ntripRover(c) {
  const settle = async () => {
    for (let i = 0; i < 40; i++) await new Promise(r => setImmediate(r));
  };
  sent.length = 0;
  ids.toasts = new El('div');
  ids.modal = new El('div');
  let lc = c.rover;
  routes = (m, p, body) => {
    if (p === '/api/casters') return [200, c.casters];
    if (p === '/api/local-caster' && m === 'GET') return lc;
    if (p === '/api/local-caster' && m === 'PUT') {
      if (c.put) {
        /* A refused PUT the station stored all the same: GET answers c.again from now on. */
        if (c.again && c.act === 'switch') lc = c.again;
        return c.put;
      }
      const d = JSON.parse(JSON.stringify(lc[1]));
      Object.assign(d.config, body);
      delete d.config.password;
      lc = [200, d];
      return lc;
    }
    return [200, {}];
  };
  ui.setStatus(c.status || null);
  const el = new El('div');
  const view = ui.ntripPage(el, '');
  await settle();
  const card = () => [...visible(el)].find(e => e.tagName === 'SECTION' && [...e.walk()].some(x => x.tagName === 'H2' &&
    x.textContent === 'Local server'));
  const sw = () => (card() ? [...card().walk()].find(e => e.attrs.role === 'switch') : null);
  const r = { cards: pageParts(el).cards, before: card() ? pageParts(card()) : null, cls: card() ? card().className : null,
    sw: sw() ? [sw().getAttribute('aria-checked'), sw().getAttribute('aria-label')] : null };
  /* The "More servers" card's badge: how many slots are free. */
  const more = [...visible(el)].find(e => e.tagName === 'SECTION' && [...e.walk()].some(x => x.tagName === 'H2' &&
    x.textContent === 'More servers'));
  const badgeEl = more && [...more.walk()].find(x => /\bbadge\b/.test(x.className || ''));
  r.free = badgeEl ? badgeEl.textContent : null;
  if (c.act === 'switch') sw().fire('click');
  if (c.act === 'edit') [...card().walk()].find(e => e.tagName === 'BUTTON' && e.textContent === 'Edit').fire('click');
  if (c.act === 'reload') {
    /* The page reads its data again (a settings change), GET /api/local-caster now answering c.again. */
    lc = c.again;
    view.reload();
  }
  await settle();
  Object.assign(r, { after: card() ? pageParts(card()) : null, swAfter: sw() ? sw().getAttribute('aria-checked') : null,
    puts: sent.filter(x => x.method === 'PUT').map(x => x.body), toasts: ids.toasts.kids.map(t => t.textContent),
    dirty: view.dirty() });
  view.destroy();
  routes = null;
  ui.setStatus(null);
  return r;
}

const out = [];
(async () => {
for (const c of JSON.parse(casesJson)) {
  if (c.fn === 'rover_card') {
    out.push(roverCard(c));
    continue;
  }
  if (c.fn === 'rover_edit') {
    out.push(await roverEdit(c));
    continue;
  }
  if (c.fn === 'ntrip_rover') {
    out.push(await ntripRover(c));
    continue;
  }
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
  if (c.fn === 'notices') {
    out.push(ui.noticeItems(c.status, c.page).map(i => [i.key, i.title, i.text, [].concat(i.action || i.actions || []).map(b => b && b.textContent)]));
    continue;
  }
  if (c.fn === 'report_countdown') {
    let now = 1000000;
    const clock = ui.reportClock(() => now);
    const select = ctx.document.querySelectorAll;
    const before = sent.length;
    try {
      const card = ui.onocoyReportCard(ui.stampOnocoyReport(c.report));
      const button = [...card.walk()].find(e => e.tagName === 'BUTTON');
      ctx.document.querySelectorAll = () => [...card.walk()].filter(e => e.hasAttribute('data-onocoy-retry-until'));
      const states = [];
      for (const elapsed of c.elapsed) {
        now = 1000000 + elapsed * 1000;
        ui.tickOnocoyReportRetry();
        states.push([button.textContent, button.disabled]);
      }
      out.push({ states, requests: sent.length - before });
    } finally {
      ui.reportClock(clock);
      ctx.document.querySelectorAll = select;
    }
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
    const kids = (c.admin ? ui.adminDashboard(c.status, st, c.open || [], c.live || null, c.mon || null, c.report || null) :
      ui.publicDashboard(c.status, st, null)).filter(Boolean);
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
        /* The Performance card: per figure its name, latest reading and 24 h words; its lines (aria-label, viewBox,
         * each path [class, d]). */
        perf: v.filter(e => e.className === 'perf-row').map(r => [r.kids[0].kids[0].textContent, r.kids[1].textContent, r.kids[0].kids[2].textContent]),
        scales: v.filter(e => cls(e).indexOf(' spark-scale ') >= 0).map(e => e.textContent),
        sparks: v.filter(e => e.tagName === 'SVG' && e.attrs.class === 'spark').map(e => ({ label: e.attrs['aria-label'],
          viewBox: e.attrs.viewBox, paths: e.kids.map(k => [k.attrs.class, k.attrs.d]) })),
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
      dismiss: hv.filter(e => e.tagName === 'BUTTON' && e.getAttribute('data-act') === 'dismiss-power')
        .map(e => [e.textContent, e.getAttribute('aria-label')]),
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
    /* The slot number apart: a config from GET /api/casters has none. */
    const done = ui.turnCaster(1, { name: 'RTK2go', enabled: !c.on }, c.on);
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
    out.push({ puts: sent.filter(x => x.method === 'PUT').map(x => x.body), paths: sent.filter(x => x.method === 'PUT').map(x => x.path),
      asked: asked, saved: saved,
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
  if (c.fn === 'stats_option') {
    const f = controls(ed.el);
    out.push({ checked: f.stats.checked, shown: [...visible(ed.el)].includes(f.stats),
      notes: [...visible(ed.el)].filter(e => e.tagName === 'P' && e.className === 'hint').map(e => e.textContent) });
    continue;
  }
  /* Found again at each step: a change of server places the fields that server has. */
  for (const [what, v] of c.steps) {
    const f = controls(ed.el);
    if (what === 'stats') f[what].checked = v;
    else f[what].value = v;
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
        case(0, None, [], {"hostname": "ono diy!"}, inspect=True),
        case(1, None, [("caster", "onocoy")], dict(ONOCOY_NOT_SET_UP_ELSEWHERE, hostname="onodiy"), inspect=True),
        case(0, dict(ONOCOY_SET_UP, tls=True, port=2121), [], inspect=True),
        case(0, stored("10.0.2.2", "fast", name="Onocoy", port=2111, mountpoint="MP0TEST1", username="user0test",
                       test_onocoy=True), [], inspect=True),
    ])
    edit, new, switched, tls, test = out
    assert edit["title"] == "Edit Onocoy" and new["title"] == switched["title"] == "New server"
    for o in out:
        assert o["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Username", "Password", "Show Onocoy statistics"], o
        assert o["pace"] is False and o["details"] == 0 and o["waits"] == [], o
        assert o["hints"] == ["Any name; shows only in your Onocoy dashboard.", "Reference station credential."], o
        assert o["help"] == "The Onocoy console gives the username and password, under NTRIP Credentials. Onocoy's guide ↗", o
    assert edit["fixed"] == new["fixed"] == switched["fixed"] == "servers.onocoy.com:2101"
    assert tls["fixed"] == "servers.onocoy.com:2121" and test["fixed"] == "10.0.2.2:2111"
    assert edit["values"][:2] == ["MP0TEST1", "user0test"]
    assert new["values"][:2] == ["onodiy", ""] and switched["values"][:2] == ["onodiy", ""]


def test_onocoy_takes_the_stations_ntrip_version_until_set_up():
    """No NTRIP version on Onocoy's form: a new Onocoy slot, or one not set up yet, takes the station's (onocoy_preset:
    with 2.0 the chosen mountpoint reaches the dashboard); a slot that is set up keeps its own. The proposed mountpoint
    is saved like a typed one, and with the username and password that is all Onocoy needs."""
    v2 = {"host": "servers.onocoy.com", "port": 2101, "tls_port": 2121, "ntrip_version": 2}
    pending = stored("servers.onocoy.com", "fast", name="Onocoy", enabled=False, mountpoint="", username="",
                     password_set=False, ntrip_version=1)
    creds = [("username", "user0test"), ("password", "secret123")]
    out = run_editor([
        case(0, None, creds, {"preset": v2, "hostname": "onodiy"}),
        case(0, pending, creds, {"preset": v2, "hostname": "onodiy"}),
        case(0, dict(ONOCOY_SET_UP, ntrip_version=1), [], {"preset": v2}),
        case(1, None, [("caster", "onocoy")] + creds, dict(ONOCOY_NOT_SET_UP_ELSEWHERE, preset=v2, hostname="base-7")),
    ])
    assert [b["ntrip_version"] for b in out] == [2, 2, 1, 2], out
    assert [b["mountpoint"] for b in out] == ["onodiy", "onodiy", "MP0TEST1", "base-7"], out
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
    # The retry pace beside the password; NTRIP version and TLS in a row under them.
    assert custom["fields"] == ["NTRIP server", "Host and port", "Mountpoint", "Password", "Retry pace", "NTRIP version",
                                "TLS", "Encrypted"], custom
    assert custom["help"] == "Enter what your server gives you for a base."
    assert custom["paces"] == ["Standard (2s to 1min)", "Polite (10s to 5min)"]
    assert custom["waits"] == ["Refused logins: 1, 5, 15, then 30min."], custom["waits"]
    assert polite["waits"] == ["Refused logins: 5, 15, 30, then 60min; after 20 failed connections in a row, one try an "
                               "hour."], polite["waits"]
    assert custom_tls["fields"][-4:] == ["NTRIP version", "TLS", "Encrypted", "Certificate check"], custom_tls


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
    # To the slot's own address: the config the card holds has no slot number.
    assert off["paths"] == ["/api/casters/1"] and confirmed["paths"] == ["/api/casters/1"] * 2
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
    assert out[0] == {"line": "Streaming for 2h · 133.7 MB sent", "tone": "t-ok", "address": "rtk2go.com:2101/MINE01"}
    assert out[1] == {"line": "Stopped: needs you", "tone": "t-bad",
                      "address": "10.0.2.2:2111/MP0TEST1 · Test Onocoy slot (bench build)"}
    assert out[2] == {"line": "Waiting for the receiver", "tone": "t-warn", "address": "servers.onocoy.com:2101"}
    assert out[3]["line"] == "Login refused · next try in 4min" and out[3]["tone"] == "t-bad"
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
    s = {"api": 1, "public": False, "device": {"hostname": "onodiy", "settings_available": True},
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
CARDS = ["Sky", "Receiver", "Station", "Onocoy", "NTRIP", "Performance"]
WIDE_CARDS = ("Sky", "Performance")


def dashboards(*statuses, admin: bool = True, open_slots: list | None = None, live: dict | None = None,
               mon: dict | None = None) -> list[dict]:
    return run_editor([{"fn": "dashboard", "status": s, "admin": admin, "open": open_slots or [], "live": live, "mon": mon}
                       for s in statuses])


def matrix() -> dict[str, dict]:
    return dict(zip(MATRIX, dashboards(*MATRIX.values())))


def rx_rows(o: dict) -> list[str]:
    return [v for _, v in o["cards"]["Receiver"]["rows"]]


def test_status_page_always_has_six_cards():
    """Whatever the state the admin sees the same six cards in the same places (never a card missing): Sky across
    the page, then Receiver and Station, Onocoy and NTRIP (none of them taking the row), then Performance across the
    page. Each has one link, in its head, whose name starts with its visible word, and no button: the top card has the
    page's buttons."""
    for name, o in matrix().items():
        assert o["order"] == CARDS, name
        assert o["grid"] == "grid", name
        for title in CARDS:
            card = o["cards"][title]
            assert card["wide"] == (title in WIDE_CARDS), (name, title)
            assert card["buttons"] == 0, (name, title)
            assert len(card["links"]) == 1 and card["links"][0][3] is True, (name, title, card["links"])
            text, _, aria, _ = card["links"][0]
            assert aria.startswith(text), (name, title, card["links"])
        assert o["cards"]["Receiver"]["links"][0][:3] == ["Manage", "#/receiver", "Manage the receiver"]


STRONG = "Strong \u00b7 42\u00a0dB\u2011Hz on L1"   # SKY_LIVE's L1


def test_receiver_rows_in_every_state():
    """The Receiver card's three rows, in every state (the Sky card has the satellites and their bands): dashes while
    the receiver's figures mean nothing; the signal strength on L1 only while it delivers (so it never sits beside no
    corrections); the corrections' age; the position in a word."""
    out = dict(zip(MATRIX, dashboards(*MATRIX.values(), live=SKY_LIVE)))
    for name, o in out.items():
        assert [r[0] for r in o["cards"]["Receiver"]["rows"]] == ["Signal", "Corrections", "Position"], name
    assert rx_rows(out["healthy"]) == [STRONG, "Arriving", "Locked"]
    assert rx_rows(out["noant"]) == ["–", "None yet", "Surveying, paused"]
    for name in ("norx", "uart", "safe", "journal", "updating", "starting"):
        assert rx_rows(out[name]) == ["–"] * 3, name
    # Settings cannot be saved, said first: a missing or updating receiver still dashes every row.
    for name in ("settings_norx", "settings_flash"):
        assert out[name]["key"] == "settings" and rx_rows(out[name]) == ["–"] * 3, name
    # A gap shorter than the station's 10 s: the receiver still streams, the corrections still count as arriving, as
    # the top card says (one freshness rule for the row and the counts).
    assert out["rtcm7"]["key"] == "streaming" and rx_rows(out["rtcm7"]) == [STRONG, "Arriving", "Locked"]
    (gap,) = dashboards(over(BASE, receiver={"rtcm_age_ms": 12000, "state": "recovering"}, health={"level": "recovering"},
                             casters=WAITING))
    assert rx_rows(gap)[:2] == ["–", "None for 12s"]
    assert rx_rows(out["maint"]) == [STRONG, "Arriving", "Locked"]
    assert rx_rows(out["maint_unchecked"]) == [STRONG, "Arriving", "Not checked"]
    assert rx_rows(out["survey"]) == [STRONG, "Arriving", "Surveying, 5min left"]
    # The top card says the fixed position looks wrong: the row agrees in amber, without its sentence.
    assert out["fixed_far"]["head"] == "Fixed position looks wrong" and rx_rows(out["fixed_far"])[2] == "Fixed, check it"
    assert out["fixed_far"]["cards"]["Receiver"]["tones"] == [["warn", "Fixed, check it"]]
    # A stall: the fixture still has satellites, but nothing arrives.
    assert rx_rows(out["stall"])[:2] == ["–", "None for 6s"]
    assert rx_rows(out["configuring"])[:2] == ["–", "Arriving"]
    assert rx_rows(out["held"])[:2] == ["–", "None yet"]



SKY_LIVE = {
    "totals": {"satellites": 31, "tracked": 31, "signals": 86, "with_pos": 1, "dropped": 0, "cnr_dbhz": 38,
               "bands": {"l1": {"satellites": 31, "cnr_dbhz": 42}, "l2": {"satellites": 23, "cnr_dbhz": 34},
                         "l5": {"satellites": 22, "cnr_dbhz": 38}, "l6": {"satellites": 10, "cnr_dbhz": 32}},
               "systems": [{"sys": "gps", "satellites": 11, "bands": {"l1": 11, "l2": 10, "l5": 8}},
                           {"sys": "galileo", "satellites": 8, "bands": {"l1": 8, "l2": 8, "l5": 8, "l6": 6}},
                           {"sys": "qzss", "satellites": 0, "bands": {}}]},
    "satellites": [{"sys": "gps", "prn": 12, "in_msm": True, "cnr_dbhz": {"l1": 44}, "el_deg": 45, "az_deg": 210,
                    "sky_cnr_dbhz": 44},
                   {"sys": "galileo", "prn": 5, "in_msm": True, "cnr_dbhz": {"l1": 40}, "el_deg": None, "az_deg": None}]}


def test_sky_card():
    """The Sky card, across the page under the top card: while the corrections arrive, how many satellites are tracked
    with their mean C/N0, bars of the satellites per constellation (those with any) and per band, and the sky plot
    without the satellites' names (only those with a position, each named in its title); otherwise one muted line. Its
    one link, Details, opens the Receiver page."""
    healthy, noant, safe = dashboards(MATRIX["healthy"], MATRIX["noant"], MATRIX["safe"], live=SKY_LIVE)
    sky = healthy["cards"]["Sky"]
    assert healthy["order"][0] == "Sky" and sky["wide"]
    assert sky["links"] == [["Details", "#/receiver", "Details on the Receiver page", True]]
    assert sky["big"][0] == "31"
    text = sky["text"]
    assert "31satellitesConstellation" in text and "C/N0" not in text
    # Each constellation with satellites, its count, then a dot per band (named in its title).
    assert "SatellitesL1L2L5L6GPS11Galileo8" in text and "QZSS" not in text
    assert "G12 (GPS 12): elevation 45°, azimuth 210°, 44 dB-Hz" in text and "E05" not in text
    assert "Fainter" not in text
    for o in (noant, safe, matrix()["healthy"]):
        assert o["cards"]["Sky"]["notes"] == [["No satellites tracked yet.", "muted"]], o["key"]
        assert o["cards"]["Sky"]["big"] is None


def test_power_lost_line_has_a_dismiss_button():
    """The admin's "Also: Power was lost." line has a × (named Dismiss: Power was lost) beside Details; no other line has
    one, and a visitor's page has none."""
    lost = over(BASE, alerts=[{"id": "power_lost", "source": "health"}, {"id": "link_loss", "source": "health"}])
    admin, = dashboards(lost)
    visitor, = dashboards(lost, admin=False)
    assert {"power_lost", "link_loss"} <= {a[0] for a in admin["also"]}
    assert admin["dismiss"] == [["×", "Dismiss: Power was lost"]]
    assert visitor["dismiss"] == []


def test_signal_row_says_onocoys_need_once():
    """The Signal row: the strength on L1 (Fair or Weak in amber), then what Onocoy's quality score misses while it
    counts, in amber, only when nothing else on the page says it (the top card's line, Onocoy's verdict line, the bands
    "Also:" line); without Onocoy, the strength only."""
    few = MATRIX["quality"]
    few_no_alert = over(BASE, receiver=FEW)
    refused = over(BASE, receiver={"rtcm1230_supported": False})
    why_says = MATRIX["quality_down"]
    why_other = over(BASE, receiver=FEW, casters=[st_caster(0, "Onocoy", "backoff", reason="timeout"), RTK])
    no_gl = MATRIX["noglonass"]
    off = over(BASE, receiver=FEW, casters=[ONO_OFF, RTK])
    alert = MATRIX["quality_down_alert"]
    out = dashboards(few, few_no_alert, refused, why_says, why_other, no_gl, off, alert, live=SKY_LIVE)
    sig = [dict(o["cards"]["Receiver"]["rows"])["Signal"] for o in out]
    tones = [o["cards"]["Receiver"]["tones"] for o in out]
    assert out[0]["head"] == "Too few bands for Onocoy" and sig[0] == STRONG and tones[0] == []
    assert sig[1] == STRONG
    assert out[2]["key"] == "quality" and sig[2] == STRONG
    # Onocoy's verdict line says it (another state on top).
    assert out[3]["key"] == "casters_down" and out[3]["cards"]["Onocoy"]["notes"][0][0].startswith("Only 3 bands")
    assert sig[3] == STRONG
    assert out[4]["cards"]["Onocoy"]["notes"][0][0] == "Offline time is counting now."
    assert sig[4] == STRONG + " \u00b7 3 bands, Onocoy needs 4" and tones[4] == [["warn", " \u00b7 3 bands, Onocoy needs 4"]]
    assert sig[5] == STRONG + " \u00b7 no GLONASS" and tones[5] == [["warn", " \u00b7 no GLONASS"]]
    assert out[6]["key"] == "streaming" and sig[6] == STRONG and tones[6] == []
    # The bands "Also:" line says it: neither the row nor Onocoy's verdict line again (At risk stays).
    assert "bands" in [a[0] for a in out[7]["also"]] and sig[7] == STRONG
    assert out[7]["cards"]["Onocoy"]["chip"] == ["At risk", "badge warn"]
    assert not any(n[0].startswith("Only 3 bands") for n in out[7]["cards"]["Onocoy"]["notes"])
    # A fair or weak signal in amber; no figures from the receiver yet: the notes alone, else a dash.
    fair = {**SKY_LIVE, "totals": {**SKY_LIVE["totals"], "bands": {"l1": {"satellites": 31, "cnr_dbhz": 37}}}}
    weak = {**SKY_LIVE, "totals": {**SKY_LIVE["totals"], "bands": {}, "cnr_dbhz": 30}}
    (f,), (w,), (none_ok, none_note) = (dashboards(BASE, live=fair), dashboards(BASE, live=weak),
                                        dashboards(BASE, why_other))
    assert dict(f["cards"]["Receiver"]["rows"])["Signal"] == "Fair \u00b7 37\u00a0dB\u2011Hz on L1"
    assert f["cards"]["Receiver"]["tones"] == [["warn", "Fair \u00b7 37\u00a0dB\u2011Hz on L1"]]
    assert dict(w["cards"]["Receiver"]["rows"])["Signal"] == "Weak \u00b7 30\u00a0dB\u2011Hz"
    assert dict(none_ok["cards"]["Receiver"]["rows"])["Signal"] == "\u2013"
    assert dict(none_note["cards"]["Receiver"]["rows"])["Signal"] == "3 bands, Onocoy needs 4"


def test_casters_card_lists_counting_onocoy_first():
    """One line per caster set up: Onocoy first while it counts (no share: its own card has its day), left out when it
    is not set up or turned off; today's share for the others that are on. "No other servers." beside an Onocoy that is
    set up, else "No servers yet.", with Add (Manage when the top card's button adds one); when it shows in safe mode.
    The public form: names and states, no share, no link, and no card while it lists none."""
    swapped = over(BASE, casters=[st_caster(0, "RTK2go", "streaming", protected=False, onocoy_ready=False, **RTK2GO),
                                  st_caster(2, "Onocoy", "streaming"),
                                  st_caster(1, "Spare", "disabled", today_online_s=50)])
    no_slots = over(BASE, casters=[])
    public = {"api": 1, "public": True, "device": {"hostname": "onodiy"}, "safe_mode": {"active": False},
              "receiver": {"state": "streaming", "best_bands": 4},
              "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}, {"slot": 1, "name": "Bench", "state": "disabled"}],
              "health": {"level": "ok"}}
    public_safe = {"api": 1, "public": True, "device": {"hostname": "onodiy"}, "safe_mode": {"active": True},
                   "receiver": {"state": "idle", "best_bands": 0}, "casters": [], "health": {"level": "failed"}}
    names = ["healthy", "setup", "onooff", "onooff_none", "nocasters", "safe", "partly"]
    out = dashboards(*[MATRIX[n] for n in names], swapped, no_slots)
    pub = dashboards(public, public_safe, {**public, "casters": []}, admin=False)
    cas = {n: o["cards"]["NTRIP"] for n, o in zip(names + ["swapped", "no_slots"], out)}
    assert cas["healthy"]["lines"] == ["Onocoy · Streaming", "RTK2go · Streaming · 99.97% today"]
    # Each separator stays with the part after it: a phone breaks before a separator, never after it.
    assert [" ".join(p.split()) for p in cas["healthy"]["nowrap"]] == ["· Streaming", "· Streaming", "· 99.97% today"]
    assert cas["swapped"]["lines"] == ["Onocoy · Streaming", "RTK2go · Streaming · 99.97% today",
                                       "Spare · Off"]
    assert cas["setup"]["lines"] == cas["onooff"]["lines"] == ["RTK2go · Streaming · 99.97% today"]
    assert cas["partly"]["lines"][1] == "RTK2go · Login refused · 0% today"
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
    assert cas["safe"]["lines"] == [] and cas["safe"]["notes"] == [["Not available in safe mode.", "muted"]]
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
    assert rows[0]["Running for"] == "1d"
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
    # Today's online share is the figure (most stations are online all day); the offline time left a row.
    assert ono["healthy"]["chip"] == ["On track", "badge ok"] and ono["healthy"]["big"][0].endswith("online today")
    assert "%" in ono["healthy"]["big"][0] and ono["healthy"]["bar"] is None
    assert [r[0] for r in ono["healthy"]["rows"]] == ["Offline time left", "Online today", "Offline today"]
    assert dict(ono["healthy"]["rows"])["Offline time left"] == "7min 43s"
    assert ono["healthy"]["links"][0][:3] == ["Manage", "#/ntrip", "Manage Onocoy"]
    for o in ono.values():
        assert all(n[0] not in ("", "–") for n in o["notes"]), o["notes"]
    shows = {n: o["notes"] for n, o in ono.items() if o["chip"] is None}
    assert shows == {"setup": [["Onocoy is not set up yet.", "muted"]], "onooff_none": [["Onocoy is turned off.", "muted"]],
                     "setup_rx": [["Onocoy is not set up yet.", "muted"]], "onooff": [["Onocoy is turned off.", "muted"]],
                     "safe": [["Not available in safe mode.", "muted"]], "nocasters": [["Onocoy is not set up yet.", "muted"]],
                     "refused": [["Onocoy is not set up yet.", "muted"]], "alloff": [["Onocoy is not set up yet.", "muted"]]}
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
    "Also:" line; Onocoy not set up or off said once by the top card and once by the Onocoy card's own line."""
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
            # The Onocoy card's own line ("Onocoy is not set up yet.", the owner's words) may repeat the top card.
            own = ("Onocoy is not set up yet.", "Onocoy is turned off.")
            atoms = [v for r in card["rows"] for v in r] + lines + [n[0] for n in card["notes"] if n[0] not in own]
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
            shows = [n[0] for n in ono if n[0] in ("Onocoy is not set up yet.", "Onocoy is turned off.")]
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
    assert refused["line"] == "RTK2go is not streaming: login refused. Next try in 4min." and refused["subject"] == 1
    assert not any(a[0].startswith("caster:") for a in refused["also"]) and refused["heroButtons"] == []
    # The line names RTK2go: its row's word is neutral, not a second red statement.
    assert refused["cards"]["NTRIP"]["lineTones"] == ["muted"] and refused["cards"]["NTRIP"]["tones"] == []
    assert refused["cards"]["Onocoy"]["links"][0][:2] == ["Set up", "#/ntrip/onocoy"]
    # Something streams: Onocoy not set up stays on top, "too" is true, and the refused caster gets its "Also:" line.
    assert too["key"] == "onocoy_setup" and too["line"].endswith(" to stream to Onocoy too.")
    assert ["caster:2", "Bench not streaming", "Next try in 1min 30s.", "bad"] in too["also"]
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
    public = {"api": 1, "public": True, "device": {"hostname": "onodiy"}, "receiver": {"state": "detecting"},
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
    assert out[3] == ["", "No contact", "status", "9s", "true", "No contact with the station for 9s",
                      "No contact with the station", "#/", True, False, False, True]
    # Past a minute the count is its largest unit (it fits a phone's header); the title keeps the whole time.
    assert out[4][1] == "No contact" and out[4][3] == "1min" and out[4][5].endswith("1min 40s")
    assert [o[3] for o in out[7:11]] == ["59s", "59min", "2h", "3d"]
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
    public = {"api": 1, "public": True, "device": {"hostname": "onodiy"}, "safe_mode": {"active": False},
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

def test_onocoy_offline_time_left_row():
    """The Offline time left row: today's allowance left, plain while On track, amber at risk, gone once missed (the
    verdict and its line say it). No bar: the online share is the card's figure."""
    ono = lambda **kw: [st_caster(0, "Onocoy", "streaming", **kw)]
    cases = [st_status(availability={"protected_down_today_s": 10, "budget_left_s": 463}),
             st_status(availability={"protected_down_today_s": 22, "budget_left_s": 451}),
             st_status(availability={"protected_down_today_s": 300, "budget_left_s": 173}),
             st_status(casters=ono(streak_ok=False, budget_left_s=0), availability={"protected_down_today_s": 500, "budget_left_s": 0}),
             st_status(casters=ono(streak_ok=False, data_rate_permille=950), availability={"protected_down_today_s": 10,
                                                                                         "budget_left_s": 200})]
    out = run_editor([{"fn": "dashboard", "status": x, "admin": True} for x in cases])
    cards = [o["cards"]["Onocoy"] for o in out]
    left = [dict(c["rows"]).get("Offline time left") for c in cards]
    assert left[:3] == ["7min 43s", "7min 31s", "2min 53s"] and all(c["bar"] is None for c in cards)
    assert ["warn", "2min 53s"] in cards[2]["tones"] and not any(t[1] == "7min 43s" for t in cards[0]["tones"])
    assert left[3] is None and left[4] is None


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
    settings = st_status(device={"hostname": "onodiy", "settings_available": False},
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
                              "normal operation again in 45min."]], safe_["msgs"]
    assert safe_["buttons"] == [["Updates", "#/settings/updates"], ["Leave safe mode", "leave-safe"]]
    assert settings_["msgs"] == [["msg bad", "Settings cannot be saved",
                                  "The station runs, but changes are not saved this boot. Restart it."]], settings_["msgs"]
    assert settings_["buttons"] == [["Restart", "#/settings/restart"]]
    assert trial_["msgs"] == [["msg info", "Trying new Wi-Fi", "Trying Cabin: 1min 20s left. If it gets no address "
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
    assert dict(gone)["Wi-Fi"] == "Home, offline for 30s"
    # The board, first, once the station names it (GET /api/system's board).
    marked = dict(sys, board={"id": "um980-ntrx", "name": "NTRIP-X", "source": "marker"})
    (named,) = run_editor([{"fn": "chip", "sys": marked, "status": status, "wifi": qemu, "net": {}}])
    assert named[0] == ["Board", "NTRIP-X"] and [r[0] for r in named[1:]] == [r[0] for r in bare]


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
    assert rows[3][:3] == ["Ran 50s", "Power-on", "v0.2.8"]
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
    # Newest first and in time order: the start's heading below what happened after it.
    head, drop, rx, start = r["rows"]
    assert head == ["TH", "when", None, None, "Event"]
    assert start[:2] == ["TH", "when"] and start[3].startswith("about ")
    assert start[4] == "Started after power-on or a reset by its pin"
    assert drop[:3] == ["TD", "when", start[2]] and not drop[3].startswith("about") and drop[4].startswith("Wi-Fi dropped")
    assert rx[:3] == ["TD", "when", start[2]] and rx[3].startswith("about ") and rx[4] == "Receiver at the start Kept running"


SET_WIFI = {"configured": True, "online": True, "associated": True, "ssid": "Home", "ip": "192.168.1.50", "rssi": -82,
            "upstream": "ok", "mdns_host": "onodiy",
            "ap": {"active": False, "ssid": "onodiy-1A2B", "reasons": [], "clients": 0, "password_set": True}}
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


def test_settings_is_network_updates_the_receiver_and_the_danger_zone_without_fold_outs():
    """Settings: Network (the Wi-Fi with its signal in words and dBm, Change network, the station's name and public status
    page, the setup hotspot with its name, password and its own password button), Updates (then Install manually: both
    files and the receiver from a PC), Receiver (a link to Receiver settings) and the Danger zone (restart, the admin
    password with the BOOT-button way back, reverting the app, the receiver wiring, factory reset). No fold-outs anywhere: the hotspot's password field comes with its button, the update
    session's record shows whole."""
    on = {**SET_WIFI, "online": False, "rssi": None, "offline_s": 420,
          "ap": {**SET_WIFI["ap"], "active": True, "reasons": ["fallback"], "clients": 1, "password_set": False}}
    off, hot, sess = run_editor([
        {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": SET_UPDATE, "click": "change"},
        {"fn": "settings_page", "status": st_status(), "wifi": on, "update": SET_UPDATE, "click": "set password"},
        {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": SET_UPDATE, "flash": SET_FLASH}])
    assert off["cards"] == ["Network", "Updates", "Receiver", "Danger zone"] and off["details"] == 0
    assert off["subs"] == ["Setup hotspot", "Install manually"]
    rows = dict(off["rows"])
    assert rows["Wi-Fi"] == "Home" and rows["Signal"] == " Weak (\u221282\u00a0dBm)"
    assert rows["Network name"] == "onodiy-1A2B" and rows["Password"] == "Show (change)"
    # Change network small in the title row; the hotspot's password through the link in its row, no button of its own.
    assert off["buttons"] == ["Change network", "Show", "change", "Check now", "Choose file", "Choose file", "Start",
                              "Receiver settings", "Restart", "Change password", "Revert", "Edit pins", "Factory reset"]
    assert off["lines"] == ["Opens for first setup, when the station cannot reach your Wi-Fi, and after BOOT is held.",
                            "Position, signals, correction messages and how the receiver is set up."]
    # The hotspot's password: its field and buttons once its button is pressed (the admin password's stays as it is).
    assert off["after"]["fields"] == ["Station name", "New password"]
    assert off["after"]["buttons"][:5] == ["Change network", "Show", "change", "Show", "Cancel"]
    assert off["after"]["buttons"][-5:] == ["Restart", "Change password", "Revert", "Edit pins", "Factory reset"]
    # Offline, the hotspot open without a password: no signal row (no word without its dBm), nothing to remove.
    assert "Signal" not in dict(hot["rows"]) and dict(hot["rows"])["Password"] == "None (set password)"
    assert "set password" in hot["buttons"] and "Remove password" not in hot["after"]["buttons"]
    # The update session: what it recorded, shown whole, and the receiver's own text under its title.
    assert sess["cards"] == ["Network", "Updates", "Receiver", "Receiver update session", "Danger zone"] and sess["details"] == 0
    assert sess["subs"][-1] == "What the receiver sent"
    srows = dict(sess["rows"])
    assert srows["Package"].startswith("2.9") and srows["Transfers / loader attempts"] == "1 / 1" and srows["Blocks resent"] == "3"

WIRING = {"rx_gpio": 1, "tx_gpio": 2, "port": 1, "saved": None, "default": {"rx_gpio": 1, "tx_gpio": 2, "port": 1},
          "pins": [1, 2, 4, 5, 17, 18]}


def wiring_answer(saved, active=None):
    """PUT /api/system's answer once `saved` is stored (None: the defaults), the station still on `active`: a restart
    is required when the next start's wiring (the saved one, else the defaults) is not this one."""
    w = {**WIRING, **(active or {}), "saved": saved}
    nxt = saved or WIRING["default"]
    return {"hostname": "onodiy", "hostname_active": "onodiy", "public_status": False, "wiring": w,
            "reboot_required": any(nxt[k] != w[k] for k in ("rx_gpio", "tx_gpio", "port"))}


def test_danger_zone_receiver_wiring_row_and_its_dialog():
    """Settings, Danger zone: the receiver wiring's row says the pins and port in use and whether they are the defaults;
    Edit pins opens a dialog with three lists (the defaults marked, the pin the other list holds greyed out). Saving
    sends the wiring, or null for the defaults; the row then says what the next start uses."""
    base = {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": SET_UPDATE}
    saved = {"rx_gpio": 17, "tx_gpio": 18, "port": 2, "problem": None}
    d, moved, back, broken, rows_only = run_editor([
        {**base, "wire": {"pick": [17, 18, 2], "answer": wiring_answer(saved)}},
        {**base, "wiring": {**WIRING, "rx_gpio": 17, "tx_gpio": 18, "port": 2, "saved": saved},
         "wire": {"pick": [1, 2, 1], "answer": wiring_answer(None, {"rx_gpio": 17, "tx_gpio": 18, "port": 2})}},
        {**base, "wiring": {**WIRING, "rx_gpio": 17, "tx_gpio": 18, "port": 2}, "wire": {}},
        {**base, "wiring": {**WIRING, "saved": {"rx_gpio": 26, "tx_gpio": 2, "port": 1,
                                                 "problem": "GPIO26 is used by flash/PSRAM."}}},
        {**base, "wiring": {**WIRING, "saved": {**saved}, }},
    ])
    rows = dict(d["settings"])
    assert rows["Receiver wiring"] == "RX GPIO1, TX GPIO2, receiver COM1 (the defaults). Change only on a board wired differently."
    assert [t for t, _ in d["settings"]][-5:] == ["Restart the station", "Reset admin password", "Revert firmware",
                                                    "Receiver wiring", "Factory reset"]
    dlg = d["wire"]["dialog"]
    assert dlg["fields"] == ["ESP32 RX pin", "ESP32 TX pin", "Receiver port"]
    assert dlg["hints"] == ["Wired to the receiver's TXD.", "Wired to the receiver's RXD.", "Receiver firmware updates need COM1."]
    assert dlg["buttons"] == ["Cancel", "Save"]
    rx, tx, port = d["wire"]["lists"]
    assert [o[0] for o in rx] == ["GPIO1 (default)", "GPIO2", "GPIO4", "GPIO5", "GPIO17", "GPIO18"]
    assert [o[0] for o in tx] == ["GPIO1", "GPIO2 (default)", "GPIO4", "GPIO5", "GPIO17", "GPIO18"]
    assert [o[0] for o in port] == ["COM1 (default)", "COM2", "COM3"]
    # The pin one list holds is greyed out in the other.
    assert [o[0] for o in rx if o[1]] == ["GPIO2"] and [o[0] for o in tx if o[1]] == ["GPIO1"]
    picked_rx, picked_tx, _ = d["wire"]["picked"]
    assert [o[0] for o in picked_rx if o[1]] == ["GPIO18"] and [o[0] for o in picked_tx if o[1]] == ["GPIO17"]
    assert d["wire"]["puts"] == [["/api/system", {"wiring": {"rx_gpio": 17, "tx_gpio": 18, "port": 2}}]]
    assert d["wire"]["open"] is False and d["wire"]["answer_reboot"] is True
    after = d["wire"]["after"]
    assert dict(after["settings"])["Receiver wiring"].startswith("RX GPIO1, TX GPIO2, receiver COM1 (the defaults).")
    assert ["info", "Saved: RX GPIO17, TX GPIO18, receiver COM2. Takes effect after the next restart."] in after["msgs"]
    # Running on a saved wiring: the dialog starts from it; choosing the defaults sends null (the build's), and says so.
    assert dict(moved["settings"])["Receiver wiring"] == ("RX GPIO17, TX GPIO18, receiver COM2. Change only on a board "
                                                          "wired differently.")
    assert [o[0] for o in moved["wire"]["lists"][2] if o[2]] == ["COM2"]
    assert moved["wire"]["puts"] == [["/api/system", {"wiring": None}]]
    assert ["info", "Saved: back to the defaults (RX GPIO1, TX GPIO2, receiver COM1). Takes effect after the next "
                    "restart."] in moved["wire"]["after"]["msgs"]
    # Saved back to the defaults before a restart (the dialog starts from them); a saved wiring the board no longer
    # takes, with its reason.
    assert ["info", "Saved: back to the defaults (RX GPIO1, TX GPIO2, receiver COM1). Takes effect after the next "
                    "restart."] in back["msgs"]
    assert [[o[0] for o in lst if o[2]] for lst in back["wire"]["lists"]] == [["GPIO1 (default)"], ["GPIO2 (default)"],
                                                                             ["COM1 (default)"]]
    # A saved wiring this build refuses keeps the link off: the row and its note say so.
    assert dict(broken["settings"])["Receiver wiring"] == "Off this start. Change only on a board wired differently."
    assert ["bad", "The saved wiring (RX GPIO26, TX GPIO2, receiver COM1) cannot be used: GPIO26 is used by flash/PSRAM. "
                   "The receiver link stays off until you change it and restart."] in broken["msgs"]
    assert ["info", "Saved: RX GPIO17, TX GPIO18, receiver COM2. Takes effect after the next restart."] in rows_only["msgs"]


def test_danger_zone_revert_names_the_version_and_hides_a_same_version_slot():
    """Revert firmware (a Danger zone row): names the version the other slot holds; not offered when that slot holds
    the version running (a reinstall left both the same)."""
    base = {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI}
    older = {**SET_UPDATE, "app": {**SET_UPDATE["app"], "rollback_version": "v0.2.0"}}
    same = {**SET_UPDATE, "app": {**SET_UPDATE["app"], "rollback_version": "v0.3.0"}}
    a, b = run_editor([{**base, "update": older}, {**base, "update": same}])
    assert dict(a["settings"])["Revert firmware"] == "Revert to v0.2.0." and "Revert" in a["buttons"]
    assert "Revert firmware" not in dict(b["settings"]) and "Revert" not in b["buttons"]


SIG = {"best_sys": "galileo", "best_bands": 4, "e6_lost": False, "epoch_age_ms": 420, "rtcm1230_supported": True,
       "epochs": 900, "epoch_gaps": 0, "epoch_jumps": 0, "messages": [],
       "systems": [{"sys": "gps", "satellites": 11, "bands": ["l1", "l2", "l5"], "cnr_dbhz": {"l1": 45, "l2": 41, "l5": 44}},
                   {"sys": "glonass", "satellites": 9, "bands": ["l1", "l2"], "cnr_dbhz": {"l1": 45, "l2": 43}},
                   {"sys": "galileo", "satellites": 11, "bands": ["l1", "l2", "l5", "l6"],
                    "cnr_dbhz": {"l1": 41, "l2": 41, "l5": 41, "l6": 38}},
                   {"sys": "beidou", "satellites": 9, "bands": ["l1", "l2", "l5", "l6"],
                    "cnr_dbhz": {"l1": 45, "l2": 43, "l5": 41, "l6": 43}}]}


def test_signals_card_says_the_bands_once():
    """The Receiver page's Signals card: while corrections come, the hints one line each (satellites, signal strength,
    the bands) and no headline above them; the band table stays. Short of 4 bands, the bands line says what Onocoy
    needs. Without corrections, the headline and the table only."""
    three = {**SIG, "best_sys": "gps", "best_bands": 3,
             "systems": [{**x, "bands": x["bands"][:3]} for x in SIG["systems"]]}
    good, few, idle = run_editor([{"fn": "signals", "live": True, "sig": SIG, "master": 7},
                                  {"fn": "signals", "live": True, "sig": three, "master": 7},
                                  {"fn": "signals", "live": False, "sig": SIG, "master": 7}])
    assert good["big"] is None and good["table"] and good["lines"] == ["checks lines"]
    assert good["checks"] == [["ok", "\u2713Good: 40 satellites tracked"],
                              ["ok", "\u2713Good: Strong signals (44\u00a0dB\u2011Hz on L1)"],
                              ["ok", "\u2713Good: 4 bands on Galileo and BeiDou"]], good
    assert good["words"] == ["Good: ", "Good: ", "Good: "]
    assert few["big"] is None
    assert few["checks"][-1] == ["warn", "!Check: Onocoy needs 4 bands: signal group 3 6 adds Galileo E6"], few
    # No corrections from the receiver: no verdicts, the headline and the table only.
    assert idle["checks"] == [] and idle["big"] == "4 bands on Galileo and BeiDou" and idle["table"]
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
                    "rtcm": [{"msg": 1005, "period_s": 1}, {"msg": 1077, "period_s": 1}], "sat_view_output": True,
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
# GET /api/gnss/live while the profile sends the satellite positions: no window open, every satellite placed.
LIVE = {"window": {"open": False, "left_s": 0, "max_min": 60, "default_min": 10, "sky": "on_profile", "sky_why": "none",
                   "last_end": "none"},
        "totals": {"satellites": 2, "tracked": 2, "with_pos": 2, "dropped": 0, "cnr_dbhz": 43, "msm_age_ms": 400,
                   "sky_age_ms": 4000, "bands": {"l1": {"satellites": 2, "cnr_dbhz": 44}}, "systems": []},
        "satellites": [{"sys": "gps", "prn": 12, "cnr_dbhz": {"l1": 44}, "el_deg": 45, "az_deg": 210},
                       {"sys": "galileo", "prn": 5, "cnr_dbhz": {"l1": 41}, "el_deg": 30, "az_deg": 90}]}
# The same before any position came this boot.
LIVE_NOPOS = {**LIVE, "totals": {**LIVE["totals"], "with_pos": 0, "sky_age_ms": None},
              "satellites": [{**x, "el_deg": None, "az_deg": None} for x in LIVE["satellites"]]}
PREVIEW = {"steps": [{"text": "VERSIONA", "kind": "query", "when": "always", "saved": False},
                     {"text": "CONFIG SIGNALGROUP 3 6", "kind": "write", "when": "if_differs", "saved": True},
                     {"text": "SAVECONFIG", "kind": "write", "when": "always", "saved": True}], "saveconfig": True}


def rx_page(arg: str, status: dict, gnss: dict, sig: dict, **kw) -> dict:
    return {"fn": "rx_page", "arg": arg, "status": status, "gnss": gnss, "sig": sig, **kw}


def test_receiver_pages_show_everything_without_fold_outs():
    """The Receiver page, its old sky address and Receiver settings, built whole in the states the other tests build
    (streaming with a locked position and four bands, no antenna, a maintenance resumed with nothing kept, the held
    first configuration, no receiver, a comparison running and one waiting for a choice): no <details> anywhere but
    Receiver settings' Advanced, the only fold kept (closed until opened), nor in the held
    configuration's dialog. What the folds held is in plain view where it matters: the position in its own card,
    the messages in the Receiver card (once any arrived), the sky map and every satellite while the profile sends
    their positions (no window to open, no button), the profile's correction messages and the Sky map box, the
    comparison's form while idle (only why while a start would be refused) and its results (each band's C/N0 inline)
    while it runs. With manual receiver setup no comparison card, nor the link to it, unless one still runs."""
    maint_st = st_status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600})
    norx_st = st_status(receiver={"state": "no_receiver", "rtcm_age_ms": None},
                        alerts=[{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 90}])
    held_g = {**GNSS, "first_apply_held": True, "autocfg": False, "profile_applied": False, "profile_acknowledged": False}
    run_st = st_status(receiver={"signal_test": {"state": "measuring", "phase": 1, "left_s": 1160}})
    done_st = st_status(receiver={"signal_test": {"state": "deciding", "decide_left_s": 84000}})
    off_g = {**GNSS, "profile": {**GNSS["profile"], "sat_view_output": False}}
    noant_off = {**NOANT_G, "profile": {**NOANT_G["profile"], "sat_view_output": False}}
    manual_g = {**GNSS, "autocfg": False, "recovery": "alerts_only"}
    cases = {
        "stream": rx_page("", st_status(), GNSS, SIG_MSGS, live=LIVE),
        "live": rx_page("live", st_status(), GNSS, SIG_MSGS, live=LIVE),
        "sky_off": rx_page("", st_status(), off_g, SIG_MSGS, live=LIVE),
        "sky_waits": rx_page("", st_status(), GNSS, SIG_MSGS, live=LIVE_NOPOS),
        "manual_waits": rx_page("", st_status(), manual_g, SIG_MSGS, live=LIVE_NOPOS),
        "noant": rx_page("", NOANT_ST, noant_off, NOANT_SIG),
        "maint": rx_page("", maint_st, {**GNSS, **MAINT_G, "profile": NOANT_G["profile"], "survey": {"active": False}}, NOANT_SIG),
        "norx": rx_page("", norx_st, {**GNSS, "state": "no_receiver"}, NOANT_SIG),
        "held": rx_page("", st_status(), held_g, SIG_MSGS, preview=PREVIEW, confirm=True, live=LIVE),
        "settings": rx_page("settings", st_status(), GNSS, SIG_MSGS, openAdvanced=True),
        "settings_bad": rx_page("settings", st_status(), GNSS, SIG_MSGS, badStationId=True),
        "settings_noant": rx_page("settings", NOANT_ST, NOANT_G, NOANT_SIG),
        "noant_satview": rx_page("", NOANT_ST, NOANT_G, NOANT_SIG),
        "no_restart": rx_page("settings", st_status(), {**GNSS, "recovery": "no_reboot"}, SIG_MSGS),
        "settings_unlocked": rx_page("settings", st_status(), {**GNSS, "position": {"locked": False, "source": "none"}}, SIG_MSGS),
        "manual_running": rx_page("settings", run_st, manual_g, SIG_MSGS, test=SGT_RUN),
        "running": rx_page("settings", run_st, GNSS, SIG_MSGS, test=SGT_RUN, test2=SGT_RUN2),
        "deciding": rx_page("settings", done_st, GNSS, SIG_MSGS, test=SGT_DONE),
        "settings_um980": rx_page("settings", st_status(), {**GNSS, "receiver": {**RX, "model": "UM980"},
                                                            "profile": {**GNSS["profile"], "signal_group_um980": 2}},
                                  SIG_MSGS, openAdvanced=True),
    }
    out = dict(zip(cases, run_editor(list(cases.values()))))
    folded = {k for k, c in cases.items() if c["arg"] == "settings"}
    assert all(v["details"] <= 1 for v in out.values())
    assert all(out[k]["details"] == 1 for k in folded)
    assert all(out[k]["summaries"] == ["Advanced"] for k in folded)
    assert out["held"]["dialog"]["details"] == 0

    stream = out["stream"]
    assert stream["cards"] == ["Receiver", "Position", "Signals"]
    rows = dict(stream["rows"])
    assert rows["Model"] == "UM982 \u00b7 R4.10Build25164" and "Source" not in rows
    assert rows["Latitude"].startswith("45.1") and "Sent to servers" not in rows
    assert "Position details" in stream["summaries"]
    assert "Set. Updates automatically after a confirmed move." in stream["lines"]
    assert "Messages" in stream["subs"] and any(r.startswith("1005 Station position") for r in stream["trs"])
    assert "Reset location" not in stream["buttons"] and "Receiver settings" in stream["buttons"]
    # Logs' rows, not here.
    assert not {"Link", "Part number", "Chip ID", "Answers", "Identity confirmed", "Epochs"} & set(rows)

    # The profile sends the positions: the sky map and every satellite under the table, nothing to open or close, no
    # note on pauses; the station is only read (the old address shows the same).
    for k in ("stream", "live"):
        sky = out[k]
        assert sky["sky"] and any(r.startswith("G12") for r in sky["trs"]) and any(r.startswith("E05") for r in sky["trs"])
        assert not any("on the map" in t or "no position" in t for t in sky["lines"])   # every one placed: nothing to say
        assert not {"Show the sky", "Close", "Extend", "Extend by 10min"} & set(sky["buttons"]), sky["buttons"]
        assert not any("pause" in t for t in sky["lines"]) and sky["notes"] == []
        assert not any("Sky map" in m[1] for m in sky["msgs"])
        assert sky["live"] and set(sky["live"]) == {"GET"}, sky["live"]
    # Before the first positions: the list, an empty sky and why; with manual setup, what sends them.
    assert out["sky_waits"]["sky"] and "Waiting for the first satellite positions from the receiver." in out["sky_waits"]["lines"]
    assert "No satellite positions yet. With manual setup, Apply again in Receiver settings sends them." in \
        out["manual_waits"]["lines"]
    # Off: one line with the way to turn it on, no sky, no list and no button; the station is never asked.
    off = out["sky_off"]
    assert ["info", "Turn on Sky map in Receiver settings to see the satellites on a map."] in off["msgs"]
    assert not off["sky"] and not any(r.startswith("G12") for r in off["trs"]) and "Show the sky" not in off["buttons"]
    assert off["live"] == [] and off["notes"] == []
    # The held first configuration: why the sky waits, nothing read for it.
    assert ["info", "To see the sky map, configure the receiver first."] in out["held"]["msgs"] and out["held"]["live"] == []

    # Nothing known about the position but its line: a row of the Receiver card, no card of its own.
    noant = out["noant"]
    assert noant["cards"] == ["Receiver", "Signals"]
    assert dict(noant["rows"])["Position"] == "Surveying, paused while no corrections arrive."
    # No signals: one banner says so, and (Sky map off) where to turn the sky map on, with the way to Receiver settings;
    # no big headline, and no button that cannot work.
    assert ["info", "No signals yet" "Turn on Sky map in Receiver settings to see the satellites on a map."] in noant["msgs"]
    assert noant["notes"] == [] and "Show the sky" not in noant["buttons"]
    # Sky map on with manual setup and no antenna: the banner alone, no empty sky, no button.
    satview = out["noant_satview"]
    assert ["info", "No signals yet"] in satview["msgs"] and not satview["sky"]
    assert satview["notes"] == [] and "Show the sky" not in satview["buttons"]
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
    assert settings["cards"] == ["Position", "Receiver profile", "Receiver setup", "Compare two signal groups",
                                 "Ask the receiver"]
    # Advanced closed: its title only; the Sky map box in view, under the signal groups.
    assert settings["subs"] == ["Signals", "Correction messages"]
    assert {"Group A", "Group B", "Time per group"} <= set(settings["fields"])
    assert not {"Station ID", "Receiver link speed (COM1)"} & set(settings["fields"]) and "Apply again" not in settings["buttons"]
    assert "Sky map" in settings["checks"]
    # The defaults say so: signal group 3 6, Automatic setup, the restart box.
    assert "3 6 Galileo E6 (default)" in settings["choices"] and "7 0 Most signals" in settings["choices"]
    assert "Automatic (default)" in settings["choices"] and "Manual" in settings["choices"]
    assert ("On by default. When restarting the receiver did not help; at most once an hour and 3 times in 24 "
            "hours.") in settings["hints"]
    assert ("On (default): the receiver sends where each satellite is every 10s, so the Receiver page shows them "
            "on a sky map. Off: no sky map.") in settings["hints"]
    # Opened: the station ID, the link speed, the antenna model and Apply again; still open once the forms rebuild.
    # A UM980: its own groups, group 2 the default, no UM982 pairs and no comparison.
    u = out["settings_um980"]
    assert "2 All signals (default)" in u["choices"] and "1 Factory setting" in u["choices"]
    assert not any(c.startswith("3 6") for c in u["choices"])
    assert "Compare two signal groups" not in u["cards"]
    assert "Elevation mask (\u00b0)" in u["fields"]
    assert "Second antenna" not in u["adv"]["fields"]
    adv = settings["adv"]
    assert adv["subs"] == ["Signals", "Correction messages", "Antenna model"]
    assert {"Station ID", "Receiver link speed (COM1)"} <= set(adv["fields"]) and "Apply again" in adv["buttons"]
    assert adv["rebuilt"] and adv["stillOpen"]
    # A station ID out of range in the closed fold: it opens, the field says why and has the focus; nothing is sent.
    bad = out["settings_bad"]["invalid"]
    assert bad == {"open": True, "focused": True, "puts": 0, "errors": ["A number from 0 to 4095."]}, bad
    assert "Start comparison" in settings["buttons"]
    assert "Not sure? Compare two groups" in settings["buttons"]
    assert any(t.startswith("Tries two signal groups") for t in settings["lines"])
    # The messages listed without opening anything; the editor behind Edit.
    assert settings["msgList"] == ["1005 Station position", "1077 GPS observations (MSM7)"]
    assert "Message" not in settings["fields"] and "Antenna model (IGS name)" not in settings["fields"]
    # A start the station would refuse: why, beside Start, without the form.
    blocked = out["settings_unlocked"]
    assert "Group A" not in blocked["fields"] and "Start comparison" in blocked["buttons"]
    # Automatic setup: its restart box under it, ticked for full recovery (no_reboot: the same box cleared).
    restart = "Restart the station if the receiver stops answering"
    assert restart in settings["checks"] and restart in out["no_restart"]["checks"]
    # Manual setup cannot start one: no card and no link to it; nor the restart box.
    manual = out["settings_noant"]
    assert restart not in manual["checks"]
    assert manual["cards"] == ["Position", "Receiver profile", "Receiver setup", "Ask the receiver"]
    assert "Not sure? Compare two groups" not in manual["buttons"] and "Start comparison" not in manual["buttons"]
    # One that still runs stays, without a start form, with the way to it.
    still = out["manual_running"]
    assert still["cards"][-2:] == ["Compare two signal groups", "Ask the receiver"] and "Stop" in still["buttons"]
    assert "Not sure? Compare two groups" in still["buttons"] and "Group A" not in still["fields"]

    running = out["running"]
    assert "Group A" not in running["fields"] and not any(t.startswith("Tries two") for t in running["lines"])
    # Each band's C/N0 and its satellites inline; on a phone a cell wraps only between its parts, each ending with its dot.
    assert ("Galileo | 10.9 satellites\u00a0\u00b7 4 bandsL1 44.6 (10.9)\u00a0\u00b7 L2 42.8 (10.9)\u00a0\u00b7 "
            "L5 44.7 (9.6)\u00a0\u00b7 L6 41.2 (8.3) | \u2013") in running["trs"]
    assert any(t.startswith("Satellites rise and set, so their counts differ between the two measurements; the bands and "
                            "signals are what a group changes. Onocoy score: constellations \u00d7 bands, from Onocoy's quality scale")
               for t in running["lines"]) and "Stop" in running["buttons"]
    # Its next answer patches the same results box (a keyboard focus on it stays) with the new figures.
    assert running["again"]["same"]
    assert any(r.startswith("Galileo | 10.9 satellites") and "L1 45.0 (10.9)" in r for r in running["again"]["trs"])
    deciding = out["deciding"]
    assert {"Keep 3 6 (Galileo E6)", "Keep 7 0 (most signals)"} <= set(deciding["buttons"])


def test_comparison_buttons_send_their_requests():
    """The comparison's buttons do what they say: Keep sends the group pressed (0: the first, 1: the second), Stop asks
    first and then ends the test, and Start (after its dialog) sends the two groups."""
    done_st = st_status(receiver={"signal_test": {"state": "deciding", "decide_left_s": 84000}})
    run_st = st_status(receiver={"signal_test": {"state": "measuring"}})
    keep_b, keep_a, stop, start = run_editor([
        rx_page("settings", done_st, GNSS, SIG_MSGS, test=SGT_DONE, press="Keep 7 0 (most signals)"),
        rx_page("settings", done_st, GNSS, SIG_MSGS, test=SGT_DONE, press="Keep 3 6 (Galileo E6)"),
        rx_page("settings", run_st, GNSS, SIG_MSGS, test=SGT_RUN, press="Stop", confirm2="Stop"),
        rx_page("settings", st_status(), GNSS, SIG_MSGS, press="Start comparison", confirm2="Start")])
    assert keep_b["pressed"] == [["POST", "/api/gnss/signal-test/keep", {"group": 1}]]
    assert keep_a["pressed"] == [["POST", "/api/gnss/signal-test/keep", {"group": 0}]]
    assert stop["pressed"] == [["DELETE", "/api/gnss/signal-test", None]]
    assert [x[:2] for x in start["pressed"]] == [["POST", "/api/gnss/signal-test"]]
    assert len(start["pressed"][0][2]["groups"]) == 2


def test_receiver_pages_say_refusals_the_survey_and_a_stream_off_the_position():
    """What the folds held, in states the whole-page test does not build: the antenna and B1C/B2a steps the receiver
    refused, as messages under Corrections; while a survey runs, the position the receiver sends in the Position card
    with its source in a word and automatic locking explained; a locked position whose stream lies away from it, in amber."""
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
    assert rows["Latitude"].startswith("45.2") and "Source" not in rows
    assert "Sent to servers" not in rows and "Reference set on" not in rows
    assert not {"Lock this position", "Reset location"} & set(survey["buttons"])
    assert "Show the map" in survey["buttons"]
    assert any(t.startswith("Surveying from GNSS data, about 5") and "automatically" in t for t in survey["lines"])

    assert dict(offset["rows"])["Sent to servers"] == "1.32\u00a0m from this position"
    assert "1.32\u00a0m from this position" in offset["warn"]


def test_position_actions_only_offer_locking_after_a_manual_survey():
    unlocked = {**GNSS, "profile": {**GNSS["profile"], "base_mode": "survey_in", "fixed": None},
                "position": {"locked": False, "source": "none"}}
    manual = {**unlocked, "autocfg": False}
    states = [GNSS, {**GNSS, "autocfg": False}, unlocked, manual,
              {**manual, "survey": {"active": True}}, {**manual, "first_apply_held": True}]
    pages = run_editor([rx_page("", st_status(), g, SIG_MSGS) for g in states])
    assert all("Reset location" not in page["buttons"] for page in pages)
    assert ["Lock this position" in page["buttons"] for page in pages] == [False, False, False, True, False, False]
    assert "Show the map" in pages[2]["buttons"]   # position from the receiver, before the station stores its lock


def test_live_gnss_coordinates_do_not_claim_to_be_the_correction_reference():
    own = {**GNSS["arp"], "valid": True, "age_s": 2, "horizontal_sigma_m": 1.33}
    page, = run_editor([rx_page("", st_status(), {**GNSS, "own_position": own}, SIG_MSGS, openPosition=True)])
    assert "Live GNSS estimate" not in dict(page["rows"])
    rows = dict(page["position"]["rows"])
    assert rows["Source"] == "Survey-in"
    assert rows["Live horizontal uncertainty (σ)"] == "1.33\u00a0m"
    assert "Live GNSS estimate" in rows
    assert rows["Latitude"].startswith(str(GNSS["position"]["lat_deg"]))
    assert rows["Sent to servers"] == "Correction reference"
    assert "Reference set on" in rows and "Reference accuracy (σ)" in rows


def test_logs_receiver_card_says_the_descriptor_once():
    """Logs' Receiver card: the descriptor casters get (1033) names the receiver and its antenna; where that is
    what the identity and the read-back above say, one row says it matches instead of repeating them. A field that
    differs shows every field, that one in amber; one that cannot be checked (nothing read back since the start) shows
    as it is. While the first configuration is held, the card lists the commands it would send."""
    differ = {**GNSS, "rtcm1033": {**GNSS["rtcm1033"], "antenna": "TESTANT1", "receiver_serial": "TEST0002"}}
    unread = {**GNSS, "rx_settings": {"valid": False}}
    held = {**GNSS, "first_apply_held": True}
    # A UM982's own descriptor: its chip ID for the serial, "build-subbuild" for the firmware: the same receiver.
    chip = {**GNSS, "rtcm1033": {**GNSS["rtcm1033"], "receiver_fw": "25164-42227", "receiver_serial": "efuse-test"}}
    same, other, nothing_read, commands, own = run_editor([
        {"fn": "rx_details", "g": GNSS, "full": True}, {"fn": "rx_details", "g": differ, "full": True},
        {"fn": "rx_details", "g": unread, "full": True}, {"fn": "rx_details", "g": held, "preview": PREVIEW, "full": True},
        {"fn": "rx_details", "g": chip, "full": True}])
    rows = dict(same["rows"])
    assert rows["Descriptor (1033)"] == "Received 400ms ago, matches the receiver"
    assert not {"Receiver serial", "Receiver type", "Receiver firmware", "Antenna", "Antenna serial", "Antenna setup ID"} & set(rows)
    assert rows["Serial number"] == "TEST0001" and "Commands the first configuration sends" not in same["subs"]

    rows = dict(other["rows"])
    assert rows["Descriptor (1033)"] == "Received 400ms ago"
    assert rows["Receiver serial"] == "TEST0002 (differs)" and rows["Antenna"] == "TESTANT1 (differs)"
    assert rows["Receiver type"] == "UM982" and rows["Antenna setup ID"] == "0"
    assert other["warn"] == ["TEST0002 (differs)", "TESTANT1 (differs)"]

    rows = dict(nothing_read["rows"])
    assert rows["Descriptor (1033)"] == "Received 400ms ago, matches the receiver"
    assert rows["Antenna"] == "none" and rows["Antenna setup ID"] == "0" and "Receiver serial" not in rows

    assert "Commands the first configuration sends" in commands["subs"] and commands["lis"] == 3
    rows = dict(own["rows"])
    assert rows["Descriptor (1033)"] == "Received 400ms ago, matches the receiver" and own["warn"] == []
    assert not {"Receiver serial", "Receiver firmware"} & set(rows)


# ---- Your rover (local NTRIP): its card and editor on the NTRIP page, its line on the Status page ----

def lcast(enabled: bool = True, pw: bool = True, state: str = "listening", clients: tuple = (), refused: dict | None = None,
          errno: int | None = None, mount: str = "onodiy") -> dict:
    """GET /api/local-caster as API.md section 10a shows it."""
    ref = {"not_local": 0, "login": 0, "throttled": 0, "full": 0, "bad_request": 0, **(refused or {})}
    return {"config": {"enabled": enabled, "mountpoint": mount, "username": "rover", "password_set": pw},
            "status": {"state": state, "port": 2101, "max_clients": 4, "clients": list(clients), "sessions": len(clients),
                       "bytes_sent": 0, "stalled": 0, "listen_errno": errno, "refused": ref}}


ROVER_A = {"addr": "192.168.2.50", "ntrip_version": 2, "chunked": True, "connected_s": 3725, "bytes_sent": 6172000,
           "epochs_dropped": 0, "agent": "NTRIP RTKLIB/2.4.3"}
ROVER_B = {"addr": "fd12:3456:789a:1:2c4f:9aff:fe31:7b2d", "ntrip_version": 1, "chunked": False, "connected_s": 340,
           "bytes_sent": 577000, "epochs_dropped": 3, "agent": None}
NET = {"mdns_host": "onodiy", "ip": "192.168.1.50"}
RX_OK = {"rtcm_age_ms": 180}
ROVER_LIMITS = "At most 4 rovers at once. Only devices on your home network can connect."
ROVER_OFF = "Stream location data locally so a rover or device can use it on your network without internet."
NB = " "


def rover_cards(*cases) -> list[dict]:
    return run_editor([{"fn": "rover_card", "d": d, "status": {"network": net, "receiver": rx}} for d, net, rx in cases])


def test_rover_card_says_how_to_point_a_rover_at_the_station():
    """Off: one line on what it does, with the address. On, always in full and without fold-outs: the state, then what to
    type into the rover (host by name and IP, port, mountpoint, username, the password only as set or not, NTRIP
    1.0 or 2.0), the values to copy marked as such, and the limits."""
    off, off_nonet, on, ip_only = rover_cards((lcast(enabled=False, pw=False), NET, RX_OK), (lcast(enabled=False), {}, RX_OK),
                                              (lcast(), NET, RX_OK), (lcast(pw=True), {"mdns_host": None, "ip": "192.168.1.50"}, RX_OK))
    assert off["lines"] == [ROVER_OFF] and off["rows"] == [] and off["state"] is None
    assert off_nonet["lines"] == [ROVER_OFF]
    assert on["state"] == ["muted", "Waiting for a rover"]
    assert on["rows"] == [["Host", "onodiy.local or 192.168.1.50"], ["Port", "2101"], ["Mountpoint", "onodiy"],
                          ["Username", "rover"], ["Password", "Show"], ["NTRIP version", "1.0 or 2.0"]]
    assert on["copy"] == ["onodiy.local", "192.168.1.50", "2101", "onodiy", "rover"]
    assert on["lines"] == [ROVER_LIMITS] and on["msgs"] == [] and on["rovers"] == []
    assert ip_only["rows"][0] == ["Host", "192.168.1.50"]
    for o in (off, on, ip_only):
        assert o["details"] == 0 and "caster" not in json.dumps(o).lower() and "server" not in json.dumps(o).lower()


def test_rover_card_states_rovers_and_refusals():
    """The state line in each state (its colour: green with rovers, red when it cannot listen, else neutral), one line
    per rover (address, NTRIP version, for how long, what it was sent, epochs dropped for a slow link), what was refused
    since the start (only what happened; a throttled login counts as a wrong password), and a warning while the receiver
    sends no corrections."""
    two, one, error, wifi, stale, quiet, fresh = rover_cards(
        (lcast(clients=(ROVER_A, ROVER_B), refused={"login": 1, "throttled": 1, "not_local": 1, "full": 1, "bad_request": 1}),
         NET, RX_OK),
        (lcast(clients=(ROVER_A,), refused={"bad_request": 2}), NET, RX_OK),
        (lcast(state="error", errno=112), NET, RX_OK),
        (lcast(state="wait_network"), {"mdns_host": "onodiy", "ip": None}, RX_OK),
        (lcast(clients=(ROVER_A,)), NET, {"rtcm_age_ms": 3001}),
        (lcast(), NET, {"rtcm_age_ms": None}),
        (lcast(), NET, {"rtcm_age_ms": 3000}))
    assert two["state"] == ["t-ok", "2 rovers connected"] and one["state"] == ["t-ok", "1 rover connected"]
    assert two["rovers"] == [
        f"192.168.2.50 · NTRIP 2.0 · for 1h 2min · 5.9{NB}MB sent",
        f"fd12:3456:789a:1:2c4f:9aff:fe31:7b2d · NTRIP 1.0 · for 5min 40s · 563{NB}KB sent · 3 epochs dropped (slow link)"]
    assert two["lines"] == [ROVER_LIMITS, "Refused since the station started: 2 wrong passwords, 1 from outside your network, "
                                          "1 while full, 1 bad request."]
    assert one["lines"][1] == "Refused since the station started: 2 bad requests."
    assert error["state"] == ["t-bad", f"Not listening on port 2101 (error 112). Trying again every 10s."]
    assert wifi["state"] == ["muted", "Waiting for Wi-Fi. Rovers connect over your home network, not the setup hotspot."]
    assert wifi["rows"][0] == ["Host", "onodiy.local"]
    no_rtcm = [["warn", "No corrections from the receiver right now: connected rovers get nothing."]]
    assert stale["msgs"] == quiet["msgs"] == no_rtcm and fresh["msgs"] == two["msgs"] == []
    # Just turned on, before the station's task reports: not "Off" beside a switch that is on.
    (starting,) = rover_cards((lcast(state="off"), NET, RX_OK))
    assert starting["state"] == ["muted", "Starting"]


def rover_edit(cfg: dict, steps: list, turn_on: bool = False, answer: list | None = None) -> dict:
    return run_editor([{"fn": "rover_edit", "cfg": cfg, "steps": steps, "turnOn": turn_on, "answer": answer}])[0]


def test_rover_editor_fields_say_what_they_do_and_their_defaults():
    """Three fields, each with what it is and its default; the password is write-only (its placeholder says a stored one
    stays); opened by the switch with no password stored, it asks for one first and its Save turns the feature on."""
    edit = rover_edit(lcast()["config"], [])["view"]
    first = rover_edit(lcast(enabled=False, pw=False)["config"], [], turn_on=True)["view"]
    for v in (edit, first):
        assert v["fields"] == ["Mountpoint", "Username", "Password"] and v["details"] == 0
        assert v["hints"] == ["The name your rover asks for. Default: the station's name (onodiy). Letters, digits, dots, "
                              "hyphens or underscores.", "Default: rover.", "8 to 63 characters, no spaces. No default: you choose it."]
        assert v["cards"] == [] and "caster" not in json.dumps(v).lower()
    assert edit["placeholder"] == "Stored, leave empty to keep" and first["placeholder"] == ""
    assert edit["lines"] == [] and edit["buttons"] == ["Show", "Save", "Cancel"]
    assert first["lines"] == ["Choose a password first."] and first["buttons"] == ["Show", "Save and turn on", "Cancel"]


def test_rover_editor_sends_the_fields_and_keeps_the_stored_password():
    """Save sends the mountpoint and the username, the password only when one is typed (empty keeps the stored one),
    and `enabled` only from the switch's first-time editor; a save keeps whether the feature is on."""
    cfg = lcast()["config"]
    kept = rover_edit(cfg, [("mountpoint", " ROVER "), ("username", "me.2")])
    assert kept["puts"] == [{"mountpoint": "ROVER", "username": "me.2"}] and kept["saved"] and not kept["dirty"]
    assert kept["toasts"] == ["Saved."]
    new_pw = rover_edit(cfg, [("password", "p4ss:word!")])
    assert new_pw["puts"] == [{"mountpoint": "onodiy", "username": "rover", "password": "p4ss:word!"}]
    first = rover_edit(lcast(enabled=False, pw=False)["config"], [("password", "secret123")], turn_on=True)
    assert first["puts"] == [{"mountpoint": "onodiy", "username": "rover", "password": "secret123", "enabled": True}]
    assert first["toasts"] == ["Saved. The local server is on."]


def test_rover_editor_checks_the_stations_rules_and_shows_its_refusals():
    """The station's rules are checked before anything is sent, in its words, under the field; a refusal the station
    words goes under its field too, anything else into the editor's error box."""
    cfg = lcast()["config"]
    bad = rover_edit(cfg, [("mountpoint", "a/b"), ("username", ""), ("password", "with space")])
    assert bad["puts"] == [] and bad["errors"] == ["1 to 47 letters, digits, dots, hyphens or underscores.",
                                                   "1 to 31 letters, digits, dots, hyphens or underscores.",
                                                   "8 to 63 characters, letters, digits and symbols, no spaces."]
    assert rover_edit(cfg, [("password", "short")])["errors"] == ["8 to 63 characters, letters, digits and symbols, no spaces."]
    assert rover_edit(cfg, [("password", "x" * 64)])["errors"] != []
    none = rover_edit(lcast(enabled=False, pw=False)["config"], [], turn_on=True)
    assert none["puts"] == [] and none["errors"] == ["Choose a password."]
    # Off with no password stored, a plain save needs none.
    assert rover_edit(lcast(enabled=False, pw=False)["config"], [])["errors"] == []
    refused = rover_edit(lcast(enabled=False, pw=False)["config"], [("password", "secret123")], turn_on=True,
                         answer=[400, {"error": {"code": "invalid_field", "field": "password", "message": "needed to turn this on"}}])
    assert refused["errors"] == ["Needed to turn this on."] and not refused["saved"] and refused["dirty"]
    busy = rover_edit(cfg, [], answer=[409, {"error": {"code": "settings_unavailable", "message": "x"}}])
    assert busy["box"] == "The settings storage is unavailable this boot, so nothing can be saved." and busy["errors"] == []


PAGE_CASTERS = {"max_slots": 2, "warnings": [], "onocoy_preset": {"host": "servers.onocoy.com", "port": 2101},
                "slots": [{"slot": 0, "config": stored("rtk2go.com", "polite", name="RTK2go"), "status": {"state": "streaming"}},
                          {"slot": 1, "config": stored("", "standard", name="", enabled=False, password_set=False),
                           "status": {"state": "disabled"}}]}
PAGE_STATUS = st_status(network={"online": True, "ip": "192.168.1.50", "mdns_host": "onodiy"},
                        casters=[st_caster(0, "RTK2go", "streaming")])


def ntrip_rover(rover: list, act: str | None = None, put: list | None = None, again: list | None = None,
                casters: dict | None = None) -> dict:
    return run_editor([{"fn": "ntrip_rover", "casters": casters or PAGE_CASTERS, "rover": rover, "act": act, "put": put,
                        "again": again, "status": PAGE_STATUS}])[0]


def test_more_servers_counts_the_slots_beside_onocoy():
    """The "More servers" badge counts the free slots out of those Onocoy does not use (4 slots with Onocoy set up:
    3 for other servers)."""
    plain = ntrip_rover([200, lcast()])
    assert plain["free"] == "1 of 2 slots free"
    ono = {**PAGE_CASTERS, "max_slots": 4, "slots": [
        {"slot": 0, "config": stored("servers.onocoy.com", "standard", name="Onocoy"), "status": {"state": "streaming"}},
        {"slot": 1, "config": stored("rtk2go.com", "polite", name="RTK2go"), "status": {"state": "streaming"}},
        {"slot": 2, "config": stored("", "standard", name="", enabled=False, password_set=False), "status": {"state": "disabled"}},
        {"slot": 3, "config": stored("", "standard", name="", enabled=False, password_set=False), "status": {"state": "disabled"}}]}
    assert ntrip_rover([200, lcast()], casters=ono)["free"] == "2 of 3 slots free"


def test_rover_card_on_the_ntrip_page():
    """After the servers and the add card, the whole row: "Local server" with Edit and its switch; hidden in safe mode
    (no config) and on a station without the endpoint, the servers as before."""
    on = ntrip_rover([200, lcast()])
    assert on["cards"] == ["RTK2go", "More servers", "Local server"] and on["cls"] == "card wide"
    assert on["sw"] == ["true", "Local server"] and on["before"]["buttons"] == ["Edit", "", "Show"]
    assert on["before"]["rows"][2] == ["Mountpoint", "onodiy"]
    safe = ntrip_rover([200, {"config": None, "status": lcast(enabled=False)["status"]}])
    old = ntrip_rover([404, {"error": {"code": "not_found"}}])
    for o in (safe, old):
        assert o["before"] is None and o["cards"] == ["RTK2go", "More servers"], o
    # Read again: a failed read keeps the card as it was; the config changed elsewhere redraws it.
    blip = ntrip_rover([200, lcast()], act="reload", again=[500, {"error": {"code": "internal"}}])
    assert blip["after"] == blip["before"]
    changed = ntrip_rover([200, lcast()], act="reload", again=[200, lcast(enabled=False)])
    assert changed["after"]["lines"] == [ROVER_OFF] and changed["swAfter"] == "false"


def test_rover_switch_saves_at_once_and_asks_for_a_password_first():
    """The switch sends {"enabled": ...} alone and says so; turning it on with no password stored sends nothing and opens
    the editor, which asks for one first; a station that still refuses opens it too."""
    off = ntrip_rover([200, lcast()], act="switch")
    assert off["puts"] == [{"enabled": False}] and off["swAfter"] == "false"
    assert off["after"]["lines"] == [ROVER_OFF] and off["toasts"] == ["The local server is off."]
    on = ntrip_rover([200, lcast(enabled=False)], act="switch")
    assert on["puts"] == [{"enabled": True}] and on["swAfter"] == "true"
    assert on["after"]["rows"][0] == ["Host", "onodiy.local or 192.168.1.50"] and on["toasts"] == ["The local server is on."]
    first = ntrip_rover([200, lcast(enabled=False, pw=False)], act="switch")
    assert first["puts"] == [] and first["after"]["lines"] == ["Choose a password first."]
    assert first["after"]["fields"] == ["Mountpoint", "Username", "Password"] and not first["dirty"]
    refused = ntrip_rover([200, lcast(enabled=False)], act="switch",
                          put=[400, {"error": {"code": "invalid_field", "field": "password", "message": "needed to turn this on"}}])
    assert refused["puts"] == [{"enabled": True}] and refused["after"]["lines"] == ["Choose a password first."]
    edit = ntrip_rover([200, lcast()], act="edit")
    assert edit["after"]["fields"] == ["Mountpoint", "Username", "Password"] and edit["after"]["lines"] == []
    # A 500 on turning on (the config stored, the caster could not start): the card reads the station again, so the
    # switch shows on with the error state; a refusal that stored nothing leaves it off.
    stored = ntrip_rover([200, lcast(enabled=False)], act="switch", put=[500, internal("ESP_ERR_NO_MEM")],
                         again=[200, lcast(state="error", errno=12)])
    assert stored["puts"] == [{"enabled": True}] and stored["swAfter"] == "true", stored
    assert stored["after"]["rows"][0] == ["Host", "onodiy.local or 192.168.1.50"]
    kept = ntrip_rover([200, lcast(enabled=False)], act="switch", put=[500, internal("ESP_FAIL")],
                       again=[200, lcast(enabled=False)])
    assert kept["swAfter"] == "false" and kept["after"]["lines"] == [ROVER_OFF], kept


def test_status_ntrip_card_says_your_rover_in_one_line():
    """While the local server is on, one line after the servers, built like theirs: "1 connected" in green, else its state in
    a word (not listening in red); nothing while it is off or in safe mode, and never in the public form."""
    lc = lambda state="listening", clients=0, enabled=True: {"enabled": enabled, "state": state, "clients": clients}
    out = dashboards(over(BASE, local_caster=lc(clients=1)), over(BASE, local_caster=lc(clients=2)),
                     over(BASE, local_caster=lc()), over(BASE, local_caster=lc("wait_network")),
                     over(BASE, local_caster=lc("error")), over(BASE, local_caster=lc("off", enabled=False)),
                     over(MATRIX["nocasters"], local_caster=lc(clients=1)),
                     over(MATRIX["safe"], local_caster=lc("off", enabled=False)), over(BASE, local_caster=lc("off")))
    cas = [o["cards"]["NTRIP"] for o in out]
    assert cas[8]["lines"][2:] == ["Local server · starting"]
    servers = ["Onocoy · Streaming", "RTK2go · Streaming · 99.97% today"]
    assert [c["lines"][2:] for c in cas[:5]] == [["Local server · 1 connected"], ["Local server · 2 connected"],
                                                 ["Local server · waiting for a rover"], ["Local server · waiting for Wi-Fi"],
                                                 ["Local server · not listening"]]
    assert [c["lineTones"][2:] for c in cas[:5]] == [["t-ok"], ["t-ok"], ["muted"], ["muted"], ["t-bad"]]
    assert cas[0]["lines"][:2] == servers and cas[5]["lines"] == servers
    # No server yet: the note stays, the rover's line under it.
    assert cas[6]["notes"] == [["No servers yet.", "muted"]] and cas[6]["lines"] == ["Local server · 1 connected"]
    assert cas[7]["lines"] == [] and cas[7]["notes"] == [["Not available in safe mode.", "muted"]]
    for o in out:
        assert o["order"] == CARDS and o["cards"]["NTRIP"]["buttons"] == 0
    (pub,) = dashboards({"api": 1, "public": True, "device": {"hostname": "onodiy"}, "safe_mode": {"active": False},
                         "receiver": {"state": "streaming", "best_bands": 4}, "health": {"level": "ok"},
                         "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}],
                         "local_caster": lc(clients=1)}, admin=False)
    assert pub["cards"]["NTRIP"]["lines"] == ["Onocoy · Streaming"]



MON = {"running": True, "sample_s": 30, "span_s": 7200,
       "router": {"state": "ok", "ms": 3, "avg_ms": 4, "max_ms": 80, "sent": 240, "lost": 0},
       "internet": {"name": "1.1.1.1", "state": "ok", "ms": 15, "avg_ms": 20, "max_ms": 45, "sent": 240, "lost": 1},
       "server": {"name": "Onocoy", "state": "lost", "ms": None, "avg_ms": 117, "max_ms": 410, "sent": 240, "lost": 2},
       "rssi": {"now": -79, "avg": -78, "min": -84}, "cpu": {"now": 12, "avg": 10, "max": 40},
       "memory": {"free": 120000, "min": 98000}}
MON_SERIES = {"sample_s": 30, "bucket_s": 300, "buckets": 5, "samples": [10, 10, 10, 10, 3],
              "router_avg": [None, 4, 5, None, 2], "router_max": [None, 9, 8, None, 2], "router_lost": [None, 0, 1, None, 0],
              "internet_avg": [20, 21, None, 19, 20], "internet_max": [45, 30, None, 25, 22], "internet_lost": [0, 0, 1, 0, 0],
              "server_avg": [118, 117, 120, 119, 117], "server_max": [140, 130, 150, 125, 120], "server_lost": [0, 1, 0, 0, 0],
              "rssi_avg": [-79, -78, -80, -79, -78], "cpu_avg": [8, 9, 10, 9, 8], "cpu_max": [10, 20, 30, 40, 50],
              "mem_min": [98000, 98500, 99000, 99000, 98800]}


def test_status_performance_card():
    """The Performance card: per figure its name, its latest reading and the last 24 h in words (a ping without a reply
    in the warning colour), fine print for the time covered, one link to the
    Logs page; a line per figure over that time from the left edge to the right (a gap where a bucket has nothing, a lone
    value a dot (one bucket alone: a flat line), red marks where a ping got no reply, at least 10 ms of range so a millisecond's jitter stays flat). Before
    the first reading, and in safe mode, one muted line."""
    nb = " "
    (o, day, unnamed, waiting, none) = dashboards(over(BASE, monitor=MON), over(BASE, monitor={**MON, "span_s": 86100}),
                                                  over(BASE, monitor={**MON, "server": {**MON["server"], "name": ""}}),
                                                  over(BASE, monitor={**MON, "span_s": 0}), BASE, mon=MON_SERIES)
    card = o["cards"]["Performance"]
    assert card["chip"] is None and day["cards"]["Performance"]["chip"] is None
    # The time the figures cover, as fine print at the bottom.
    assert card["notes"] == [["Calculated from the 2h since the station started.", "muted small"]]
    assert day["cards"]["Performance"]["notes"] == [["Calculated from the last 24h.", "muted small"]]
    assert card["links"] == [["Details", "#/logs", "Details on the Logs page", True]] and card["buttons"] == 0
    assert card["perf"] == [
        ["Router ping", "3ms", "avg 4ms · worst 80ms · none lost"],
        ["Onocoy ping", "No reply", f"avg 117ms · worst 410ms · 2 lost (0.8%)"],
        ["Cloudflare ping", "15ms", "avg 20ms · worst 45ms · 1 lost (0.4%)"],
        ["Wi-Fi signal", " Weak", f"avg −78{nb}dBm · weakest −84{nb}dBm"],
        ["CPU", "12%", "avg 10% · peak 40%"],
        ["Free memory", f"117{nb}KB", f"lowest 96{nb}KB"],
    ]
    assert ["warn", "No reply"] in card["tones"]
    sparks = card["sparks"]
    assert [s["label"] for s in sparks] == ["Router ping over the same time", "Onocoy ping over the same time", "Cloudflare ping over the same time",
                                            "Wi-Fi signal over the same time", "CPU peaks over the same time",
                                            "Free memory over the same time"]
    assert all(s["viewBox"] == "0 0 4 32" for s in sparks)
    # Each line says its own scale (bottom to top) under it.
    assert card["scales"] == ["0 to 10ms", "0 to 120ms", "0 to 21ms", f"−80 to −70{nb}dBm", "0 to 100%", f"0{nb}B to 97{nb}KB"]
    # Router: buckets 1-2 joined, 3 empty, 4 alone (a dot); bucket 2 lost a ping; 0..10 ms (its 5 ms would fill it).
    assert sparks[0]["paths"] == [["spark-loss", "M2 0V32"], ["spark-line", "M1 18.6L2 16.0M4 23.8h0"]]
    # Cloudflare: independent latency and loss (bucket 2 has no replies).
    assert sparks[2]["paths"] == [["spark-loss", "M2 0V32"], ["spark-line", "M0 4.2L1 3.0M3 5.5L4 4.2"]]
    # CPU on 0..100 %.
    assert sparks[4]["paths"][-1] == ["spark-line", "M0 26.4L1 23.8L2 21.2L3 18.6L4 16.0"]
    assert unnamed["cards"]["Performance"]["perf"][1][0] == "Server ping"
    for x in (waiting, none):
        c = x["cards"]["Performance"]
        assert c["perf"] == [] and c["notes"] == [["Waiting for the first reading (every 30s).", "muted"]] and c["chip"] is None
    safe = matrix()["safe"]["cards"]["Performance"]
    assert safe["notes"] == [["Not available in safe mode.", "muted"]] and safe["perf"] == []
    # One bucket so far: a flat line across the box.
    (one,) = dashboards(over(BASE, monitor=MON), mon={**MON_SERIES, "buckets": 1, "router_avg": [4], "router_lost": [0]})
    assert one["cards"]["Performance"]["sparks"][0]["viewBox"] == "0 0 1 32"
    assert one["cards"]["Performance"]["sparks"][0]["paths"][-1] == ["spark-line", "M0 18.6H1"]
    # Without an enabled NTRIP target, the independent internet reading is still available.
    no_target = {"name": "", "state": "none", "ms": None, "avg_ms": None, "max_ms": None, "sent": 0, "lost": 0}
    (no_server,) = dashboards(over(MATRIX["nocasters"], monitor={**MON, "server": no_target}), mon=MON_SERIES)
    assert no_server["cards"]["Performance"]["perf"][2] == card["perf"][2]
    assert no_server["cards"]["Performance"]["perf"][1] == ["Server ping", "–", "No readings yet"]


def test_update_banner_shows_on_the_status_page_too():
    """An update shows as the "Update available" banner on every page but Settings (whose Updates card has it), the
    Status page included: its Station card only names the version. Its button says where it goes ("Settings page"):
    it opens the Updates card and starts nothing. A stopped Onocoy slot stays the Status page's own
    (its top card says it)."""
    st = over(BASE, update={"app_available": True, "app_version": "v0.1.5"})
    out = run_editor([{"fn": "notices", "status": st, "page": p} for p in ("", "ntrip", "settings")])
    update = [[n for n in o if n[0] == "update"] for o in out]
    assert [n[:3] for n in update[0]] == [["update", "Update available", "App v0.1.5."]]
    assert update[1] == update[0]
    assert update[0][0][3] == ["Later", "Settings page"]   # it opens the Updates card; it starts nothing
    assert update[2] == []
    stopped = over(BASE, casters=[st_caster(0, "Onocoy", "stopped", stop_reason="in_use"), RTK])
    (home, ntrip_page, logs) = run_editor([{"fn": "notices", "status": stopped, "page": p} for p in ("", "ntrip", "logs")])
    assert not [n for n in home if n[0].startswith("onocoy_stopped")]
    assert not [n for n in ntrip_page if n[0].startswith("onocoy_stopped")]
    assert [n[0] for n in logs if n[0].startswith("onocoy_stopped")] == ["onocoy_stopped:0"]


def test_ask_the_receiver_sends_the_chosen_command_and_shows_the_answer():
    """Receiver settings' Ask the receiver: the last answer as the receiver sent it, with what was asked and how it
    went; Ask posts the chosen command (VERSIONA first); the hint about the NTRIP servers' pause only for the commands
    that take the lease."""
    done = {"state": "done", "command": "CONFIG", "result": "answered", "reason": "",
            "reply": "$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 2*7A\n$CONFIG,MMP,CONFIG MMP ENABLE*1B", "cut": False,
            "lease_ms": 820, "age_s": 4, "ended_unix": None,
            "commands": [{"command": c, "pauses_servers": c in ("CONFIG", "ANT1GROUPA")}
                         for c in ("VERSIONA", "CONFIG", "MODE", "ANT1GROUPA", "BASEPOSA", "SPPNAVA")]}
    out = run_editor([rx_page("settings", st_status(), GNSS, SIG_MSGS, query=done, press="Ask"),
                      rx_page("settings", st_status(), GNSS, SIG_MSGS)])
    asked, fresh = out
    assert "CONFIG: Answered, 4s ago. The NTRIP servers paused for 820ms." in asked["lines"]
    assert asked["pres"] == ["$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 2*7A\n$CONFIG,MMP,CONFIG MMP ENABLE*1B"]
    assert "Copy" in asked["buttons"] and "Command" in asked["fields"]
    assert asked["pressed"] == [["POST", "/api/gnss/query", {"command": "VERSIONA"}]]
    assert "The NTRIP servers pause for about a second while it answers." not in asked["hints"]   # VERSIONA: no lease
    # Nothing asked yet: no answer box, no line.
    assert fresh["pres"] == [] and "Copy" not in fresh["buttons"] and "Ask" in fresh["buttons"]
    assert not any(t.startswith(("Asking", "VERSIONA:")) for t in fresh["lines"])


def test_ask_the_receiver_greys_out_a_command_the_receiver_build_lacks():
    """Ask the receiver: a command the receiver's build does not have (needs_build, from the station's table of builds)
    says which build has it, and Ask is greyed out while it is chosen; the other commands ask as before."""
    cmds = [{"command": c, "pauses_servers": c in ("CONFIG", "ANT1GROUPA"),
             "needs_build": 23980 if c == "ANT1GROUPA" else None}
            for c in ("VERSIONA", "CONFIG", "MODE", "ANT1GROUPA", "BASEPOSA", "SPPNAVA")]
    q = {"state": "none", "commands": cmds}
    old, other = run_editor([rx_page("settings", st_status(), GNSS, SIG_MSGS, query=q, pick="ANT1GROUPA"),
                             rx_page("settings", st_status(), GNSS, SIG_MSGS, query=q, pick="MODE")])
    hint = "This receiver firmware does not have it: build 23980 or later does."
    assert hint in old["picked"]["hints"] and old["picked"]["ask_disabled"]
    assert "The NTRIP servers pause for about a second while it answers." not in old["picked"]["hints"]
    assert hint not in other["picked"]["hints"] and not other["picked"]["ask_disabled"]


def test_receiver_update_that_changes_the_partition_layout_needs_its_own_tick():
    """Settings, Updates: an offered receiver build with another partition layout says so in the dialog, whose Update
    receiver stays greyed out until its own checkbox is ticked; the request then carries the acknowledgement. An app
    that cannot install such a build shows why instead of a button."""
    gnss = {**SET_UPDATE["gnss"], "model": "UM980", "running": "R4.10Build11833", "running_build": 11833,
            "latest": "R4.10Build23980", "latest_build": 23980, "available": True, "flash_available": True,
            "expected_downtime_s": 251, "latest_cross_layout": True, "cross_layout_allowed": True}
    update = {**SET_UPDATE, "gnss": gnss}
    refused = {**SET_UPDATE, "gnss": {**gnss, "cross_layout_allowed": False}}
    same = {**SET_UPDATE, "gnss": {**gnss, "latest_cross_layout": False}}
    base = {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI}
    xl, no, plain = run_editor([{**base, "update": update, "press": "Update receiver"},
                                {**base, "update": refused},
                                {**base, "update": same, "press": "Update receiver"}])
    warn = "This build changes the receiver's partition layout. If the update stops partway, the station may not be able to finish it on its own."
    assert warn in [t for _, t in xl["dialog"]["msgs"]]
    assert xl["dialog"]["ok_disabled"] and not xl["dialog"]["ok_disabled_ticked"]
    assert xl["posts"] == [["/api/update/gnss", {"build": 23980, "cross_layout_ack": True}]]
    assert "Update receiver" not in no["buttons"]
    assert "This build changes the receiver's partition layout, which this app version cannot install." in [t for _, t in no["msgs"]]
    # Same layout: no extra tick, no acknowledgement.
    assert not plain["dialog"]["ok_disabled"] and warn not in [t for _, t in plain["dialog"]["msgs"]]
    assert plain["posts"] == [["/api/update/gnss", {"build": 23980, "cross_layout_ack": False}]]


def test_new_app_first_checks_can_be_finished_early():
    """Settings, Updates: while a new app version's first checks hold receiver updates back, "Finish checks now"
    (beside the app line and the receiver's "Not possible right now") asks, then ends them; never "trial" in the text.
    The crash watch alone offers no such button."""
    app = {**SET_UPDATE["app"], "running": "v0.1.6", "latest": "v0.1.6",
           "trial": {"stage": "verifying", "left_s": 540, "ends_in_s": 86000}}
    gnss = {**SET_UPDATE["gnss"], "model": "UM980", "running": "R4.10Build23980M", "running_build": 23980,
            "latest": "R4.10Build25735", "latest_build": 25735, "available": True, "flash_available": True,
            "blocked": "app_trial", "blocked_left_s": 540, "latest_cross_layout": False, "cross_layout_allowed": True}
    checking = {**SET_UPDATE, "app": app, "gnss": gnss}
    watching = {**SET_UPDATE, "app": {**app, "trial": {"stage": "watching", "left_s": 86000}},
                "gnss": {**gnss, "blocked": "none", "blocked_left_s": 0}}
    base = {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI}
    on, off = run_editor([{**base, "update": checking, "press": "Finish checks now"}, {**base, "update": watching}])
    assert on["buttons"].count("Finish checks now") == 2
    assert "Not possible right now: the new app version is still being checked (about 9min left). Finish checks now" in \
        [t for _, t in on["msgs"]]
    assert on["dialog"]["title"] == "Finish the checks now?" if "title" in on["dialog"] else True
    assert on["posts"] == [["/api/update/accept", {"confirm": True}]]
    assert "Finish checks now" not in off["buttons"]
    texts = " ".join(t for _, t in on["msgs"]) + " ".join(on.get("lines", []))
    assert "trial" not in texts.lower()


def test_open_session_message_is_hidden_while_the_receiver_update_runs():
    """Settings, Updates: while this receiver update runs its own open session blocks a second one; "Not possible right
    now: a receiver update session is open" is not shown then (the steps already say what runs), only when it waits."""
    gnss = {**SET_UPDATE["gnss"], "model": "UM980", "running": "R4.10Build23980M", "running_build": 23980,
            "latest": "R4.10Build25735", "latest_build": 25735, "available": True, "flash_available": True,
            "blocked": "journal_open", "blocked_left_s": 0, "latest_cross_layout": False, "cross_layout_allowed": True}
    upd = {**SET_UPDATE, "gnss": gnss}
    base = {"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": upd}
    running = {"journal": "running", "progress": {"done": 696, "total": 2954}, "flash_available": True,
               "last": None, "actions": {}}
    waiting = {"journal": "waiting", "flash_available": True, "last": None, "actions": {}}
    on, off = run_editor([{**base, "flash": running}, {**base, "flash": waiting}])
    fin = {**running, "progress": {"phase": "finishing", "frames_acked": 2954, "frames_total": 2954}}
    xfer = {**running, "progress": {"phase": "transferring", "bytes_sent": 700000, "bytes_total": 3027628,
                                       "frames_acked": 696, "frames_total": 2954}}
    done, going = run_editor([{**base, "flash": fin}, {**base, "flash": xfer}])
    # The last step has no count to show: a moving bar and a line that says to wait.
    norm = lambda r: [[t, c.split()] for t, c in r["progress"]]
    assert ["Receiver update: Finishing, please wait. This can take a few minutes.", ["bar", "indet"]] in norm(done)
    assert ["Receiver update: Transferring 23% (block 696 of 2,954)", ["bar"]] in norm(going)
    said = lambda r: [t for _, t in r["msgs"]]
    assert not any(t.startswith("Not possible right now") for t in said(on))
    assert "Not possible right now: a receiver update session is open." in said(off)


def test_last_receiver_update_message_can_be_dismissed():
    """Settings, Updates: "Updated: the receiver runs ..." stays until the user dismisses it (its ×); then it is gone."""
    last = {"outcome": "success", "failed_rule": 0, "cross_layout": False, "check_only": False, "reapply_ok": True,
            "firmware": "R4.10Build11833", "build": 11833, "reason": "updated", "ended_unix": 1792678804,
            "ended_age_s": 4}
    flash = {"journal": "none", "flash_available": True, "last": last, "actions": {}}
    shown, = run_editor([{"fn": "settings_page", "status": st_status(), "wifi": SET_WIFI, "update": SET_UPDATE,
                          "flash": flash, "click": "×"}])
    said = [t for _, t in shown["msgs"]]
    assert "Updated: the receiver runs R4.10Build11833.×" in said
    assert not any(t.startswith("Updated:") for _, t in shown["after"]["msgs"])


def test_onocoy_statistics_are_an_explicit_opt_in():
    bodies = run_editor([
        case(0, ONOCOY_SET_UP, []),
        case(0, ONOCOY_SET_UP, [("stats", True)]),
        case(0, {**ONOCOY_SET_UP, "onocoy_stats": True}, []),
        case(0, {**ONOCOY_SET_UP, "onocoy_stats": True}, [("stats", False)]),
    ])
    assert [b["onocoy_stats"] for b in bodies] == [False, True, True, False]
    opts = run_editor([
        {**case(0, ONOCOY_SET_UP, []), "fn": "stats_option"},
        {**case(0, {**ONOCOY_SET_UP, "onocoy_stats": True}, []), "fn": "stats_option"},
        {**case(1, stored("rtk2go.com", "polite"), []), "fn": "stats_option"},
    ])
    assert opts[0]["checked"] is False and opts[0]["shown"] is True
    assert opts[1]["checked"] is True
    assert opts[2]["shown"] is False
    assert "Uses these Onocoy station credentials to download" in opts[0]["notes"][0]
    assert "Refreshes daily" in opts[0]["notes"][0]


def test_onocoy_report_is_optional_and_dated_separately_from_local_estimates():
    status = over(BASE, casters=[{**BASE["casters"][0], "onocoy_stats": True}, *BASE["casters"][1:]])
    report = {"enabled": True, "busy": False, "state": "ok", "checked_unix": 1793016000, "manual_in_s": 0,
              "days": [{"date": "2026-10-25", "uptime_s": 86400, "latency_avg": 0.12, "latency_max": 0.4, "rewards": 1.25},
                       {"date": "2026-10-23", "rewards": 0.75}, {"date": "2026-10-22", "rewards": 2}]}
    out = run_editor([{"fn": "dashboard", "admin": True, "status": status, "report": report},
                      {"fn": "dashboard", "admin": True, "status": BASE, "report": report},
                      {"fn": "dashboard", "admin": True, "status": over(status, casters=[{**status["casters"][0], "enabled": False}]), "report": report},
                      {"fn": "dashboard", "admin": True, "status": status, "report": {**report, "days": report["days"][:1]}},
                      {"fn": "dashboard", "admin": True, "status": status, "report": {**report, "days": [
                          {**report["days"][0], "rewards": None}, {**report["days"][1], "rewards": 0}]}}])
    c = out[0]["cards"]["Onocoy API report"]
    assert out[0]["order"].index("Onocoy API report") == out[0]["order"].index("Onocoy") + 1
    assert [r[0] for r in c["rows"]] == ["Online time", "Data latency", "Latest rewards", "Previous rewards"]
    assert c["rows"][1][1] == "avg 0.12s · worst 0.4s"
    assert c["rows"][2:] == [["Latest rewards", "1.25 ONO"], ["Previous rewards", "0.75 ONO"]]
    assert out[3]["cards"]["Onocoy API report"]["rows"][3] == ["Previous rewards", "–"]
    assert out[4]["cards"]["Onocoy API report"]["rows"][2:] == [["Latest rewards", "–"], ["Previous rewards", "0 ONO"]]
    assert len(c["notes"]) == 1
    meta = c["notes"][0][0]
    assert "Onocoy" in meta and "2026" in meta and "updates daily" in meta
    assert len(meta.split()) <= 10 and meta in c["nowrap"]
    assert c["buttons"] == 0
    assert "Last checked" not in c["text"] and "Refresh in" not in c["text"]
    assert out[0]["cards"]["NTRIP"]["wide"] is True
    assert out[1]["cards"]["NTRIP"]["wide"] is False
    assert "Onocoy API report" not in out[1]["cards"] and "Onocoy API report" not in out[2]["cards"]


def test_onocoy_report_refusal_explains_account_access_and_retry_wait():
    status = over(BASE, casters=[{**BASE["casters"][0], "onocoy_stats": True}, *BASE["casters"][1:]])
    refused = {"enabled": True, "busy": False, "state": "auth", "http_status": 403,
               "checked_unix": None, "manual_in_s": 300, "days": []}
    out = run_editor([{"fn": "dashboard", "admin": True, "status": status, "report": r}
                      for r in (refused, {**refused, "manual_in_s": 0})])
    card = out[0]["cards"]["Onocoy API report"]
    assert "Onocoy Settings → API access" in card["text"]
    assert "turn on API access so these station credentials can download reports" in card["text"]
    assert "Refresh in 5min" in card["text"]
    assert any(link[1] == "https://console.onocoy.com/settings/api-access" for link in card["links"])
    assert "Refresh in" not in out[1]["cards"]["Onocoy API report"]["text"]


def test_onocoy_retry_counts_down_without_fetching_or_rebuilding():
    (r,) = run_editor([{"fn": "report_countdown", "report": {"state": "auth", "busy": False,
                       "manual_in_s": 35, "days": []}, "elapsed": [0, 1, 34, 35, 40]}])
    assert r["states"] == [["Refresh in 35s", True], ["Refresh in 34s", True], ["Refresh in 1s", True],
                           ["Refresh", False], ["Refresh", False]]
    assert r["requests"] == 0
