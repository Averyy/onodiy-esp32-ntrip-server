# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Sentences the web UI builds from the station's status (components/web/ui/js), run in Node.

The UI's modules are loaded the way the bundle joins them (every js/*.js but
the start-up in 99-main.js, in file-name order, inside one function), with a
document that answers every lookup and timers that never fire, and the
functions under test are called on status objects as the API sends them.
Skipped when Node.js is not installed.
"""

from __future__ import annotations

import json
import re
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
const ctx = {
  Node: function Node() {},
  document: { addEventListener: noop, getElementById: () => el, createElement: () => el, createElementNS: () => el,
              createTextNode: () => el, querySelector: () => el, documentElement: el, body: el, hidden: false },
  window: { addEventListener: noop, matchMedia: () => ({ matches: false, addEventListener: noop }) },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
  localStorage: storage, sessionStorage: storage,
  setTimeout: noop, clearTimeout: noop, setInterval: noop, clearInterval: noop,
  crypto: globalThis.crypto, TextEncoder: globalThis.TextEncoder, console,
};
vm.createContext(ctx);
/* refusalText: what the UI says for an error answer {status, error: {code, ...}} (errText on an ApiError). */
/* healthCardShown: Logs' Health card is on its page (the notices' cases unless one says otherwise). */
const fns = vm.runInContext('"use strict";\n(function () {\n' + src +
  '\nhealthCardShown = true;\nreturn { restartPlanText, restartPlanShort, onocoyNow: a => onocoyNow(a[0], a[1]), onocoyElsewhereText, connRow, journalDetail, presetInfo: () => ({ PRESETS, PRESET_INFO }),\n' +
  '  journalItems: a => { S.status = a[3] || null; return journalItems(a[0], a[1], a[2]); }, evFilter: () => EV_FILTER,\n' +
  '  healthChip: a => healthChip(a[0], a[1], a[2]), sinceStart: hl => sinceStart(hl), logParts: l => logParts(l),\n' +
  '  timeRange: a => timeRange(a[0], a[1]), logsTabs: () => LOGS_TABS.map(t => t[1]), dayParts: a => dayParts(a[0], a[1]),\n' +
  '  wifiChip: w => wifiChip(w), apChip: ap => apChip(ap), recoveryHint: () => recoveryHint(),\n' +
  '  ago: s => checkedAgo(Date.now() / 1000 - s),\n' +
  '  onlineShare: a => onlineShare(a[0], a[1]),\n' +
  '  hero: a => { const t = stationState(a[0], a[1]);\n' +
  '    return { title: t.head, tone: t.tone, key: t.key, line: t.lead, alert: t.alertId, notice: t.notice, trial: t.trial,\n' +
  '      action: t.actions.length ? (t.actions[0].act || t.actions[0].label) : null, actions: t.actions.map(x => x.act || x.label),\n' +
  '      primary: t.actions.some(x => x.primary), also: t.also.map(x => x.id), alsoText: t.also.map(x => x.title + \'. \' + x.text),\n' +
  '      facts: t.facts, delivering: t.delivering, ono: t.onoStreaming, quality: t.quality,\n' +
  '      subject: t.subject, plan: t.plan }; },\n' +
  '  onocoy: a => { const t = stationState(a, true), d = onocoyDay(a, t);\n' +
  '    return d && { verdict: d.verdict, tone: d.tone, why: d.why, left: d.left, used: d.used, share: d.share, hero: t.key }; },\n' +
  '  checks: d => receptionChecks(d),\n' +
  '  notices: a => noticeItems(a[0], a[1]).map(i => [i.key, i.title]),\n' +
  '  noticesNoCard: a => { healthCardShown = false; const r = noticeItems(a[0], a[1]).map(i => i.key); healthCardShown = true;\n' +
  '    return r; },\n' +
  '  pageKey: a => { shell.current = { name: a[0], arg: a[1], view: {} }; const k = pageKey(); shell.current = null; return k; },\n' +
  '  healthItems: a => { const st = stationState(a, true), r = healthItems(a.alerts || [], st);\n' +
  '    return { state: r.state ? st.key : null, alerts: r.alerts.map(x => x.id), notice: healthNotice(a) }; },\n' +
  '  checkFailure: u => checkFailure(u),\n' +
  '  wifiRows: a => wifiRows(a[0], a[1]).filter(Boolean).map(r => [r[0], typeof r[1] === \'string\' ? r[1] : null]),\n' +
  '  linkCounts: a => linkCounts(a[0], a[1]).filter(Boolean),\n' +
  '  startKind: a => startKind(a[0], a[1]), rxBoot: v => (Array.isArray(v) ? rxBootSentence(v[0], v[1]) : rxBootSentence(v)),\n' +
  '  noticeLines: a => noticeItems(a[0], a[1]).map(i => [i.key, i.tone, i.urgent, i.title + (i.text ? \'. \' + i.text : \'\'),\n' +
  '    Array.isArray(i.actions) ? i.actions.length : i.actions ? 1 : 0]),\n' +
  '  fold: a => { const f = foldNotices(noticeItems(a[0], a[1]), a[2]); return [f.shown.map(i => i.key), f.more]; },\n' +
  '  moved: a => movedTo(a[0], a[1]), nav: () => NAV.map(n => n[1]),\n' +
  '  word: a => { const t = stationState(a[0], a[1]); return [t.key, t.tone, stateWord(t), t.head]; },\n' +
  '  stateWordKeys: () => Object.keys(STATE_WORD),\n' +
  '  pwProblem: a => adminPasswordProblem(a[0], a[1]), rssi: a => rssiView(a),\n' +
  '  sgName: a => sgName(a[0], a[1]), sgChoices: () => SG_CHOICES, recovery: () => L.recovery,\n' +
  '  recoveryLbl: k => lbl(L.recovery, k), manageLbl: k => lbl(L.manage, k), fwText: a => fwText(a[0], a[1]), dist: m => fmtDist(m),\n' +
  '  sgtBlock: g => sgtBlock(g), sgtLine: t => sgtLine(t), profileChip: g => profileChip(g), saveLabel: a => saveLabel(a[0], a[1]),\n' +
  '  sgtNotice: st => { const n = sgTestNotice(st); return n && n.slice(0, 3); },\n' +
  '  rxChip: s => receiverChip(s), posLine: a => positionLine(a[0], a[1]), statusPos: a => positionText(a[0], a[1]),\n' +
  '  headline: sig => signalsHeadline(sig), sigChecks: a => signalChecks(a[0], a[1]), msgCheck: a => msgCheck(a[0], a[1], a[2]),\n' +
  '  manage: () => MANAGE, manageKey: a => manageKey(a[0], a[1]), manageCustom: a => manageCustom(a[0], a[1]),\n' +
  '  doing: hl => recoveryDoing(hl), healthPlan: a => healthPlanText(a[0], stationState(a[1], true)),\n' +
  '  rxAlerts: a => (a[0].alerts || []).filter(rxAlertShown(stationState(a[1], true), false)),\n' +
  '  stepSaid: a => { const st = stationState(a[1], true); return stepSaid(a[0], st, healthItems(a[1].alerts || [], st).state); },\n' +
  '  nextAttempt: a => nextAttemptRow(a[0], stationState(a[1], true)),\n' +
  '  posChip: a => (Array.isArray(a) ? positionChip(a[0], a[1]) : positionChip(a)),\n' +
  '  fixPending: a => fixPendingText(a[0], a[1], a[2]), configWords: a => configWords(a[0], a[1]), profileTaken: g => profileTaken(g),\n' +
  '  arpCopy: g => arpCopyable(g), recoveryNow: hl => recoveryNow(hl),\n' +
  '  refusalText: r => errText(new ApiError(r.status, r.error)) };\n})()', ctx);
const out = JSON.parse(casesJson).map(([fn, arg]) => fns[fn](arg));
process.stdout.write(JSON.stringify(out));
"""


def run_ui(cases: list[tuple[str, object]]) -> list:
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is not installed")
    res = subprocess.run([node, "-e", HARNESS, str(UI_JS), json.dumps(cases)], capture_output=True, text=True,
                         timeout=60)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


# Receiver management "Me (alerts only)" as the Status page's top card says it: a fact with a Change link to its card.
ME_FACT = [["Receiver management", "You (alerts only)", "#/receiver/settings/manage"]]

# The hold-overlay station whose replay stopped: the ladder's episode with the admin page open.
HOLD = {"reboot_hold": ["admin"], "reboot_hold_left_s": 1578, "reboot_cap_wait_s": None,
        "reboot_cap_limit": None, "reboot_recheck": False}


def test_restart_plan_only_in_full_recovery():
    full, no_reboot, alerts, unknown = run_ui([
        ("restartPlanText", {**HOLD, "recovery_mode": "full"}),
        ("restartPlanText", {**HOLD, "recovery_mode": "no_reboot"}),
        ("restartPlanText", {**HOLD, "recovery_mode": "alerts_only"}),
        ("restartPlanText", HOLD),
    ])
    assert full is not None and "restarts itself" in full and "Postponed" in full and "27 min" in full
    # The modes without the ESP32 restart never announce one, whatever the station reports.
    assert no_reboot is None
    assert alerts is None
    assert unknown is None


def test_restart_plan_cap_and_recheck_in_full_recovery_only():
    cases = []
    for mode in ("full", "no_reboot", "alerts_only"):
        cases.append(("restartPlanText", {"recovery_mode": mode, "reboot_hold": [], "reboot_hold_left_s": None,
                                          "reboot_cap_wait_s": 900, "reboot_cap_limit": "hour"}))
        cases.append(("restartPlanText", {"recovery_mode": mode, "reboot_hold": [], "reboot_hold_left_s": None,
                                          "reboot_cap_wait_s": None, "reboot_recheck": True}))
    out = run_ui(cases)
    assert out[0] is not None and "Next possible in" in out[0]
    assert out[1] is not None and "after the next search" in out[1]
    assert out[2:] == [None, None, None, None]
    # Nothing pending in full recovery: no sentence either.
    assert run_ui([("restartPlanText", {"recovery_mode": "full", "reboot_hold": []})]) == [None]


def test_restart_plan_in_a_few_words_for_the_status_page():
    """The Status and Receiver pages' short form of the restart plan: the same cases as the full sentence, in a few
    words, and nothing whenever the full sentence says nothing (Logs keeps that one)."""
    base = {"recovery_mode": "full", "reboot_hold": [], "reboot_hold_left_s": None, "reboot_cap_wait_s": None,
            "reboot_cap_limit": None, "reboot_recheck": False}
    out = run_ui([
        ("restartPlanShort", {**base, **HOLD}),
        ("restartPlanShort", {**base, "reboot_hold": ["setup"], "reboot_hold_left_s": 1200}),
        ("restartPlanShort", {**base, "reboot_cap_wait_s": 2400, "reboot_cap_limit": "hour"}),
        ("restartPlanShort", {**base, **HOLD, "reboot_cap_wait_s": 2400, "reboot_cap_limit": "day"}),
        ("restartPlanShort", {**base, "reboot_recheck": True}),
        ("restartPlanShort", base),
        ("restartPlanShort", {**base, **HOLD, "recovery_mode": "no_reboot"}),
        ("restartPlanShort", {**base, **HOLD, "recovery_mode": "alerts_only"}),
    ])
    assert out[0] == "station restart in 27\u00a0min at the latest"
    # The setup hold has no end: what it waits for, not a time.
    assert out[1] == "station restart postponed (first setup not finished)"
    assert out[2] == "station restart possible in 40\u00a0min"
    # The cap outlasts the hold: the restart waits for the cap.
    assert out[3] == "station restart possible in 40\u00a0min"
    assert out[4] == "station may restart after the next search"
    assert out[5:] == [None, None, None]
    # On the Status page it follows the next search on one muted line.
    no_rx = status(receiver={"state": "no_receiver", "rtcm_age_ms": None}, alerts=[NO_RX],
                   health={"level": "recovering", "last_step": "rescan", **HOLD})
    searching = status(receiver={"state": "no_receiver", "rtcm_age_ms": None},
                       alerts=[{**NO_RX, "next_detect_in_s": 0, "saw_bytes": True}])
    a, b = heroes(no_rx, searching)
    assert a["line"] == "Nothing arrives from the receiver. Check its power and the COM1 cable."
    assert a["plan"] == "Next search in 24\u00a0s \u00b7 station restart in 27\u00a0min at the latest"
    assert "GPIO" not in a["line"] + a["plan"]
    assert b["line"] == "Data arrives, but the station cannot read it. Check the wiring and the receiver's port speed."
    assert b["plan"] == "Searching now"


def test_setup_done_says_onocoy_as_it_is():
    """The setup's last screen: Onocoy's state from the same state as the Status page. Streaming only with
    corrections behind it; a refused login with its count and Edit; not set up offers the Onocoy step again."""
    no_rx = dict(receiver={"state": "no_receiver", "rtcm_age_ms": None}, alerts=[NO_RX])
    stall = dict(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"})
    cases = [
        (status(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False)]), None),
        (status(casters=[caster(0, "Onocoy", "disabled", enabled=False)]), None),
        (status(casters=[caster(0, "Onocoy", "connecting")]), None),
        (status(casters=[caster(0, "Onocoy", "auth_failed", reason="auth", next_retry_s=290)]), 1),
        (status(casters=[caster(0, "Onocoy", "auth_failed", reason="auth")]), None),
        (status(casters=[caster(0, "Onocoy", "auth_failed", reason="login_blocked")]), None),
        (status(casters=[caster(0, "Onocoy", "stopped", stop_reason="rejected")]), 3),
        (status(), None),
        # The station still says streaming during a stall: never "Streaming" here either.
        (status(**stall), None),
        (status(casters=[caster(0, "Onocoy", "wait_data")], **no_rx), None),
        (status(casters=[caster(0, "Onocoy", "backoff", reason="dns", next_retry_s=20)]), None),
        # Trying again after a failure: the title stays (no flicker with each attempt).
        (status(casters=[caster(0, "Onocoy", "connecting", reason="timeout")]), None),
        (status(safe_mode={"active": True, "crashes": 3}, receiver={"state": "idle"}, casters=[caster(0, "Onocoy", "disabled")],
                alerts=[{"id": "safe_mode", "source": "health"}]), None),
    ]
    out = run_ui([("onocoyNow", [s, n]) for s, n in cases])
    got = [(o["key"], o["tone"], o["head"], o["act"]) for o in out]
    assert got == [
        ("setup", "warn", "Onocoy is not set up", "setup"),
        ("setup", "warn", "Onocoy is turned off", "setup"),
        ("connecting", "info", "Connecting to Onocoy\u2026", None),
        ("refused", "bad", "Login refused (1 of 3)", "edit"),
        ("refused", "bad", "Login refused", "edit"),
        ("refused", "bad", "Onocoy is blocking logins", "edit"),
        ("stopped", "bad", "Onocoy stopped: login refused 3 times", "edit"),
        ("streaming", "ok", "Streaming to Onocoy", None),
        ("receiver", "info", "Waiting for the receiver", None),
        ("receiver", "info", "Waiting for the receiver", None),
        ("failing", "warn", "Not connected yet", None),
        ("failing", "warn", "Not connected yet", None),
        ("safe_mode", "bad", "Casters are off in safe mode", None),
    ], got
    assert out[3]["text"].endswith("Next try in 4\u00a0min 50\u00a0s.")
    assert out[8]["text"] == "Onocoy starts once corrections arrive. Now: receiver stopped sending data."
    assert out[10]["text"] == "DNS lookup failed. Next try in 20\u00a0s."
    assert out[11]["text"] == "Timed out. Trying again now."
    # Plain words only: no state names or protocol terms.
    for o in out:
        assert not any(w in (o["head"] + o["text"]).lower() for w in ("rtcm", "auth_failed", "ntrip", "backoff")), o


def test_onocoy_hint_never_says_set_up_for_a_slot_that_is_not():
    not_ready, off, streams = run_ui([
        ("onocoyElsewhereText", {"slot": 0, "ready": False, "enabled": False}),
        ("onocoyElsewhereText", {"slot": 0, "ready": True, "enabled": False}),
        ("onocoyElsewhereText", {"slot": 2, "ready": True, "enabled": True}),
    ])
    # The Onocoy card on the same page says "Not set up" for a slot without credentials.
    assert "is set up" not in not_ready and "not set up" in not_ready and "credentials" in not_ready
    assert off == "Onocoy is already set up here and turned off. Open it to turn it on."
    assert "already streams to Onocoy" in streams and "one stream per station" in streams
    # The user never sees slot numbers: the note names Onocoy (its button reads "Open Onocoy").
    for text in (not_ready, off, streams):
        assert "slot" not in text.lower()


def test_every_caster_choice_has_its_box():
    """Every caster in the editor's list has a box: one line saying what it is, 1 to 4 short steps, and a link
    to the caster's own guide (Other caster has no guide). Nothing that goes stale: no dates."""
    (got,) = run_ui([("presetInfo", None)])
    ids = [p["id"] for p in got["PRESETS"]]
    info = got["PRESET_INFO"]
    assert sorted(ids) == sorted(info), (ids, list(info))
    for pid, p in info.items():
        assert p["what"] and p["what"].endswith("."), pid
        assert 1 <= len(p["steps"]) <= 4, pid
        assert all(s.endswith(".") and len(s) <= 130 for s in p["steps"]), pid
        text = " ".join([p["what"], *p["steps"]])
        assert not any(m in text for m in ("2025", "2026", "2027", "as of", "Rev1 =", "kbit", "Mbit")), pid
        if pid == "custom":
            assert p["guide"] is None
        else:
            label, url = p["guide"]
            assert label and url.startswith(("https://", "http://rtk2go.com/")), pid


