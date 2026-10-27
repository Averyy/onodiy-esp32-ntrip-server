# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The web UI's api(): bounded GET retries and a deadline covering headers and the complete JSON body."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

UI_JS = Path(__file__).resolve().parents[3] / "components" / "web" / "ui" / "js"

HARNESS = r"""
const fs = require('fs'), path = require('path'), vm = require('vm');
const [dir, casesJson] = process.argv.slice(-2);
const files = fs.readdirSync(dir).filter(f => f.endsWith('.js') && f !== '99-main.js').sort();
const src = files.map(f => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n');
const noop = () => {};
const el = new Proxy(function () {}, { get: (t, k) => (k === Symbol.toPrimitive ? () => '' : el), apply: () => el,
                                       construct: () => el });
const storage = { getItem: () => null, setItem: noop, removeItem: noop };
let script = [], calls = 0;
const ctx = {
  Node: function Node() {},
  document: { addEventListener: noop, getElementById: () => el, createElement: () => el, createElementNS: () => el,
              createTextNode: () => el, querySelector: () => el, documentElement: el, body: el, hidden: false },
  window: { addEventListener: noop, matchMedia: () => ({ matches: false, addEventListener: noop }) },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
  localStorage: storage, sessionStorage: storage,
  setTimeout: (fn, ms) => setTimeout(fn, Math.min(ms, 5)), clearTimeout: clearTimeout, setInterval: noop, clearInterval: noop,
  AbortController: globalThis.AbortController, crypto: globalThis.crypto, TextEncoder: globalThis.TextEncoder, console,
  fetch: async (path, init) => {
    calls++;
    const step = script.shift() || 'ok';
    if (step === 'network') throw new TypeError('Failed to fetch');
    if (step === 'abort') { const e = new Error('aborted'); e.name = 'AbortError'; throw e; }
    if (step.startsWith('body_')) {
      return { ok: true, status: 200, headers: { get: () => 'application/json' },
        json: () => {
          if (step === 'body_network') return Promise.reject(new TypeError('Body connection lost'));
          return new Promise((resolve, reject) => {
            const aborted = () => { const e = new Error('body aborted'); e.name = 'AbortError'; reject(e); };
            if (init.signal.aborted) return aborted();
            init.signal.addEventListener('abort', aborted, { once: true });
            if (step === 'body_slow') setTimeout(() => resolve({ fine: true }), 1);
          });
        } };
    }
    const status = step === 'ok' ? 200 : Number(step);
    return { ok: status < 300, status: status, headers: { get: () => 'application/json' },
             json: async () => (status < 300 ? { fine: true } : { error: { code: 'internal' } }) };
  },
};
vm.createContext(ctx);
const api = vm.runInContext('"use strict";\n(function () {\n' + src + '\nreturn api;\n})()', ctx);
(async () => {
  const out = [];
  for (const c of JSON.parse(casesJson)) {
    script = c.script.slice();
    calls = 0;
    try {
      let watchdog;
      const r = await Promise.race([
        api(c.method, '/api/x', c.method === 'GET' ? undefined : {}),
        new Promise((resolve, reject) => { watchdog = setTimeout(() => reject({ code: 'never_settled' }), 100); }),
      ]).finally(() => clearTimeout(watchdog));
      out.push({ calls, ok: r });
    } catch (e) {
      out.push({ calls, error: e.code });
    }
  }
  process.stdout.write(JSON.stringify(out));
})();
"""


def run(cases: list[dict]) -> list[dict]:
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is not installed")
    res = subprocess.run([node, "-e", HARNESS, str(UI_JS), json.dumps(cases)], capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def test_a_get_that_lost_its_connection_is_sent_once_more():
    out = run([
        {"method": "GET", "script": ["network", "ok"]},        # one lost connection: the second try answers
        {"method": "GET", "script": ["network", "network"]},   # lost twice: the error, after two tries
        {"method": "GET", "script": ["500"]},                  # an answer, even an error: never again
        {"method": "GET", "script": ["abort"]},                # a timeout: never again
        {"method": "POST", "script": ["network", "ok"]},       # a change: never sent twice
        {"method": "PUT", "script": ["network", "ok"]},
        {"method": "GET", "script": ["ok"]},
    ])
    assert out == [
        {"calls": 2, "ok": {"fine": True}},
        {"calls": 2, "error": "network"},
        {"calls": 1, "error": "internal"},
        {"calls": 1, "error": "timeout"},
        {"calls": 1, "error": "network"},
        {"calls": 1, "error": "network"},
        {"calls": 1, "ok": {"fine": True}},
    ]


def test_deadline_covers_body_read_without_replaying_mutations():
    out = run([
        {"method": "GET", "script": ["body_stall", "ok"]},
        {"method": "POST", "script": ["body_stall", "ok"]},
        {"method": "GET", "script": ["body_slow"]},
        {"method": "POST", "script": ["body_slow"]},
        {"method": "GET", "script": ["body_network", "ok"]},
        {"method": "GET", "script": ["body_network", "body_network", "ok"]},
        {"method": "POST", "script": ["body_network", "ok"]},
    ])
    assert out == [
        {"calls": 1, "error": "timeout"},
        {"calls": 1, "error": "timeout"},
        {"calls": 1, "ok": {"fine": True}},
        {"calls": 1, "ok": {"fine": True}},
        {"calls": 2, "ok": {"fine": True}},
        {"calls": 2, "error": "network"},
        {"calls": 1, "error": "network"},
    ]
