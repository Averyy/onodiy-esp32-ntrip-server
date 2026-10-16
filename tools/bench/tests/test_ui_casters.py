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
bubble up through the parents there) and Diagnostics' "Clear journal"
against the station's answers to it. Skipped when Node.js is not
installed.
"""

from __future__ import annotations

import json
import shutil
import subprocess
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

class Node {}
class Text extends Node {
  constructor(t) { super(); this.nodeType = 3; this.data = t; }
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
      n.parent = this;
      this.kids.push(n);
    }
  }
  appendChild(k) { this.append(k); return k; }
  prepend(...kids) { this.kids.unshift(...kids.map(k => (k instanceof Node ? k : new Text(String(k))))); }
  replaceChildren(...kids) { this.kids = []; this.append(...kids); }
  remove() {}
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
  set textContent(t) { this.kids = t === '' || t === null || t === undefined ? [] : [new Text(String(t))]; }
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
              querySelectorAll: () => [], documentElement: new El('html'), body: new El('body'), hidden: false,
              visibilityState: 'visible' },
  window: { addEventListener: noop, removeEventListener: noop,
            matchMedia: () => ({ matches: false, addEventListener: noop }), confirm: () => true,
            scrollTo: noop },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
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
  '\nreturn { casterEditor, openCasterWarning, casterDetails, casterSummary, casterChip, stationState, adminDashboard,\n' +
  '  publicDashboard, noReceiverPanel, startWizard, manageCtl, eventCard, healthCard, chipCard, signalsHead, bandTable, kv,\n' +
  '  setStatus: s => { S.status = s; }, setSession: s => { S.session = s; } };\n})()',
  ctx);

/* The elements on screen: hidden ones and everything inside them left out. */
function* visible(e) {
  if (e.hidden) return;
  yield e;
  for (const c of e.kids) if (c instanceof El) yield* visible(c);
}

/* What the editor shows: its heading, the field names in the main part and under "More options", Onocoy's server
 * line, the box (open, or folded under its summary) and whether a retry pace shows. */
function inspect(root) {
  const vis = [...visible(root)];
  const text = e => e.textContent;
  const details = vis.filter(e => e.tagName === 'DETAILS');
  const more = details.find(e => text(e.kids[0]) === 'More options');
  const folds = details.filter(e => e !== more);
  const inside = els => new Set(els.flatMap(e => [...visible(e)]));
  const inMore = inside(more ? [more] : []), inFold = inside(folds);
  const names = els => els.filter(e => e.tagName === 'LEGEND' || (e.tagName === 'SPAN' && e.className === 'lbl') ||
    (e.tagName === 'LABEL' && e.className === 'check')).map(e => (e.tagName === 'LABEL' ? text(e.kids[1].kids[0]) : text(e)));
  const main = vis.filter(e => !inMore.has(e) && !inFold.has(e));
  const server = main.find(e => e.tagName === 'P' && text(e).startsWith('Server: '));
  return {
    title: text(vis.find(e => e.tagName === 'H2')),
    main: names(main),
    more: names([...inMore]),
    server: server ? text(server) : null,
    box: main.some(e => e.tagName === 'DIV' && e.className === 'preset'),
    folded: folds.map(e => text(e.kids[0])),
    pace: vis.some(e => e.kids.some(k => k instanceof Text && k.data.indexOf('Retry pace') >= 0)),
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
    password: all.find(e => e.tagName === 'INPUT' && e.attrs.type === 'password'),
    name: all.find(e => e.tagName === 'INPUT' && e.attrs.maxlength === '23'),
    more: all.find(e => e.tagName === 'DETAILS' && e.kids[0].textContent === 'More options'),
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

/* Receiver management from (autocfg, recovery) to the choice `to`: the PUT bodies in the order sent; fail: the
 * member whose request the station refuses (500). */
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
  for (const r of radios) r.checked = r.value === c.to;
  radios.find(r => r.value === c.to).fire('change');
  all.find(e => e.tagName === 'BUTTON' && e.textContent === 'Save').fire('click');
  await settle();
  routes = null;
  return sent.filter(x => x.method === 'PUT').map(x => x.body);
}

/* Diagnostics' event card with two records: "Clear journal…", confirmed, answered with c.answer ([status, body]).
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

/* The Diagnostics Health card for (hl, status): the chip [text, class], the messages' titles and lines, the rows. */
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

/* The Receiver page's Signals card for c.sig (GET /api/gnss/signals) while corrections come (c.live): its headline, the
 * hints (tone, text), the words only assistive technology reads, and whether the band table shows. */
function signalsView(c) {
  const el = new El('div');
  el.append(...ui.signalsHead(c.sig, c.live).filter(Boolean), ui.bandTable(c.sig.systems));
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
  if (c.fn === 'health') {
    out.push(healthView(c));
    continue;
  }
  if (c.fn === 'signals') {
    out.push(signalsView(c));
    continue;
  }
  if (c.fn === 'chip') {
    const dl = [...ui.chipCard(c.sys, c.status, c.wifi, c.net).walk()].find(e => e.tagName === 'DL');
    const pairs = [];
    for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
    out.push(pairs);
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
    const parts = ui.casterSummary(Object.assign({ slot: 0 }, c.cfg), c.st, { port: 2101 }).filter(Boolean);
    out.push({ address: parts[0].textContent, chip: ui.casterChip(c.st) });
    continue;
  }
  if (c.fn === 'dashboard') {
    /* The Status page as built from a status: the top card's title, line and short line, its facts, and per card
     * (by title) the label/value rows and the one-line casters. */
    const st = ui.stationState(c.status, c.admin);
    const kids = (c.admin ? ui.adminDashboard(c.status, st, c.open || []) : ui.publicDashboard(c.status, st, null)).filter(Boolean);
    const all = kids.flatMap(k => [...visible(k)]);
    const pairs = v => v.filter(e => e.tagName === 'DL').flatMap(dl => {
      const r = [];
      for (let i = 0; i + 1 < dl.kids.length; i += 2) r.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
      return r;
    });
    const hero = all.find(e => e.tagName === 'SECTION' && e.className.indexOf('hero') >= 0);
    const hv = [...visible(hero)];
    const cards = {};
    for (const sec of all.filter(e => e.tagName === 'SECTION' && e !== hero)) {
      const v = [...visible(sec)];
      const bar = v.find(e => (e.className || '').indexOf('bar') === 0);
      cards[v.find(e => e.tagName === 'H2').textContent] = { rows: pairs(v),
        lines: v.filter(e => e.className === 'cline').map(e => e.kids[0].textContent),
        bar: bar ? { cls: bar.className, role: bar.attrs.role, label: bar.attrs['aria-label'], now: bar.attrs['aria-valuenow'],
          text: bar.attrs['aria-valuetext'] || null, width: bar.kids[0].style.width } : null };
    }
    const plan = hv.find(e => e.tagName === 'P' && e.className === 'muted small');
    out.push({ head: hv.find(e => e.tagName === 'H2').textContent, line: hv.find(e => e.className === 'line').textContent,
      plan: plan ? plan.textContent : null, facts: pairs(hv), cards: cards });
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
  if (c.fn === 'details') {
    ui.setStatus(c.uptime === undefined ? null : { uptime_s: c.uptime });
    const kids = ui.casterDetails(c.st, c.reconnects === null ? undefined : c.reconnects).filter(Boolean);
    const all = kids.flatMap(k => [...k.walk()]);
    const pairs = [];
    for (const dl of all.filter(e => e.tagName === 'DL')) {
      for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
    }
    out.push({ rows: pairs, titles: all.filter(e => e.tagName === 'H3').map(e => e.textContent),
      notes: all.filter(e => e.tagName === 'P').map(e => e.textContent) });
    continue;
  }
  sent.length = 0;
  const ed = ui.casterEditor({ slot: c.slot, config: c.config }, Object.assign({ preset: {} }, c.opts || {}));
  const f = controls(ed.el);
  for (const [what, v] of c.steps) {
    if (what === 'caster' || what === 'pace') {
      f[what].value = v;
      f[what].fire('change');
    } else {
      f[what].value = v;
      f[what].fire('input');
    }
  }
  if (c.inspect) {
    out.push(inspect(ed.el));
    continue;
  }
  ed.el.fire('submit');
  const put = sent.find(s => s.method === 'PUT');
  out.push(put ? put.body : { errors: f.errors(), more_open: !!f.more.open, title: f.title.textContent });
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
    """A preset whose pace the owner sets (Centipede, Emlid, RTKDirect show it under More options) set nothing in
    this editor: switching to Other caster keeps the owner's pace, stored or picked here."""
    out = run_editor([
        # The stored Centipede slot on Polite, switched to Other caster.
        case(2, stored("crtk.net", "polite"), [("caster", "custom")] + OTHER),
        # The same with Emlid and RTKDirect.
        case(2, stored("caster.emlid.com", "polite"), [("caster", "custom")] + OTHER),
        case(2, stored("ntrip.rtkdirect.com", "polite", ntrip_version=2, username="me"), [("caster", "custom")] + OTHER),
        # A new slot: Centipede, Polite picked under More options, then Other caster.
        case(1, None, [("caster", "centipede"), ("pace", "polite"), ("caster", "custom")] + OTHER, ONOCOY_ELSEWHERE),
        # The stored Emlid slot on Polite through RTK2go (which fixes polite) to Other: the stored pace comes back.
        case(2, stored("caster.emlid.com", "polite"), [("caster", "rtk2go"), ("caster", "custom")] + OTHER),
        # Polite picked on Other, then a preset with its own pace, then Other again: the owner's pace comes back.
        case(1, None, [("caster", "custom"), ("pace", "polite"), ("caster", "centipede"), ("caster", "custom")] + OTHER,
             ONOCOY_ELSEWHERE),
    ])
    assert [b.get("backoff_profile") for b in out] == ["polite"] * 6, out


