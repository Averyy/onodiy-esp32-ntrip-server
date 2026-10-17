# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The app shell's handling of the station's WebSocket events (components/web/ui/js/20-shell.js), run in Node.

The UI's modules are loaded the way the bundle joins them (every js/*.js but
the start-up in 99-main.js, in file-name order, inside one function), with a
document that answers every lookup. Timers are kept, not run: a case says when
the ones due so far fire ("settle"). Each case opens a page with reload() and
dirty() (a form being edited or not), sends it events as the WebSocket hands
them on, and reports how often the page reloaded and which toasts showed.
Skipped when Node.js is not installed.
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
const el = new Proxy(function () {}, {
  get: (t, k) => (k === Symbol.toPrimitive ? () => '' : el),
  apply: () => el,
  construct: () => el,
});
const storage = { getItem: () => null, setItem: noop, removeItem: noop };
let timers = [];
const ctx = {
  Node: function Node() {},
  document: { addEventListener: noop, getElementById: () => el, createElement: () => el, createElementNS: () => el,
              createTextNode: () => el, querySelector: () => el, documentElement: el, body: el, hidden: false },
  window: { addEventListener: noop, matchMedia: () => ({ matches: false, addEventListener: noop }) },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
  localStorage: storage, sessionStorage: storage,
  setTimeout: (fn, ms) => { timers.push({ fn, ms }); return timers.length; }, clearTimeout: noop,
  setInterval: noop, clearInterval: noop,
  crypto: globalThis.crypto, TextEncoder: globalThis.TextEncoder, console,
};
vm.createContext(ctx);
const ui = vm.runInContext('"use strict";\n(function () {\n' + src +
  '\nconst toasts = [];\ntoast = (text, tone) => toasts.push(text + " (" + tone + ")");\n' +
  'return { shell, bus, toasts, own: () => CLIENT_ID };\n})()', ctx);

const out = JSON.parse(casesJson).map(c => {
  timers = [];
  ui.toasts.length = 0;
  const reloads = {};
  const page = name => ({ name: name, arg: '', view: c.noReload ? {} : {
    reload() { reloads[name] = (reloads[name] || 0) + 1; },
    dirty: () => !!c.dirty,
  } });
  ui.shell.started = !c.bare;
  ui.shell.current = page('casters');
  const delays = [];
  for (const step of c.steps) {
    if (step === 'settle') {
      const due = timers;
      timers = [];
      for (const t of due) {
        delays.push(t.ms);
        t.fn();
      }
    } else if (step === 'leave') {
      ui.shell.current = page('receiver');
    } else {
      const m = Object.assign({ type: 'event' }, step);
      if (m.client === 'own') m.client = ui.own();
      ui.bus.emit('event', m);
    }
  }
  ui.shell.current = null;
  ui.shell.started = false;
  return { reloads: reloads, toasts: ui.toasts.slice(), delays: delays };
});
process.stdout.write(JSON.stringify(out));
"""


def run_shell(cases: list[dict]) -> list[dict]:
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is not installed")
    res = subprocess.run([node, "-e", HARNESS, str(UI_JS), json.dumps(cases)], capture_output=True, text=True,
                         timeout=60)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def changed(ns: str = "gnss", client: str | None = "0123456789abcdef") -> dict:
    """A settings_changed event as web.c sends it (client: the id of the tab whose request changed it)."""
    return {"event": "settings_changed", "ns": ns, "client": client}


WARNING = "Settings were changed elsewhere. Reload this page to see them. (warn)"
# What an idle station sends on the socket besides the status: none of it is a change of settings.
IDLE = [{"event": "station_state", "state": "streaming"}, {"event": "caster_state", "slot": 0, "state": "connected"},
        {"event": "network", "what": "time_synced"}, {"event": "update_state", "state": "idle"},
        {"event": "scan_done"}]


def test_events_of_an_idle_station_reload_nothing():
    """The open page reloads only on settings_changed: the station's other events (and its status every
    second) leave it alone."""
    out = run_shell([{"steps": IDLE + ["settle"]}, {"dirty": True, "steps": IDLE + ["settle"]}])
    assert out == [{"reloads": {}, "toasts": [], "delays": []}] * 2


def test_one_change_in_a_few_events_reloads_once():
    """A change made in another tab comes as a few events in a row (the profile's batch, then the base position
    erased; new Wi-Fi details, then the pending pair erased): the page reloads once, after they settled."""
    out = run_shell([
        {"steps": [changed(), changed(), changed(client=None), "settle"]},
        # Two changes apart: two reloads.
        {"steps": [changed(), "settle", changed("cst1"), "settle"]},
        # Nothing before the events settle.
        {"steps": [changed(), changed()]},
    ])
    assert out[0] == {"reloads": {"casters": 1}, "toasts": [], "delays": [300]}
    assert out[1]["reloads"] == {"casters": 2} and out[1]["toasts"] == []
    assert out[2]["reloads"] == {}


def test_a_form_being_edited_gets_one_warning_and_keeps_its_input():
    """While a form is being edited the page is not reloaded (it would throw the input away): one warning for
    the change, however many events it came as. The tab's own save warns of nothing."""
    out = run_shell([
        {"dirty": True, "steps": [changed(), changed(), changed(), "settle"]},
        {"dirty": True, "steps": [changed(client="own"), changed(client="own"), "settle"]},
        # The own save and another tab's change in the same moment: the other one is still told.
        {"dirty": True, "steps": [changed(client="own"), changed(client=None), "settle"]},
        # The own save with no form open: the page reads what it saved.
        {"steps": [changed(client="own"), "settle"]},
    ])
    assert out[0] == {"reloads": {}, "toasts": [WARNING], "delays": [300]}
    assert out[1] == {"reloads": {}, "toasts": [], "delays": [300]}
    assert out[2]["toasts"] == [WARNING] and out[2]["reloads"] == {}
    assert out[3]["reloads"] == {"casters": 1} and out[3]["toasts"] == []


def test_a_page_left_before_the_events_settled_is_not_touched():
    """The events were for the page that was open; the next page read its data when it opened."""
    out = run_shell([
        {"steps": [changed(), "leave", "settle"]},
        {"dirty": True, "steps": [changed(), "leave", "settle"]},
    ])
    assert out == [{"reloads": {}, "toasts": [], "delays": [300]}] * 2


def test_a_change_after_the_page_was_left_reloads_the_new_page():
    """A second, separate change that comes inside the window after a move to another page: that page may have read
    its data before it, so it reloads once (the old page still never does). A form being edited there is warned."""
    out = run_shell([
        {"steps": [changed(), "leave", changed(), "settle"]},
        {"dirty": True, "steps": [changed(), "leave", changed(), "settle"]},
    ])
    assert out[0] == {"reloads": {"receiver": 1}, "toasts": [], "delays": [300]}
    assert out[1] == {"reloads": {}, "toasts": [WARNING], "delays": [300]}


def test_pages_outside_the_app_and_pages_without_reload_ignore_it():
    """The login and the setup (the shell not started) and a page that has nothing to reload (the Status page
    follows the status stream)."""
    out = run_shell([
        {"bare": True, "steps": [changed(), "settle"]},
        {"noReload": True, "steps": [changed(), "settle"]},
        {"noReload": True, "dirty": True, "steps": [changed(), "settle"]},
    ])
    assert [o["reloads"] for o in out] == [{}, {}, {}]
    assert [o["toasts"] for o in out] == [[], [], []]