def test_no_receiver_refusal_text():
    no_rx, other = run_ui([
        ("refusalText", {"status": 409, "error": {"code": "no_receiver", "message": "No receiver is connected"}}),
        ("refusalText", {"status": 409, "error": {"code": "not_no_receiver", "message": "x"}}),
    ])
    assert no_rx == "No receiver is connected. Connect it, then try again."
    # The detection refusal keeps its own words: the two codes are not the same thing.
    assert other != no_rx and "not looking for a receiver" in other


def test_unknown_rssi_never_shows_dbm():
    """A Wi-Fi drop's RSSI is a reading only from -127 to -1 dBm: null (this build's unknown), 0 and the -128
    of journal records from earlier builds are unknown and show no signal figure."""
    out = run_ui([("journalItems", [[jrec(1, "wifi_disconnect", 201, b, 5)], True, None]) for b in (None, -128, 0, -67, -127, -1)])
    det = [[i["det"] for i in items if "head" not in i][0] for items in out]
    for text in det[:3]:
        assert "dBm" not in text and "signal" not in text
        assert "201" in text  # the reason stays
    assert "signal -67 dBm" in det[3]
    assert "signal -127 dBm" in det[4] and "signal -1 dBm" in det[5]


def test_connection_row_rssi():
    row = {"kind": "wifi_disconnect", "reason": 8, "slot": None, "slot_name": None}
    unknown, known, zero, old = run_ui([
        ("connRow", {**row, "rssi": None}),
        ("connRow", {**row, "rssi": -60}),
        ("connRow", {**row, "rssi": 0}),
        ("connRow", {**row, "rssi": -128}),
    ])
    assert unknown[:2] == ["Wi-Fi dropped", "left the network (8)"]
    assert known[1] == "left the network (8), signal -60 dBm"
    assert "dBm" not in zero[1] and "dBm" not in old[1]


def test_event_list_pairs_a_drop_with_its_return_and_folds_repeats():
    """The journal's connection records as one list (no separate connection history): a drop and the return after
    it are one row, the same row three times in a row is one row with a "x3" toggle; a drop without a return stays
    as it is. Casters by their name now, never a slot number."""
    rc, to, dup = 5, 4, 5   # remote_close and timeout (ntrip_fail_class_t), duplicate (ntrip_stop_t)
    recs = [jrec(5, "caster_stop", dup << 8, 0, 5)]
    for i in range(3):
        recs += [jrec(10 + 2 * i, "caster_down", rc << 8, 3600, 10 + 2 * i), jrec(11 + 2 * i, "caster_up", 2 << 16, 20, 11 + 2 * i)]
    recs += [jrec(20, "wifi_disconnect", 202, -67, 20), jrec(21, "wifi_up", 3, 40, 21), jrec(30, "caster_down", to << 8, 60, 30)]
    st = {"casters": [{"slot": 0, "name": "Onocoy"}]}
    every, casters = run_ui([("journalItems", [recs, True, None, st]),
                             ("journalItems", [recs, True, ["caster_down", "caster_up", "caster_stop"], st])])
    rows = [i for i in every if "head" not in i]
    assert [r["ev"] for r in rows] == ["Onocoy disconnected", "Wi-Fi dropped", "Onocoy disconnected", "Onocoy stopped"]
    assert rows[0]["det"] == "after 1\u00a0min online, timed out" and rows[0]["tone"] == "warn"
    assert rows[1]["det"] == "back after 40\u00a0s, authentication failed (wrong password?) (202), signal -67 dBm"
    assert rows[2]["det"] == "back after 20\u00a0s, closed by the caster, 2 failed attempts"
    assert len(rows[2]["all"]) == 3 and rows[2]["id"] == "r10"
    assert rows[3]["det"] == "duplicate stream, no new attempt until you act" and rows[3]["tone"] == "bad"
    for r in rows:
        assert "slot" not in (r["ev"] + r["det"]).lower()
    # The Casters filter keeps the pairs and drops the Wi-Fi.
    assert [r["ev"] for r in casters if "head" not in r] == ["Onocoy disconnected", "Onocoy disconnected", "Onocoy stopped"]


def jrec(seq: int, kind: str, a: int = 0, b: int = 0, up: int = 1, t: int | None = None) -> dict:
    return {"seq": seq, "time_unix": t, "uptime_s": up, "kind": kind, "a": a, "b": b}


def roam_a(ch0: int, ch1: int, by: int, same: bool = False, kept: bool = False) -> int:
    return ch0 | (ch1 << 8) | (by << 16) | ((1 << 24) if same else 0) | ((1 << 25) if kept else 0)


def roam_b(r0: int, r1: int, secs: int) -> int:
    return (r0 & 0xFF) | ((r1 & 0xFF) << 8) | (secs << 16)


def test_event_list_names_moves_between_access_points():
    """A move to another access point of the network is one row of its own (never paired with a drop): who moved
    it, the channels and signals before and after, the time offline; an own reconnect that stayed is a reconnect.
    The Wi-Fi filter keeps them, the casters filter does not; no access point address anywhere."""
    recs = [jrec(1, "wifi_roam", roam_a(1, 6, 1), roam_b(-78, -52, 3), 100),
            jrec(2, "wifi_roam", roam_a(6, 6, 2), roam_b(-75, -58, 4), 4000),
            jrec(3, "wifi_roam", roam_a(6, 6, 2, True), roam_b(-74, -73, 5), 9000),
            jrec(4, "wifi_roam", roam_a(11, 1, 2), roam_b(0, 0, 2), 15000),
            jrec(5, "wifi_disconnect", 8, -70, 16000), jrec(6, "wifi_up", 1, 6, 16006)]
    every, wifi, casters = run_ui([("journalItems", [recs, True, None]),
                                   ("journalItems", [recs, True, ["wifi_disconnect", "wifi_up", "wifi_roam"]]),
                                   ("journalItems", [recs, True, ["caster_down", "caster_up", "caster_stop"]])])
    rows = [(r["ev"], r["det"]) for r in every if "head" not in r]
    # The time offline first: a phone cuts the end of the row off.
    assert rows == [
        ("Wi-Fi dropped", "back after 6\u00a0s, left the network (8), signal -70 dBm"),
        ("Wi-Fi moved", "2\u00a0s offline, to a stronger access point, channel 11 to 1"),
        ("Wi-Fi reconnected", "5\u00a0s offline, no stronger access point after all"),
        ("Wi-Fi moved", "4\u00a0s offline, to a stronger access point, channel 6, signal -75 to -58 dBm"),
        ("Wi-Fi moved", "3\u00a0s offline, at the access point's request, channel 1 to 6, signal -78 to -52 dBm"),
    ], rows
    assert [(r["ev"], r["det"]) for r in wifi if "head" not in r] == rows
    assert casters == []
    assert not re.search(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", json.dumps(every))
    # Two moves of the same kind in a row are one row with a toggle; a move and a stayed reconnect are not.
    folded = run_ui([("journalItems", [[jrec(1, "wifi_roam", roam_a(1, 6, 1), roam_b(-78, -52, 3), 100),
                                        jrec(2, "wifi_roam", roam_a(6, 1, 1), roam_b(-80, -50, 2), 900)], True, None])])[0]
    rows = [r for r in folded if "head" not in r]
    assert len(rows) == 1 and len(rows[0]["all"]) == 2


def test_event_list_says_a_move_kept_the_connections():
    """A move that kept the address says so first, instead of a time offline (nothing was offline), so a phone's row
    shows it; one that stayed on the same access point is no reconnect (nothing dropped); a move asked for on the bench
    console says "on request"."""
    recs = [jrec(1, "wifi_roam", roam_a(1, 6, 1, kept=True), roam_b(-78, -52, 1), 100),
            jrec(2, "wifi_roam", roam_a(6, 6, 2, True, True), roam_b(-74, -73, 2), 4000),
            jrec(3, "wifi_roam", roam_a(6, 11, 3, kept=True), roam_b(-60, -55, 2), 9000),
            jrec(4, "wifi_roam", roam_a(11, 11, 3, True, True), roam_b(-55, -56, 2), 15000),
            jrec(5, "wifi_roam", roam_a(11, 1, 3), roam_b(-56, -50, 9), 20000)]
    (items,) = run_ui([("journalItems", [recs, True, None])])
    rows = [(r["ev"], r["det"]) for r in items if "head" not in r]
    assert rows == [
        ("Wi-Fi moved", "9\u00a0s offline, on request, channel 11 to 1, signal -56 to -50 dBm"),
        ("Wi-Fi stayed", "connections kept, on request, the same access point"),
        ("Wi-Fi moved", "connections kept, on request, channel 6 to 11, signal -60 to -55 dBm"),
        ("Wi-Fi stayed", "connections kept, no stronger access point after all"),
        ("Wi-Fi moved", "connections kept, at the access point's request, channel 1 to 6, signal -78 to -52 dBm"),
    ], rows


def test_event_journal_one_heading_per_start_and_repeats_folded():
    """A start is one heading (its boot and session records, and the restart record that announced it, folded in),
    its other records newest first, 27 recovery steps in a row one row with a toggle; no "#seq" or "≈" in the text."""
    t0 = 1760500000
    first = [jrec(1, "boot", 0, 0, 1), jrec(2, "session", -1, 1, 1)]
    steps = [jrec(3 + i, "ladder_step", 0, (0 << 8) | 2, 60 + i, t0 + 60 + i) for i in range(27)]
    second = [jrec(30, "reboot", 1, up=200, t=t0 + 200), jrec(31, "boot", 1, 1, 1), jrec(32, "session", 44, 3, 1),
              jrec(33, "wifi_disconnect", 201, -70, 30, t0 + 400)]
    entries = first + steps + second
    every, starts, casters, part = run_ui([
        ("journalItems", [entries, True, None]),
        ("journalItems", [entries, True, ["boot", "session", "reboot", "safe_mode", "rx_boot"]]),
        ("journalItems", [entries, True, ["caster_down", "caster_up", "caster_stop"]]),
        ("journalItems", [steps[-5:] + second, False, None]),
    ])
    heads = [i["head"][1] for i in every if "head" in i]
    assert heads == ["Restarted by you (previous run 44\u00a0s)", "Started after power-on or a reset by its pin"]
    rows = [i for i in every if "head" not in i]
    # The restart record went into the heading; a recovery step is named after its recovery, the step is the detail.
    assert [r["ev"] for r in rows] == ["Wi-Fi dropped", "Receiver recovery"]
    assert rows[1]["det"] == "reopening the link to the receiver"
    assert len(rows[1]["all"]) == 27 and "\u2013" in rows[1]["when"]
    assert rows[0]["det"] == "network not found (201), signal -70 dBm"
    text = json.dumps(every, ensure_ascii=False)
    assert "≈" not in text and "#" not in text and "slot" not in text.lower()
    # Starts only: the two headings; casters only: nothing; the oldest start not loaded yet is a plain "Start".
    assert [("head" in i) for i in starts] == [True, True]
    assert casters == []
    assert [i["head"][1] for i in part if "head" in i] == ["Restarted by you (previous run 44\u00a0s)", "Start"]


def test_event_journal_sky_sessions_and_plain_rows():
    """A sky session (opened, satellite output on, extended, closed, output off) is one row: the time span and how long
    it was open; three sessions in a row fold into one row with a toggle. A refused output command is said, an
    accepted one and the corrections held back are not. Data loss, recovery steps and the network pass in words."""
    t0 = 1760500000
    sky = []
    for k, (seq, t) in enumerate(((10, 0), (20, 40), (30, 80))):
        sky += [jrec(seq, "live_view", 1, 600, 100 + t, t0 + t), jrec(seq + 1, "live_view", 4, 107, 101 + t, t0 + t + 1),
                jrec(seq + 2, "live_view", 2, 1200, 105 + t, t0 + t + 5), jrec(seq + 3, "live_view", 3 | (1 << 8), 12, 112 + t, t0 + t + 12),
                jrec(seq + 4, "live_view", 5 | ((1 if k == 2 else 0) << 8), 90, 113 + t, t0 + t + 13)]
    first = [jrec(1, "boot", 0, 0, 1), jrec(2, "session", -1, 1, 1)]
    other = [jrec(40, "ladder_step", 0, (1 << 8) | 8, 200, t0 + 200), jrec(41, "data_loss", 5, 453, 210, t0 + 210),
             jrec(42, "ladder_step", 1, (1 << 16) | (0 << 8) | 0, 220, t0 + 220), jrec(43, "ladder_step", 0, (2 << 8) | 5, 230, t0 + 230),
             jrec(44, "live_view", 3, 30, 240, t0 + 240)]
    (items,) = run_ui([("journalItems", [first + sky + other, True, None])])
    rows = [i for i in items if "head" not in i]
    assert [(r["ev"], r["det"]) for r in rows] == [
        ("Sky view", "closed after 30\u00a0s"),                      # a lone record of a session not loaded whole
        ("Receiver recovery", "restarting the receiver: failed"),
        ("Network recovery", "started: Onocoy down"),
        ("Receiver data lost", "453 bytes"),
        ("Receiver recovery", "healthy again"),
        ("Sky view", "12\u00a0s, satellite output off: refused by the receiver"),
        ("Sky view", "12\u00a0s"),
    ], rows
    # The two sessions without trouble are one row with a toggle, its time the span of both.
    assert len(rows[6]["all"]) == 2 and "\u2013" in rows[6]["when"]
    text = json.dumps(items, ensure_ascii=False)
    assert "RTCM held" not in text and "accepted" not in text and "framer" not in text and "protected" not in text


def test_event_filter_covers_every_journal_kind():
    (filters,) = run_ui([("evFilter", None)])
    kinds = {k for f in filters if f[2] for k in f[2]}
    api = (ROOT / "components" / "web" / "API.md").read_text(encoding="utf-8")
    section = api[api.index("### `GET /api/journal?cursor=N`"):api.index("### `DELETE /api/journal`")]
    documented = set(re.findall(r"^\| `([a-z_]+)` \|", section, flags=re.M)) - {"kind"}
    assert documented and documented <= kinds, documented - kinds
    assert filters[0][:2] == ["all", "All events"]


def test_today_online_share_on_the_dashboard():
    """Whole numbers at 0 % and 100 % (no "0.00 %"), two decimals in between, rounded down: never "100.00 %"
    while the caster was offline for a moment."""
    out = run_ui([("onlineShare", [0, 600]), ("onlineShare", [600, 0]), ("onlineShare", [1, 2]),
                  ("onlineShare", [86399, 1])])
    assert out == ["0\u00a0%", "100\u00a0%", "33.33\u00a0%", "99.99\u00a0%"], out


# ---- The station's state (stationState): the Status page's top card, Onocoy's day, the notices it covers ----

def caster(slot: int, name: str, state: str, **kw) -> dict:
    c = {"slot": slot, "name": name, "empty": False, "enabled": state != "disabled", "protected": name == "Onocoy",
         "host": "servers.onocoy.com" if name == "Onocoy" else name.lower() + ".example", "state": state, "reason": "none",
         "next_retry_s": 0, "stop_reason": "none", "onocoy_ready": name == "Onocoy"}
    if name == "Onocoy":
        c.update({"streak_ok": True, "budget_s": 473, "budget_left_s": 463, "today_online_s": 3600, "today_offline_s": 10,
                  "data_rate_permille": 1000, "connected_s": 3600})
    c.update(kw)
    return c


def status(**kw) -> dict:
    """An admin GET /api/status: the receiver streams 4 bands with GLONASS, Onocoy streams, nothing is wrong."""
    s = {"api": 1, "public": False, "uptime_s": 23520,
         "device": {"hostname": "onocoydiy", "settings_available": True},
         "safe_mode": {"active": False, "retry_in_s": None, "crashes": 0},
         "network": {"online": True, "ssid": "Home"},
         "receiver": {"state": "streaming", "model": "UM982", "rtcm_age_ms": 180, "journal": "none", "uart_closed": False,
                      "rx_maintenance": False, "maintenance_left_s": 0, "first_apply_held": False, "best_sys": "galileo",
                      "best_bands": 4, "glonass": True, "rtcm1230_supported": True, "position_locked": True,
                      "profile_applied": True, "signal_test": None},
         "casters": [caster(0, "Onocoy", "streaming")],
         "health": {"level": "ok", "last_step": "none", "recovery_mode": "full"},
         "alerts": [], "availability": {"protected_down_today_s": 10, "budget_left_s": 463}, "update": {}}
    for k, v in kw.items():
        if isinstance(v, dict) and isinstance(s.get(k), dict):
            s[k] = {**s[k], **v}
        else:
            s[k] = v
    return s


def heroes(*statuses, admin: bool = True) -> list[dict]:
    return run_ui([("hero", [st, admin]) for st in statuses])


NO_RX = {"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 24}


def test_state_every_condition_in_order():
    """Each condition of the list, alone: its key, title and colour. Then each next to the one after it: the
    earlier one wins (what stops everything first, Onocoy's quality last), except the antenna reason during a
    recovery, which says the recovery under it."""
    conds = {
        "safe_mode": dict(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                          casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}]),
        "journal": dict(receiver={"journal": "waiting"}),
        "settings": dict(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}]),
        "updating": dict(receiver={"state": "flashing", "journal": "running"}),
        "uart_closed": dict(receiver={"state": "no_receiver", "uart_closed": True}, alerts=[NO_RX]),
        "no_receiver": dict(receiver={"state": "no_receiver", "rtcm_age_ms": None}, alerts=[NO_RX],
                            health={"level": "recovering", "last_step": "rescan"}),
        "rx_stopped": dict(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"}),
        "no_satellites": dict(health={"level": "failed"}, alerts=[{"id": "no_satellites", "source": "health"}],
                              casters=[caster(0, "Onocoy", "wait_data")]),
        "no_rtcm": dict(health={"level": "failed"}, alerts=[{"id": "no_rtcm", "source": "health"}],
                        casters=[caster(0, "Onocoy", "wait_data")]),
        "stopped": dict(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")]),
        "maintenance": dict(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 830}),
        "signal_test": dict(receiver={"state": "configuring", "signal_test": {"state": "measuring", "phase": 0, "left_s": 600,
                                                                              "group": {"master": 3, "slave": 6}}}),
        "wifi_trial": dict(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"}),
        "receiver": dict(receiver={"state": "configuring"}, casters=[caster(0, "Onocoy", "wait_data")]),
        "onocoy_setup": dict(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False), caster(1, "RTK2go", "backoff")]),
        "casters_down": dict(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed", reason="auth")],
                             receiver={"best_bands": 3}),
        "quality": dict(receiver={"best_bands": 3}),
        "streaming": dict(),
    }
    expect = {
        "safe_mode": ("Safe mode", "bad"), "journal": ("Receiver update needs you", "warn"),
        "settings": ("Settings cannot be saved", "bad"), "updating": ("Updating the receiver", "info"),
        "uart_closed": ("Receiver port closed", "bad"), "no_receiver": ("No receiver found", "bad"),
        "rx_stopped": ("Receiver stopped sending data", "warn"), "no_satellites": ("Antenna sees no satellites", "bad"),
        "no_rtcm": ("Receiver sends no corrections", "bad"), "stopped": ("Onocoy stopped", "bad"),
        "maintenance": ("Receiver maintenance", "info"), "signal_test": ("Comparing signal groups", "info"),
        "wifi_trial": ("Trying new Wi-Fi", "info"), "receiver": ("Configuring the receiver", "info"),
        "onocoy_setup": ("Onocoy not set up", "warn"), "casters_down": ("Partly streaming", "warn"),
        "quality": ("Too few bands for Onocoy", "warn"), "streaming": ("Streaming", "ok"),
    }
    keys = list(conds)
    out = heroes(*[status(**conds[k]) for k in keys])
    for k, o in zip(keys, out):
        assert (o["key"], o["title"], o["tone"]) == (k, *expect[k]), (k, o)
        text = (o["title"] + " " + o["line"]).lower()
        # No health level, no setting name and no internal state on top, ever.
        assert not any(w in text for w in ("alerts only", "alerts_only", "needs attention", "healthy", "recovery:",
                                           "degraded", "saveconfig", "rtcm")), (k, o)
    # Two conditions at once: the earlier one in the list wins.
    pairs = []
    for a, b in zip(keys, keys[1:]):
        merged = {}
        for part in (conds[b], conds[a]):
            for kk, vv in part.items():
                if kk == "alerts":
                    merged["alerts"] = merged.get("alerts", []) + vv
                elif isinstance(vv, dict) and isinstance(merged.get(kk), dict):
                    merged[kk] = {**merged[kk], **vv}
                else:
                    merged[kk] = vv
        pairs.append((a, b, status(**merged)))
    got = heroes(*[p[2] for p in pairs])
    for (a, b, _), o in zip(pairs, got):
        if (a, b) == ("rx_stopped", "no_satellites"):
            # The recovery runs and the receiver answers without corrections: the antenna, in amber (the recovery's
            # line goes under it, test_recovery_says_what_it_does_now).
            assert (o["key"], o["tone"]) == ("no_satellites", "warn"), (a, b, o)
            continue
        assert o["key"] == a, (a, b, o)


