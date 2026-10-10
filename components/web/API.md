# Web API contract

This is the contract between the web backend (`components/web`) and the
browser UI embedded in the same app image. Both sides follow it; a change to
either side that breaks it changes this file in the same commit. The data
behind each endpoint comes from the public headers of `station`,
`ntrip_server`, `network`, `fw_update`, `pkg_store`, `health`, `diag`,
`ledger`, `gnss_obs`, `settings` and `lifecycle`; field meanings follow those
headers unless this file says otherwise.

API version: **1** (`api` in `GET /api/session` and `GET /api/status`).
Adding fields or endpoints keeps the number; removing or re-typing a field
bumps it. Clients ignore fields they do not know and show enum values they do
not know verbatim.

Contents: 1 Conventions · 2 Request checks · 3 Authentication · 4 Endpoint
index · 5 Session and setup · 6 Status · 7 WebSocket · 8 Receiver ·
9 Receiver update sessions · 10 Casters · 11 Wi-Fi and system settings ·
12 Updates · 13 Uploads · 14 Diagnostics · 15 System actions · 16 Captive
portal and first setup · 17 What is refused when · 18 Server limits and
implementation notes · 19 Open items · Appendix: enumerations.

---

## 1. Conventions

**Transport.** Plain HTTP on port 80 (LAN device; no TLS). JSON is UTF-8,
`Content-Type: application/json`. Every API response carries
`Cache-Control: no-store`.

**Types.** The examples show each field's type: a number written without a
fraction is an integer, one with a fraction (`*_deg`, `*_m`) is a float;
strings, booleans, arrays and objects as shown. A field may be `null` where
its notes or the sentinel rule below say so.

**Units are in the field name:**

| Suffix | Unit |
|---|---|
| `_s`, `_ms` | seconds, milliseconds (durations and ages) |
| `_unix` | Unix time, seconds UTC; `null` when no trusted clock was set at the time |
| `_pct`, `_permille` | 0-100, 0-1000 (integers) |
| `_deg`, `_m` | decimal degrees (positions: WGS84, 9 decimals; sky angles: integers), metres (4 decimals) |
| `_dbhz` | carrier-to-noise density, dB-Hz (integer) |
| `_bytes` or a plain count | bytes or events, as the name says |

**Values.**
- Counters are JSON integers. The 64-bit counters of the headers stay far
  below 2^53, so JavaScript reads them exactly.
- `null` means unknown or not available (yet). Header sentinels map to
  `null`: `UINT32_MAX` ("never", "unknown"), unix time 0, an `esp_err_t` of 0.
- Enums are lowercase strings (appendix), not the C numbers (the raw event
  journal is the one exception, section 14). Bit-flag sets (alerts, AP
  reasons) are arrays of strings.
- SHA-256 digests are 64 lowercase hex characters. Program addresses are hex
  strings (`"0x4200abcd"`). An `esp_err_t` is its name (`"ESP_ERR_TIMEOUT"`).
- Strings from the receiver or a caster are copied as received and escaped
  for JSON (control bytes as `\u00XX`).

**Success.** `200` with a JSON body for synchronous calls (the updated
resource, or `{"ok": true}`); `202` with `{"accepted": true, ...}` when the
call only starts work (progress arrives over the WebSocket and in the
matching `GET`).

**Errors.** Every error has the same body:

```json
{"error": {"code": "journal_open", "message": "A receiver update session is open", "field": null, "retry_in_s": null, "by": null}}
```

`code` is stable and machine-readable; `message` is English text for logs
and fallbacks (the UI words its own text per code). `field` names the
offending request field (`invalid_field`), `retry_in_s` comes with
`rate_limited` and `app_trial`, `by` with `reboot_inhibited` (the holder's
label, e.g. `"receiver flash"`). Other fields may appear per code (listed
with the endpoint).

| HTTP | Codes |
|---|---|
| 400 | `bad_json`, `invalid_field`, `confirm_required`, `bad_request`, `invalid_image`, `size_mismatch` |
| 401 | `unauthorized` (no session, or it expired: show the login) |
| 403 | `forbidden` (Origin or CSRF check failed, public status off), `wrong_password`, `not_on_setup_ap` |
| 404 | `not_found`, `nothing_staged`, `no_journal`, `no_coredump` |
| 405 | `method_not_allowed` |
| 409 | state conflicts: `busy`, `journal_open`, `not_waiting`, `app_trial`, `app_update`, `safe_mode`, `not_in_safe_mode`, `receiver_not_streaming`, `unsupported_model`, `package_pinned`, `manifest_changed`, `not_offered`, `rolled_back_ack_required`, `reboot_inhibited`, `settings_unavailable`, `no_position_yet`, `caster_policy`, `sha256_mismatch`, `downgrade`, `no_rollback_image`, `admin_already_set`, `admin_not_set` |
| 411 | `length_required` (uploads) |
| 413 | `too_large` |
| 415 | `unsupported_media_type` |
| 421 | `bad_host` |
| 422 | `not_this_project` (app upload) |
| 429 | `rate_limited` (with `retry_in_s`) |
| 500 | `internal` (`message` carries the `esp_err_t` name) |
| 501 | `not_available_yet` (receiver flashing is not built yet), `checks_disabled` (this build has no manifest key) |
| 503 | `too_many_clients` (WebSocket) |

---

## 2. Request checks

Applied to every request before any handler runs.

1. **Host.** The `Host` header (port ignored) must be the station's current
   IPv4 address, the AP address (`192.168.4.1`), the hostname in use,
   `<hostname>.local`, or `<hostname>.` followed by one of `lan`, `home`,
   `home.arpa`, `localdomain`, `internal`, `fritz.box` (names compared
   without case). Anything else on the
   station interface gets `421 bad_host` (DNS rebinding). On the AP
   interface a foreign Host is the captive portal instead (section 16).
2. **Origin.** On every `POST`, `PUT`, `DELETE` and WebSocket handshake that
   carries an `Origin` header, its host must equal the request's `Host` and
   its scheme must be `http`; otherwise `403 forbidden`.
3. **CSRF.** `POST` and `PUT` need `Content-Type: application/json` (a
   `charset` parameter is allowed), also when the body is empty. Uploads
   need `Content-Type: application/octet-stream` plus the header
   `X-Requested-With: onocoydiy`. Anything else: `415`. `DELETE` needs no
   body: a cross-site page cannot send a `DELETE` without a CORS preflight,
   which this server never grants. There is no CORS: no `Access-Control-*`
   header is ever sent, and `OPTIONS` gets `405`.
4. **Sizes.** JSON bodies at most 2 KB (`PUT /api/casters/{n}/ca`: 6 KB),
   else `413`. Uploads: section 13.
5. **Response headers** on every page and API response:
   `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
   `Referrer-Policy: no-referrer`; HTML also gets a Content-Security-Policy
   that allows only the device's own origin (scripts, styles, images,
   connections including the WebSocket) and forbids framing.

UI assets are pre-compressed, embedded in the image and served with
`Content-Encoding: gzip`, `ETag: "<app version>"` and `Cache-Control:
no-cache` (`304` on a matching `If-None-Match`). The UI routes with the URL
fragment (`/#/casters`), so the server serves only `/`, `/index.html` and the
fixed asset names; other paths outside `/api/` and `/ws` get `404` (on the
AP interface: the captive redirect).

---

## 3. Authentication

**One admin account, no default password.** The password is set during
first setup (`POST /api/setup/admin`) and stored as `sys/admin_hash`: a v1
blob `{iterations u32, salt[16], PBKDF2-HMAC-SHA256[32]}`, derived through
PSA (`psa_key_derivation_*` with `PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)`,
cost = `iterations`, salt = 16 random bytes from `esp_fill_random`, 32-byte
output, constant-time compare). The iteration count is chosen so one check
takes about 0.3 s on the device (starting value 10 000, to be measured); it
is stored with each hash, so it can change without a migration. Passwords
are 8-64 bytes of UTF-8 without control characters.

**Sessions.**
- `POST /api/session` with the password sets the cookie
  `sid=<32 hex>; HttpOnly; SameSite=Strict; Path=/` (128 random bits; no
  `Secure`: plain HTTP; no `Max-Age`: a browser-session cookie).
- Sessions live in RAM only: a reboot logs everyone out. At most 4; a fifth
  login ends the least recently used one.
- Idle timeout 30 min, extended by every authenticated HTTP request and
  every message a WebSocket client sends (the UI pings only while its tab is
  visible, so a forgotten tab does not stay logged in). Absolute lifetime
  24 h.
- `DELETE /api/session` ends the session. Changing the password ends every
  other session. When `sys/admin_hash` changes or disappears behind the
  web's back (BOOT-button admin reset, factory reset; seen through
  `SETTINGS_EVENT`), every session ends.
- An ended session's WebSocket is closed with code `4001`.

**Login throttling** (global, RAM): the first 5 consecutive failures cost
nothing extra; after the n-th failure (n >= 5) the next attempt is accepted
only `min(2^(n-4), 300)` s later. Earlier attempts get `429 rate_limited`
with `retry_in_s`, without checking the password and without counting. A
success resets the count. A wrong `current` password in
`PUT /api/admin-password` counts the same way.