def test_other_caster_drops_the_pace_a_preset_set():
    """RTK2go's polite (set by the preset, or fixed for a stored RTK2go slot) is not kept on Other caster: back to the
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


def test_a_refused_field_under_more_options_opens_it():
    """Name, host and port of a preset wait under "More options": a field refused there opens it first, so the
    error is on screen; one in the main part leaves it closed. A new caster's heading is "New caster", whichever
    caster is chosen."""
    out = run_editor([
        case(1, None, [("caster", "centipede"), ("name", "N" * 30)] + NEW, ONOCOY_ELSEWHERE),
        case(1, None, [("caster", "centipede"), ("password", "secret")], ONOCOY_ELSEWHERE),
    ])
    assert out[0]["errors"] == ["Too long: use a shorter name."] and out[0]["more_open"] is True, out[0]
    assert out[1]["errors"] == ["Enter the mountpoint."] and out[1]["more_open"] is False, out[1]
    assert out[0]["title"] == out[1]["title"] == "New caster"


ONOCOY_SET_UP = stored("servers.onocoy.com", "fast", name="Onocoy", mountpoint="MP0TEST1", username="user0test")
# Another slot holds Onocoy, not set up: a new slot may still be switched to Onocoy.
ONOCOY_NOT_SET_UP_ELSEWHERE = {"onocoyElsewhere": {"slot": 0, "ready": False, "enabled": False}}


def test_onocoy_editor_shows_the_server_as_a_line_and_little_else():
    """Onocoy asks for what its console gives: the server as one line of text (no inputs that look editable),
    mountpoint, username, password. No retry pace; the NTRIP version and removing the stored password wait under
    "More options" (TLS only while it is on). A test slot shows its own server."""
    out = run_editor([
        case(0, ONOCOY_SET_UP, [], inspect=True),
        case(0, None, [], inspect=True),
        case(1, None, [("caster", "onocoy")], ONOCOY_NOT_SET_UP_ELSEWHERE, inspect=True),
        case(0, dict(ONOCOY_SET_UP, tls=True, port=2121), [], inspect=True),
        case(0, stored("10.0.2.2", "fast", name="Onocoy", port=2111, mountpoint="MP0TEST1", username="user0test",
                       test_onocoy=True), [], inspect=True),
    ])
    edit, new, switched, tls, test = out
    assert edit["title"] == "Edit Onocoy" and new["title"] == switched["title"] == "New caster"
    for o in out:
        assert o["main"] == ["Caster", "Mountpoint", "Username", "Password", "Stream to this caster"], o
        assert o["pace"] is False, o
        assert "Host" not in o["more"] and "Port" not in o["more"] and "Name" not in o["more"], o
    assert edit["server"] == new["server"] == switched["server"] == "Server: servers.onocoy.com:2101"
    assert tls["server"] == "Server: servers.onocoy.com:2121" and test["server"] == "Server: 10.0.2.2:2111"
    assert edit["more"] == ["NTRIP version", "Remove the stored password"], edit
    assert new["more"] == switched["more"] == ["NTRIP version"], new
    assert tls["more"] == ["NTRIP version", "Use TLS (encrypted connection)", "Remove the stored password"], tls


def test_the_box_folds_once_the_caster_is_set_up():
    """A new caster, or one without its credentials, shows its box open; a stored caster that is set up shows it
    folded under "How to set up <caster>", until another caster is chosen."""
    out = run_editor([
        case(0, None, [], inspect=True),
        case(0, ONOCOY_SET_UP, [], inspect=True),
        case(0, dict(ONOCOY_SET_UP, username="", password_set=False), [], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [("caster", "centipede")], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go"), [("caster", "centipede"), ("caster", "rtk2go")], inspect=True),
        case(1, stored("rtk2go.com", "polite", name="RTK2go", password_set=False), [], inspect=True),
    ])
    assert [(o["box"], o["folded"]) for o in out] == [
        (True, []), (False, ["How to set up Onocoy"]), (True, []), (False, ["How to set up RTK2go"]), (True, []),
        (False, ["How to set up RTK2go"]), (True, [])], out
    # Other casters keep their server fields; RTK2go's fixed pace is one line under More options.
    assert out[4]["server"] is None and "Host" in out[4]["more"] and out[3]["pace"] is True


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


def details(st: dict, reconnects: int | None = None) -> dict:
    return {"fn": "details", "st": st, "reconnects": reconnects}


QUIET = {"state": "streaming", "state_age_s": 600, "last_status_line": "ICY 200 OK", "last_status_code": 200,
         "epochs_dropped_today": 0, "epochs_sent_today": 600, "lag_ms_now": 40, "max_lag_ms": 1000,
         "bytes_sent_today": 1200000, "resolved_ip": "203.0.113.7", "dns_cached_used": 0, "login_rejects": 0,
         "login_blocks": 0, "kicked_sessions": 0, "last_session_text": "", "last_err": None, "data_rate_permille": None,
         "failures_by_class": {k: 0 for k in ("dns", "connect", "tls", "timeout", "remote_close", "write_stall", "keepalive",
                                              "auth", "mount_busy", "config", "banned", "login_blocked", "in_use",
                                              "replaced", "local")}}


def test_connection_details_list_only_what_happened():
    """Connection details: at most 12 rows, each only when it has something to say (the ones that count trouble only
    when there is some), and the failures by cause only for causes that happened (no table of zeros)."""
    quiet, rough, waiting = run_editor([
        details(QUIET, 2),
        details(dict(QUIET, state="backoff", login_rejects=1, kicked_sessions=1, data_rate_permille=974,
                     last_session_text="ERROR - Bad Password", last_err="ESP_FAIL",
                     failures_by_class=dict(QUIET["failures_by_class"], timeout=3, dns=1)), None),
        # Never reached anything this start: no reply, no address, no lag.
        details(dict(QUIET, state="wait_data", last_status_line="", last_status_code=0, resolved_ip=None, lag_ms_now=0,
                     max_lag_ms=0, bytes_sent_today=0), 0),
    ])
    assert [r[0] for r in quiet["rows"]] == ["State since", "Last reply", "Epochs dropped today",
                                             "Lag now / highest", "Reconnects today", "Sent today", "Address"], quiet
    assert dict(quiet["rows"])["Reconnects today"] == "2" and quiet["titles"] == []
    labels = [r[0] for r in rough["rows"]]
    # The firmware's error code ("ESP_FAIL") is never a row: the causes below say it in words.
    assert labels[:11] == ["State since", "Last reply", "Caster said during a session", "Refused logins / login blocks",
                           "Sessions taken over (score)", "Data rate", "Epochs dropped today", "Lag now / highest",
                           "Reconnects today", "Sent today", "Address"], labels
    assert rough["titles"] == ["Failures by cause"]
    assert rough["rows"][11:] == [["DNS lookup failed", "1"], ["Timed out", "3"]], rough["rows"]
    assert dict(rough["rows"])["Reconnects today"] == "–"
    assert [r[0] for r in waiting["rows"]] == ["State since", "Epochs dropped today", "Reconnects today", "Sent today"], waiting
    # The RAM counts start at a restart: one muted line says so, only when the station restarted today (UTC).
    restarted, ran = run_editor([dict(details(QUIET, 2), uptime=1), dict(details(QUIET, 2), uptime=200 * 86400)])
    assert restarted["notes"] == ["Dropped epochs and data sent count from the last restart."], restarted
    assert ran["notes"] == [] and quiet["notes"] == []


# ---- The caster cards' address line and chip; the Status page's receiver rows and caster lines ----

def test_caster_card_shows_host_and_mountpoint_and_one_chip():
    """The card's line says where it streams (host and mountpoint), not the protocol; a test slot says so in that
    line, not in a second chip; a stopped slot's chip says "Needs you" (its panel says why)."""
    out = run_editor([
        {"fn": "card", "cfg": stored("rtk2go.com", "polite", ntrip_version=2, tls=True), "st": {"state": "streaming"}},
        {"fn": "card", "cfg": stored("10.0.2.2", "fast", port=2111, mountpoint="MP0TEST1", test_onocoy=True),
         "st": {"state": "stopped", "stop_reason": "rejected"}},
        {"fn": "card", "cfg": stored("servers.onocoy.com", "fast", mountpoint=""), "st": {"state": "wait_data"}},
    ])
    assert out[0] == {"address": "rtk2go.com/MINE01", "chip": ["Streaming", "ok"]}
    assert out[1] == {"address": "10.0.2.2/MP0TEST1 \u00b7 Test Onocoy slot (bench build)", "chip": ["Needs you", "bad"]}
    assert out[2] == {"address": "servers.onocoy.com", "chip": ["Waiting for the receiver", "warn"]}