def test_hero_says_what_blocks_first_and_what_to_do():
    onocoy_not_set_up = status(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False)],
                               health={"level": "degraded", "recovery_mode": "alerts_only"})
    maintenance = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 830})
    # The QEMU run: no receiver found while the recovery still reports recovering.
    no_rx = status(receiver={"state": "no_receiver", "rtcm_age_ms": None}, health={"level": "recovering", "last_step": "rescan"},
                   alerts=[NO_RX], casters=[caster(0, "Onocoy", "wait_data")])
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart",
                                                             "step": "link_restart", "step_phase": "running"})
    alerts_only = status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                         alerts=[{"id": "no_rtcm", "source": "health"}], casters=[caster(0, "Onocoy", "wait_data")])
    silent_alerts_only = status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                                alerts=[{"id": "receiver_silent", "source": "health"}], casters=[caster(0, "Onocoy", "wait_data")])
    out = heroes(onocoy_not_set_up, maintenance, no_rx, stall, alerts_only, silent_alerts_only)

    assert out[0]["title"] == "Onocoy not set up" and out[0]["tone"] == "warn", out[0]
    assert out[0]["line"] == "Add your Onocoy credentials to start streaming."
    assert out[0]["action"] == "Set up Onocoy" and out[0]["primary"] is True
    # Receiver management shows as a fact only when it is not the default, by the name of the setting, the choice said
    # from the station's side ("You", where the setting's own choice says "Me"), with a link to its card.
    assert out[0]["facts"] == ME_FACT

    assert out[1]["title"] == "Receiver maintenance"
    assert out[1]["line"] == "The station sends the receiver no commands. Ends in about 14 min."
    assert out[1]["action"] == "end-maint" and out[1]["notice"] == "maintenance"

    assert out[2]["title"] == "No receiver found" and out[2]["tone"] == "bad", out[2]
    # One plain line on top (the pins are in the Receiver page's panel); the next search in the short muted line.
    assert out[2]["line"] == "Nothing arrives from the receiver. Check its power and the COM1 cable."
    assert out[2]["plan"] == "Next search in 24 s"
    assert out[2]["action"] == "detect" and out[2]["alert"] == "no_receiver" and out[2]["also"] == []

    # A stall while the station still says streaming: never "Streaming" on top, and Onocoy does not count as streaming.
    assert out[3]["title"] == "Receiver stopped sending data" and out[3]["tone"] == "warn", out[3]
    assert out[3]["line"] == "The station is reopening the link to the receiver."
    assert out[3]["ono"] is False and out[3]["delivering"] is False

    assert out[4]["title"] == "Receiver sends no corrections" and out[4]["alert"] == "no_rtcm", out[4]
    # The choice is said once, in the fact row under the line (never again in the line itself); the title says that no
    # corrections come, the line what that means and what to do.
    assert out[4]["line"] == "The receiver answers. Check that the antenna is on the socket marked M and sees the open sky."
    assert out[4]["tone"] == "bad" and out[4]["plan"] is None
    assert out[4]["facts"] == ME_FACT
    assert out[5]["title"] == "Receiver stopped sending data" and out[5]["tone"] == "bad"
    assert "recovery" not in out[5]["line"] and "choice" not in out[5]["line"]
    assert out[5]["facts"] == ME_FACT


def test_recovery_says_what_it_does_now():
    """The receiver recovery's line names the step in progress and where it is (health.step, health.step_phase):
    "searching for the receiver" only while the search runs, never in the wait after it, and never for a receiver that
    answers (the rescan checks the link to it). A receiver that answers without sending corrections gets the antenna
    check at once, in amber while the recovery still works on it, with what it does in the plan line (on Logs
    too, in the same words, under the state's line); red once it no longer acts (alerts only, or after the last step).
    Nothing says it tries again shortly while the station holds the recovery. Visitors see no step."""
    steps = ["resync", "link_restart", "rescan", "reconfigure", "receiver_reset", "reboot"]
    doing = run_ui([("doing", {"step": k, "step_phase": p}) for k in steps for p in ("running", "waiting")] +
                   [("doing", {"step": "reconfigure", "step_phase": "none"}), ("doing", {"step": "none", "step_phase": "none"}),
                    ("doing", {"level": "recovering", "last_step": "rescan"})])
    assert doing[:12] == [
        "The station is resyncing the receiver's data.", "The station resynced the receiver's data and waits for corrections.",
        "The station is reopening the link to the receiver.", "The station reopened the link and waits for corrections.",
        "The station is searching for the receiver.", "The station found the receiver and waits for corrections.",
        "The station is re-sending the receiver's settings.",
        "The station re-sent the receiver's settings and waits for corrections.",
        "The station is restarting the receiver.", "The station restarted the receiver and waits for corrections.",
        "The station is restarting itself.", "The station is restarting itself."], doing
    assert doing[12:] == [None, None, None]
    assert not any("searching" in d for d in doing[5:12:2] if d)

    hl = lambda step, phase, **kw: {"level": "recovering", "last_step": step, "step": step, "step_phase": phase,
                                     "recovery_mode": "no_reboot", **kw}
    no_rtcm = [{"id": "no_rtcm", "source": "health"}]
    wait_data = [caster(0, "Onocoy", "wait_data")]
    search = status(receiver={"rtcm_age_ms": None}, health=hl("rescan", "running"), casters=wait_data)
    found = status(receiver={"rtcm_age_ms": None}, health=hl("rescan", "waiting"), casters=wait_data)
    antenna = status(receiver={"rtcm_age_ms": None}, health=hl("reconfigure", "waiting"), alerts=no_rtcm, casters=wait_data)
    no_sky = status(receiver={"rtcm_age_ms": None}, health=hl("reconfigure", "running"),
                    alerts=[{"id": "no_satellites", "source": "health"}], casters=wait_data)
    hold = {"reboot_hold": ["admin"], "reboot_hold_left_s": 1578, "reboot_cap_wait_s": None, "reboot_cap_limit": None}
    # Full recovery: no restart is pending while the receiver answers (health reports no hold then), only for a silent one.
    antenna_full = status(receiver={"rtcm_age_ms": None}, health=hl("receiver_reset", "running", recovery_mode="full"),
                          alerts=no_rtcm, casters=wait_data)
    silent_full = status(receiver={"rtcm_age_ms": 30000}, health=hl("rescan", "running", recovery_mode="full", **hold),
                         casters=wait_data)
    # Alerts only chosen during the wait: nothing in progress, the stall reported.
    watched = status(receiver={"rtcm_age_ms": None}, casters=wait_data, alerts=no_rtcm,
                     health={"level": "degraded", "last_step": "reconfigure", "step": "none", "step_phase": "none",
                             "recovery_mode": "alerts_only"})
    # Between two steps (a step given up while the station ran a session of its own): still amber, nothing named.
    between = status(receiver={"rtcm_age_ms": None}, health=hl("none", "none"), alerts=no_rtcm, casters=wait_data)
    out = heroes(search, found, antenna, no_sky, antenna_full, watched, between, silent_full)
    assert (out[0]["key"], out[0]["tone"], out[0]["line"]) == ("rx_stopped", "warn", "The station is searching for the receiver.")
    assert out[1]["line"] == "The station found the receiver and waits for corrections." and out[1]["plan"] is None
    assert (out[2]["key"], out[2]["title"], out[2]["tone"]) == ("no_rtcm", "Receiver sends no corrections", "warn"), out[2]
    assert out[2]["line"] == "The receiver answers. Check that the antenna is on the socket marked M and sees the open sky."
    assert out[2]["plan"] == "The station re-sent the receiver's settings and waits for corrections."
    assert out[2]["alert"] == "no_rtcm" and out[2]["also"] == [] and out[2]["delivering"] is False
    assert (out[3]["key"], out[3]["title"], out[3]["tone"]) == ("no_satellites", "Antenna sees no satellites", "warn")
    assert out[3]["line"] == "Check that the antenna is on the socket marked M and sees the open sky."
    assert out[3]["plan"] == "The station is re-sending the receiver's settings."
    assert out[4]["plan"] == "The station is restarting the receiver."
    assert (out[7]["key"], out[7]["line"]) == ("rx_stopped", "The station is searching for the receiver.")
    assert out[7]["plan"] == "Station restart in 27\u00a0min at the latest"
    assert (out[5]["key"], out[5]["tone"], out[5]["plan"]) == ("no_rtcm", "bad", None)
    assert out[5]["facts"] == ME_FACT
    assert (out[6]["key"], out[6]["tone"], out[6]["plan"]) == ("no_rtcm", "warn", "The station tries again shortly.")
    silent_between = status(receiver={"rtcm_age_ms": 30000}, health=hl("none", "none"))
    assert heroes(silent_between)[0]["line"] == "The station tries again shortly."
    # "The receiver answers" only while receiver.responding says so (the Receiver page's "Answers" row).
    quiet = status(receiver={"rtcm_age_ms": None, "responding": False}, casters=wait_data, alerts=no_rtcm,
                   health={"level": "failed", "last_step": "receiver_reset", "step": "none", "step_phase": "none",
                           "recovery_mode": "alerts_only"})
    answering = status(receiver={"rtcm_age_ms": None, "responding": True}, casters=wait_data, alerts=no_rtcm,
                       health=hl("reconfigure", "waiting"))
    lines = [h["line"] for h in heroes(quiet, answering)]
    assert lines == ["Check that the antenna is on the socket marked M and sees the open sky.",
                     "The receiver answers. Check that the antenna is on the socket marked M and sees the open sky."], lines
    # Visitors: the state, no step and no alert.
    pub = heroes(antenna, admin=False)[0]
    assert (pub["key"], pub["line"], pub["plan"]) == ("rx_stopped", "The station is fixing it.", None)

    # Logs: the Status page's state and words (never the alert's other title beside them), the card's line what
    # the recovery does; the same once it has failed (red), and with the receiver not answering now.
    failed = status(receiver={"rtcm_age_ms": None}, health={"level": "failed", "last_step": "receiver_reset"}, alerts=no_rtcm,
                    casters=wait_data)
    quiet_rec = status(receiver={"rtcm_age_ms": None, "responding": False}, health=hl("reconfigure", "waiting"),
                       alerts=no_rtcm, casters=wait_data)
    items = run_ui([("healthItems", antenna), ("healthItems", found), ("healthItems", failed), ("healthItems", quiet_rec),
                    ("healthItems", no_sky)])
    assert items[0] == {"state": "no_rtcm", "alerts": [], "notice": None}
    assert items[1] == {"state": "rx_stopped", "alerts": [], "notice": None}
    assert items[2] == {"state": "no_rtcm", "alerts": [], "notice": None}
    assert items[3] == {"state": "no_rtcm", "alerts": [], "notice": None}
    assert items[4] == {"state": "no_satellites", "alerts": [], "notice": None}
    assert heroes(quiet_rec)[0]["line"] == "Check that the antenna is on the socket marked M and sees the open sky."
    # The alert says it in the same words where another state comes first ("Also:"), and "answers" only while it does.
    journal = {"receiver": {"journal": "waiting", "rtcm_age_ms": None}, "alerts": no_rtcm, "casters": wait_data}
    also = heroes(status(**journal), status(**{**journal, "receiver": {"journal": "waiting", "rtcm_age_ms": None,
                                                                        "responding": False}}))
    assert also[0]["alsoText"] == ["Receiver sends no corrections. The receiver answers. Check that the antenna is on the "
                                   "socket marked M and sees the open sky."], also[0]
    assert also[1]["alsoText"] == ["Receiver sends no corrections. Check that the antenna is on the socket marked M and sees "
                                   "the open sky."], also[1]

    # The rescan for a receiver that answers checks the link; for one that does not, it searches.
    rescan = status(receiver={"rtcm_age_ms": None}, health=hl("rescan", "running"), alerts=no_rtcm, casters=wait_data)
    r = heroes(rescan, search)
    assert r[0]["plan"] == "The station is checking the link to the receiver." and "searching" not in r[0]["plan"]
    assert r[1]["line"] == "The station is searching for the receiver."
    # Between two steps while the station holds the recovery (its own session, the grace after it): nothing promised.
    held = status(receiver={"rtcm_age_ms": None}, health={**hl("none", "none"), "ladder_suspended": True}, alerts=no_rtcm,
                  casters=wait_data)
    held_silent = status(receiver={"rtcm_age_ms": 30000}, health={**hl("none", "none"), "ladder_suspended": True})
    h2 = heroes(held, held_silent)
    assert (h2[0]["key"], h2[0]["tone"], h2[0]["plan"]) == ("no_rtcm", "warn", None), h2[0]
    assert h2[1]["line"] == "The station is fixing it." and "shortly" not in (h2[1]["plan"] or "")
    assert run_ui([("healthPlan", [held["health"], held])]) == [None]
    plans = run_ui([("healthPlan", [antenna["health"], antenna]), ("healthPlan", [found["health"], found]),
                    ("healthPlan", [antenna_full["health"], antenna_full]), ("healthPlan", [watched["health"], watched]),
                    ("healthPlan", [silent_full["health"], silent_full])])
    assert plans[0] == "The station re-sent the receiver's settings and waits for corrections."
    # The state's own line says it there: not twice.
    assert plans[1] is None
    assert plans[2] == "The station is restarting the receiver."
    assert plans[3] is None
    assert plans[4].startswith("The station restarts itself in 27")
    chips = run_ui([("healthChip", ["recovering", [{"id": "no_rtcm", "source": "health"}], {"tone": "warn"}]),
                    ("healthChip", ["degraded", [{"id": "no_rtcm", "source": "health"}], {"tone": "bad"}])])
    assert chips == [["Recovering", "warn"], ["Needs attention", "bad"]]


