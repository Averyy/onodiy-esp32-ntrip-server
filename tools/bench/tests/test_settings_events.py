# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Which settings writes announce themselves: every writer in components/ against settings_schema.h.

Every open page reloads its data on SETTINGS_EVENT_CHANGED (the WebSocket's
settings_changed), so only settings may post it. The schema marks the keys
the station stores on its own as "device state"; those are written with the
settings_set_state_* writers (no event), every other key with settings_set_*
(one event). This reads the firmware's sources: each write names its key, and
a batch's keys are judged by the commit that ends it. A key that is not a
SETTINGS_KEY_ macro at the call (a helper's parameter) is listed below with
what its callers pass.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCHEMA = ROOT / "components" / "settings" / "include" / "settings_schema.h"
COMPONENTS = ROOT / "components"

KEY_DEF = re.compile(r'#define\s+(SETTINGS_KEY_\w+)\s+"[^"]*"\s*/\*\*<(.*?)\*/')
SCALARS = r"(?:str|u8|u16|u32|i64|blob)"
NOTIFYING = re.compile(rf"\bsettings_(?:set_{SCALARS}|erase_key)\s*\(\s*[^,()]+,\s*(\w+)")
QUIET = re.compile(rf"\bsettings_(?:set_state_{SCALARS}|erase_state_key)\s*\(\s*[^,()]+,\s*(\w+)")
BATCH_SET = re.compile(r"\bsettings_batch_set_\w+\s*\(\s*\w+\s*,\s*(\w+)")
BATCH_COMMIT = re.compile(r"\bsettings_batch_commit(_state)?\s*\(")

# Writes whose key is a helper's parameter: (file, the parameter) -> the keys its callers pass.
HELPERS = {
    ("components/um98x_ctl/um98x_ctl.c", "key"): re.compile(r"\bput_str_if_changed\s*\(\s*([^,()]+)"),
    ("components/network/network.c", "key_ssid"): re.compile(r"\bstore_pair\s*\(\s*([^,()]+)"),
    ("components/network/network.c", "key_pass"): re.compile(r"\bstore_pair\s*\(\s*[^,()]+,\s*([^,()]+)"),
}


def schema() -> dict[str, bool]:
    """Every SETTINGS_KEY_ macro: True for device state."""
    keys = {name: "device state" in comment for name, comment in KEY_DEF.findall(SCHEMA.read_text())}
    assert len(keys) == SCHEMA.read_text().count("#define SETTINGS_KEY_"), "a key without its /**< ... */ comment"
    return keys


def writes(root: Path = ROOT) -> list[tuple[str, int, str, str]]:
    """(file, line, kind, key) for every settings write in root/components outside the settings component, in
    source order: kind "notify" or "quiet" for a single write; a batch's keys "batch-notify" or "batch-quiet" after
    the commit that ends them, then that commit ("commit-notify" or "commit-quiet", no key)."""
    out = []
    for f in sorted((root / "components").rglob("*.c")):
        rel = f.relative_to(root).as_posix()
        if rel.startswith("components/settings/"):
            continue
        text = f.read_text()
        line = lambda pos: text.count("\n", 0, pos) + 1   # noqa: E731
        found = sorted([(m.start(), "notify", m.group(1)) for m in NOTIFYING.finditer(text)]
                       + [(m.start(), "quiet", m.group(1)) for m in QUIET.finditer(text)]
                       + [(m.start(), "set", m.group(1)) for m in BATCH_SET.finditer(text)]
                       + [(m.start(), "commit", "quiet" if m.group(1) else "notify")
                          for m in BATCH_COMMIT.finditer(text)])
        pending: list[tuple[int, str]] = []
        for pos, what, arg in found:
            if what == "set":
                pending.append((pos, arg))
            elif what == "commit":
                out += [(rel, line(p), "batch-" + arg, k) for p, k in pending]
                out.append((rel, line(pos), "commit-" + arg, ""))
                pending = []
            else:
                out.append((rel, line(pos), what, arg))
        assert not pending, f"{rel}: batch writes with no commit after them"
    return out


def problems(found: list[tuple[str, int, str, str]], keys: dict[str, bool]) -> list[str]:
    """What is written the wrong way: device state with an event, a setting without one, a batch of device
    state only that announces itself, a key the scan cannot judge (not a SETTINGS_KEY_ macro, not in HELPERS)."""
    out = []
    batch: list[str] = []
    for rel, line, kind, key in found:
        if kind.startswith("commit-"):
            continue
        if (rel, key) in HELPERS:
            passed = [keys.get(k) for k in helper_keys(rel, key)]
            if not passed or None in passed or len(set(passed)) != 1:
                out.append(f"{rel}:{line} {key}: its callers pass {helper_keys(rel, key)}: one kind of key each")
                continue
            state = passed[0]
        elif key not in keys:
            out.append(f"{rel}:{line} {key}: not a SETTINGS_KEY_ macro (add the helper to HELPERS)")
            continue
        else:
            state = keys[key]
        if kind == "notify" and state:
            out.append(f"{rel}:{line} {key} is device state: settings_set_state_* / settings_erase_state_key")
        elif kind in ("quiet", "batch-quiet") and not state:
            out.append(f"{rel}:{line} {key} is a setting: settings_set_* / settings_erase_key announce it")
    for rel, line, kind, key in found:
        if kind == "batch-notify":
            batch.append(key)
        elif kind == "commit-notify":
            judged = [k for k in batch if k in keys]
            if len(judged) == len(batch) and all(keys[k] for k in judged):
                out.append(f"{rel}:{line} a batch of device state only ({', '.join(judged)}): "
                           "settings_batch_commit_state")
            batch = []
    return out


def helper_keys(rel: str, param: str) -> list[str]:
    """What the helper's callers pass for `param` (its own definition left out)."""
    callers = HELPERS[(rel, param)]
    return [k.strip() for f in COMPONENTS.rglob("*.c") for k in callers.findall(f.read_text())
            if not k.startswith("const ")]


def test_the_schema_tells_device_state_from_settings():
    keys = schema()
    state = {k for k, s in keys.items() if s}
    # What the station stores on its own, every minute or at every boot, or after detecting the receiver.
    for k in ("SETTINGS_KEY_LEDGER_TODAY", "SETTINGS_KEY_LEDGER_HIST", "SETTINGS_KEY_LEDGER_ALIVE",
              "SETTINGS_KEY_BOOT_HISTORY", "SETTINGS_KEY_BOOT_GEN", "SETTINGS_KEY_GNSS_MODEL", "SETTINGS_KEY_GNSS_FW",
              "SETTINGS_KEY_GNSS_BAUD", "SETTINGS_KEY_GNSS_APPLIED", "SETTINGS_KEY_GNSS_SG_TEST",
              "SETTINGS_KEY_GNSS_MAINT", "SETTINGS_KEY_GNSS_FLASH_JOB", "SETTINGS_KEY_CST_BACKOFF",
              "SETTINGS_KEY_OTA_TRIAL", "SETTINGS_KEY_OTA_LAST_CHECK", "SETTINGS_KEY_OTA_ETAG",
              "SETTINGS_KEY_OTA_MANIFEST"):
        assert k in state, k
    # What a person sets (and the receiver profile, which the station also changes at a position lock).
    for k in ("SETTINGS_KEY_HOSTNAME", "SETTINGS_KEY_ADMIN_HASH", "SETTINGS_KEY_WIFI_SSID", "SETTINGS_KEY_WIFI_AP_PASS",
              "SETTINGS_KEY_GNSS_PROFILE", "SETTINGS_KEY_GNSS_BASE_POS", "SETTINGS_KEY_GNSS_AUTOCFG",
              "SETTINGS_KEY_GNSS_RECOVERY", "SETTINGS_KEY_CST_CFG", "SETTINGS_KEY_CST_CA_PEM",
              "SETTINGS_KEY_OTA_REPO", "SETTINGS_KEY_WEB_PUBLIC", "SETTINGS_KEY_SETUP_DONE"):
        assert keys.get(k) is False, k


def test_device_state_is_written_quietly_and_settings_announce_themselves():
    found = writes()
    assert sum(1 for x in found if not x[2].startswith("commit-")) > 40, "the scan found too few writes"
    assert problems(found, schema()) == []


def test_the_helpers_pass_the_keys_their_writes_are_for():
    """um98x_ctl's put_str_if_changed writes quietly: the receiver's model and firmware only. network's
    store_pair batch announces itself: Wi-Fi credentials, settings."""
    keys = schema()
    passed = helper_keys("components/um98x_ctl/um98x_ctl.c", "key")
    assert sorted(passed) == ["SETTINGS_KEY_GNSS_FW", "SETTINGS_KEY_GNSS_MODEL"]
    assert all(keys[k] for k in passed), passed
    for param in ("key_ssid", "key_pass"):
        passed = helper_keys("components/network/network.c", param)
        assert len(passed) >= 3 and all(keys.get(k) is False for k in passed), passed


def test_the_scan_catches_device_state_written_with_an_event(tmp_path):
    """The fault this guards against: the ledger's today, written every minute with settings_set_blob, reloaded
    every open page once a minute; the applied state's batch, committed with an event after every apply."""
    (tmp_path / "components" / "ledger").mkdir(parents=True)
    (tmp_path / "components" / "ledger" / "ledger.c").write_text(
        "static void step(void) {\n"
        "    (void)settings_set_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_TODAY, buf, len);\n"
        "    settings_batch_t b;\n"
        "    (void)settings_batch_set_u32(b, SETTINGS_KEY_GNSS_APPLIED, 1);\n"
        "    (void)settings_batch_commit(b);\n"
        "    (void)settings_set_state_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_AUTOCFG, 1);\n"
        "    (void)settings_set_blob(SETTINGS_NS_GNSS, some_key, buf, len);\n"
        "}\n")
    found = writes(tmp_path)
    assert problems(found, schema()) == [
        "components/ledger/ledger.c:2 SETTINGS_KEY_LEDGER_TODAY is device state: settings_set_state_* / "
        "settings_erase_state_key",
        "components/ledger/ledger.c:6 SETTINGS_KEY_GNSS_AUTOCFG is a setting: settings_set_* / settings_erase_key "
        "announce it",
        "components/ledger/ledger.c:7 some_key: not a SETTINGS_KEY_ macro (add the helper to HELPERS)",
        "components/ledger/ledger.c:5 a batch of device state only (SETTINGS_KEY_GNSS_APPLIED): "
        "settings_batch_commit_state",
    ]