**First password ("claim").** `POST /api/setup/admin` works only while no
admin password is stored **and** the request arrived over the device's own
AP (the socket's local address is the AP address). The AP is up only while
the station is unconfigured, offline (fallback), or for 15 min after a
BOOT-button hold (recovery AP; open unless `wifi/ap_pass` is set), so a
claim needs someone within Wi-Fi range of the station while its AP is up.
Otherwise `409 admin_already_set` or `403 not_on_setup_ap`. With the
settings unavailable this boot (degraded NVS) the claimed password lives in
RAM for this boot only (`"persisted": false`).

**What works without a session.**

| Always | Only with `web/public_status = 1` |
|---|---|
| UI assets; captive-portal redirects (AP only) | `GET /api/status` in its public form (section 6) |
| `GET /api/session`, `POST /api/session` | `GET /api/gnss/signals`, `GET /api/sky` |
| `POST /api/setup/admin` (claim rules above) | `/ws` with the `status` topic in its public form |

Everything else answers `401 unauthorized` without a session. The public
form never contains a host, mountpoint, username, IP address, SSID, version
or the antenna position. `web/public_status` defaults to off.

---

## 4. Endpoint index

Access: **pub** = always, **pub\*** = without a session only when public
status is on, **adm** = admin session, **claim** = section 3.

| Method | Path | Access | Purpose | Backend |
|---|---|---|---|---|
| GET | `/api/session` | pub | Auth state, setup flags | web, settings |
| POST | `/api/session` | pub | Log in | web (PSA) |
| DELETE | `/api/session` | adm | Log out | web |
| POST | `/api/setup/admin` | claim | Set the first admin password, log in | web, settings |
| POST | `/api/setup/done` | adm | Mark first setup finished | settings `sys/setup_done` |
| PUT | `/api/admin-password` | adm | Change the password | web, settings |
| GET | `/api/status` | pub\*/adm | Dashboard summary and update banner | all `*_get_status` |
| GET | `/ws` | pub\*/adm | Live status, events, progress, log | section 7 |
| GET | `/api/gnss` | adm | Receiver, profile, base position | station, gnss_obs, settings |
| PUT | `/api/gnss/profile` | adm | Edit the base profile and auto-configuration | settings, `station_request_reconfigure` |
| POST | `/api/gnss/apply` | adm | Re-apply the profile | `station_request_reconfigure` |
| POST | `/api/gnss/position/lock` | adm | Lock the surveyed position | `station_lock_position` |
| DELETE | `/api/gnss/position/lock` | adm | Forget it and survey again | `station_request_resurvey` |
| GET | `/api/gnss/signals` | pub\*/adm | Bands, CNR, observed RTCM set | gnss_obs |
| GET | `/api/sky` | pub\*/adm | Satellites in view | `gnss_obs_get_sky` |
| GET | `/api/gnss/flash` | adm | Receiver update session, allowed actions | station |
| POST | `/api/gnss/flash/retry` | adm | Flash the same package again | `station_flash_user_action` |
| POST | `/api/gnss/flash/restart-receiver` | adm | Leave the loader, check the version | `station_flash_user_action` |
| POST | `/api/gnss/flash/clear` | adm | Forget the session | `station_flash_user_action` |
| GET | `/api/casters` | adm | All slots: config and status | ntrip_server |
| GET | `/api/casters/{n}` | adm | One slot | ntrip_server |
| PUT | `/api/casters/{n}` | adm | Edit slot n | `ntrip_server_set_config` |
| PUT, DELETE | `/api/casters/{n}/ca` | adm | Store / remove the slot's CA certificate | settings `cstN/ca_pem` |
| GET | `/api/wifi` | adm | Wi-Fi state, AP, cached scan | network |
| PUT | `/api/wifi` | adm | Station credentials, AP password | `network_set_credentials`, settings |
| POST | `/api/wifi/scan` | adm | Fresh scan | `network_scan_start` |
| GET | `/api/system` | adm | Hostname, public status, identity | settings, lifecycle |
| PUT | `/api/system` | adm | Edit hostname, public status | settings |
| GET | `/api/update` | adm | Versions, trial, banner detail | fw_update |
| POST | `/api/update/check` | adm | Check now (never installs) | `fw_update_check` |
| POST | `/api/update/app` | adm | Install the offered app | `fw_update_install_app` |
| POST | `/api/update/gnss` | adm | Receiver update from the offered build | `fw_update_install_gnss` |
| POST | `/api/update/rollback` | adm | Back to the previous app | `fw_update_rollback` |
| POST | `/api/upload/app` | adm | Manual app image | `fw_update_upload_app_*` |
| POST | `/api/upload/gnss` | adm | Manual receiver package | pkg_store, `um98x_flash_inspect_staged` |
| GET | `/api/upload/gnss` | adm | The uploaded package awaiting confirmation | pkg_store |
| POST | `/api/upload/gnss/confirm` | adm | Start the receiver update with it | `station_request_flash` |
| DELETE | `/api/upload/gnss` | adm | Cancel, release the package | `pkg_store_release` |
| GET | `/api/health` | adm | Health, loss counters, boots, core dump summary | health, gnss_link, rtcm_hub, lifecycle, diag |
| GET | `/api/availability` | adm | Per-day availability, budget | ledger, ntrip_server |
| GET | `/api/diag/tasks` | adm | Task statistics | `diag_task_stats` |
| PUT | `/api/diag/log-level` | adm | Runtime log level per tag | `diag_set_log_level` |
| GET | `/api/logs` | adm | Log ring (this or previous boot) | `diag_log_read*` |
| GET | `/api/journal` | adm | Event journal | `diag_journal_read` |
| GET, DELETE | `/api/coredump` | adm | Download / erase the core dump | `diag_coredump_*` |
| POST | `/api/system/reboot` | adm | Reboot | lifecycle |
| POST | `/api/system/factory-reset` | adm | Erase user settings, reboot | settings, lifecycle |
| POST | `/api/system/safe-mode/clear` | adm | Leave safe mode | `lifecycle_clear_safe_mode` |

---

## 5. Session and setup

### `GET /api/session` (pub)

```json
{"api": 1, "authenticated": true, "admin_set": true, "setup_done": true,
 "claim_allowed": false, "on_setup_ap": false, "public_status": false,
 "settings_available": true, "idle_left_s": 1740}
```

`claim_allowed` = no admin password stored and this request came over the
AP. `idle_left_s` is `null` without a session. The UI starts here: claim
form, login form, setup wizard or dashboard.

### `POST /api/session` (pub)

Request `{"password": "..."}`. `200 {"authenticated": true, "idle_timeout_s": 1800}`
plus `Set-Cookie`. Errors: `403 wrong_password`, `429 rate_limited`,
`409 admin_not_set` (no password yet: claim over the AP),
`409 settings_unavailable` (a stored password could not be read: recover
with the BOOT button).

### `DELETE /api/session` (adm)

`200 {"ok": true}` and `Set-Cookie: sid=; Max-Age=0; Path=/`.

### `POST /api/setup/admin` (claim)

Request `{"password": "..."}`. `200 {"authenticated": true, "persisted": true}`
plus `Set-Cookie`. Errors: `400 invalid_field` (`password`),
`409 admin_already_set`, `403 not_on_setup_ap`.

### `POST /api/setup/done` (adm)

Empty body. Writes `sys/setup_done = 1`. `200 {"setup_done": true}`.

### `PUT /api/admin-password` (adm)

Request `{"current": "...", "new": "..."}`. `200 {"ok": true}`; every other
session ends. Errors: `403 wrong_password` (throttled like a login),
`400 invalid_field` (`new`), `409 settings_unavailable`.

---

## 6. Status

### `GET /api/status` (pub\*/adm)

One object for the dashboard. The WebSocket `status` topic carries the same
object as sections (section 7), so the UI keeps one model. Admin form:

```json
{
  "api": 1,
  "public": false,
  "uptime_s": 86400,
  "device": {"hostname": "onocoydiy", "version": "v0.3.0", "dev_build": false,
             "time_unix": 1760090000, "time_source": "gnss", "settings_available": true},
  "safe_mode": {"active": false, "retry_in_s": null, "crashes": 0},
  "network": {"online": true, "ssid": "Home", "ip": "192.168.1.50", "rssi": -61,
              "upstream": "ok", "offline_s": 0, "ap_active": false, "ap_reasons": []},
  "receiver": {"state": "streaming", "model": "UM982", "firmware": "R4.10Build18975",
               "responding": true, "identity_confirmed": true, "profile_applied": true,
               "position_locked": true, "surveying": false, "survey_window_left_s": 0,
               "rtcm_age_ms": 180, "best_sys": "galileo", "best_bands": 4, "journal": "none"},
  "casters": [
    {"slot": 0, "name": "Onocoy", "enabled": true, "protected": true, "host": "servers.onocoy.com",
     "state": "streaming", "reason": "none", "state_age_s": 3600, "connected_s": 3600,
     "today_online_s": 43200, "today_offline_s": 4, "graced_uptime_permille": 1000,
     "streak_ok": true, "data_rate_permille": 1000, "budget_left_s": 469,
     "bytes_sent": 74000000, "lag_ms_now": 40, "next_retry_s": 0, "hint": ""}
  ],
  "health": {"level": "degraded", "last_step": "none", "ladder_suspended": false},
  "alerts": [{"id": "link_loss", "source": "health"}],
  "availability": {"protected_down_today_s": 4, "budget_left_s": 469},
  "update": {"app_available": true, "app_version": "v0.4.0",
             "app_reverted_version": "", "app_reverted_in_stage1": false,
             "gnss_available": true, "gnss_build": 19152, "gnss_version": "R4.10Build19152",
             "gnss_blocked_reason": "none", "gnss_blocked_left_s": 0,
             "gnss_flash_available": false, "checking": false, "last_check_unix": 1760080000}
}
```

| Field | Notes |
|---|---|
| `device.time_source` | `ledger_clock_source()`: `gnss`, `sntp` or `none` (then `time_unix` is `null`) |
| `safe_mode` | `lifecycle_get_boot_info()`: `retry_in_s` until the automatic normal-boot retry |
| `network.upstream` | last probe: `none`, `ok`, `no_ip`, `dns_failed`, `no_upstream` |
| `receiver.state` | `station_state_t` |
| `receiver.rtcm_age_ms` | since the last CRC-valid RTCM frame (`gnss_link`), `null` = never |
| `receiver.journal` | `none`, `running`, `waiting`, `unreadable`, `unknown` (section 9) |
| `casters[]` | every slot; the full status is in `/api/casters`. Protected-slot fields (`graced_uptime_permille`, `streak_ok`, `data_rate_permille`, `budget_left_s`) are `null` for other slots |
| `alerts[]` | health's alerts (`source: "health"`) and the station's (`source: "station"`); `config_failed` carries `retry_in_s` |
| `availability.budget_left_s` | protected slot's data-rate-adjusted `budget_left_s`, else `ledger_protected_budget_left_s()`; `null` without a protected slot or a trusted clock. Information only, never a gate |
| `update` | the banner (section 12); admin form only |

In safe mode the receiver section shows `state: "idle"` with the journal
read from settings, and every caster `disabled`.

Public form (no session, public status on):

```json
{"api": 1, "public": true, "uptime_s": 86400,
 "device": {"hostname": "onocoydiy"},
 "receiver": {"state": "streaming", "best_bands": 4},
 "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}],
 "health": {"level": "ok"}}
```

---

## 7. WebSocket

**Path** `/ws`, no subprotocol. Same request checks as HTTP (Host, Origin).
The handshake is authenticated by the session cookie; without one it is
accepted only when public status is on, and then only the public `status`
topic is served. Refused handshakes get the HTTP error (`401`, `403`); a
fifth concurrent client gets `503 too_many_clients`.

**Client to server** (text frames, JSON, at most 256 bytes; larger closes
with `1009`):

| Message | Effect |
|---|---|
| `{"type": "subscribe", "topics": ["status", "progress", "log"]}` | replaces the topic set (admin topics are ignored for public clients) |
| `{"type": "ping"}` | `{"type": "pong"}`; also extends the session's idle timeout |

Default topics after the handshake: `status` and `progress` (admin), `status`
(public). `event` messages go to every admin client.

**Server to client:**

| `type` | When | Body |
|---|---|---|
| `hello` | once, first | `{"type": "hello", "api": 1, "admin": true, "topics": ["status", "progress"], "status_period_ms": 1000}` |
| `status` | 1 Hz | `{"type": "status", "seq": 812, "full": false, "uptime_s": 86400, "data": {"casters": [...], "receiver": {...}}}` |
| `event` | at once, at most 10/s (coalesced) | `{"type": "event", "event": "caster_state", "slot": 0, "state": "backoff"}` |
| `progress` | at most 2 Hz while a job runs, plus one final message | `{"type": "progress", "job": "gnss_update", "phase": "downloading", "pct": 42, "done": false}` |
| `log` | every 500 ms when there are new lines, at most 4 KB per message | `{"type": "log", "cursor": 51234, "gap": false, "text": "I (1234) station: ...\n"}` |
| `pong` | reply to `ping` | `{"type": "pong"}` |

**Status deltas.** Once a second the server builds the status object of
section 6 once for all clients (the public form separately), compares each
section with the previous second's (CRC-32 of its JSON text; `casters` per
slot) and sends one message with only the sections that changed. A new
subscriber, and a client whose queue overflowed, gets `"full": true` with
every section. In a delta, `casters` lists only the changed slots (each with
its `slot`); a section present in `data` replaces the client's copy of it.
`uptime_s` is in every message, so one arrives each second: a client that
hears nothing for 5 s reconnects.