def test_onocoy_is_the_protected_slot_in_use_and_a_closed_port_is_said_once():
    """The Onocoy slot is the first protected slot that is on, else the first protected one (as the station counts
    it): a disabled Onocoy slot left in a lower slot never hides the one that streams. A closed receiver port is the
    cause of the missing receiver: the no-receiver alert's cable advice is not repeated under it."""
    left_over = caster(0, "Onocoy", "disabled", onocoy_ready=False, budget_s=None, budget_left_s=None, streak_ok=None)
    two = status(casters=[left_over, caster(1, "Onocoy", "streaming")])
    uart = status(receiver={"state": "no_receiver", "uart_closed": True, "rtcm_age_ms": None}, alerts=[NO_RX])
    a, b = heroes(two, uart)
    assert (a["key"], a["title"], a["ono"]) == ("streaming", "Streaming", True), a
    out = run_ui([("onocoy", two)])
    assert out[0]["verdict"] == "On track"
    assert b["key"] == "uart_closed" and b["also"] == [], b


def test_hero_casters_down_one_two_three_and_all_good():
    one = status(casters=[caster(0, "Onocoy", "streaming"),
                          caster(1, "Bench", "auth_failed", reason="auth", next_retry_s=30)])
    two = status(casters=[caster(0, "Onocoy", "backoff", reason="timeout"), caster(1, "Bench", "auth_failed", reason="auth")])
    three = status(casters=[caster(0, "Onocoy", "connecting"), caster(1, "Bench", "backoff", reason="dns"),
                            caster(2, "RTK2go", "wait_network")])
    good_one = status()
    good_two = status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "RTK2go", "streaming")])
    waiting = status(casters=[caster(0, "Onocoy", "wait_data"), caster(1, "RTK2go", "wait_data")])
    dns = status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "backoff", reason="dns")])
    out = heroes(one, two, three, good_one, good_two, waiting, dns)
    assert out[0]["title"] == "Partly streaming"
    assert out[0]["line"] == "Bench is not streaming: login refused. Next try in 30 s." and out[0]["action"] == "Casters"
    # The line names the caster that does not stream: no "1 of 2" count beside it.
    assert out[0]["facts"] == [] and out[0]["subject"] == 1
    assert out[1]["title"] == "Not streaming" and out[1]["tone"] == "bad"
    assert out[1]["line"] == "Onocoy and Bench are not streaming." and out[1]["facts"] == []
    # Three down: the line counts them; the Status page's Casters card lists each, so no fact counts them again.
    assert out[2]["line"] == "3 casters are not streaming."
    assert out[2]["facts"] == []
    assert out[3] == {**out[3], "title": "Streaming", "tone": "ok", "line": "Onocoy is receiving corrections.", "action": None}
    # One caster turned on: no "1 of 1" fact.
    assert out[3]["facts"] == []
    assert out[4]["line"] == "Onocoy and RTK2go are receiving corrections."
    # The line names both as streaming: the count would say it again.
    assert out[4]["facts"] == []
    assert out[5]["line"] == "Waiting for corrections from the receiver."
    assert out[6]["line"] == "Bench is not streaming: DNS lookup failed."
    # The public form: states only, no reason, no button.
    pub = heroes({"api": 1, "public": True, "uptime_s": 60, "device": {"hostname": "onocoydiy"},
                  "receiver": {"state": "streaming", "best_bands": 4},
                  "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}, {"slot": 1, "name": "Bench", "state": "backoff"}],
                  "health": {"level": "ok"}}, {"api": 1, "public": True, "uptime_s": 60, "device": {"hostname": "onocoydiy"},
                  "receiver": {"state": "no_receiver", "best_bands": 0}, "casters": [{"slot": 0, "name": "Onocoy", "state": "wait_data"}],
                  "health": {"level": "recovering"}}, admin=False)
    assert pub[0]["line"] == "Bench is not streaming." and pub[0]["action"] is None and pub[0]["title"] == "Partly streaming"
    assert pub[1]["title"] == "No receiver found" and pub[1]["line"] == "Nothing arrives from the receiver." and pub[1]["actions"] == []


def test_hero_safe_mode_stop_and_alerts_while_streaming():
    safe = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                  casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}])
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    link_loss = status(alerts=[{"id": "link_loss", "source": "health"}, {"id": "power_supply", "source": "health"},
                               {"id": "heap_fragmented", "source": "health"}])
    bare_level = status(health={"level": "degraded"})
    off = status(casters=[caster(0, "Onocoy", "disabled", enabled=False), caster(1, "RTK2go", "disabled", enabled=False, onocoy_ready=False)])
    none = status(casters=[])
    out = heroes(safe, stopped, link_loss, bare_level, off, none)
    assert out[0]["title"] == "Safe mode" and out[0]["tone"] == "bad"
    assert out[0]["line"] == "The receiver and casters are off after 3 crashes. The station tries normal operation again in 45 min."
    assert out[0]["alert"] == "safe_mode" and out[0]["notice"] == "safe_mode" and out[0]["also"] == []
    # Two buttons in safe mode: the cure (an update) and leaving it, a button the page handles.
    assert out[0]["actions"] == ["Updates", "leave-safe"]
    assert out[1]["line"] == "Duplicate stream. The station makes no new attempt until you act."
    assert out[1]["action"] == "What to do" and out[1]["notice"] == "onocoy_stopped:0"
    # Other alerts while everything streams: "Also:" lines with their own words; the title stays and so does its colour.
    assert out[2]["title"] == "Streaming" and out[2]["tone"] == "ok" and out[2]["line"] == "Onocoy is receiving corrections."
    assert out[2]["also"] == ["link_loss", "power_supply", "heap_fragmented"]
    assert out[2]["alsoText"][0].startswith("Receiver data lost. Some data from the receiver was lost")
    # The level alone (no alert to show) leaves the dot green and adds nothing.
    assert out[3]["tone"] == "ok" and out[3]["line"] == "Onocoy is receiving corrections." and out[3]["also"] == []
    assert out[4]["line"] == "Onocoy is set up but turned off. Turn it on to start streaming." and out[4]["action"] == "Turn Onocoy on"
    assert out[5]["line"] == "Add a caster to start streaming." and out[5]["action"] == "Add a caster"
    # The public form says safe mode by its one flag (no crashes, no retry, no button); an older one without it: as before.
    pub = {"api": 1, "public": True, "uptime_s": 60, "device": {"hostname": "onocoydiy"}, "safe_mode": {"active": True},
           "receiver": {"state": "idle", "best_bands": 0}, "casters": [], "health": {"level": "failed"}}
    older = {k: v for k, v in pub.items() if k != "safe_mode"}
    p_safe, p_old = heroes(pub, older, admin=False)
    assert p_safe == {**p_safe, "key": "safe_mode", "title": "Safe mode", "tone": "bad", "line": "The receiver and casters are off.",
                      "actions": []}
    assert p_old["key"] == "receiver" and p_old["title"] == "Starting up"


def test_hero_quality_minimums_for_onocoy():
    """Streaming to Onocoy, but a minimum this board can miss is missed: 4 bands, GLONASS, message 1230. Only while
    the receiver delivers, and unknown values (an older firmware, never configured) flag nothing."""
    out = heroes(status(receiver={"best_bands": 3}), status(receiver={"glonass": False}),
                 status(receiver={"rtcm1230_supported": False}), status(receiver={"best_bands": 3, "glonass": False}),
                 status(receiver={"rtcm1230_supported": None}),
                 status(receiver={"glonass": None, "rtcm1230_supported": None}),
                 status(receiver={"best_bands": 0}),
                 status(receiver={"best_bands": 3}, alerts=[{"id": "bands", "source": "health"}]),
                 status(receiver={"best_bands": 3}, casters=[caster(0, "Onocoy", "disabled", enabled=False),
                                                              caster(1, "RTK2go", "streaming", onocoy_ready=False)]))
    assert out[0]["line"] == "Onocoy gets 3 bands; its quality score needs 4 on one constellation." and out[0]["action"] == "Receiver"
    assert out[1]["title"] == "No GLONASS for Onocoy" and out[1]["line"] == "Onocoy gets no GLONASS; its quality score needs it."
    assert out[2]["title"] == "No GLONASS for Onocoy" and "message 1230" in out[2]["line"]
    assert out[3]["title"] == "Too few bands for Onocoy" and "3 bands and no GLONASS" in out[3]["line"]
    assert [o["key"] for o in out[4:7]] == ["streaming", "streaming", "streaming"]
    # The bands alert is the line: not an "Also:" line too.
    assert out[7]["key"] == "quality" and out[7]["alert"] == "bands" and out[7]["also"] == []
    # Onocoy off: its quality does not matter.
    assert out[8]["key"] == "streaming" and out[8]["line"] == "RTK2go is receiving corrections."


def test_onocoy_today_verdict_from_one_rule():
    """Missed while streak_ok is false; At risk while Onocoy does not stream now, more than 60 % of the offline time is
    used, the data rate is under 98 % after 10 minutes connected, or a quality minimum is missed; On track otherwise.
    Never "On track" while the top card says Onocoy is down or the receiver stalls."""
    ono = lambda **kw: [caster(0, "Onocoy", **{"state": "streaming", **kw})]
    cases = [
        status(),
        status(casters=ono(streak_ok=False, budget_left_s=0), availability={"protected_down_today_s": 500, "budget_left_s": 0}),
        status(casters=ono(streak_ok=False, budget_s=0, budget_left_s=0), availability={"protected_down_today_s": 0, "budget_left_s": 0}),
        status(casters=ono(state="backoff", reason="timeout")),
        status(casters=ono(state="stopped", stop_reason="in_use")),
        status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "resync"}),
        status(availability={"protected_down_today_s": 300, "budget_left_s": 173}),
        status(casters=ono(data_rate_permille=970, connected_s=900)),
        status(casters=ono(data_rate_permille=971, connected_s=34)),
        status(receiver={"best_bands": 3}),
        status(receiver={"glonass": False}, casters=ono() + [caster(1, "Bench", "auth_failed", reason="auth")]),
        status(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False)]),
        status(availability={"protected_down_today_s": 10, "budget_left_s": None}),
    ]
    out = run_ui([("onocoy", c) for c in cases])
    assert out[0] == {**out[0], "verdict": "On track", "tone": "ok", "why": None, "left": 463}
    # The bar: what is left of today's allowance (463 of 473 s), empty once missed.
    assert round(out[0]["share"], 4) == round(463 / 473, 4) and out[6]["share"] == 173 / 473
    assert out[1]["share"] == 0 and out[2]["share"] == 0 and out[12]["share"] == 0
    # Missed is amber like At risk: never weaker than it, never redder than the top card.
    assert out[1]["verdict"] == "Missed" and out[1]["tone"] == "warn" and out[1]["why"] == "Today's offline time is over the budget."
    assert out[2]["verdict"] == "Missed" and "epochs were lost" in out[2]["why"]
    assert out[3]["verdict"] == "At risk" and out[3]["why"] == "Offline time is counting now."
    assert out[4]["verdict"] == "At risk" and out[4]["hero"] == "stopped"
    assert out[5]["verdict"] == "At risk" and out[5]["hero"] == "rx_stopped"
    assert out[6]["verdict"] == "At risk" and out[6]["why"] == "Most of today's offline time is used."
    # The data rate shows only as the cause, and only after 10 minutes connected.
    assert out[7]["verdict"] == "At risk" and out[7]["why"].startswith("Data rate 97.0 %")
    assert out[8]["verdict"] == "On track" and out[8]["why"] is None
    # The top card says the quality problem: the verdict does not repeat it.
    assert out[9]["verdict"] == "At risk" and out[9]["hero"] == "quality" and out[9]["why"] is None
    assert out[10]["verdict"] == "At risk" and out[10]["hero"] == "casters_down"
    assert out[10]["why"] == "No GLONASS: Onocoy's quality score needs it."
    # Onocoy not set up: no card (the top card has the setup button).
    assert out[11] is None
    # Without a trusted clock the offline time left is unknown; the verdict still holds.
    assert out[12]["verdict"] == "On track" and out[12]["left"] is None


def test_state_during_maintenance_restarts_and_one_caster_down():
    """Receiver maintenance: the station says idle while the corrections keep coming, so Onocoy still streams and its
    day stays on track. The restart plan shows while the receiver is fixed or looked for (full recovery only). A line
    about one caster names it as its subject (its row on the page does not say why again)."""
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600, "rtcm_age_ms": 300})
    maint_quiet = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600, "rtcm_age_ms": 9000},
                         casters=[caster(0, "Onocoy", "wait_data")])
    hold = {"reboot_hold": ["admin"], "reboot_hold_left_s": 1578, "reboot_cap_wait_s": None, "reboot_cap_limit": None}
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart", **hold})
    stall_alerts_only = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "recovery_mode": "alerts_only", **hold})
    one_down = status(casters=[caster(0, "Onocoy", "streaming"), caster(2, "Bench", "auth_failed", reason="auth")])
    out = heroes(maint, maint_quiet, stall, stall_alerts_only, one_down, status())
    assert out[0]["key"] == "maintenance" and out[0]["delivering"] is True and out[0]["ono"] is True
    assert out[1]["delivering"] is False
    assert out[2]["plan"] == "Station restart in 27\u00a0min at the latest"
    assert out[3]["plan"] is None
    assert out[4]["subject"] == 2 and out[5]["plan"] is None and out[5]["subject"] is None
    days = run_ui([("onocoy", maint), ("onocoy", maint_quiet)])
    assert days[0]["verdict"] == "On track" and days[1]["verdict"] == "At risk"


def test_status_page_leaves_out_the_notice_the_top_card_says():
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600},
                   update={"app_available": True, "app_version": "v0.4.0"})
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    settings = status(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}])
    # Settings storage goes before the casters: the top card says it even while a caster is down.
    settings_down = status(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}],
                           casters=[caster(0, "Onocoy", "backoff", reason="timeout")])
    # A Wi-Fi trial is the top card's while nothing more important holds.
    trial = status(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"})
    trial_stopped = status(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"},
                           casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    out = run_ui([("notices", [maint, ""]), ("notices", [maint, "casters"]), ("notices", [stopped, ""]),
                  ("notices", [stopped, "receiver"]), ("notices", [settings, ""]), ("notices", [settings, "logs"]),
                  ("notices", [settings_down, ""]), ("notices", [trial, ""]), ("notices", [trial_stopped, ""])])
    keys = [[k for k, _ in o] for o in out]
    # The Status page's Station card says the update: no notice there; another page keeps it.
    assert keys[0] == [], out[0]
    assert keys[1] == ["maintenance", "update"]
    assert keys[2] == [] and keys[3] == ["onocoy_stopped:0"]
    assert out[3][0][1] == "Onocoy stopped: duplicate stream"
    # Logs' Health card says it too (above its alert, with Restart): no notice there either.
    assert keys[4] == [] and keys[5] == [] and keys[6] == []
    assert out[1][1][1] == "Update available"
    assert keys[7] == [] and keys[8] == ["wifi"]
    (t,) = heroes(trial)
    assert t["trial"] == "Cabin" and t["line"].startswith("If it gets no address there")


def test_band_wording_never_promises_a_top_reward():
    """4 bands is the most this receiver reaches (the fifth is Galileo E5 AltBOC, which the UM982 lacks): no page
    calls it Onocoy's top band reward."""
    for f in UI_JS.glob("*.js"):
        assert "top band reward" not in f.read_text(), f.name
    four = {"totals": {"tracked": 30, "cnr_dbhz": 44, "msm_age_ms": 100,
                       "systems": [{"sys": "galileo", "bands": {"l1": 8, "l5": 8, "l6": 7, "l2": 8}}]}}
    three = {"totals": {"tracked": 30, "cnr_dbhz": 44, "msm_age_ms": 100,
                        "systems": [{"sys": "gps", "bands": {"l1": 8, "l2": 8, "l5": 3}}]}}
    a, b = run_ui([("checks", four), ("checks", three)])
    assert a["items"][2][2] == "4 bands on one constellation: the most this receiver can reach."
    assert b["items"][2][2].startswith("Onocoy's quality score needs 4 bands on one constellation.")
    # The same in one line (the Signals card): what Onocoy needs, no reward promised.
    assert a["items"][2][3] == "4 bands on Galileo" and b["items"][2][3].startswith("Onocoy needs 4 bands")


def test_password_mismatch_and_unknown_signal():
    mismatch, short, ok = run_ui([("pwProblem", ["abcdefgh", "abcdefgi"]), ("pwProblem", ["abc", "abc"]),
                                  ("pwProblem", ["abcdefgh", "abcdefgh"])])
    # The setup step and the System page pick the repeat field by this exact text.
    assert mismatch == "The passwords do not match."
    assert short == "Use at least 8 characters." and ok is None
    assert run_ui([("rssi", 0), ("rssi", None)]) == ["–", "–"]


# ---- The receiver pages (60-receiver.js, 05-labels.js) ----

def test_signal_groups_short_and_plain():
    (choices,) = run_ui([("sgChoices", None)])
    names = run_ui([("sgName", [int(c[0].split()[0]), int(c[0].split()[1])]) for c in choices] + [("sgName", [9, 9])])
    assert names == ["3 6 (Galileo E6)", "7 0 (BeiDou B2a)", "4 5 (factory)", "5 0 (no L5)", "9 9"]
    for pair, name, line, short in choices:
        # One short line each; "(recommended)" is the Receiver page's mark, never part of the name others reuse.
        assert line.endswith(".") and len(line) <= 60, pair
        assert "recommended" not in (name + line + short).lower(), pair
        assert not any(m in line for m in ("25164", "build", "2026")), pair


def test_receiver_management_one_choice_over_two_settings():
    """Receiver settings: one choice for automatic configuration and what happens if corrections stop. Each option one
    concrete line; any other combination is Custom, says what it does and offers the option nearest to it."""
    (m,) = run_ui([("manage", None)])
    assert {k: v[:3] for k, v in m.items()} == {
        "station": [True, "full", "The station (recommended)"],
        "no_restart": [True, "no_reboot", "The station, but never restart itself"],
        "me": [False, "alerts_only", "Me (alerts only)"]}
    # One line each (at most about 80 characters: one line in the card on a desktop).
    for _, _, name, line in m.values():
        assert line.endswith(".") and len(line) <= 80 and not any(w in line for w in ("RTCM", "SAVECONFIG", "autocfg"))
    assert "restarting itself" in m["station"][3] and "never restarts" in m["no_restart"][3]
    # What the station does on its own: a profile the owner saves still reaches the receiver ("Save and apply").
    assert m["me"][3].startswith("Configures nothing on its own") and "receiver nothing" not in m["me"][3]
    keys = run_ui([("manageKey", [a, r]) for a in (True, False) for r in ("full", "no_reboot", "alerts_only")])
    assert keys == ["station", "no_restart", None, None, None, "me"]
    custom = run_ui([("manageCustom", [True, "alerts_only"]), ("manageCustom", [False, "full"]),
                     ("manageCustom", [False, "no_reboot"])])
    assert custom[0] == ["The station configures the receiver but does nothing if corrections stop.", "station"]
    assert custom[1][1] == custom[2][1] == "me"
    assert custom[1][0].endswith("still resets it and may restart itself.") and custom[2][0].endswith("still resets it.")
    # Receiver settings' choice in the owner's words; the Status page and Logs say it from the station's side, under the
    # setting's name ("Receiver management"): "You", never "Me", and short.
    assert run_ui([("recoveryLbl", "full"), ("recoveryLbl", "no_reboot"), ("recoveryLbl", "alerts_only")]) == [
        "The station", "The station, but never restart itself", "Me (alerts only)"]
    assert run_ui([("manageLbl", "full"), ("manageLbl", "no_reboot"), ("manageLbl", "alerts_only")]) == [
        "The station", "The station, no restarts", "You (alerts only)"]


def test_receiver_chip_never_says_streaming_while_corrections_stopped():
    """The Receiver card's chip comes from the same state as the Status page's top card."""
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"})
    silent = status(alerts=[{"id": "receiver_silent", "source": "health"}], health={"level": "failed"})
    no_rx = status(receiver={"state": "no_receiver"}, alerts=[NO_RX], casters=[caster(0, "Onocoy", "wait_data")])
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600, "rtcm_age_ms": 300})
    detect = status(receiver={"state": "detecting"}, casters=[caster(0, "Onocoy", "wait_data")])
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    out = run_ui([("rxChip", x) for x in (status(), stall, silent, no_rx, maint, detect, stopped)])
    assert out == [["Streaming", "ok"], ["Stopped sending data", "warn"], ["Stopped sending data", "bad"],
                   ["No receiver found", "bad"], ["Maintenance", "info"], ["Looking for the receiver", "info"],
                   ["Streaming", "ok"]]


