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
warning, the connection details). Skipped when Node.js is not installed.
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
  append(...kids) { for (const k of kids) this.kids.push(k instanceof Node ? k : new Text(String(k))); }
  appendChild(k) { this.append(k); return k; }
  prepend(...kids) { this.kids.unshift(...kids.map(k => (k instanceof Node ? k : new Text(String(k))))); }
  replaceChildren(...kids) { this.kids = []; this.append(...kids); }
  remove() {}
  addEventListener(type, fn) { (this.on[type] = this.on[type] || []).push(fn); }
  removeEventListener() {}
  fire(type) {
    const ev = { type, target: this, preventDefault: noop, stopPropagation: noop };
    for (const fn of this.on[type] || []) fn(ev);
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
  querySelector() { return null; }
  querySelectorAll() { return []; }
  closest() { return null; }
  contains(x) { for (const e of this.walk()) if (e === x) return true; return false; }
  *walk() { yield this; for (const c of this.kids) if (c instanceof El) yield* c.walk(); }
}

const sent = [];
const storage = { getItem: () => null, setItem: noop, removeItem: noop };
const ctx = {
  Node,
  document: { addEventListener: noop, getElementById: () => new El('div'), createElement: t => new El(t),
              createElementNS: (ns, t) => new El(t), createTextNode: t => new Text(t), querySelector: () => new El('div'),
              querySelectorAll: () => [], documentElement: new El('html'), body: new El('body'), hidden: false,
              visibilityState: 'visible' },
  window: { addEventListener: noop, matchMedia: () => ({ matches: false, addEventListener: noop }), confirm: () => true },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
  localStorage: storage, sessionStorage: storage,
  setTimeout: noop, clearTimeout: noop, setInterval: noop, clearInterval: noop,
  AbortController: globalThis.AbortController, TextEncoder: globalThis.TextEncoder, crypto: globalThis.crypto,
  console,
  fetch: async (p, init) => {
    sent.push({ path: p, method: init.method, body: init.body ? JSON.parse(init.body) : null });
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ slot: 0, config: {} }) };
  },
};
vm.createContext(ctx);
const ui = vm.runInContext('"use strict";\n(function () {\n' + src +
  '\nreturn { casterEditor, openCasterWarning, casterDetails };\n})()', ctx);

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

const out = [];
for (const c of JSON.parse(casesJson)) {
  if (c.fn === 'warning') {
    const w = ui.openCasterWarning(c.data, c.slot);
    out.push(w ? [...w.walk()].filter(e => e.tagName === 'STRONG').map(e => e.textContent).join('') : null);
    continue;
  }
  if (c.fn === 'details') {
    const kids = ui.casterDetails(c.st, c.reconnects === null ? undefined : c.reconnects).filter(Boolean);
    const all = kids.flatMap(k => [...k.walk()]);
    const pairs = [];
    for (const dl of all.filter(e => e.tagName === 'DL')) {
      for (let i = 0; i + 1 < dl.kids.length; i += 2) pairs.push([dl.kids[i].textContent, dl.kids[i + 1].textContent]);
    }
    out.push({ rows: pairs, titles: all.filter(e => e.tagName === 'H3').map(e => e.textContent) });
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
                     last_session_text="ERROR - Bad Password", last_err="timed out",
                     failures_by_class=dict(QUIET["failures_by_class"], timeout=3, dns=1)), None),
        # Never reached anything this start: no reply, no address, no lag.
        details(dict(QUIET, state="wait_data", last_status_line="", last_status_code=0, resolved_ip=None, lag_ms_now=0,
                     max_lag_ms=0, bytes_sent_today=0), 0),
    ])
    assert [r[0] for r in quiet["rows"]] == ["State since", "Last reply", "Epochs dropped today (since the last restart)",
                                             "Lag now / highest", "Reconnects today", "Sent today (since the last restart)",
                                             "Address"], quiet
    assert dict(quiet["rows"])["Reconnects today"] == "2" and quiet["titles"] == []
    labels = [r[0] for r in rough["rows"]]
    assert labels[:12] == ["State since", "Last reply", "Caster said during a session", "Refused logins / login blocks",
                           "Sessions taken over (score)", "Data rate", "Epochs dropped today (since the last restart)",
                           "Last error", "Lag now / highest", "Reconnects today", "Sent today (since the last restart)",
                           "Address"], labels
    assert rough["titles"] == ["Failures by cause"]
    assert rough["rows"][12:] == [["DNS lookup failed", "1"], ["Timed out", "3"]], rough["rows"]
    assert dict(rough["rows"])["Reconnects today"] == "–"
    assert [r[0] for r in waiting["rows"]] == ["State since", "Epochs dropped today (since the last restart)",
                                               "Reconnects today", "Sent today (since the last restart)"], waiting