def st_caster(slot: int, name: str, state: str, **kw) -> dict:
    """A caster of GET /api/status (Onocoy set up and counting)."""
    c = {"slot": slot, "name": name, "empty": False, "enabled": state != "disabled", "protected": name == "Onocoy",
         "host": name.lower() + ".example", "state": state, "reason": "none", "next_retry_s": 0, "stop_reason": "none",
         "onocoy_ready": name == "Onocoy", "today_online_s": 600, "today_offline_s": 0}
    if name == "Onocoy":
        c.update({"streak_ok": True, "budget_s": 473, "budget_left_s": 463, "data_rate_permille": 1000, "connected_s": 600})
    c.update(kw)
    return c


def st_status(**kw) -> dict:
    s = {"api": 1, "public": False, "device": {"hostname": "onocoydiy", "settings_available": True},
         "safe_mode": {"active": False}, "network": {"online": True},
         "receiver": {"state": "streaming", "model": "UM982", "rtcm_age_ms": 180, "journal": "none", "best_sys": "galileo",
                      "best_bands": 4, "glonass": True, "rtcm1230_supported": True, "position_locked": False,
                      "surveying": True, "survey_window_left_s": 300},
         "casters": [st_caster(0, "Onocoy", "streaming")],
         "health": {"level": "ok", "last_step": "none", "recovery_mode": "full"},
         "alerts": [], "availability": {"protected_down_today_s": 0, "budget_left_s": 463}}
    for k, v in kw.items():
        s[k] = {**s[k], **v} if isinstance(v, dict) and isinstance(s.get(k), dict) else v
    return s