def test_survey_pauses_without_data_on_both_pages():
    surveying = {"position": {"locked": False, "source": "survey"}, "survey": {"active": True, "window_left_s": 660},
                 "profile": {"base_mode": "survey_in"}}
    locked = {"position": {"locked": True, "source": "survey"}, "survey": {"active": False}, "profile": {"base_mode": "fixed",
              "fixed": {"lat_deg": 1, "lon_deg": 2, "height_m": 3}}}
    typed = {"position": {"locked": False, "source": "none"}, "profile": {"base_mode": "fixed", "fixed": {"lat_deg": 1}}}
    out = run_ui([("posLine", [surveying, True]), ("posLine", [surveying, False]), ("posLine", [locked, False]),
                  ("posLine", [typed, True]), ("posLine", [{"position": {"source": "none"}}, True]),
                  ("statusPos", [{"surveying": True, "survey_window_left_s": 660}, True]),
                  ("statusPos", [{"surveying": True, "survey_window_left_s": 660}, False]),
                  ("statusPos", [{"surveying": False, "position_known": False}, True]),
                  ("statusPos", [{"surveying": False, "position_known": True}, True]),
                  ("statusPos", [{"surveying": False}, True])])
    assert out[0] == "Surveying, about 11\u00a0min left. Then lock it."
    assert out[1] == "Surveying, paused while no corrections arrive."
    assert out[2].startswith("Locked.") and out[3].startswith("Fixed (typed in).") and out[4] == "No position yet."
    assert out[5] == "Surveying, 11\u00a0min left" and out[6] == "Surveying, paused"
    # After a survey that found nothing the Status page says what the Receiver page says; "Not locked" only for a
    # position the station has (or from a firmware that does not say).
    assert out[7] == "No position yet" and out[8] == "Not locked" and out[9] == "Not locked"


SIG = {"best_sys": "galileo", "best_bands": 4, "e6_lost": False, "epoch_age_ms": 420, "rtcm1230_supported": True,
       "systems": [{"sys": "gps", "satellites": 11, "bands": ["l1", "l2", "l5"], "cnr_dbhz": {"l1": 45, "l2": 41, "l5": 44}},
                   {"sys": "glonass", "satellites": 9, "bands": ["l1", "l2"], "cnr_dbhz": {"l1": 45, "l2": 43}},
                   {"sys": "galileo", "satellites": 11, "bands": ["l1", "l2", "l5", "l6"],
                    "cnr_dbhz": {"l1": 41, "l2": 41, "l5": 41, "l6": 38}},
                   {"sys": "qzss", "satellites": 1, "bands": ["l1", "l2", "l5", "l6"], "cnr_dbhz": {"l1": 40}}]}


def test_signals_card_headline_and_hints():
    """The Signals card: the best constellation while observations arrive, how long they have been missing when they
    stop (never "No signals yet" after a stall). The hints are the antenna check's, from the signal summary: QZSS never
    counts as the best; E6 lost, no GLONASS and a refused 1230 are added when they apply."""
    stale = {**SIG, "epoch_age_ms": 37000}
    out = run_ui([("headline", SIG), ("headline", stale), ("headline", {"epoch_age_ms": None, "systems": []})])
    assert out == ["Galileo · 4 bands", "No signals for 37\u00a0s", "No signals yet"]
    no_e6 = {**SIG, "best_bands": 3, "systems": [s if s["sys"] != "galileo" else {**s, "bands": ["l1", "l2", "l5"]}
                                                 for s in SIG["systems"] if s["sys"] != "qzss"]}
    lost = {**SIG, "e6_lost": True}
    no_glo = {**SIG, "systems": [s for s in SIG["systems"] if s["sys"] != "glonass"]}
    refused = {**SIG, "rtcm1230_supported": False}
    good, bad3, e6, glo, r1230 = run_ui([("sigChecks", [x, 7]) for x in (SIG, no_e6, lost, no_glo, refused)])
    line = lambda r: [[i[0], i[3]] for i in r["items"]]
    # One line each; the 4 bands are the headline's, not a hint as well.
    assert line(good) == [["ok", "32 satellites tracked"], ["ok", "Strong signals (42\u00a0dB\u2011Hz)"]] and good["ok"] is True
    assert line(bad3)[2] == ["warn", "Onocoy needs 4 bands: signal group 3 6 adds Galileo E6"] and bad3["ok"] is False
    # Galileo short of E6 with 3 6 already applied: not that group again, the antenna. The group not read yet: the
    # plain line.
    applied, unknown = run_ui([("sigChecks", [no_e6, 3]), ("sigChecks", [no_e6, None])])
    assert line(applied)[2] == ["warn", "Onocoy needs 4 bands: Galileo E6 is not tracked, check the antenna covers L6"]
    assert line(unknown)[2] == ["warn", "Onocoy needs 4 bands: change the signal group in Receiver settings"]
    assert line(e6)[-1] == ["bad", "Galileo E6 lost: it was tracked before with this signal group"] and e6["ok"] is False
    assert line(glo)[-1] == ["warn", "No GLONASS: Onocoy's quality score needs it"]
    assert line(r1230)[-1] == ["warn", "Message 1230 refused: GLONASS does not count for Onocoy"]


def test_position_message_waits_for_the_survey():
    """While the receiver surveys it sends no position (1005): that row waits, it is not red."""
    never = {"msg": 1005, "count": 0, "age_ms": None, "expected_period_s": 1}
    ok = {"msg": 1077, "count": 900, "age_ms": 400, "expected_period_s": 1}
    late = {"msg": 1077, "count": 900, "age_ms": 9000, "expected_period_s": 1}
    out = run_ui([("msgCheck", [never, True, True]), ("msgCheck", [never, True, False]), ("msgCheck", [ok, True, True]),
                  ("msgCheck", [late, True, True]), ("msgCheck", [late, False, False])])
    assert out == ["waits", "missing", "", "missing", ""]


def test_message_and_satellite_lists_are_one_line_rows():
    """No stacked blocks for the RTCM messages and the satellites on a phone: a table row each. The satellites keep one
    line; a message's name wraps in its half-width card rather than ending in "…"."""
    for name in ("65-signals.js", "66-live.js"):
        text = (UI_JS / name).read_text()
        assert "stack-sm" not in text and "stack: true" not in text, name
    assert "class: 'rows msgs'" in (UI_JS / "65-signals.js").read_text()
    assert "class: 'rows'" in (UI_JS / "66-live.js").read_text()
    css = (UI_JS.parent / "app.css").read_text()
    assert "table.rows th, table.rows td { padding: 0.35rem 0.45rem; white-space: nowrap; }" in css
    assert "table.rows.msgs td.name { white-space: normal; overflow: visible; }" in css


def test_firmware_build_shown_once_and_distances_in_mm_and_cm():
    out = run_ui([("fwText", ["R4.10Build25164", 25164]), ("fwText", ["R4.10", 25164]), ("fwText", [None, 25164]),
                  ("dist", 0.0021), ("dist", 0.0), ("dist", 0.023), ("dist", 3.5), ("dist", None)])
    assert out[:3] == ["R4.10Build25164", "R4.10 (build 25164)", "\u2013"]
    assert out[3:] == ["2\u00a0mm", "0\u00a0mm", "2.3\u00a0cm", "3.50\u00a0m", "\u2013"]


def test_comparison_says_why_it_cannot_start_in_the_stations_order():
    ok = {"state": "streaming", "autocfg": True, "first_apply_held": False, "maintenance": {"on": False},
          "position": {"locked": True}}
    out = run_ui([("sgtBlock", {**ok, "first_apply_held": True, "autocfg": False}),
                  ("sgtBlock", {**ok, "autocfg": False}),
                  ("sgtBlock", {**ok, "maintenance": {"on": True}}),
                  ("sgtBlock", {**ok, "state": "no_receiver", "position": {"locked": False}}),
                  ("sgtBlock", {**ok, "position": {"locked": False}}),
                  ("sgtBlock", ok)])
    assert out == ["Configure the receiver first.", "Needs automatic configuration on.", "Not during receiver maintenance.",
                   "Needs a connected receiver.", "Lock the base position first.", ""]


def test_comparison_lines_short_and_in_hours():
    measuring = {"state": "measuring", "phase": 0, "left_s": 1490, "running_group": {"master": 3, "slave": 6}}
    switching = {"state": "switching", "phase": 1, "left_s": 0, "running_group": {"master": 7, "slave": 0}}
    st = status(receiver={"signal_test": {"state": "deciding", "decide_left_s": 86000, "group": {"master": 7, "slave": 0}}})
    line, sw, idle, notice = run_ui([("sgtLine", measuring), ("sgtLine", switching), ("sgtLine", {"state": "idle"}),
                                     ("sgtNotice", st)])
    assert line == "Group 1 of 2, 3 6 (Galileo E6): measuring, about 25\u00a0min left."
    assert sw == "Switching the receiver to 7 0 (BeiDou B2a). Corrections pause briefly."
    assert idle is None
    # The notice says what the top card says, in one line.
    assert notice[1] == "Choose a signal group"
    assert notice[2] == "With no choice the old group comes back in about 24\u00a0h." and " 1 d" not in notice[2]
    # Elsewhere a running comparison is one title and a few words.
    running = status(receiver={"signal_test": {"state": "measuring", "phase": 1, "left_s": 300, "group": {"master": 7, "slave": 0}}})
    switching = status(receiver={"signal_test": {"state": "switching", "phase": 0, "left_s": 0, "group": {"master": 3, "slave": 6}}})
    n, sw = run_ui([("sgtNotice", running), ("sgtNotice", switching)])
    assert n[1] == "Comparing signal groups"
    assert n[2] == "Group 2 of 2, about 5\u00a0min left."
    assert sw[2] == "Corrections pause briefly."


def test_profile_chip_and_maintenance_notice_everywhere_but_settings():
    chips = run_ui([("profileChip", {"state": "configuring", "autocfg": True, "profile_applied": True}),
                    ("profileChip", {"state": "streaming", "autocfg": True, "config_failed": True}),
                    ("profileChip", {"state": "streaming", "autocfg": True, "profile_applied": True}),
                    ("profileChip", {"state": "streaming", "autocfg": True, "profile_applied": True, "first_apply_held": True}),
                    ("profileChip", {"state": "no_receiver", "autocfg": True, "profile_applied": False})])
    assert chips == [["Applying", "info"], ["Failed", "bad"], ["Applied", "ok"], ["Not applied", "warn"], ["Not applied", "warn"]]
    # The owner manages the receiver ("Me (alerts only)"): a grey chip that says what the Receiver page says, applied or
    # not; amber only while the station manages it. Applying and a failure still show (a saved profile is sent once).
    own = {"state": "streaming", "autocfg": False, "first_apply_held": False}
    chips = run_ui([("profileChip", {**own, "profile_applied": False}), ("profileChip", {**own, "profile_applied": True}),
                    ("profileChip", {**own, "state": "configuring"}), ("profileChip", {**own, "config_failed": True}),
                    ("profileChip", {**own, "first_apply_held": True})])
    assert chips == [["Managed by you", ""], ["Managed by you", ""], ["Applying", "info"], ["Failed", "bad"],
                     ["Not applied", "warn"]]
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600})
    out = run_ui([("notices", [maint, p]) for p in ("settings", "receiver", "receiver/settings", "receiver/live", "casters", "logs",
                                                     "logs/history")])
    # Settings has maintenance next to the receiver update, with End now: no notice that repeats it there; nor
    # Logs' Overview, whose Health card says it with End now. Every other page has it.
    assert [[k for k, _ in o] for o in out] == [[]] + [["maintenance"]] * 4 + [[], ["maintenance"]]



def test_a_profile_the_receiver_acknowledged_reads_applied_not_checked():
    """The board without an antenna: the recovery applied the profile and the receiver acknowledged it, but no window with
    RTCM checked it. Receiver settings' chip says "Applied" in grey (green once checked) and the Receiver card says when it
    is checked; amber "Not applied" only when the receiver does not hold the profile (never acknowledged, the RTCM that
    flows differs, or an older firmware that does not say)."""
    board = {"state": "streaming", "autocfg": True, "first_apply_held": False, "identity_confirmed": True,
             "maintenance": {"on": False}, "profile_applied": False, "profile_acknowledged": True, "rtcm_differs": False}
    checked = {**board, "profile_applied": True}
    never = {**board, "profile_acknowledged": False}
    differs = {**board, "rtcm_differs": True}
    older = {k: v for k, v in board.items() if k != "profile_acknowledged"}
    held = {**never, "first_apply_held": True, "autocfg": False}
    out = run_ui([("profileChip", board), ("profileChip", checked), ("profileChip", never), ("profileChip", differs),
                  ("profileChip", older), ("profileChip", {**board, "state": "configuring"}),
                  ("configWords", [board, False]), ("configWords", [board, True]), ("configWords", [checked, False]),
                  ("configWords", [never, False]), ("configWords", [held, False]),
                  ("profileTaken", board), ("profileTaken", checked), ("profileTaken", never), ("profileTaken", differs)])
    assert out == [["Applied", ""], ["Applied", "ok"], ["Not applied", "warn"], ["Not applied", "warn"],
                   ["Not applied", "warn"], ["Applying", "info"],
                   ["Applied, checked once corrections flow", ""], ["Applied, being checked", ""], ["Applied", "ok"],
                   ["Not applied", "warn"], ["Waiting for your OK", "warn"],
                   True, True, False, False]
    # The Status page's position row: a fixed position the receiver acknowledged is locked, checked or not.
    r = {"rx_maintenance": False, "identity_confirmed": True, "position_locked": True, "position_known": True,
         "surveying": False, "profile_applied": False}
    out = run_ui([("statusPos", [{**r, "profile_acknowledged": True}, False]), ("statusPos", [r, False]),
                  ("statusPos", [{**r, "profile_applied": True}, True])])
    assert out == ["Locked", "Fixed, not applied yet", "Locked"]
    # The Receiver card passes whether corrections arrive; the held first configuration's watch and its follow-up take a
    # profile the receiver acknowledged as configured.
    rx = (UI_JS / "60-receiver.js").read_text()
    assert "['Configuration', configLine(g, !st || st.delivering)]" in rx
    assert "if (profileTaken(g) && !g.first_apply_held) {" in rx
    assert "g.first_apply_held || !profileTaken(g) || g.autocfg) return false;" in rx