**Events:** `station_state` (`state`), `caster_state` (`slot`, `state`),
`update_state` (`state`), `network` (`what`: `online`, `offline`, `ap_up`,
`ap_down`, `time_synced`), `scan_done`, `settings_changed` (`ns`; advisory:
reload that page's data), `receiver_restarted`, `rebooting` (`reason`; sent
from `LIFECYCLE_EVENT_PRE_REBOOT`, then the server closes with `1012`).

**Progress jobs** (`job`, `phase`; `pct` or `bytes`/`total` where known):

| `job` | Phases |
|---|---|
| `check` | `checking` |
| `app_update` | `downloading` → `rebooting` |
| `app_upload` | `receiving` → `verifying` → `rebooting` |
| `gnss_update` | `downloading`; when it ends with `ok` the station took the request and the `gnss_flash` job follows |
| `gnss_upload` | `receiving` → `inspecting` |
| `gnss_flash` | the flash phases (`um98x_flash_phase_t`): `checking`, `entering`, `transferring`, `committing`, `finishing`, `done`, `failed`, `needs_user`, with `bytes_sent`, `bytes_total`, `retries`, `transfers`, `failure` |

The final message has `"done": true` and `"result"`: `ok` or a result code
(section 12), plus `message` (the `esp_err_t` name) on failure.

**Close codes:** `1012` the device reboots, `4001` session ended (logout,
expiry, password change), `4003` client too slow (its queue stayed full for
30 s), `1009` client message too large.

**Back-pressure.** Pushes go through `httpd_queue_work` and never block a
producer. Each client has a queue of at most 8 messages / 16 KB. When it is
full: a `status` message is dropped and the client gets a full snapshot
next time; a `log` message is dropped and the next one has `"gap": true`;
`progress` keeps only the newest; `event` drops the oldest.

---

## 8. Receiver

### `GET /api/gnss` (adm)

```json
{
  "state": "streaming",
  "receiver": {"valid": true, "model": "UM982", "firmware": "R4.10Build18975", "build": 18975,
               "serial": "...", "pn": "...", "efuse_id": "...", "compile_date": "2025/04/30"},
  "responding": true, "identity_confirmed": true,
  "alerts": [],
  "profile_applied": true, "rtcm1230_supported": true,
  "config_failed": false, "config_retry_in_s": 0, "reconfigure_refused": false,
  "detect_failures": 0,
  "link": {"baud": 115200, "mode": "stream", "rx_age_ms": 12, "rtcm_age_ms": 180},
  "autocfg": true,
  "profile": {
    "signal_group_master": 3, "signal_group_slave": 6,
    "base_mode": "survey_in", "survey_in_s": 600, "fixed": null,
    "station_id": 1234, "com1_baud": 115200,
    "rtcm": [{"msg": 1005, "period_s": 1}, {"msg": 1033, "period_s": 1}, {"msg": 1077, "period_s": 1}],
    "sat_view_output": false
  },
  "position": {"locked": true, "source": "survey", "lat_deg": 45.123456789, "lon_deg": -75.123456789,
               "height_m": 85.1234, "sigma_m": 0.0, "locked_unix": 1759000000},
  "survey": {"active": false, "window_left_s": 0},
  "arp": {"valid": true, "station_id": 1234, "ecef_m": [918129.1234, -4346071.2345, 4561977.3456],
          "lat_deg": 45.123456789, "lon_deg": -75.123456789, "height_m": 85.1251,
          "age_ms": 800, "offset_m": 0.0021}
}
```

| Field | Notes |
|---|---|
| `alerts` | station alerts only (`station_alert_t`) |
| `link.mode` | `off`, `stream`, `command`, `raw` (`gnss_link`) |
| `profile.fixed` | `{lat_deg, lon_deg, height_m}` in `fixed` mode, else `null`; fields a mode does not use are ignored on input and come back cleared (`survey_in_s` 0 in `fixed` mode) |
| `position` | locked or last surveyed (`station_status_t.position`); `source`: `none`, `survey`, `averaged`, `user`; `sigma_m` 0 = unknown |
| `arp` | latest CRC-valid 1005/1006 (`gnss_obs_get_arp`), converted to WGS84; `offset_m` = 3-D distance to the locked position, `null` when not locked; `{"valid": false}` before the first frame |