def test_status_page_says_each_fact_once():
    """No receiver: one plain line on top, the next search and the restart in one short line, and no band or GLONASS
    rows (nothing to say) while the survey reads paused. A caster the top card names shows its name and today's share
    only, with no count of streaming casters beside it. The public page waits for the receiver in plain words."""
    hold = {"reboot_hold": ["admin"], "reboot_hold_left_s": 1578}
    no_rx = st_status(receiver={"state": "no_receiver", "rtcm_age_ms": None},
                      alerts=[{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 23}],
                      health={"level": "recovering", "last_step": "rescan", **hold},
                      casters=[st_caster(0, "Onocoy", "wait_data")])
    stall = st_status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"})
    partly = st_status(casters=[st_caster(0, "Onocoy", "streaming"),
                                st_caster(1, "Bench", "auth_failed", reason="auth", next_retry_s=50, today_online_s=0,
                                          today_offline_s=20)])
    public = {"api": 1, "public": True, "device": {"hostname": "onocoydiy"}, "receiver": {"state": "no_receiver"},
              "casters": [{"slot": 0, "name": "Onocoy", "state": "wait_data"}, {"slot": 1, "name": "Bench", "state": "disabled",
                                                                               "enabled": False}],
              "health": {"level": "recovering"}}
    out = run_editor([{"fn": "dashboard", "status": x, "admin": True} for x in (no_rx, stall, st_status(), partly)] +
                     [{"fn": "dashboard", "status": public, "admin": False}])
    assert out[0]["line"] == "Nothing arrives from the receiver. Check its power and the COM1 cable."
    assert out[0]["plan"] == "Next search in 23\u00a0s \u00b7 station restart in 27\u00a0min at the latest"
    assert out[0]["cards"]["Receiver"]["rows"] == [["Position", "Surveying, paused"]]
    assert out[1]["head"] == "Receiver stopped sending data"
    assert out[1]["cards"]["Receiver"]["rows"] == [["Position", "Surveying, paused"]]
    assert [r[0] for r in out[2]["cards"]["Receiver"]["rows"]] == ["Bands", "GLONASS", "Position"]
    assert out[2]["cards"]["Receiver"]["rows"][2] == ["Position", "Surveying, 5\u00a0min left"]
    assert out[3]["line"] == "Bench is not streaming: login refused. Next try in 50\u00a0s."
    # The caster the top card names keeps its one-word state in its line (the top card says why and when).
    assert out[3]["cards"]["Other casters"]["lines"] == ["Bench \u00b7 Login refused \u00b7 0\u00a0% today"]
    assert out[3]["facts"] == []
    assert out[4]["cards"]["Casters"]["lines"] == ["Onocoy \u00b7 Waiting for the receiver", "Bench \u00b7 Off"]