def test_a_paused_survey_reads_paused_on_its_chip():
    """Receiver settings' Position chip agrees with the Receiver page: a survey while no corrections arrive is paused,
    in grey, not a blue "Surveying"; it follows the status like the page's line."""
    sv = {"position": {"locked": False, "source": "survey"}, "profile": {"base_mode": "survey_in", "fixed": None},
          "survey": {"active": True, "window_left_s": 600}, "maintenance": {"on": False}, "identity_confirmed": True}
    out = run_ui([("posChip", [sv, False]), ("posChip", [sv, True]), ("posChip", sv), ("posLine", [sv, False]),
                  ("posChip", [{**sv, "position": {"locked": True, "source": "survey"}}, False])])
    assert out == [["Paused", ""], ["Surveying", "info"], ["Surveying", "info"],
                   "Surveying, paused while no corrections arrive.", ["Locked", "ok"]]
    rx = (UI_JS / "60-receiver.js").read_text()
    assert "const c = positionChip(gg, !st || st.delivering);" in rx
    assert "if (forms && g) forms.status();" in rx


def test_save_buttons_say_they_apply_while_you_manage_the_receiver():
    """Receiver settings' Save buttons (Position, Receiver profile): "Save and apply" while the owner manages the
    receiver (automatic configuration off: Me, or the Custom combination), as a changed profile reaches the receiver all
    the same; the card's own "Save position" / "Save profile" while the station manages it, and during the held first
    configuration."""
    station = {"autocfg": True, "recovery": "full", "first_apply_held": False}
    me = {"autocfg": False, "recovery": "alerts_only", "first_apply_held": False}
    custom = {"autocfg": False, "recovery": "full", "first_apply_held": False}
    held = {"autocfg": False, "recovery": "alerts_only", "first_apply_held": True}
    out = run_ui([("saveLabel", [g, w]) for g in (station, me, custom, held) for w in ("position", "profile")])
    assert out == ["Save position", "Save profile", "Save and apply", "Save and apply", "Save and apply", "Save and apply",
                   "Save position", "Save profile"]

# ---- The system pages (90-system.js, 30-auth.js) ----

def test_system_wifi_chip_and_recovery_line():
    out = run_ui([("wifiChip", {"configured": False}), ("wifiChip", {"configured": True, "online": True}),
                  ("wifiChip", {"configured": True, "associated": True}), ("wifiChip", {"configured": True}),
                  ("recoveryHint", None)])
    assert out[:4] == [["Not set up", "plain"], ["Online", "ok"], ["Connecting", "warn"], ["Offline", "bad"]]
    # One line under the admin password: what to do, no paragraph (the login page has the steps in full).
    assert out[4] == "Forgot it? Hold BOOT for 10 s, then join the station's own Wi-Fi and open http://192.168.4.1/."
    # Logs' Wi-Fi: a row each, no value joined to the next with a dot (a wrapped line never starts with one). The
    # signal's words are the casters harness's (it has a DOM).
    net = {"online": True, "ssid": "Home", "rssi": -61}
    ap = {"ssid": "Home", "online": True, "rssi": -61, "bssid": "AA:BB:CC:DD:EE:FF", "channel": 11, "tx_power_dbm": 19.5,
          "ap_steering": None}
    rows, bare, off, unset, none = run_ui([("wifiRows", [net, ap]), ("wifiRows", [net, None]),
                                           ("wifiRows", [{"online": False, "ssid": "Home", "offline_s": 90}, ap]),
                                           ("wifiRows", [{"ssid": ""}, None]), ("wifiRows", [None, None])])
    assert rows == [["Wi-Fi", "Home"], ["Signal", None], ["Access point", "AA:BB:CC:DD:EE:FF"], ["Channel", "11"],
                    ["Transmit power", "19.5\u00a0dBm"], ["Access point can steer", "Unknown"]]
    assert not any("\u00b7" in (r[1] or "") for r in rows)
    assert bare == [["Wi-Fi", "Home"], ["Signal", None]]
    assert off == [["Wi-Fi", "Home, offline for 1\u00a0min 30\u00a0s"]]
    assert unset == [["Wi-Fi", "Not set up"]] and none == [["Wi-Fi", "\u2013"]]


def test_logs_wifi_says_moves_and_whether_the_access_point_steers():
    """Logs' Wi-Fi: whether the access point can ask the station to move, in plain words while online with an
    access point (Unknown until it is read; none without one, as in QEMU), and the moves to another access point this
    start once there was one, beside the drops."""
    net = {"online": True, "ssid": "Home", "rssi": -61}
    ap = {"ssid": "Home", "online": True, "rssi": -61, "bssid": "AA:BB:CC:DD:EE:FF", "channel": 11}
    yes, no, unknown = run_ui([("wifiRows", [net, {**ap, "ap_steering": v}]) for v in (True, False, None)])
    assert yes[-1] == ["Access point can steer", "Yes"] and no[-1] == ["Access point can steer", "No"]
    assert unknown[-1] == ["Access point can steer", "Unknown"]
    # No access point (QEMU), or a build that never reads it (GET /api/wifi leaves ap_steering out): no row, never a
    # lasting "Unknown".
    qemu, never = run_ui([("wifiRows", [net, {**ap, "bssid": None, "ap_steering": None}]), ("wifiRows", [net, ap])])
    assert all(r[0] != "Access point can steer" for r in qemu)
    assert never[-1] == ["Channel", "11"] and all(r[0] != "Access point can steer" for r in never)
    moved, none, drops, nothing = run_ui([
        ("linkCounts", [{"reconnects": 0}, {**ap, "roams": 2}]),
        ("linkCounts", [{"reconnects": 0}, {**ap, "roams": 0}]),
        ("linkCounts", [{"reconnects": 1, "last_disconnect_reason": 8}, {**ap, "roams": 1}]),
        ("linkCounts", [{}, None])])
    assert moved == [["Moves to another access point", "2 this start"]]
    assert none == [] and nothing == []
    assert drops[0][0] == "Wi-Fi drops" and drops[0][1].startswith("1 this start, last: ")
    assert drops[1] == ["Moves to another access point", "1 this start"]


def test_own_wifi_chip_warns_only_while_it_is_open_to_anyone():
    out = run_ui([("apChip", {"active": False}), ("apChip", {"active": False, "password_set": True}),
                  ("apChip", {"active": True, "password_set": True}), ("apChip", {"active": True})])
    # Folded in the Wi-Fi card: its chip is all a closed fold shows, so an open access point without a password says
    # so there.
    assert out == [["Off", ""], ["Off", ""], ["On", "info"], ["On, no password", "warn"]]


def test_update_check_age_in_whole_units():
    out = run_ui([("ago", s) for s in (30, 600, 5400, 7 * 3600, 3 * 86400)])
    assert out == ["just now", "10\u00a0min ago", "2\u00a0h ago", "7\u00a0h ago", "3\u00a0days ago"]


def test_update_check_failure_says_the_status_once_and_no_error_code():
    """A failed check in one sentence with the HTTP status once; the firmware's error code stays in the log."""
    u = {"last_check_result": "no_release", "release_repo": "Averyy/onocoydiy-esp-um982", "last_http_status": 404,
         "last_result": {"job": "check", "message": "ESP_ERR_INVALID_RESPONSE"}, "next_check_in_s": 3600}
    (f,) = run_ui([("checkFailure", u)])
    assert f == {"tone": "warn", "text": "No release found at github.com/Averyy/onocoydiy-esp-um982 (HTTP 404). The repository "
                 "may be private or have no releases yet."}
    assert "ESP_ERR" not in json.dumps(f) and json.dumps(f).count("404") == 1


def test_a_survey_without_data_stays_paused_after_its_window():
    """The station's survey window ends while no data arrives, but the receiver's survey has not moved: both pages and
    Receiver settings' chip still say it is paused, never "No position yet" or "Not locked". With corrections back it
    reads as surveying until it finishes; once it finished without a lock, as before."""
    after = {"position": {"locked": False, "source": "none"}, "profile": {"base_mode": "survey_in", "fixed": None},
             "survey": {"active": False, "window_left_s": 0, "unfinished": True}, "maintenance": {"on": False},
             "identity_confirmed": True}
    done = {**after, "survey": {"active": False, "window_left_s": 0, "unfinished": False}}
    rx = {"rx_maintenance": False, "identity_confirmed": True, "position_locked": False, "position_known": False,
          "surveying": False, "survey_window_left_s": 0, "survey_unfinished": True}
    out = run_ui([("posLine", [after, False]), ("posLine", [after, True]), ("posLine", [done, False]),
                  ("posChip", [after, False]), ("posChip", [after, True]), ("posChip", [done, False]),
                  ("statusPos", [rx, False]), ("statusPos", [rx, True]),
                  ("statusPos", [{**rx, "survey_unfinished": False}, False]),
                  ("statusPos", [{**rx, "survey_unfinished": False, "position_known": True}, True])])
    assert out == ["Surveying, paused while no corrections arrive.", "Surveying. Lock it when it ends.", "No position yet.",
                   ["Paused", ""], ["Surveying", "info"], ["Not locked", ""],
                   "Surveying, paused", "Surveying", "No position yet", "Not locked"]


def test_nothing_is_being_checked_during_maintenance():
    """In receiver maintenance with corrections flowing the station checks nothing: an acknowledged profile that no
    window checked yet says it is checked after maintenance, not "being checked"."""
    g = {"state": "idle", "autocfg": True, "first_apply_held": False, "identity_confirmed": True,
         "maintenance": {"on": True}, "profile_applied": False, "profile_acknowledged": True, "rtcm_differs": False}
    out = run_ui([("configWords", [g, True]), ("configWords", [g, False]), ("profileChip", g),
                  ("configWords", [{**g, "profile_applied": True}, True]),
                  ("configWords", [{**g, "maintenance": {"on": False}}, True])])
    assert out == [["Applied, checked after maintenance", ""], ["Applied, checked after maintenance", ""], ["Applied", ""],
                   ["Applied", "ok"], ["Applied, being checked", ""]]


def test_receiver_unchecked_during_maintenance_says_so():
    """Maintenance that went on across a station restart with nothing kept of what the station knew: no identity this
    boot, and none before maintenance ends. The rows say the receiver is not checked, never "not yet" or "not applied";
    with the identity known (kept, or no restart) they read as before. A check without a verdict in maintenance is not
    "being checked": nothing is."""
    unchecked = {"state": "idle", "maintenance": {"on": True}, "identity_confirmed": False, "autocfg": True,
                 "profile_applied": False, "position": {"locked": False, "source": "none"}, "survey": {"active": False},
                 "profile": {"base_mode": "survey_in", "fixed": None}}
    known = {**unchecked, "identity_confirmed": True}
    locked = {**unchecked, "position": {"locked": True, "source": "survey"}, "profile": {"base_mode": "fixed", "fixed": {}}}
    surveying = {**unchecked, "survey": {"active": True, "window_left_s": 600}}
    out = run_ui([("posLine", [unchecked, True]), ("posLine", [known, True]), ("posLine", [locked, True]),
                  ("posLine", [surveying, True]),
                  ("profileChip", unchecked), ("profileChip", known), ("profileChip", {**unchecked, "autocfg": False}),
                  ("posChip", unchecked), ("posChip", known), ("posChip", surveying)])
    # The Receiver card says it once, in a line of its own: the position row goes (None) unless the station knows it.
    assert out == [None, "No position yet.", "Locked. If the antenna moves, survey again.",
                   "Surveying, about 10\u00a0min left. Then lock it.",
                   ["Not checked", ""], ["Not applied", "warn"], ["Managed by you", ""],
                   ["Not checked", ""], ["Not locked", ""], ["Surveying", "info"]]
    r = {"rx_maintenance": True, "identity_confirmed": False, "profile_applied": False, "position_locked": False,
         "position_known": False, "surveying": False}
    out = run_ui([("statusPos", [r, True]), ("statusPos", [{**r, "position_known": True}, True]),
                  ("statusPos", [{**r, "position_locked": True}, True]),
                  ("statusPos", [{**r, "identity_confirmed": True}, True]),
                  ("statusPos", [{**r, "identity_confirmed": True, "position_locked": True}, True]),
                  ("statusPos", [{**r, "surveying": True, "survey_window_left_s": 300}, True]),
                  ("statusPos", [{**r, "rx_maintenance": False}, True])])
    assert out == ["Not checked during maintenance", "Not checked during maintenance", "Locked", "No position yet",
                   "Fixed, not applied yet", "Surveying, 5\u00a0min left", "No position yet"]
    out = run_ui([("fixPending", ["checking", True, False]), ("fixPending", ["checking", False, False]),
                  ("fixPending", ["checking", True, True]), ("fixPending", ["no_solution", True, False]),
                  ("fixPending", ["refused", True, False])])
    assert out == ["Not checked during maintenance", "Being checked", "\u2013", "Not reported yet", "Not reported by this firmware"]
    # The Receiver card: one line instead of model, configuration and position rows that would each repeat it; the
    # Settings receiver row says it beside maintenance.
    rx = (UI_JS / "60-receiver.js").read_text()
    assert "unchecked ? h('p', { class: 'muted' }, NOT_CHECKED + '.') : null" in rx
    assert "(unchecked ? null : ['Model', h('span', { class: 'muted' }, 'Not identified yet')])" in rx
    upd = (UI_JS / "70-updates.js").read_text()
    assert "rx_maintenance ? NOT_CHECKED + '.' : 'Its firmware shows once the station finds it.'" in upd


def test_receiver_maintenance_starts_in_one_place():
    """Settings' Updates card, next to the receiver update, is the one place that starts it (the notices and the
    Status page end it); Receiver settings has no maintenance card any more."""
    starts = [f.name for f in sorted(UI_JS.glob("*.js")) if "/api/gnss/maintenance', { duration_s" in f.read_text()]
    assert starts == ["70-updates.js"]
    assert "'Receiver maintenance'" not in (UI_JS / "60-receiver.js").read_text()


def test_no_top_level_name_is_declared_twice():
    """The modules share one scope in the bundle: a second top-level function of the same name silently replaces
    the first everywhere (a page's own link helper once dropped the caster box's "↗")."""
    seen: dict[str, str] = {}
    dup = []
    for f in sorted(UI_JS.glob("*.js")):
        for n, line in enumerate(f.read_text().splitlines(), 1):
            m = re.match(r"(?:async\s+)?function\s+(\w+)|(?:const|let|var|class)\s+(\w+)\b", line)
            if not m:
                continue
            name = m.group(1) or m.group(2)
            where = f"{f.name}:{n}"
            if name in seen:
                dup.append((name, seen[name], where))
            seen.setdefault(name, where)
    assert dup == [], dup


# ---- The shell: five tabs, old addresses, notices (20-shell.js) ----

def test_five_tabs_and_old_addresses_land_on_the_new_pages():
    out = run_ui([("nav", None)] + [("moved", a) for a in (
        ["signals", ""], ["signals", "live"], ["signals", "live/start"], ["live", ""], ["system", ""], ["updates", ""],
        ["updates", "session"], ["dashboard", ""], ["casters", "onocoy"], ["receiver", "settings"], ["settings", ""],
        ["logs", ""], ["logs", "console"], ["", ""])])
    # Logs is a tab of its own, between the receiver and the settings.
    assert out[0] == ["Status", "Casters", "Receiver", "Logs", "Settings"]
    # The old Status address lands on "#/" (the Status tab marked), not on a page with no tab.
    assert out[1:9] == ["receiver", "receiver/live", "receiver/live/start", "receiver/live", "settings/wifi", "settings/updates",
                        "settings/session", ""]
    # Pages that exist stay where they are.
    assert out[9:] == [None] * 6