### `PUT /api/gnss/profile` (adm)

Both members optional: `profile` (the whole profile object as above) and
`autocfg` (bool, `gnss/autocfg`).

```json
{"profile": {"signal_group_master": 3, "signal_group_slave": 6, "base_mode": "fixed", "survey_in_s": 600,
             "fixed": {"lat_deg": 45.123456789, "lon_deg": -75.123456789, "height_m": 85.1234},
             "station_id": 1234, "com1_baud": 115200,
             "rtcm": [{"msg": 1005, "period_s": 1}], "sat_view_output": false},
 "autocfg": true}
```

The profile is stored with `um98x_profile_encode()` (validated by
`um98x_profile_valid()`, which has the last word) and, when it differs from
the stored one, re-applied with `station_request_reconfigure()` (runs once
the receiver streams, whatever `autocfg` says). Field rules checked first,
so `invalid_field` can name the field: `station_id` 0-4095; `com1_baud` one
of 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600;
`survey_in_s` 1-3600 in `survey_in` mode; `fixed` required in `fixed` mode
(latitude ±90, longitude ±180, height -1000 to 10000 m); `rtcm` at most 16
entries, `msg` 1001-4095, `period_s` >= 1; types 1101-1107 are refused (they
never reach a caster); ephemeris types (1019, 1020, 1042, 1045, 1046) need
`com1_baud` >= 460800.

`202 {"accepted": true, "saved": true, "apply": "queued"}`. `apply` is
`queued`, `unchanged` (same profile, or only `autocfg` changed),
`safe_mode` (stored; the station applies it after safe mode, when
`autocfg` is on) or `unsupported_model` (stored; never applied to this
receiver). Errors: `400 invalid_field`, `409 journal_open` (any receiver
update journal open), `409 settings_unavailable`.

### `POST /api/gnss/apply` (adm)

Empty body. `202 {"accepted": true}`. Errors: `409 journal_open`,
`409 safe_mode`, `409 unsupported_model`.

### `POST /api/gnss/position/lock` (adm)

Empty body: lock the latest surveyed position (the station switches the
profile to `fixed` and reconfigures). `202 {"accepted": true}`. Errors:
`409 no_position_yet` (no 1005 yet), `409 unsupported_model`,
`409 journal_open`, `409 safe_mode`.

### `DELETE /api/gnss/position/lock` (adm)

Forget the locked position and survey again. The UI warns first: the
reported antenna position moves, and casters that check it may re-validate
the station. `202 {"accepted": true}`. Errors as for apply.

### `GET /api/gnss/signals` (pub\*/adm)

The signal view, built from the MSM data (`gnss_obs`); same body for public
and admin clients.

```json
{
  "best_sys": "galileo", "best_bands": 4, "e6_lost": false,
  "epoch_age_ms": 420, "epochs": 86400, "epoch_gaps": 0,
  "systems": [
    {"sys": "galileo", "satellites": 9, "bands": ["l1", "l5", "l6", "e5ab"],
     "signal_ids": [2, 15, 16, 22, 23], "cnr_dbhz": {"l1": 44, "l5": 47, "l6": null, "e5ab": 46}}
  ],
  "messages": [{"msg": 1077, "count": 86400, "period_ms": 1000, "age_ms": 420, "expected_period_s": 1}],
  "rtcm1230_supported": true
}
```

`systems` lists only present constellations. `signal_ids` are the MSM
signal IDs seen in the last 10 s; `cnr_dbhz` is the mean per band (MSM7
only; `null` = not available). `messages` merges the observed statistics
with the profile's list (`expected_period_s` `null` = not in the profile);
it is `null` while `gnss_obs_get_msg_stats` is not available.

### `GET /api/sky` (pub\*/adm)

```json
{"available": true, "reason": null,
 "satellites": [{"sys": "gps", "prn": 12, "el_deg": 45, "az_deg": 210, "cnr_dbhz": 44}]}
```