def test_status_quality_said_once_and_closed_port_panel():
    """While the top card says what Onocoy needs (too few bands), the Bands row keeps its mark without repeating it;
    another top card leaves the row's note. A closed receiver port: the panel says so, without the cable advice and
    without "Check again" (the station keeps reopening its port)."""
    few = st_status(receiver={"best_bands": 3, "best_sys": "gps"})
    few_down = st_status(receiver={"best_bands": 3, "best_sys": "gps"},
                         casters=[st_caster(0, "Onocoy", "streaming"), st_caster(1, "Bench", "auth_failed", reason="auth")])
    out = run_editor([{"fn": "dashboard", "status": x, "admin": True} for x in (few, few_down)] + [
        {"fn": "norx", "status": {"receiver": {"state": "no_receiver", "uart_closed": True}, "alerts": []}},
        {"fn": "norx", "status": {"receiver": {"state": "no_receiver", "next_detect_in_s": 90},
                                  "alerts": [{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 90}]}}])
    assert out[0]["head"] == "Too few bands for Onocoy"
    assert dict(out[0]["cards"]["Receiver"]["rows"])["Bands"] == "3 bands on GPS \u2715"
    assert out[1]["head"] == "Partly streaming"
    assert dict(out[1]["cards"]["Receiver"]["rows"])["Bands"] == "3 bands on GPS \u2715Onocoy needs 4."
    assert out[2] == ["Receiver port closed", "The station cannot open its port to the receiver and keeps trying.", False]
    assert out[3][0] == "No receiver found" and "cable" in out[3][1] and out[3][2] is True