def test_header_says_the_station_state_in_a_word_in_the_top_cards_colour():
    """The header's indicator reads the Status page's state (stationState, the public form for a visitor): every key
    it decides has its word, chosen for it (none falls back to the long title, none is left for a key that is gone),
    in the top card's colour, short enough for a phone's header. "Streaming" only green, "Not streaming" only red,
    amber words for what needs a look, plain words for what is under way: the header never contradicts the page."""
    src = (UI_JS / "15-state.js").read_text()
    # pick('key', ...), and the antenna's reasons (pick(ladder.id, ...): no satellites, no corrections).
    keys = set(re.findall(r"pick\('([a-z_]+)'", src)) | {"no_satellites", "no_rtcm"}
    (mapped,) = run_ui([("stateWordKeys", None)])
    assert set(mapped) == keys
    recovering = {"level": "recovering", "last_step": "reconfigure", "step": "reconfigure", "step_phase": "running"}
    wait = [caster(0, "Onocoy", "wait_data")]
    cases = [
        (dict(safe_mode={"active": True, "crashes": 3}, receiver={"state": "idle"}, casters=[caster(0, "Onocoy", "disabled")]),
         ("safe_mode", "bad", "Safe mode")),
        (dict(receiver={"journal": "waiting"}), ("journal", "warn", "Needs you")),
        (dict(device={"settings_available": False}), ("settings", "bad", "Settings error")),
        (dict(receiver={"state": "flashing", "journal": "running"}), ("updating", "info", "Updating")),
        (dict(receiver={"state": "flash_recovery"}), ("updating", "warn", "Updating")),
        (dict(receiver={"state": "no_receiver", "uart_closed": True}, alerts=[NO_RX]), ("uart_closed", "bad", "Port closed")),
        (dict(receiver={"state": "no_receiver", "rtcm_age_ms": None}, alerts=[NO_RX]), ("no_receiver", "bad", "No receiver")),
        (dict(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"}),
         ("rx_stopped", "warn", "Recovering")),
        (dict(health={"level": "failed"}, alerts=[{"id": "receiver_silent", "source": "health"}], casters=wait),
         ("rx_stopped", "bad", "Not streaming")),
        (dict(health=recovering, alerts=[{"id": "no_satellites", "source": "health"}], casters=wait),
         ("no_satellites", "warn", "Recovering")),
        (dict(health={"level": "failed"}, alerts=[{"id": "no_satellites", "source": "health"}], casters=wait),
         ("no_satellites", "bad", "Not streaming")),
        (dict(health=recovering, alerts=[{"id": "no_rtcm", "source": "health"}], casters=wait), ("no_rtcm", "warn", "Recovering")),
        # The owner's board without its antenna, receiver management "Me (alerts only)".
        (dict(health={"level": "degraded", "recovery_mode": "alerts_only"}, alerts=[{"id": "no_rtcm", "source": "health"}],
              casters=wait), ("no_rtcm", "bad", "Not streaming")),
        (dict(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")]), ("stopped", "bad", "Caster stopped")),
        (dict(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 830}), ("maintenance", "info", "Maintenance")),
        (dict(receiver={"state": "configuring", "signal_test": {"state": "measuring", "phase": 0, "left_s": 600,
                                                               "group": {"master": 3, "slave": 6}}}),
         ("signal_test", "info", "Comparing")),
        (dict(receiver={"signal_test": {"state": "deciding", "decide_left_s": 3600, "group": {"master": 3, "slave": 6}}}),
         ("signal_test", "warn", "Needs you")),
        (dict(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"}), ("wifi_trial", "info", "Trying Wi-Fi")),
        (dict(receiver={"first_apply_held": True}), ("held", "warn", "Needs you")),
        (dict(receiver={"state": "idle"}, casters=wait), ("receiver", "info", "Starting up")),
        (dict(receiver={"state": "detecting"}, casters=wait), ("receiver", "info", "Searching")),
        (dict(receiver={"state": "configuring"}, casters=wait), ("receiver", "info", "Configuring")),
        (dict(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False), caster(1, "RTK2go", "backoff")]),
         ("onocoy_setup", "warn", "Not set up")),
        (dict(casters=[caster(0, "Onocoy", "disabled")]), ("no_caster", "warn", "No caster")),
        (dict(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed", reason="auth")]),
         ("casters_down", "warn", "Partly streaming")),
        (dict(casters=[caster(0, "Onocoy", "backoff", reason="refused")]), ("casters_down", "bad", "Not streaming")),
        (dict(alerts=[{"id": "fixed_far", "source": "station", "distance_m": 40, "height_m": 2}]),
         ("fixed_far", "warn", "Wrong position")),
        (dict(receiver={"best_bands": 3}), ("quality", "warn", "Few bands")),
        (dict(receiver={"glonass": False}), ("quality", "warn", "No GLONASS")),
        (dict(receiver={"rtcm1230_supported": False}), ("quality", "warn", "No GLONASS")),
        (dict(), ("streaming", "ok", "Streaming")),
    ]
    out = run_ui([("word", [status(**c), True]) for c, _ in cases])
    for (_, want), got in zip(cases, out):
        assert tuple(got[:3]) == want, (want, got)
    # Every key the state decides is said by one of these, and the header never says a long title.
    assert {g[0] for g in out} == keys
    for key, tone, word, head in out:
        assert len(word) <= 16, (key, word)
        assert (word == "Streaming") == (tone == "ok"), (key, tone, word)
        assert word != "Not streaming" or tone == "bad", (key, tone, word)
    # A visitor without a session: the public form's state, in the same words.
    public = [status(public=True), status(public=True, casters=[]),
              status(public=True, receiver={"state": "no_receiver", "rtcm_age_ms": None})]
    assert [g[:3] for g in run_ui([("word", [x, False]) for x in public])] == [
        ["streaming", "ok", "Streaming"], ["no_caster", "warn", "No caster"], ["no_receiver", "bad", "No receiver"]]


def test_logs_three_parts_and_the_old_diagnostics_addresses():
    """Overview, History and Console (the page is Logs: its live log cannot be "Logs" too); every address of
    Diagnostics and its parts lands where that content is now, so a bookmark keeps working."""
    out = run_ui([("logsTabs", None)] + [("moved", ["diag", a]) for a in
                                         ("", "history", "logs", "availability", "journal", "tasks", "coredump", "foo",
                                          "history/x")])
    assert out[0] == ["Overview", "History", "Console"]
    assert out[1:] == ["logs", "logs/history", "logs/console", "logs/history", "logs/history", "logs", "logs", "logs",
                       "logs/history"]


def test_every_link_in_the_ui_goes_to_a_page_that_exists():
    """No button, link or notice points at an old address (it would only redirect) or a part a page does not have."""
    pages = {"": None, "casters": None, "receiver": {"settings", "live"}, "settings": {"updates", "session", "wifi", "restart"},
             "logs": {"history", "console"}}
    # The places a part is opened at (#/receiver/settings/manage: the Receiver management card).
    places = {("receiver", "settings"): {"manage"}, ("receiver", "live"): {"start"}}
    bad = []
    for f in sorted(UI_JS.glob("*.js")):
        for m in re.finditer(r"'#/([a-z]*)(?:/([a-z]+))?(?:/([a-z]+))?[a-z/]*'", f.read_text()):
            name, sub, at = m.group(1), m.group(2), m.group(3)
            if name not in pages or (sub and pages[name] is not None and sub not in pages[name]) or \
                    (at and at not in places.get((name, sub), set())):
                bad.append(f"{f.name}: {m.group(0)}")
    assert bad == []
    # Receiver settings opened at that card: its page, the card a focus target with that id.
    rx = (UI_JS / "60-receiver.js").read_text()
    assert "id: 'manage', tabindex: '-1'" in rx and "arg === 'settings/manage'" in rx


# Every notice at once: a slot stopped by the login guard, settings storage gone, maintenance, new Wi-Fi settings
# waiting, the first configuration held, a comparison running and an update.
BUSY = status(device={"settings_available": False},
              receiver={"rx_maintenance": True, "maintenance_left_s": 600, "first_apply_held": True,
                        "signal_test": {"state": "measuring", "phase": 0, "left_s": 300, "group": {"master": 3, "slave": 6}}},
              network={"online": True, "ssid": "Home", "cred_pending": True, "pending_ssid": "Cabin"},
              casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")],
              update={"app_available": True, "app_version": "v0.4.0"})


def test_notices_one_line_one_button_worst_first():
    (lines,) = run_ui([("noticeLines", [BUSY, "logs/history"])])
    assert [ln[0] for ln in lines] == ["settings", "onocoy_stopped:0", "wifi", "maintenance", "held", "signal_test", "update"]
    assert [ln[1] for ln in lines] == ["bad", "bad", "warn", "info", "info", "info", "info"]
    # What needs the user now never folds away: settings storage, the stopped slot, maintenance.
    assert [ln[0] for ln in lines if ln[2]] == ["settings", "onocoy_stopped:0", "maintenance"]
    for key, _, _, line, buttons in lines:
        # One line each: the title and a few words; one button (the update adds Later).
        assert len(line) <= 70, line
        assert buttons == (2 if key == "update" else 1), key
    assert lines[0][3] == "Settings cannot be saved. Restart the station."
    assert lines[2][3] == "New Wi-Fi settings for Cabin not confirmed yet"
    assert lines[3][3] == "Receiver maintenance. Ends in about 10\u00a0min."


def test_notices_fold_after_two_but_never_the_urgent_ones():
    folded, opened = run_ui([("fold", [BUSY, "logs/history", False]), ("fold", [BUSY, "logs/history", True])])
    # The first two of the others (by tone), then "+2 more".
    assert folded == [["settings", "onocoy_stopped:0", "wifi", "maintenance", "held"], 2]
    assert opened == [["settings", "onocoy_stopped:0", "wifi", "maintenance", "held", "signal_test", "update"], 2]
    # Two or fewer others: nothing folds.
    calm = status(update={"app_available": True, "app_version": "v0.4.0"}, receiver={"first_apply_held": True})
    (f,) = run_ui([("fold", [calm, "casters", False])])
    assert f == [["held", "update"], 0]


def test_a_notice_hides_on_the_page_whose_card_says_it():
    safe = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                  casters=[caster(0, "Onocoy", "disabled")])
    uart = status(receiver={"state": "no_receiver", "uart_closed": True})
    journal = status(receiver={"journal": "waiting"})
    trial = status(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"})
    pages = ["casters", "receiver", "receiver/settings", "settings", "logs", "logs/history"]
    cases = [(BUSY, p) for p in pages] + [(x, p) for x in (safe, uart, journal, trial) for p in ("", "settings", "receiver")]
    out = [[k for k, _ in o] for o in run_ui([("notices", list(c)) for c in cases])]
    every = {"settings", "onocoy_stopped:0", "wifi", "maintenance", "held", "signal_test", "update"}
    # Logs' Health card says the station's state (settings storage) above its alerts: its notice goes there,
    # not on History.
    hidden = {"casters": {"onocoy_stopped:0"}, "receiver": {"held"}, "receiver/settings": {"signal_test"},
              "settings": {"wifi", "update", "maintenance"}, "logs": {"settings"}, "logs/history": set()}
    for p, keys in zip(pages, out[:6]):
        assert set(keys) == every - hidden[p], p
    # The Status page: the one its top card says (settings storage here), the update (its Station card says it) and every
    # stopped caster (the top card names each, as its subject or in an "Also:" line).
    ((status_keys,),) = [[[k for k, _ in o] for o in run_ui([("notices", [BUSY, ""])])]]
    assert set(status_keys) == every - {"settings", "update", "onocoy_stopped:0"}, status_keys
    two = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate"),
                          caster(1, "Bench", "stopped", stop_reason="rejected")])
    two_k = [[k for k, _ in o] for o in run_ui([("notices", [two, p]) for p in ("", "receiver", "casters")])]
    assert two_k == [[], ["onocoy_stopped:0", "onocoy_stopped:1"], []]
    (two_hero,) = heroes(two)
    assert two_hero["key"] == "stopped" and two_hero["also"] == ["caster:1"]
    safe_k, uart_k, journal_k, trial_k = out[6:9], out[9:12], out[12:15], out[15:18]
    # The Status page's top card says each of them; Settings has safe mode, the update session and the Wi-Fi trial;
    # the Receiver page's panel says the closed port.
    assert safe_k == [[], [], ["safe_mode"]]
    assert uart_k == [[], ["uart_closed"], []]
    assert journal_k == [[], [], ["journal"]]
    assert trial_k == [[], [], ["wifi"]]
    # Logs: the stopped Onocoy and the closed port are said by the Health card; History keeps their notices.
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="rejected")],
                     alerts=[{"id": "heap_fragmented", "source": "health"}])
    diag = [[k for k, _ in o] for o in run_ui([("notices", [x, p]) for x in (stopped, uart) for p in ("logs", "logs/history")])]
    assert diag == [[], ["onocoy_stopped:0"], [], ["uart_closed"]]
    # Safe mode with its alert: the Health card says the state (the same red as the alert), so its notice goes there.
    # A warn state beside a warn alert: the card says the state, nothing to cover.
    safe_alert = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                        casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}])
    (k,) = run_ui([("notices", [safe_alert, "logs"])])
    assert [x for x, _ in k] == []
    # Until the Health card is on the page (loading, or its first request failed) the notice stays.
    no_card = run_ui([("noticesNoCard", [x, "logs"]) for x in (stopped, safe_alert, BUSY)])
    assert no_card == [["onocoy_stopped:0"], ["safe_mode"], ["settings", "onocoy_stopped:0", "wifi", "maintenance", "held",
                                                             "signal_test", "update"]]


def test_the_page_a_notice_is_on():
    """An address the page itself opens is that page for the notices: #/logs/foo the Overview, #/receiver/live the
    Receiver page, #/receiver/settings/manage Receiver settings; History, Console and Receiver settings are pages of
    their own."""
    out = run_ui([("pageKey", a) for a in (["logs", ""], ["logs", "foo"], ["logs", "history"], ["logs", "console/x"],
                                           ["receiver", ""], ["receiver", "live"], ["receiver", "live/start"],
                                           ["receiver", "settings"], ["receiver", "settings/manage"], ["casters", "onocoy"],
                                           ["", ""])])
    assert out == ["logs", "logs", "logs/history", "logs/console", "receiver", "receiver", "receiver", "receiver/settings",
                   "receiver/settings", "casters", ""]



# ---- Logs (80-diag.js) ----

def test_health_chip_never_disagrees_with_the_status_page():
    """The chip's colour comes from the alerts; with none named (a caster down makes the station "degraded"), from
    the Status page's own state, so Logs never shows amber beside a green Status page or the other way."""
    rx = {"id": "no_receiver", "source": "station"}
    power = {"id": "power_supply", "source": "health"}
    held = {"id": "rx_held", "source": "health"}
    heap = {"id": "heap_fragmented", "source": "health"}
    out = run_ui([("healthChip", ["ok", [], None]), ("healthChip", ["failed", [rx, power], None]),
                  ("healthChip", ["degraded", [power], None]), ("healthChip", ["degraded", [], {"tone": "warn"}]),
                  ("healthChip", ["degraded", [], {"tone": "ok"}]), ("healthChip", ["degraded", [held], None]),
                  ("healthChip", ["recovering", [], None]), ("healthChip", ["degraded", [], {"tone": "info"}]),
                  # The worse of the two: Onocoy stopped (red) beside a fragmented heap (amber) is red.
                  ("healthChip", ["degraded", [heap], {"tone": "bad"}]), ("healthChip", ["ok", [], {"tone": "warn"}]),
                  ("healthChip", ["degraded", [rx], {"tone": "warn"}]), ("healthChip", ["degraded", [held], {"tone": "warn"}]),
                  ("healthChip", ["degraded", [heap], {"tone": "info"}])])
    assert out == [["Healthy", "ok"], ["Needs attention", "bad"], ["Needs attention", "warn"], ["Needs attention", "warn"],
                   ["Healthy", "ok"], ["Needs attention", "info"], ["Recovering", "warn"], ["Busy", "info"],
                   ["Needs attention", "bad"], ["Needs attention", "warn"], ["Needs attention", "bad"],
                   ["Needs attention", "warn"], ["Needs attention", "warn"]]


def test_health_card_says_the_state_at_least_as_bad_as_every_alert():
    """The Status page's state goes above the alerts while it is at least as bad as each of them: a stall (amber)
    beside the profile alert (amber) keeps both; safe mode (red) takes its own alert's place and covers its notice;
    Partly streaming beside a fragmented heap keeps both. No receiver found leaves it to its alert (Logs'
    fuller words, with the search)."""
    drift = {"id": "profile_drift", "source": "health"}
    heap = {"id": "heap_fragmented", "source": "health"}
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"},
                   alerts=[drift])
    safe = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                  casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}])
    partly = status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed", reason="auth")],
                    alerts=[heap])
    no_rx = status(receiver={"state": "no_receiver", "rtcm_age_ms": None}, alerts=[NO_RX],
                   casters=[caster(0, "Onocoy", "wait_data")])
    out = run_ui([("healthItems", x) for x in (stall, safe, partly, no_rx, status(alerts=[heap]))])
    assert out[0] == {"state": "rx_stopped", "alerts": ["profile_drift"], "notice": None}
    assert out[1] == {"state": "safe_mode", "alerts": [], "notice": "safe_mode"}
    assert out[2] == {"state": "casters_down", "alerts": ["heap_fragmented"], "notice": None}
    assert out[3] == {"state": None, "alerts": ["no_receiver"], "notice": None}
    # Streaming (green) never goes above an alert.
    assert out[4] == {"state": None, "alerts": ["heap_fragmented"], "notice": None}
    # Safe mode and its retry are the Health card's line: This start says neither again.
    js = (UI_JS / "80-diag.js").read_text()
    assert "'Normal retry in'" not in js and "badge('Safe mode'" not in js


def test_since_this_start_names_only_what_happened():
    out = run_ui([("sinceStart", {"gnss_stalls": 0, "recoveries": 0, "caster_restarts": 0, "network_recoveries": 0}),
                  ("sinceStart", {"gnss_stalls": 2, "recoveries": 1, "caster_restarts": 0, "network_recoveries": 3})])
    assert out == ["No stalls or recoveries", "2 receiver stalls, 1 recovery step, 3 network recoveries"]