At most 64 satellites. `available: false` with `reason`: `sat_view_off`
(the profile's `sat_view_output` is off; the UI offers to turn it on) or
`not_available_yet`.

---

## 9. Receiver update sessions

A receiver update session is recorded in a journal (`gnss/flash_job`) while
it is open. `journal` (here and in `/api/status`):

| Value | Meaning | Lockout |
|---|---|---|
| `none` | no session | — |
| `running` | a session or its boot-time recovery runs | full: no app updates, reboots or receiver config changes |
| `waiting` | stopped, waiting for the user's choice (also while boot-time recovery is report-only) | reduced: app updates and reboots work; the receiver is left alone |
| `unreadable` | a damaged journal, or one written by a newer firmware; counts as waiting | reduced; only `clear` |
| `unknown` | settings unavailable this boot: the state cannot be read; nothing is sent to the receiver | no action possible |

### `GET /api/gnss/flash` (adm)

```json
{
  "journal": "waiting",
  "record": {"state": "needs_user", "phase": "transferring", "sha256": "9ac25d1d...", "size": 3027628,
             "expected_fw": "R4.10Build19152", "expected_build": 19152,
             "pre_model": "UM982", "pre_fw": "R4.10Build18975", "pre_build": 18975,
             "app_baud": 115200, "boot_baud": 460800, "transfers": 1, "entry_attempts": 1,
             "last_acked_block": 1984, "failure": "transport", "changed_unix": 1760000000},
  "progress": {"phase": "needs_user", "bytes_sent": 2031616, "bytes_total": 3027628, "retries": 3,
               "transfers": 1, "failure": "transport", "last_err": "ESP_ERR_TIMEOUT"},
  "receiver_in_loader": false,
  "receiver_text": "...last 4 KB of receiver text...",
  "actions": {"retry": {"allowed": false, "why": "app_trial"},
              "restart_receiver": {"allowed": true, "why": null},
              "clear": {"allowed": true, "why": null}},
  "flash_available": false
}
```

`record` is `null` for `none`, `unreadable` and `unknown`. `receiver_in_loader`
is the station alert for a receiver found at its loader without a session
(then `restart_receiver` is offered with `journal: "none"`). `receiver_text`
comes from `station_get_flash_log()` (empty until there is any).
`flash_available` is `false` until receiver flashing is built.

`why` codes: `no_journal`, `not_waiting` (a session runs), `safe_mode`
(only `clear` there), `settings_unavailable`, `app_trial`, `app_update`,
`not_available_yet`.

### `POST /api/gnss/flash/retry`, `/restart-receiver`, `/clear` (adm)

Request `{"confirm": true}` (else `400 confirm_required`); the UI shows each
action's warning first:
- retry: the same package is flashed again (needs the package still pinned);
- restart-receiver: the receiver leaves its loader and the version is
  checked; a broken application may only come back after a power cycle;
- clear: the session is forgotten and the package released; the receiver
  may not run valid firmware.

`202 {"accepted": true}` (`clear` completes at once: `200 {"ok": true}`).
Errors: `404 no_journal`, `409 not_waiting`, `409 safe_mode`,
`409 app_trial`, `409 app_update`, `409 settings_unavailable`,
`501 not_available_yet` (retry and restart with a journal, until receiver
flashing is built).

---

## 10. Casters

Slots `0` to `max_slots - 1` (`CONFIG_NTRIP_MAX_CASTERS`).

### `GET /api/casters` (adm)

```json
{
  "max_slots": 4,
  "onocoy_preset": {"host": "servers.onocoy.com", "port": 2101, "tls_port": 2121, "ntrip_version": 1,
                    "backoff_profile": "fast"},
  "slots": [{"slot": 0, "config": {}, "status": {}}]
}
```

`config` and `status` are the objects below.

`GET /api/casters/{n}` returns one `{"slot", "config", "status"}` entry. In
safe mode `config` is `null` (the slots were never loaded) and every status
is `disabled`.

**`config`** (`ntrip_caster_cfg_t`):

```json
{"enabled": true, "name": "Onocoy", "host": "servers.onocoy.com", "port": 2101,
 "ntrip_version": 1, "tls": false, "trust": "bundle", "ca_pem_set": false,
 "mountpoint": "<credential username>", "username": "", "password_set": true,
 "label": "", "v2_raw_body": false, "backoff_profile": "fast", "protected": true,
 "dup_stream_ack": false}
```

Passwords are never returned (`password_set` only). For an Onocoy slot the
UI labels `mountpoint` "Credential username" and `label` "Dashboard label"
(NTRIP 2.0 only); it shows no `username` field.

**`status`** (`ntrip_caster_status_t`):

```json
{"state": "streaming", "reason": "none", "state_age_s": 3600, "progress_age_ms": 120,
 "connects": 3, "failures": 1, "disconnects": 2, "restarts": 0,
 "failures_by_class": {"dns": 0, "connect": 1, "tls": 0, "timeout": 0, "remote_close": 0, "write_stall": 0,
                       "keepalive": 0, "auth": 0, "mount_busy": 0, "config": 0, "banned": 0},
 "connected_since_unix": 1760086400, "connected_s": 3600,
 "today_online_s": 43200, "today_offline_s": 4,
 "graced_uptime_permille": 1000, "streak_ok": true, "data_rate_permille": 1000, "budget_left_s": 469,
 "bytes_sent": 74000000, "frames_sent": 900000, "epochs_sent": 86000, "epochs_sent_today": 43200,
 "frames_dropped": 0, "bytes_dropped": 0, "epochs_dropped": 0, "epochs_dropped_today": 0,
 "frames_skipped_idle": 12, "lag_drops": 0, "laps": 0, "lag_ms_now": 40, "max_lag_ms": 1000,
 "backoff_attempt": 0, "next_retry_s": 0,
 "last_status_code": 200, "last_status_line": "ICY 200 OK", "server_header": "",
 "resolved_ip": "35.207.172.57", "dns_cached_used": 0,
 "tls_version": null, "tls_cipher": null, "hint": "", "last_err": null}
```

"Today" is the current UTC day. The protected-slot fields are `null` for
other slots; `tls_*` are `null` without TLS; `max_lag_ms` is the slot's lag
limit.

### `PUT /api/casters/{n}` (adm)

Body: the `config` object. Fields left out keep their stored value (a form
that does not show a field cannot reset it). Output-only fields
(`password_set`, `ca_pem_set`, `protected`) are ignored. Special fields:
- `password`: a non-empty value replaces the stored one; empty or missing
  keeps it, but only while `host` stays the same (pointed at another host the
  slot never sends the old password there); `"clear_password": true` removes
  it.
- `protected` is set by the server: true exactly for a host in the
  `onocoy.com` domain. `backoff_profile` is forced to `fast` for that host
  and to `polite` for `rtk2go.com`; elsewhere `standard` or `polite` as sent
  (`fast` is refused there: `400 invalid_field`).
- `dup_stream_ack`: see below.

Validation (`400 invalid_field` with `field`): no control characters
anywhere; `name` <= 23, `host` <= 63 (a DNS name or an IPv4 literal),
`mountpoint` <= 47 of `[A-Za-z0-9._-]`, `username` <= 47 without `:`,
`password` <= 63 (NTRIP 1.0: no spaces), `label` <= 47 of `[A-Za-z0-9]`;
`port` 1-65535; `ntrip_version` 1 or 2; an enabled slot needs `host`,
`port`, `mountpoint`, a password (1.0) and a username (2.0, except Onocoy).

**Caster policy** (`409 caster_policy` with `reason`):
- `second_onocoy`: another enabled slot already streams to Onocoy;
- `second_rtk2go`: another enabled slot already streams to RTK2go;
- `dup_stream_ack_required`: enabling this slot would stream to Onocoy and
  to an open caster (RTK2go, Centipede) at the same time. Onocoy bans
  duplicated streams, and anyone can re-inject a stream from an open caster.
  The UI shows the warning (suggesting a private RTK2go reservation and
  telling Onocoy support that the user is the original provider of that
  stream), and on the user's tick sends the same body again with
  `"dup_stream_ack": true`. The acknowledgement is stored with the slot
  being saved.

`200` with the slot's `{"slot", "config", "status"}`; only that slot
reconnects. Also `409 safe_mode`, `409 settings_unavailable`.

### `PUT /api/casters/{n}/ca`, `DELETE /api/casters/{n}/ca` (adm)

`PUT {"pem": "-----BEGIN CERTIFICATE-----\n..."}` (at most 4 KB, PEM
certificates only) stores the slot's CA for `trust: "custom_ca"`; `DELETE`
removes it. The slot reconnects. `200` with the slot entry. Errors:
`400 invalid_field` (`pem`), `409 safe_mode`, `409 settings_unavailable`.

---

## 11. Wi-Fi and system settings

### `GET /api/wifi` (adm)

```json
{
  "configured": true, "credentials_unreadable": false, "ssid": "Home",
  "online": true, "associated": true, "ip": "192.168.1.50", "rssi": -60, "channel": 6,
  "time_synced": true, "upstream": "ok", "probe_age_s": 120,
  "reconnects": 1, "connect_failures": 0, "offline_s": 0, "retry_in_s": 0,
  "last_disconnect_reason": 8, "fallback_dns": false,
  "ladder": {"active": false, "rung": 0, "withheld": "none", "actions": 0},
  "ap": {"active": false, "ssid": "onocoydiy-1A2B", "reasons": [], "clients": 0,
         "recovery_left_s": 0, "password_set": false},
  "scan": {"running": false, "age_s": 30,
           "results": [{"ssid": "Home", "rssi": -55, "channel": 6, "secured": true}]}
}
```

`last_disconnect_reason` is the numeric `wifi_err_reason_t`. `scan.results`
are the cached ones (no radio activity; at most 24); `age_s` is `null`
before the first scan.

### `PUT /api/wifi` (adm)

```json
{"ssid": "Home", "password": "...", "ap_password": ""}
```

- `ssid` 1-32 bytes; `password` `""` (open network), 8-63 characters or 64
  hex digits. A missing `password` keeps the stored one when `ssid` is
  unchanged; for a new SSID it is required (`400 invalid_field`).
- `ap_password` (optional): 8-63 characters sets the AP password (WPA2),
  `""` removes it (the AP is then open; the UI warns); missing = unchanged.
  It takes effect the next time the AP comes up.

`202 {"accepted": true}`: the station switches about a second later; an AP
that is up stays up until a minute after the station has an IP, so the UI
polls `GET /api/wifi` for `online` and `ip`. Changing Wi-Fi drops every
caster session (the UI warns). With the settings unavailable the
credentials are used for this boot only (`"persisted": false`).

### `POST /api/wifi/scan` (adm)

Empty body. `202 {"accepted": true}`; `scan_done` event when finished. A
scan while connected stalls the casters for seconds: the UI warns first.
Errors: `409 busy` (a scan runs, or the station is connecting),
`429 rate_limited` (one scan per 15 s).

### `GET /api/system` (adm)

```json
{"hostname": "onocoydiy", "hostname_active": "onocoydiy", "public_status": false,
 "setup_done": true, "settings_available": true,
 "version": "v0.3.0", "project": "onocoydiy-esp-um982", "idf": "v6.1", "mac": "AA:BB:CC:DD:EE:FF",
 "chip": {"model": "esp32s3", "revision": "v0.2", "flash_mb": 16, "psram_mb": 8}}
```

### `PUT /api/system` (adm)

`{"hostname": "roof-station", "public_status": true}`, both optional.
`hostname` 1-32 of `[A-Za-z0-9-]`, not starting or ending with `-`; it takes
effect at the next boot (`"reboot_required": true`). `200` with the system
object. Errors: `400 invalid_field`, `409 settings_unavailable`.

---

## 12. Updates

Every update, app and receiver, starts with a user action. The device checks
the release manifest once a day (and on "check now"); a check never
downloads an image or a package.

**Banner** (`update` in `/api/status`, admin sessions only, on every page):
shown when `app_available`, `gnss_available` or `app_reverted_in_stage1`.
"App vX / receiver build Y available - Update / Later". "Later" is kept in
the browser (`localStorage`, keyed by `app_version` and `gnss_build`) until
a newer version appears; the server keeps no "later" state.

| Field | Source |
|---|---|
| `app_available`, `app_version` | `app_update_available`, `latest_version` |
| `app_reverted_version`, `app_reverted_in_stage1` | the last version rolled back; true when it rolled back in its first minute (offer "install again?") |
| `gnss_available`, `gnss_build`, `gnss_version` | `gnss_update_available`, `gnss_latest_build`, `gnss_latest` |
| `gnss_blocked_reason`, `gnss_blocked_left_s` | `fw_update_gnss_block_t`; `left_s` for `app_trial` |
| `gnss_flash_available` | `false` until receiver flashing is built |
| `checking`, `last_check_unix` | state `checking`; `ota/last_check` |

### `GET /api/update` (adm)

```json
{
  "state": "idle", "progress_pct": 0, "checks_enabled": true, "last_check_unix": 1760080000,
  "last_result": {"job": "gnss_update", "result": "not_available_yet", "message": "ESP_ERR_NOT_SUPPORTED"},
  "app": {"running": "v0.3.0", "dev_build": false, "latest": "v0.4.0", "available": true,
          "latest_rolled_back": false, "reverted": "", "reverted_in_stage1": false,
          "trial": {"stage": "none", "left_s": 0}, "rollback_available": true},
  "gnss": {"model": "UM982", "running": "R4.10Build18975", "running_build": 18975,
           "latest": "R4.10Build19152", "latest_build": 19152, "available": true,
           "blocked": "none", "blocked_left_s": 0,
           "expected_downtime_s": 252, "budget_left_s": 469, "flash_available": false}
}
```

| Field | Notes |
|---|---|
| `state` | `fw_update_state_t` |
| `checks_enabled` | `false` when the build has no manifest key (manual uploads still work) |
| `last_result` | outcome of the last job the backend saw end (`job`: `check`, `app_update`, `app_upload`, `gnss_update`, `gnss_upload`, `rollback`), `null` this boot until one ended |
| `app.trial.stage` | `none`, `confirming`, `verifying`, `watching` |
| `app.latest_rolled_back` | the latest release is one that was rolled back here: not offered, installable with a warning |
| `gnss.expected_downtime_s` | `station_expected_flash_downtime_s()` for the offered package (PROVISIONAL, to be confirmed on hardware); information, never a gate |
| `gnss.budget_left_s` | as in `/api/status`, for the dialog |

**Result codes** (`last_result.result`, final `progress` messages): `ok`,
`not_available_yet`, `refused` (the state changed between click and start),
`download_failed`, `checksum_mismatch`, `size_mismatch`, `invalid_image`,
`signature_failed`, `package_rejected` (a receiver package failed a
pre-flight rule; with `failed_rule`, 1-12 as numbered in `um98x_flash.h`,
and `cross_layout`), `failed` (anything else; `message` has the
`esp_err_t`).

### `POST /api/update/check` (adm)

Empty body. `202 {"accepted": true}` (also when a check already runs).
Errors: `501 checks_disabled`, `409 app_trial` (the new image's first
minute), `409 busy` (an install runs), `429 rate_limited` (one manual check
per 60 s).

### `POST /api/update/app` (adm)

`{"version": "v0.4.0", "rolled_back_ack": false}`: `version` is what the
banner showed. Downloads into the inactive slot, verifies against the
manifest, activates, reboots, then runs the two-stage trial.
`202 {"accepted": true}`; progress job `app_update`, then a `rebooting`
event. Errors: `409 manifest_changed` (the offer changed since the banner),
`409 not_offered` (not newer than the running release), `409
rolled_back_ack_required` (a rolled-back version: resend with
`rolled_back_ack: true` after the warning), `409 journal_open` (a receiver
update runs), `409 busy`, `409 app_trial` (stage 1 or verify window).

### `POST /api/update/gnss` (adm)

The user confirmed the receiver dialog. Before the request the UI shows:
version, `expected_downtime_s` against `budget_left_s`, "do not power off
the station or plug anything into the receiver's USB port", "the station
profile will be re-applied", and while `flash_available` is `false`, that
installing receiver firmware is not available in this firmware version yet.

`{"build": 19152, "cross_layout_ack": false}`. One click runs the chain:
download the manifest's package for the detected model into the package
slot (size and SHA-256 from the signed manifest, checked on the stream and
on the stored copy), then start the session on the station. `202
{"accepted": true}`; progress job `gnss_update`, then `gnss_flash`.

**Until receiver flashing is built** the chain ends after the download:
the start step answers `not_available_yet`, the package is released and
nothing is sent to the receiver. The final progress message and
`last_result` say `not_available_yet`; the UI shows "Receiver package
downloaded and verified; installing it is not available yet. Nothing on the
receiver changed."

Errors (all before anything is downloaded): `409 not_offered` (the build
changed or nothing is offered; dev builds are offered nothing), `409
app_trial` (`retry_in_s` = time left), `409 app_update`, `409
journal_open`, `409 receiver_not_streaming`, `409 unsupported_model`, `409
safe_mode`, `409 package_pinned`, `409 busy`.

### `POST /api/update/rollback` (adm)

`{"confirm": true}`. Marks the running version as rolled back and reboots
into the other slot. `202 {"accepted": true}`. Errors: `409
no_rollback_image`, `409 reboot_inhibited`, `409 busy`, `409
journal_open`.

---

## 13. Uploads

Raw bodies: `Content-Type: application/octet-stream`,
`X-Requested-With: onocoydiy`, `Content-Length` required (`411`; no chunked
uploads). One upload at a time (`409 busy`). The body is streamed in 4 KB
pieces straight to flash; nothing is buffered whole. A client that
disconnects aborts the upload (the slot or package area stays empty).

### `POST /api/upload/app` (adm)

Size at most the inactive app slot (4 MB), else `413` before any write.
The first 4 KB are checked before the slot is opened: the image's app
description must name this project (`onocoydiy-esp-um982`), else
`422 not_this_project` (the query `?any_project=1` overrides it after a
warning). Then the image goes through `fw_update_upload_app_*`, which claims
the update slot and a reboot inhibit before the first byte.

`200 {"sha256": "...", "size": 1234567, "version": "v0.4.0+dev", "project": "onocoydiy-esp-um982", "reboot": true}`;
the device reboots into the image about a second after the response and
runs the trial. Errors: `400 invalid_image` (not a valid app for this chip),
`400 size_mismatch` (fewer bytes than announced), `409 journal_open`,
`409 busy`, `409 app_trial`.

### `POST /api/upload/gnss` (adm)

Size at most the package slot (`pkg_store_capacity()`, 6 MB minus 4 KB).
Refused before anything is stored: `409 safe_mode`, `409 app_trial`, `409
app_update`, `409 journal_open`, `409 receiver_not_streaming`, `409
unsupported_model`, `409 package_pinned`, `409 busy`.

Stored with `pkg_store_begin(size, NULL)` (SHA-256 computed on the way),
then inspected without the receiver. The response, also returned by
`GET /api/upload/gnss` while it waits:

```json
{
  "staged": {"size": 3027628, "sha256": "9ac25d1d359e5815b13291f61405faef53d71e306a602184546b32584b35ec71",
             "model": "UM982", "layout": 1, "layout_build": 18975,
             "stamps": [0, 0, 0, 0, 0], "expires_in_s": 900},
  "receiver": {"model": "UM982", "firmware": "R4.10Build18975", "build": 18975},
  "expected_downtime_s": 252, "budget_left_s": 469, "flash_available": false
}
```

`layout` is the index of the known partition table the package carries
(`-1` unknown), `layout_build` the receiver build it belongs to (`0`
unknown), `stamps` the five block date stamps. The user compares the
SHA-256 with the published one and gives the expected firmware string.
Unconfirmed after 15 min (`expires_in_s`), the package is released.

### `POST /api/upload/gnss/confirm` (adm)

```json
{"sha256": "9ac25d1d...", "expected_fw": "R4.10Build19152", "cross_layout_ack": false}
```

`sha256` must equal the staged digest (`409 sha256_mismatch`).
`expected_fw` <= 47 characters with a `Build<n>` part (`400 invalid_field`);
a build lower than the receiver's is `409 downgrade`. Then
`station_request_flash()`, which repeats every state check: `202
{"accepted": true}`, progress job `gnss_flash`. The package rules run on the
station's RAM copy after that; a refusal ends the job with `result:
"package_rejected"` and releases the package. A package with another
partition layout than the receiver's is refused until tested on hardware;
after that it needs `cross_layout_ack`. Until receiver flashing is built:
`501 not_available_yet` and the package is released. Other errors:
`404 nothing_staged` and the `409` codes of the upload.

### `DELETE /api/upload/gnss` (adm)

Releases the uploaded package (`pkg_store_release()`). `200 {"ok": true}`;
`404 nothing_staged`. A package pinned by an open journal is never released
here.

---

## 14. Diagnostics

All admin only.

### `GET /api/health`

```json
{
  "level": "ok", "alerts": [], "last_step": "none", "ladder_suspended": false,
  "uptime_s": 86400, "gnss_stalls": 0, "recoveries": 0, "caster_restarts": 0,
  "network_recoveries": 0, "rtcm_gap_ms": 230,
  "heap": {"internal_free": 120000, "internal_min": 98000, "internal_largest": 64000, "psram_free": 7000000},
  "inhibited_by": null, "update_slot": "none",
  "boot": {"reset_reason": "sw", "reset_class": "planned", "prev_reboot_reason": "app_update",
           "boots_since_power_on": 3, "crashes": 0, "brownouts_since_power_on": 0,
           "safe_mode": false, "safe_mode_retry_in_s": null, "stable": true, "prev_session_uptime_s": 3600},
  "boot_history": [{"reset_class": "planned", "reset_reason": "sw", "planned_reason": "app_update",
                    "boot_no": 3, "prev_uptime_s": 3600, "version": "v0.3.0"}],
  "link": {"mode": "stream", "baud": 115200, "rx_bytes": 0, "tx_bytes": 0,
           "hw_fifo_overflows": 0, "rx_buffer_full": 0, "rx_events_dropped": 0, "line_errors": 0,
           "baud_switch_bytes": 0, "rtcm_held": 0, "flushed_bytes": 0,
           "last_rx_age_ms": 10, "last_rtcm_age_ms": 180,
           "framing": {"bytes_in": 0, "bytes_discarded": 0, "rtcm_frames": 0, "rtcm_crc_errors": 0,
                       "rtcm_reserved_nonzero": 0, "rtcm_bad_length": 0, "text_lines": 0,
                       "text_rejected": 0, "text_aborts": 0, "text_overflows": 0, "bin_skipped": 0,
                       "resyncs": 0, "prompts": 0, "prompts_ignored": 0}},
  "hub": {"frames_in": 0, "bytes_in": 0, "epochs_in": 0, "epochs_by_mmb": 0, "epochs_by_time": 0,
          "epochs_by_gap": 0, "publish_failures": 0, "consumers": 1, "capacity": 262144,
          "last_frame_age_ms": 180},
  "station": {"state": "streaming", "rtcm_excluded": 0, "detect_failures": 0, "detect_saw_bytes": false,
              "lease_held": false, "since_lease_release_ms": null},
  "obs": {"epochs": 86400, "epoch_gaps": 0},
  "network": {"reconnects": 1, "connect_failures": 0, "offline_s": 0, "last_disconnect_reason": 8,
              "fallback_dns": false, "probe": "ok", "probe_age_s": 120,
              "ladder": {"active": false, "rung": 0, "withheld": "none", "actions": 0}},
  "coredump": {"present": false, "size": 0, "task": "", "pc": null, "backtrace": []}
}
```

Per-caster loss counters are in `/api/casters`. `boot_history` is the last
16 boots, newest first (`prev_uptime_s` `null` = unknown). `inhibited_by` is
the label of a held reboot inhibit; `update_slot` is `none`, `app` or
`receiver`. The UI warns that a core dump may contain secrets before
offering the download.

### `GET /api/availability`

```json
{
  "clock": "gnss", "utc_second_of_day": 43210,
  "slots": [{"slot": 0, "name": "Onocoy", "protected": true}],
  "protected": {"down_today_s": 4, "budget_left_s": 469, "data_rate_permille": 1000,
                "graced_uptime_permille": 1000, "streak_ok": true},
  "today": {"utc_day": 20371, "date": "2026-10-10", "down_s": [4, 0, 0, 0], "planned_s": [0, 0, 0, 0],
            "reconnects": [1, 0, 0, 0], "power_off_s": 0, "rx_overflows": 0},
  "history": []
}
```

Per-slot arrays have `max_slots` entries. `history` holds up to 30 past days
in the `today` shape, newest first. `down_s` counts seconds a slot should
have streamed but did not; `planned_s` the planned part of it (receiver
work, app updates). `clock` is `none` until a trusted time exists
(`utc_second_of_day` is then `null`). The budget is information only.

### `GET /api/diag/tasks`

`{"tasks": [{"name": "gnss_rx", "priority": 18, "core": 1, "stack_free_min": 1234, "cpu_pct": 3}]}`.
`core` `null` = not pinned; `cpu_pct` since the previous call (poll every
2 s or slower).

### `PUT /api/diag/log-level`

`{"tag": "station", "level": "debug"}`: `tag` <= 15 characters or `"*"`;
`level` `none`, `error`, `warn`, `info`, `debug`, `verbose`. Not persisted.
`200 {"ok": true}`.

### `GET /api/logs?cursor=N[&boot=prev][&format=text]`

`{"cursor": 51234, "more": false, "gap": false, "text": "..."}` with at most
8 KB of whole lines newer than `cursor` (start with 0; pass the returned
`cursor` back; `more: true` = call again at once). `gap` = lines were
overwritten before they were read. `boot=prev` reads the previous boot's
ring (empty after a power cut or an app image change). `format=text`
streams the whole ring as `text/plain` with
`Content-Disposition: attachment; filename="log-<version>.txt"`.

### `GET /api/journal?cursor=N`

```json
{"cursor": 77, "more": true,
 "entries": [{"seq": 77, "time_unix": 1760000000, "uptime_s": 3, "kind": "boot", "a": 1, "b": 4}]}
```

At most 64 entries per call, oldest first, `seq > cursor`. `time_unix`
`null` = no trusted clock then. `a` and `b` by `kind`:

| `kind` | `a` | `b` |
|---|---|---|
| `boot` | reset class | planned reboot reason |
| `session` | previous session uptime s (-1 unknown) | raw reset reason |
| `reboot` | reason | — |
| `ladder_step` | ladder (0 GNSS, 1 network, 2 caster) | `(outcome << 8) \| code` (network: `(reason << 16) \| ...`); see `diag.h`, `network.h` |
| `app_update` | result (`esp_err_t`) | — |
| `app_revert` | trial stage | — |
| `gnss_flash` | result (`esp_err_t`) | attempt |
| `safe_mode` | episode | — |
| `wifi_disconnect` | `wifi_err_reason_t` | RSSI |
| `data_loss` | counter id (`diag_loss_id_t`) | increase since the previous record for that id |

Enum numbers are the C values; the UI decodes them with the appendix
tables.

### `GET /api/coredump`, `DELETE /api/coredump`

`GET` streams the ELF dump (`application/octet-stream`,
`Content-Disposition: attachment; filename="coredump-<version>.elf"`) for
`idf.py coredump-info`; `404 no_coredump` when none. `DELETE` erases it:
`200 {"ok": true}`. The summary is in `/api/health`.

---

## 15. System actions

All admin only; the response goes out before anything restarts.

### `POST /api/system/reboot`

Empty body. `202 {"accepted": true, "reboot_in_ms": 1000}`, then a
`rebooting` event. `409 reboot_inhibited` (with `by`) while a receiver
update, an app image write or the app's verify window holds an inhibit.

### `POST /api/system/factory-reset`

`{"confirm": true}`. Erases the user settings (Wi-Fi, admin password,
casters, receiver profile and locked position, update repository, web
settings) and reboots into first setup with an open setup AP. Kept: the
receiver update journal, the last receiver identity, app trial and
rollback records, boot history, availability ledger. `202 {"accepted":
true}`. Errors: `409 journal_open` (a receiver update runs; allowed while it
waits for the user), `409 reboot_inhibited`, `409 settings_unavailable`,
`400 confirm_required`.

### `POST /api/system/safe-mode/clear`

Empty body. Reboots into normal mode with the crash count cleared.
`202 {"accepted": true}`. Errors: `409 not_in_safe_mode`,
`409 reboot_inhibited` (nothing changed).

While in safe mode the UI shows why (`safe_mode.crashes`), when the
automatic retry comes (`retry_in_s`), that the receiver and casters are off,
and that updates (check, banner, app installs) still work.

---

## 16. Captive portal and first setup

**Captive portal.** While the AP is up (setup, fallback or recovery), the
network component answers every DNS A query with `192.168.4.1` and sends
DHCP option 114 = `http://192.168.4.1/`. The web server, for requests that
arrive over the AP with a Host that is not the device's (section 2), answers
`302 Found`, `Location: http://192.168.4.1/`, `Cache-Control: no-store`. That
covers the OS probes (`/generate_204`, `/gen_204`, `/hotspot-detect.html`,
`/library/test/success.html`, `/connecttest.txt`, `/redirect`, `/ncsi.txt`,
`/canonical.html`, `/success.txt`) and any other page. HTTPS is not served,
so HTTPS probes fail and the OS falls back to its HTTP probe. The same page
serves claim, login and wizard; `GET /api/session` tells which. The wizard
must work inside the OS's captive-portal browser: cookies work there for
the session, but nothing may rely on `localStorage` surviving.

**First setup** (unconfigured station, open AP `onocoydiy-XXXX`):
1. `GET /api/session`: `admin_set: false`, `claim_allowed: true`.
2. `POST /api/setup/admin`: the admin password; the response logs in.
3. Optional: the Onocoy slot, `PUT /api/casters/0` with `onocoy_preset`,
   the credential username and password.
4. Wi-Fi: `GET /api/wifi` (cached scan), `POST /api/wifi/scan` if needed
   (nothing streams yet), then `POST /api/setup/done` and `PUT /api/wifi`.
5. The UI polls `GET /api/wifi` every second until `online`, then shows
   the station's address (`ip`, and the hostname). The AP follows the
   station's channel once it connects, so phones may drop off it briefly;
   the UI keeps polling and otherwise tells the user to join their own
   Wi-Fi and find the device there. The AP goes down a minute after the
   station has an IP.

Wi-Fi comes last because joining it ends the AP; Wi-Fi credentials can
never be saved before an admin password exists (every Wi-Fi call needs a
session).

**Recovery.** BOOT held 10 s clears the admin password and opens the
recovery AP for 15 min (open unless an AP password is set; longer while a
client stays connected): the claim works again over that AP. BOOT held
20 s is a factory reset (refused while a receiver update runs).

---

## 17. What is refused when

`✓` allowed; otherwise the error code (`409` unless noted).

| Action | Receiver update running | Update waiting for user | Safe mode | App update running | App trial |
|---|---|---|---|---|---|
| Profile save / apply / lock / re-survey | `journal_open` | `journal_open` | save ✓, others `safe_mode` | ✓ | ✓ |
| Caster edit | ✓ | ✓ | `safe_mode` | ✓ | ✓ |
| Wi-Fi edit, scan | ✓ | ✓ | ✓ | ✓ | ✓ |
| Update check | ✓ | ✓ | ✓ | `busy` | stage 1: `app_trial` |
| App install / upload | `journal_open` | ✓ | ✓ | `busy` | stage 1, verify window: `app_trial` |
| App rollback | `journal_open` | ✓ | ✓ | `busy` | ✓ |
| Receiver update (banner, upload, confirm) | `journal_open` | `journal_open` | `safe_mode` | `app_update` | `app_trial` |
| Flash retry | `not_waiting` | ✓ | `safe_mode` | `app_update` | `app_trial` |
| Restart receiver | `not_waiting` | ✓ | `safe_mode` | ✓ | ✓ |
| Clear journal | `not_waiting` | ✓ | ✓ | ✓ | ✓ |
| Reboot | `reboot_inhibited` | ✓ | ✓ | `reboot_inhibited` | verify window: `reboot_inhibited` |
| Factory reset | `journal_open` | ✓ | ✓ | `reboot_inhibited` | verify window: `reboot_inhibited` |

Safe-mode clear works only in safe mode (`not_in_safe_mode` otherwise) and
is refused with `reboot_inhibited` while any reboot inhibit is held.
Receiver updates are also refused (`receiver_not_streaming`,
`unsupported_model`, `package_pinned`) whenever those hold. With the
settings unavailable this boot, every call that would persist something
answers `settings_unavailable`, except Wi-Fi credentials and the first admin
password (both kept for this boot), and a journal in state `unknown` allows
no receiver update action.

---

## 18. Server limits and implementation notes

- **Server:** `esp_http_server`, task priority 5, 8 KB stack in internal
  RAM; `max_open_sockets` 7 (the lwIP socket budget counts on it),
  `lru_purge_enable`, `max_uri_handlers` 64 with wildcard matching for
  `/api/casters/*`, `max_req_hdr_len` 2048 (cookies and long browser
  headers), receive and send timeouts 10 s. WebSocket clients: at most 4.
- **Handlers never block for long.** Station, caster, network and update
  calls are asynchronous; a handler reads snapshots and returns. The
  password check (about 0.3 s) runs on the server task. Uploads and
  downloads run as async requests (`httpd_req_async_handler_begin`) on one
  worker task with an internal-RAM stack (flash writes), so status pushes
  continue meanwhile.
- **Memory:** large snapshots (`station_status_t`, caster status) are read
  into PSRAM or static storage, never onto the server stack. JSON is
  written by a streaming writer into a 2 KB PSRAM buffer and sent with
  `httpd_resp_send_chunk`, so no response needs a large contiguous block
  (`/api/casters` with 8 slots is about 10 KB). The upload buffer is 4 KB of
  internal RAM. cJSON (PSRAM hooks) parses requests only.
- **Events:** the web subscribes to `STATION_EVENT`, `NTRIP_EVENT`,
  `FW_UPDATE_EVENT`, `NETWORK_EVENT`, `SETTINGS_EVENT` and
  `LIFECYCLE_EVENT`; handlers only set flags and queue work. A one-second
  timer builds the status, polls progress at 2 Hz while a job runs and
  reads new log text every 500 ms.
- **Before a reboot** (`LIFECYCLE_EVENT_PRE_REBOOT`) the web sends the
  `rebooting` event and closes WebSocket clients with `1012`; a shutdown hook
  lets pending responses go out first.
- **Interface of a request:** the local socket address (`getsockname` on
  `httpd_req_to_sockfd`) tells the AP from the station interface (claim rule,
  captive redirects).
- The web component also needs `um98x_ctl` (profile codec, validation and
  WGS84/ECEF conversion) and `um98x_flash` (`um98x_flash_inspect_staged`)
  in its `PRIV_REQUIRES`; both are lower layers.

---

## 19. Open items

Things this contract needs that the current headers do not provide yet:

1. **Boot history** (`/api/health` `boot_history`): `lifecycle.h` has no
   reader for `life/boots`; it needs one (the codec is private to
   lifecycle).
2. **Caster policy reason**: `ntrip_server_set_config()` returns
   `ESP_ERR_NOT_ALLOWED` for all three policy cases; the API needs to know
   which (a reason out-parameter or a check function in `ntrip_server.h`).
3. **Receiver flashing available**: `flash_available` needs a source that
   is not a constant in the web component (for example a query in
   `station.h`).
4. **Rolled-back versions**: `fw_update.h` exposes only the last reverted
   version and whether the latest release was rolled back, not the whole
   list.
5. **Refused BOOT-button factory reset**: health refuses it during a
   receiver update and should show that in the UI, but `health_status_t`
   has no field for it.
6. Not implemented yet behind existing headers (the API shows `null` or
   `available: false`): per-band CNR, epoch gaps, message statistics and the
   sky view (`gnss_obs`), the receiver text of a flash session
   (`station_get_flash_log`), the availability ledger.

---

## Appendix: enumerations

Each enum is listed in C order. Its C value is the position counted from
its first value: 0 for most, 1 for `diag_event_t`, `diag_loss_id_t` and
`um98x_journal_state_t`. The journal's `a` and `b` carry these C values.

| Enum (C) | API strings |
|---|---|
| `station_state_t` | `idle`, `detecting`, `configuring`, `streaming`, `flashing`, `flash_recovery`, `recovering`, `no_receiver` |
| `station_alert_t` | `unsupported_model`, `config_failed`, `position_mismatch`, `antenna_moved`, `receiver_attention`, `receiver_in_loader` |
| `health_level_t` | `ok`, `degraded`, `recovering`, `failed` |
| `health_step_t` | `none`, `resync`, `link_restart`, `rescan`, `reconfigure`, `receiver_reset`, `reboot` |
| `health_alert_t` | `power_supply`, `receiver_power`, `bands`, `profile_drift`, `heap_fragmented`, `safe_mode`, `settings_degraded`, `no_upstream`, `link_loss` |
| `ntrip_state_t` | `disabled`, `wait_network`, `wait_data`, `connecting`, `streaming`, `backoff`, `auth_failed`, `paused`, `config_error`, `banned` |
| `ntrip_fail_class_t` | `none`, `dns`, `connect`, `tls`, `timeout`, `remote_close`, `write_stall`, `keepalive`, `auth`, `mount_busy`, `config`, `banned` |
| `ntrip_trust_t` | `bundle`, `custom_ca`, `insecure` |
| `ntrip_backoff_profile_t` | `fast`, `standard`, `polite` |
| `um98x_base_mode_t` | `survey_in`, `fixed` |
| `um98x_pos_source_t` | `none`, `survey`, `averaged`, `user` |
| `um98x_flash_phase_t` | `idle`, `checking`, `entering`, `transferring`, `committing`, `finishing`, `done`, `failed`, `needs_user` |
| `um98x_journal_state_t` | `prepared`, `baud_changing`, `reset_sent`, `menu2_sent`, `transferring`, `eot_sent`, `verifying`, `failed`, `needs_user` |
| `um98x_flash_failure_t` | `none`, `transport`, `package`, `unconfirmed`, `no_answer`, `app_not_starting`, `entry`, `journal` |
| `fw_update_state_t` | `idle`, `checking`, `downloading_app`, `downloading_gnss`, `reboot_pending`, `failed`, `uploading_app` |
| `fw_update_trial_t` | `none`, `confirming`, `verifying`, `watching` |
| `fw_update_gnss_block_t` | `none`, `app_trial`, `journal_open`, `unsupported_model`, `safe_mode`, `app_update`, `receiver_not_streaming` |
| `network_probe_t` | `none`, `ok`, `no_ip`, `dns_failed`, `no_upstream` |
| `network_ap_reason_t` | `setup`, `fallback`, `linger`, `recovery` |
| `network_ladder_why_t` | `none`, `upstream_ok`, `no_upstream`, `journal_open`, `interval`, `inhibited`, `ap_in_use` |
| `gnss_link_mode_t` | `off`, `stream`, `command`, `raw` |
| `lifecycle_reset_class_t` | `power_on`, `planned`, `crash`, `brownout`, `other` |
| `lifecycle_reboot_reason_t` | `none`, `user`, `factory_reset`, `admin_reset`, `app_update`, `app_revert`, `gnss_ladder`, `network_ladder`, `low_heap`, `safe_mode_retry`, `safe_mode_cleared`, `nvs_slot_switch`, `boot_failure` |
| `esp_reset_reason_t` | `unknown`, `poweron`, `ext`, `sw`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deepsleep`, `brownout`, `sdio`, `usb`, `jtag`, `efuse`, `pwr_glitch`, `cpu_lockup` |
| `ledger_clock_t` | `none`, `sntp`, `gnss` |
| `gnss_sys_t` | `gps`, `glonass`, `galileo`, `sbas`, `qzss`, `beidou`, `navic` |
| `gnss_band_t` | `l1`, `l2`, `l5`, `l6`, `e5ab` |
| `diag_event_t` | `boot`, `reboot`, `ladder_step`, `app_update`, `app_revert`, `gnss_flash`, `safe_mode`, `wifi_disconnect`, `data_loss`, `session` |
| `diag_loss_id_t` | `hw_fifo`, `rx_buffer_full`, `line_errors`, `rtcm_crc`, `framer_discarded`, `hub_publish`, `baud_switch`, `rx_events_dropped`; 16 and up: caster slots |