def test_setup_onocoy_step_sends_the_duplicate_stream_answer():
    """The setup's Onocoy step while RTK2go is on: the station asks about the duplicate stream; ticking the box (its
    input event reaches the form) keeps the question with "Save anyway" enabled, and that sends the answer."""
    (r,) = run_editor([{"fn": "wizard_dup"}])
    assert r == {"asked": True, "kept": True, "enabled": True, "puts": [False, True]}, r


def test_receiver_management_saves_its_two_settings_in_a_safe_order():
    """The station stores automatic configuration and the recovery one after the other, so the choice sends one per
    request: the recovery first when automatic configuration goes off, else automatic configuration first. A refusal
    halfway never leaves automatic recovery on for a receiver the station does not configure."""
    out = run_editor([{"fn": "manage", "autocfg": True, "recovery": "full", "to": "me"},
                      {"fn": "manage", "autocfg": False, "recovery": "alerts_only", "to": "station"},
                      {"fn": "manage", "autocfg": True, "recovery": "full", "to": "no_restart"},
                      {"fn": "manage", "autocfg": True, "recovery": "full", "to": "me", "fail": "autocfg"},
                      {"fn": "manage", "autocfg": False, "recovery": "alerts_only", "to": "station", "fail": "recovery"}])
    assert out[0] == [{"recovery": "alerts_only"}, {"autocfg": False}]
    assert out[1] == [{"autocfg": True}, {"recovery": "full"}]
    assert out[2] == [{"recovery": "no_reboot"}]
    # Refused halfway: the recovery is already off (to "me"), automatic configuration already on (to "station").
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
    # The clear could not start: nothing was cleared, the list stays as it is.
    r = journal_clear(500, internal("ESP_FAIL"))
    assert r["toasts"] == [] and not r["reread"]
    assert r["error"] == "Nothing was cleared: the journal flash could not be written."
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