def test_starts_in_a_word_and_the_receiver_at_power_on_in_a_few():
    """Earlier starts: a short word per start, a planned one by why it was planned. This start: whether the receiver
    kept running, in a few words (nothing about reset pins); nothing while not known."""
    out = run_ui([("startKind", ["power_on", "none"]), ("startKind", ["crash", "none"]), ("startKind", ["brownout", "none"]),
                  ("startKind", ["other", "none"]), ("startKind", ["planned", "app_update"]), ("startKind", ["planned", "user"]),
                  ("startKind", ["planned", "gnss_ladder"]), ("startKind", ["planned", "something_new"]),
                  ("rxBoot", "kept_running"), ("rxBoot", "restarted"), ("rxBoot", "pending"), ("rxBoot", "unknown"),
                  ("rxBoot", ["kept_running", "power_on"]), ("rxBoot", ["restarted", "power_on"]),
                  ("rxBoot", ["kept_running", "brownout"]), ("rxBoot", ["restarted", "brownout"]),
                  ("rxBoot", ["pending", "brownout"])])
    assert out[:8] == ["Power-on", "Crash", "Power dip", "Reset", "Update", "Restart", "Recovery", "Restart"]
    assert out[8:12] == ["The receiver kept running", "The power was lost", "", ""]
    # After a power dip (a brownout start) the receiver is judged too: it kept running, or the dip restarted it as well.
    assert out[12:] == ["The receiver kept running", "The power was lost", "The receiver kept running",
                        "The receiver restarted too", ""]
    assert all(len(w) <= 14 for w in out[:8])


def test_the_receiver_at_the_start_event_in_a_few_words():
    """History's 'Receiver at the start' row (after a power-on or a power dip): what happened, without how the station
    judged it; a receiver that restarted too lost the power as well, whichever start it was."""
    out = run_ui([("journalDetail", {"kind": "rx_boot", "a": a, "b": b}) for a, b in ((1, 1), (2, 3), (0, 0))])
    assert out == ["Kept running", "Restarted too: the power was lost", "Not known"]
    # One fact, one word: Overview's "This start" after a power-on says it as History does.
    (overview,) = run_ui([("rxBoot", ["restarted", "power_on"])])
    assert overview == "The power was lost" and out[1].endswith(overview.lower())


def test_a_paused_recovery_says_what_it_waits_for():
    """Logs' "Receiver management" while the recovery waits: what for, so a frozen countdown under it makes sense
    and it is plain that it goes on by itself; the choice otherwise."""
    def hl(**st):
        return {"ladder_suspended": True, "recovery_mode": "full", "station": {"state": "streaming", "lease_held": False,
                                                                              "since_lease_release_ms": None, **st}}
    out = run_ui([("recoveryNow", hl(state="configuring", lease_held=True)),
                  ("recoveryNow", hl(lease_held=True)), ("recoveryNow", hl(since_lease_release_ms=4000)),
                  ("recoveryNow", hl(state="idle", rx_maintenance=True)), ("recoveryNow", hl(state="detecting")),
                  ("recoveryNow", hl(state="flashing")), ("recoveryNow", hl(state="idle")),
                  ("recoveryNow", hl(since_lease_release_ms=60000)),
                  ("recoveryNow", {**hl(), "ladder_suspended": False}),
                  ("recoveryNow", {**hl(), "ladder_suspended": False, "recovery_mode": "no_reboot"}),
                  ("recoveryNow", {**hl(), "ladder_suspended": False, "recovery_mode": "alerts_only"})])
    # Otherwise the choice, as the Status page names it.
    assert out == ["Paused while the receiver is configured"] * 3 + [
        "Paused (receiver maintenance)", "Paused while the station looks for the receiver",
        "Paused during the receiver update", "Paused", "Paused", "The station", "The station, no restarts",
        "You (alerts only)"], out


def test_log_lines_and_event_times_fit_one_line():
    out = run_ui([("logParts", "I (122407) gnss_link: host baud 460800 -> 115200"), ("logParts", "E (3723000) ntrip: x"),
                  ("logParts", "ets Jun  8 2016 00:22:57"), ("timeRange", ["9:40 AM", "9:42 AM"]),
                  ("timeRange", ["9:40 AM", "9:42 AM"]), ("timeRange", ["Oct 14, 11:58 PM", "Oct 15, 12:03 AM"])])
    assert out[0] == ["0:02:02", "I", "gnss_link: host baud 460800 -> 115200"]
    assert out[1] == ["1:02:03", "E", "ntrip: x"]
    assert out[2] is None   # shown as it is
    assert out[3] == "9:40–9:42 AM" and out[4] == "9:40–9:42 AM"
    assert out[5] == "Oct 15, 12:03 AM"   # too long for the time column: the newest only


def test_logs_lists_are_one_line_rows():
    """No stacked blocks on a phone: the log, the event list, the starts and the tasks keep one row per item."""
    js = (UI_JS / "80-diag.js").read_text()
    assert "stack: true" not in js and "stack-sm" not in js
    css = (UI_JS.parent / "app.css").read_text()
    assert "stack-sm" not in css and "table.events, table.events tbody" not in css
    assert "pre.log .ln { display: block; overflow: hidden; text-overflow: ellipsis; cursor: pointer; }" in css
    assert "td.when { width: 1%; white-space: nowrap;" in css
    assert ("table.events td + td { width: 100%; max-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }"
            in css)


def test_a_day_of_the_last_30_in_words():
    """One line a day, only what happened; the planned time is a part of the offline time."""
    out = run_ui([("dayParts", [{"down_s": [612, 5], "planned_s": [60, 0], "reconnects": [4, 0], "power_off_s": 1800, "lost_epochs": 38,
                                 "rx_overflows": 1}, 0]),
                  ("dayParts", [{"down_s": [0, 5], "planned_s": [0, 0], "reconnects": [1, 0], "power_off_s": 0}, 0]),
                  ("dayParts", [{"down_s": [0], "planned_s": [0], "reconnects": [0], "power_off_s": 0, "lost_epochs": 0}, 0])])
    assert out[0] == ["10\u00a0min 12\u00a0s offline", "1\u00a0min of it planned", "4 reconnects", "38 epochs lost",
                      "30\u00a0min powered off", "1 receiver data overflow"]
    assert out[1] == ["1 reconnect"]
    assert out[2] == []


# ---- The fixed-position check: the warning on the Status page, its words, and History ----

FIXED_FAR = {"id": "fixed_far", "source": "station", "horizontal_m": 118, "vertical_m": 4, "distance_m": 118,
             "height_only": False}
ANTENNA_LINE = "The receiver answers. Check that the antenna is on the socket marked M and sees the open sky."


def test_fixed_position_far_says_how_far_and_where_to_check():
    out = heroes(
        status(alerts=[FIXED_FAR]),
        status(alerts=[{**FIXED_FAR, "horizontal_m": 3, "vertical_m": -82, "distance_m": 82, "height_only": True}]),
        status(alerts=[{**FIXED_FAR, "horizontal_m": 2340, "distance_m": 2340}]),
        status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed", reason="auth")],
               alerts=[FIXED_FAR]),
        status(receiver={"best_bands": 3}, alerts=[FIXED_FAR]))
    # Streaming, but every correction carries a wrong position: the top card, amber, with where to fix it.
    assert (out[0]["key"], out[0]["title"], out[0]["tone"]) == ("fixed_far", "Fixed position looks wrong", "warn")
    assert out[0]["line"] == "The fixed position is 118\u00a0m from where the receiver is. Check it in Receiver settings."
    assert out[0]["actions"] == ["Receiver settings"] and out[0]["alert"] == "fixed_far" and out[0]["also"] == []
    assert out[1]["line"] == "The fixed height is 82\u00a0m below where the receiver is. Check it in Receiver settings."
    assert out[2]["line"].startswith("The fixed position is 2.3\u00a0km from where the receiver is.")
    # A caster down comes first; the warning is then an "Also:" line with the same words.
    assert out[3]["key"] == "casters_down" and out[3]["also"] == ["fixed_far"]
    assert out[3]["alsoText"] == ["Fixed position looks wrong. The fixed position is 118\u00a0m from where the receiver is. "
                                  "Check it in Receiver settings."]
    # Before the band count for Onocoy, which says nothing about the position.
    assert out[4]["key"] == "fixed_far" and out[4]["also"] == []
    # Visitors see no alerts.
    assert heroes(status(alerts=[FIXED_FAR]), admin=False)[0]["key"] == "streaming"


def test_no_corrections_from_a_fixed_position_far_enough_to_stop_them():
    """A receiver whose own solution is 300 m or more from its fixed position sends no corrections: the position is the
    reason, not the antenna (it computes a solution: it has sky). Nearer, the antenna line stays, the warning beside it."""
    def stalled(dist):
        return status(health={"level": "failed"}, casters=[caster(0, "Onocoy", "wait_data")],
                      alerts=[{"id": "no_rtcm", "source": "health"},
                              {**FIXED_FAR, "horizontal_m": dist, "distance_m": dist}])
    far, near = heroes(stalled(420), stalled(120))
    assert (far["key"], far["title"]) == ("no_rtcm", "Receiver sends no corrections")
    # One sentence that says the position is why.
    assert far["line"] == ("The fixed position is 420\u00a0m from where the receiver is, which stops them. "
                           "Check it in Receiver settings.")
    assert far["also"] == [] and far["actions"] == ["Receiver settings"]
    assert near["key"] == "no_rtcm" and near["line"] == ANTENNA_LINE and near["also"] == ["fixed_far"]
    assert near["actions"] == []
    # Logs and the Receiver card say it once too: the state, without the warning again under it (failed or while
    # the recovery runs), and the warning beside the antenna line when it is nearer.
    recovering = stalled(1400)
    recovering["health"] = {"level": "recovering", "last_step": "receiver_reset", "step": "receiver_reset",
                            "step_phase": "waiting", "recovery_mode": "full"}
    items = run_ui([("healthItems", stalled(2340)), ("healthItems", recovering), ("healthItems", stalled(120))])
    assert items[0] == {"state": "no_rtcm", "alerts": [], "notice": None}
    assert items[1] == {"state": "no_rtcm", "alerts": [], "notice": None}
    assert items[2] == {"state": "no_rtcm", "alerts": ["fixed_far"], "notice": None}
    gnss = {"alerts": ["fixed_far", "config_failed"]}
    rx = run_ui([("rxAlerts", [gnss, stalled(2340)]), ("rxAlerts", [gnss, stalled(120)]), ("rxAlerts", [gnss, status()])])
    assert rx == [["config_failed"], ["fixed_far", "config_failed"], ["fixed_far", "config_failed"]]


def test_fixed_position_far_never_beside_a_green_chip_or_a_bare_mark():
    """While corrections flow with the fixed position far off, the Receiver card's chip is amber like the Status page's
    top card, which says it (its title, its line or an "Also:" line); the Status page's position row does not."""
    chips = run_ui([("rxChip", status(alerts=[FIXED_FAR])),
                    ("rxChip", status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed",
                                                                                         reason="auth")], alerts=[FIXED_FAR])),
                    ("rxChip", status())])
    assert chips == [["Position looks wrong", "warn"], ["Position looks wrong", "warn"], ["Streaming", "ok"]]
    # The Status page's position row says "Locked" whatever the check says: the top card says the position looks wrong.
    words = run_ui([("statusPos", [{"position_locked": True, "profile_applied": True}, True])])
    assert words == ["Locked"]


def test_a_session_of_the_stations_own_keeps_the_recovery_line():
    """The station configures the receiver (an apply of its own) while the recovery waits after a step: the Status
    page keeps the recovery's amber card and its line (health keeps the level and the reason meanwhile), never the red
    one of a recovery that gave up."""
    hl = {"level": "recovering", "last_step": "receiver_reset", "step": "receiver_reset", "step_phase": "waiting",
          "recovery_mode": "no_reboot", "ladder_suspended": True}
    no_rtcm = [{"id": "no_rtcm", "source": "health"}]
    wait_data = [caster(0, "Onocoy", "wait_data")]
    during = status(receiver={"state": "configuring", "rtcm_age_ms": None, "responding": True}, health=hl,
                    alerts=no_rtcm, casters=wait_data)
    before = status(receiver={"state": "streaming", "rtcm_age_ms": None, "responding": True},
                    health={**hl, "ladder_suspended": False}, alerts=no_rtcm, casters=wait_data)
    out = heroes(before, during)
    assert [(h["key"], h["tone"], h["plan"]) for h in out] == [
        ("no_rtcm", "warn", "The station restarted the receiver and waits for corrections.")] * 2, out


def test_health_card_rows_say_the_step_and_the_next_attempt_only_where_they_hold():
    """The last recovery step's row goes only while the card says the step in progress (the line under it, or the
    recovery's own state line); with another state on top (no receiver found while the rescan's wait runs, the port
    closed during a link restart) the row stays. No next attempt in alerts only: its pass sends nothing."""
    hl = lambda step, phase, **kw: {"level": "recovering", "last_step": step, "step": step, "step_phase": phase,
                                     "recovery_mode": "full", **kw}
    wait_data = [caster(0, "Onocoy", "wait_data")]
    norx = status(receiver={"state": "no_receiver", "rtcm_age_ms": None, "next_detect_in_s": 240}, alerts=[NO_RX],
                  health=hl("rescan", "waiting"), casters=wait_data)
    uart = status(receiver={"state": "no_receiver", "uart_closed": True, "rtcm_age_ms": None}, alerts=[NO_RX],
                  health=hl("link_restart", "running"), casters=wait_data)
    antenna = status(receiver={"rtcm_age_ms": None}, health=hl("reconfigure", "waiting"),
                     alerts=[{"id": "no_rtcm", "source": "health"}], casters=wait_data)
    silent = status(receiver={"rtcm_age_ms": 30000}, health=hl("rescan", "running"), casters=wait_data)
    said = run_ui([("stepSaid", [x["health"], x]) for x in (norx, uart, antenna, silent)])
    assert said == [False, False, True, True]
    alerts_only = status(receiver={"rtcm_age_ms": None}, alerts=[{"id": "no_rtcm", "source": "health"}], casters=wait_data,
                         health={"level": "failed", "last_step": "reconfigure", "recovery_mode": "alerts_only"})
    full = status(receiver={"rtcm_age_ms": None}, alerts=[{"id": "no_rtcm", "source": "health"}], casters=wait_data,
                  health={"level": "failed", "last_step": "receiver_reset", "recovery_mode": "full"})
    rows = run_ui([("nextAttempt", [{**alerts_only["health"], "gnss_retry_in_s": 472}, alerts_only]),
                   ("nextAttempt", [{**full["health"], "gnss_retry_in_s": 472}, full])])
    assert rows[0] is None and rows[1][0] == "Next recovery attempt in"


def test_fixed_position_check_in_history():
    out = run_ui([("journalDetail", {"kind": "fixed_check", "a": (4 << 8) | 1, "b": 118}),
                  ("journalDetail", {"kind": "fixed_check", "a": (-82 << 8) | 1, "b": 3}),
                  ("journalDetail", {"kind": "fixed_check", "a": (1 << 8) | 2, "b": 2}),
                  ("journalDetail", {"kind": "fixed_check", "a": 3, "b": 0}),
                  ("journalDetail", {"kind": "fixed_check", "a": (34 << 8) | 4, "b": 2}),
                  ("evFilter", None)])
    # Short enough for a phone's row: how far off (the warning says which way); cleared, whichever warning it was.
    assert out[:5] == ["118\u00a0m off", "Height 82\u00a0m off", "Looks right again", "Changed",
                       "Sea level, 34\u00a0m off"]
    receiver = [f for f in out[5] if f[0] == "receiver"][0]
    assert "fixed_check" in receiver[2]


SEA_LEVEL = {"id": "fixed_far", "source": "station", "horizontal_m": 2, "vertical_m": 34, "distance_m": 34,
             "height_only": True, "sea_level": True}


def test_fixed_height_typed_as_height_above_sea_level_says_so_and_what_to_enter():
    """A sea-level height is the same warning (amber, Receiver settings) in its own words: what it looks like and the height
    the field wants, without the metres (the Receiver page's Details row has them)."""
    out = heroes(status(alerts=[SEA_LEVEL]),
                 status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed", reason="auth")],
                        alerts=[SEA_LEVEL]))
    assert (out[0]["key"], out[0]["title"], out[0]["tone"]) == ("fixed_far", "Fixed position looks wrong", "warn")
    assert out[0]["line"] == ("The fixed height looks like a height above sea level. "
                              "Enter the height above the ellipsoid in Receiver settings.")
    assert out[0]["actions"] == ["Receiver settings"] and out[0]["alert"] == "fixed_far"
    assert out[1]["key"] == "casters_down" and out[1]["alsoText"] == [
        "Fixed position looks wrong. The fixed height looks like a height above sea level. "
        "Enter the height above the ellipsoid in Receiver settings."]
    chips = run_ui([("rxChip", status(alerts=[SEA_LEVEL]))])
    assert chips == [["Position looks wrong", "warn"]]


def test_use_the_current_position_only_when_it_copies_another_position():
    """A fixed base sends the position it was given in every 1005: copying that repairs nothing, so the button is there only
    for a 1005 that is not the locked position (a survey's, or the receiver's own while someone else configures it)."""
    arp = {"valid": True, "lat_deg": 45.1, "lon_deg": -75.1, "height_m": 85.1}
    out = run_ui([("arpCopy", {"position": {"locked": True}, "arp": {**arp, "offset_m": 0.0021}}),
                  ("arpCopy", {"position": {"locked": True}, "arp": {**arp, "offset_m": 0.0}}),
                  ("arpCopy", {"position": {"locked": False}, "arp": {**arp, "offset_m": None}}),
                  ("arpCopy", {"position": {"locked": True}, "arp": {**arp, "offset_m": 3.2}}),
                  ("arpCopy", {"position": {"locked": False}, "arp": {"valid": False}}),
                  ("arpCopy", {})])
    assert out == [False, False, True, True, False, False]