# ---- Status: Onocoy today's bar; Diagnostics: the Health card and Chip and Wi-Fi; Signals; long values; plain port ----

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
    bars = [o["cards"]["Onocoy today"]["bar"] for o in out]
    assert bars[0] == {"cls": "bar ok", "role": "meter", "label": "Today's offline time left", "now": "97",
                       "text": "97\u00a0% left", "width": "97.9%"}, bars[0]
    # The screenshots' "On track, 7 min 31 s left": nearly full, not nearly empty.
    assert bars[1]["width"] == "95.3%" and bars[1]["cls"] == "bar ok"
    assert bars[2]["width"] == "36.6%" and bars[2]["cls"] == "bar warn" and bars[2]["text"] == "36\u00a0% left"
    # Missed: empty and red, whatever offline time the budget still shows.
    assert bars[3] == {**bars[3], "width": "0.0%", "cls": "bar bad", "now": "0", "text": "0\u00a0% left"}
    assert bars[4]["width"] == "0.0%" and bars[4]["cls"] == "bar bad"


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
    assert stop["buttons"] == [["What to do", "#/casters"]]
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


def test_chip_card_names_the_access_point_and_the_transmit_power():
    """Chip and Wi-Fi: the network and signal in one row; the access point with its channel and the transmit power
    in one row each, only while known and online (QEMU's link has neither)."""
    status = st_status(network={"online": True, "ssid": "Home", "rssi": -60})
    wifi = {"ssid": "Home", "online": True, "rssi": -60, "channel": 11, "bssid": "AA:BB:CC:DD:EE:FF", "tx_power_dbm": 19.5}
    qemu = dict(wifi, channel=None, bssid=None, tx_power_dbm=None)
    off = st_status(network={"online": False, "ssid": "Home", "offline_s": 30})
    sys = {"chip": {"model": "ESP32-S3", "revision": "v0.2", "flash_mb": 16, "psram_mb": 8}, "idf": "v6.1", "mac": "11:22:33:44:55:66"}
    full, bare, gone = run_editor([{"fn": "chip", "sys": sys, "status": status, "wifi": wifi, "net": {}},
                                   {"fn": "chip", "sys": sys, "status": status, "wifi": qemu, "net": {}},
                                   {"fn": "chip", "sys": sys, "status": off, "wifi": wifi, "net": {}}])
    rows = dict(full)
    assert [r[0] for r in full] == ["Chip", "ESP-IDF", "Wi-Fi", "Access point", "Transmit power", "MAC address"]
    assert rows["Access point"] == "AA:BB:CC:DD:EE:FF \u00b7 channel 11" and rows["Transmit power"] == "19.5\u00a0dBm"
    assert "channel" not in rows["Wi-Fi"]
    assert [r[0] for r in bare] == [r[0] for r in gone] == ["Chip", "ESP-IDF", "Wi-Fi", "MAC address"]


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
    good, few, idle = run_editor([{"fn": "signals", "live": True, "sig": SIG}, {"fn": "signals", "live": True, "sig": three},
                                  {"fn": "signals", "live": False, "sig": SIG}])
    assert good["big"] == "Galileo \u00b7 4 bands" and good["table"] and good["lines"] == ["checks lines"]
    assert good["checks"] == [["ok", "\u2713Good: 40 satellites tracked"], ["ok", "\u2713Good: Strong signals (42\u00a0dB\u2011Hz)"]], good
    assert not any("band" in c[1] or "Galileo" in c[1] for c in good["checks"])
    assert good["words"] == ["Good: ", "Good: "]
    assert few["big"] == "GPS \u00b7 3 bands"
    assert few["checks"][-1] == ["warn", "!Check: Onocoy needs 4 bands: signal group 3 6 adds Galileo E6"], few
    # No corrections from the receiver: no verdicts, the headline and the table only.
    assert idle["checks"] == [] and idle["big"] == "Galileo \u00b7 4 bands" and idle["table"]


def test_long_values_wrap_at_their_separators():
    """A long token in a label/value row wraps at its separators (a <wbr>, shown as "|"), never mid-word; short values
    and words are left alone. On a phone the wide cards' labels keep to 40 % of the row, as everywhere else."""
    (vals,) = run_editor([{"fn": "kv", "rows": [["Expected firmware", "R4.10Build26000"],
                                                ["Access point", "AA:BB:CC:DD:EE:FF \u00b7 channel 11"],
                                                ["Server", "servers.onocoy.com:2101"], ["Network", "Home"],
                                                ["Signal", "-61 dBm"], ["Note", "Remove the stored password"]]}])
    assert vals == ["R4.10|Build26000", "AA:|BB:|CC:|DD:|EE:|FF \u00b7 channel 11", "servers.|onocoy.|com:|2101", "Home", "-61 dBm",
                    "Remove the stored password"]
    css = (UI_JS.parent / "app.css").read_text()
    assert ".tight-kv .kv { grid-template-columns: minmax(7.5rem, min(13rem, 40%)) minmax(0, 1fr); }" in css


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
