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
and fallbacks (the UI words its own text per code). For an `invalid_field`
whose parser words the field's rule (the caster fields, section 10)
`message` is that rule in lowercase words ("at most 23 bytes (fewer
characters with accents or symbols), no control characters"), so the UI
shows "<field label>: <rule>"; otherwise it is the code's default text.
`field` names the
offending request field (`invalid_field`), `retry_in_s` comes with
`rate_limited` and `app_trial`, `by` with `reboot_inhibited` (the holder's
label, e.g. `"receiver flash"`). Other fields may appear per code (listed
with the endpoint): `reason` with `caster_policy` and with the `busy` of a
refused receiver maintenance (`receiver_session`, `live_view_output`).

| HTTP | Codes |
|---|---|
| 400 | `bad_json`, `invalid_field`, `confirm_required`, `bad_request`, `invalid_image`, `size_mismatch` |
| 401 | `unauthorized` (no session, or it expired: show the login) |
| 403 | `forbidden` (Origin or CSRF check failed, public status off), `wrong_password`, `not_on_setup_ap` |
| 404 | `not_found`, `nothing_staged`, `no_journal`, `no_coredump`, `nothing_pending` |
| 405 | `method_not_allowed` |
| 409 | state conflicts: `busy`, `journal_open`, `not_waiting`, `app_trial`, `app_update`, `safe_mode`, `not_in_safe_mode`, `receiver_not_streaming`, `unsupported_model`, `package_pinned`, `manifest_changed`, `not_offered`, `rolled_back_ack_required`, `reboot_inhibited`, `settings_unavailable`, `no_position_yet`, `caster_policy`, `sha256_mismatch`, `downgrade`, `no_rollback_image`, `admin_already_set`, `admin_not_set`, `receiver_maintenance`, `not_in_maintenance`, `live_view_closed`, `not_no_receiver`, `same_image` |
| 411 | `length_required` (uploads) |
| 413 | `too_large` |
| 415 | `unsupported_media_type` |
| 421 | `bad_host` |
| 422 | `not_this_project` (app upload) |
| 429 | `rate_limited` (with `retry_in_s`) |
| 500 | `internal` (`message` carries the `esp_err_t` name) |
| 501 | `not_available_yet` (receiver flashing is not enabled in this build: `flash_available` false), `checks_disabled` (this build cannot check for updates: no valid release repository, or it requires signed manifests and none of its keys is usable) |
| 503 | `too_many_clients` (WebSocket) |

---

## 2. Request checks

Applied to every request before any handler runs.

1. **Host.** The `Host` header (port ignored) must be the station's current
   IPv4 address, the AP address (`192.168.4.1`), `<mdns_host>.local` (the
   name the mDNS responder answers, section 5: the hostname, or
   `<hostname>-2` after a name conflict; only while the responder runs),
   the hostname in use, or `<hostname>.` followed by one of `lan`, `home`,
   `home.arpa`, `localdomain`, `internal` (the DHCP name with a search
   domain; names compared without case): only suffixes that never resolve
   in public DNS, so a router domain that is a public zone (such as
   `fritz.box`) is not accepted; use the IP address or the `.local` name
   there. Anything else on the station interface gets `421 bad_host` (DNS
   rebinding). On the AP interface a foreign Host is the captive portal
   instead (section 16).
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
   connections including the WebSocket) and forbids framing, with one
   exception for images: `https://tile.openstreetmap.org`, the tiles of the
   live view's optional station map (section 8). The page requests tiles
   only after the user asks for the map; the tile images carry
   `referrerpolicy="origin"`, because the tile server's usage policy asks
   for a Referer (the page's origin, nothing more); everything else keeps
   `no-referrer`.

UI assets are pre-compressed, embedded in the image and served with
`Content-Encoding: gzip`, `ETag: "<app version>-<UI digest>"` (the digest
covers the compressed assets, so a development build whose version string
does not change still gets a fresh UI) and `Cache-Control:
no-cache` (`304` on a matching `If-None-Match`). The UI routes with the URL
fragment (`/#/casters`), so the server serves only `/`, `/index.html` and the
fixed asset names; other paths outside `/api/` and `/ws` get `404` (on the
AP interface: the captive redirect). `HEAD` on the page and the assets
answers as `GET` does (status, `ETag`, `Content-Type`, `Content-Encoding`,
`Content-Length`, the page's Content-Security-Policy; `304` on a matching
`If-None-Match`) without the body; `HEAD` on `/api/` paths and `/ws` stays
`405` (also without a body).

The fixed asset names are `/app.js`, `/app.css` and `/favicon.svg` (`/` and
`/index.html` are the page). The build generates the table of them from
`components/web/ui/` (`tools/ui_bundle.py`: path, content type, gzip body;
`priv_include/web_assets.h`); `web_asset_find()` maps a request URI to an
entry (query ignored) and `web_etag_match()` evaluates `If-None-Match`. The
page loads only these files (no inline script or style), so the
Content-Security-Policy can stay at the device's own origin (the map tiles
above are the one exception, images only).

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
  `SETTINGS_EVENT`), every session ends. A BOOT-button admin reset also
  clears a password that lives only in RAM because the settings are
  unavailable (seen through `health_status_t.admin_resets`).
- An ended session's WebSocket is closed with code `4001`.

**Login throttling** (per client, RAM): a client is its IPv4 address (an
IPv6 address: its /64 prefix) together with the interface it came over
(station or AP); the 16 clients seen last are tracked. A client's first 5
consecutive failures cost nothing extra; after its n-th failure (n >= 5) its
next attempt is accepted only `min(2^(n-4), 300)` s later. Earlier attempts
get `429 rate_limited` with that client's `retry_in_s`, without checking the
password and without counting. A success resets that client's count; a new
admin password resets every count. One client's failures never delay
another client. Against guessing from many addresses, each interface also
has a ceiling: at most 30 failures in a burst, then one more every 120 s;
while it is reached, attempts over that interface get `429 rate_limited`
too. A single throttled client stays well below the ceiling, and failures
on the station interface never touch the AP's, so nobody on the LAN can lock
out an owner who logs in over the station's own AP. A wrong `current`
password in `PUT /api/admin-password` counts the same way.

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

Everything else answers `401 unauthorized` without a session; the
right-hand column answers `403 forbidden` without a session while public
status is off. The public form never contains a host, mountpoint, username,
IP address, SSID, version or the antenna position. `web/public_status`
defaults to off.

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
| POST | `/api/setup/step` | adm | Keep the setup wizard's next step | settings `sys/setup_step` |
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
| POST | `/api/gnss/maintenance` | adm | Start or extend receiver maintenance | `station_maintenance_start` |
| DELETE | `/api/gnss/maintenance` | adm | End it now | `station_maintenance_end` |
| GET | `/api/gnss/live` | adm | Live view: window, satellites, totals | `station_live_view_touch`, `gnss_obs_get_live` |
| POST | `/api/gnss/live` | adm | Open or extend the live view | `station_live_view_start` |
| DELETE | `/api/gnss/live` | adm | Close it | `station_live_view_stop` |
| POST | `/api/gnss/detect` | adm | No receiver found: search again now | `station_request_detect` |
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
| POST | `/api/wifi/pending/retry` | adm | Try the pending credentials again | `network_retry_pending_credentials` |
| POST | `/api/wifi/pending/discard` | adm | Forget the pending credentials | `network_discard_pending_credentials` |
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
| GET | `/api/diag/flash-capture` | adm | Receiver byte capture download (bench builds) | `um98x_flash_capture_read` |
| PUT | `/api/diag/log-level` | adm | Runtime log level per tag | `diag_set_log_level` |
| GET | `/api/logs` | adm | Log ring (this or previous boot) | `diag_log_read*` |
| GET | `/api/journal` | adm | Event journal | `diag_journal_read` |
| GET | `/api/connections` | adm | Connection history (casters, Wi-Fi) | `diag_journal_read`, ntrip_server |
| GET, DELETE | `/api/coredump` | adm | Download / erase the core dump | `diag_coredump_*` |
| POST | `/api/system/reboot` | adm | Reboot | lifecycle |
| POST | `/api/system/factory-reset` | adm | Erase user settings, reboot | settings, lifecycle |
| POST | `/api/system/safe-mode/clear` | adm | Leave safe mode | `lifecycle_clear_safe_mode` |

---

## 5. Session and setup

### `GET /api/session` (pub)

```json
{"api": 1, "authenticated": true, "admin_set": true, "setup_done": false,
 "claim_allowed": false, "on_setup_ap": false, "public_status": false,
 "settings_available": true, "idle_left_s": 1740,
 "hostname": "onocoydiy", "mdns_host": "onocoydiy", "ap_ssid": "onocoydiy-1A2B",
 "setup_step": "reception"}
```

`claim_allowed` = no admin password stored and this request came over the
AP. `idle_left_s` is `null` without a session. The UI starts here: claim
form, login form, setup wizard or dashboard.

| Field | Notes |
|---|---|
| `hostname` | the station name in use (status `device.hostname`); the login page shows it |
| `mdns_host` | the host label the mDNS responder answers, without `.local`: the hostname, or `<hostname>-2` (then `-3`, ...) after the responder renamed itself because another device answered the name; `null` while the responder does not run. The renamed label is never stored: the next boot probes the hostname again |
| `ap_ssid` | the station's own access point name (`onocoydiy-XXXX`), `null` before Wi-Fi is set up |
| `setup_step` | the setup wizard's next step as the station keeps it (`POST /api/setup/step`): `wifi`, `reception`, `onocoy`, `casters`; `null` without a session, once setup is done, or when none is stored |

The names are no new disclosure: DHCP, mDNS and the access point's beacon
carry them anyway.

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

Empty body. Writes `sys/setup_done = 1` and erases `sys/setup_step`.
`200 {"setup_done": true}`.

### `POST /api/setup/step` (adm)

Request `{"step": "wifi" | "reception" | "onocoy" | "casters"}`: the step
the wizard shows next, kept in `sys/setup_step` (u8: 1 `wifi`, 2
`reception`, 3 `onocoy`, 4 `casters`; 0 or missing = none; user
configuration, so a factory reset erases it). The wizard posts it before it
shows that step, so a setup that moves from the setup network to the home
network (or a reload, or a new login) continues there.
`200 {"setup_step": "onocoy"}`. Errors: `400 invalid_field` (`step`; also
for `"done"`, which is `POST /api/setup/done`), `409 settings_unavailable`
(the UI then keeps the order in memory).

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
              "upstream": "ok", "probe_age_s": 120, "offline_s": 0, "ap_active": false, "ap_reasons": [],
              "ap_close_in_s": null, "mdns_host": "onocoydiy",
              "assoc_no_ip_s": 0, "ladder_triggers": [],
              "cred_trial": false, "cred_trial_left_s": 0, "cred_pending": false, "pending_ssid": null,
              "cred_reverted": false, "cred_reverted_why": null},
  "receiver": {"state": "streaming", "model": "UM982", "firmware": "R4.10Build18975",
               "responding": true, "identity_confirmed": true, "profile_applied": true,
               "position_locked": true, "surveying": false, "survey_window_left_s": 0,
               "rtcm_age_ms": 180, "best_sys": "galileo", "best_bands": 4, "journal": "none",
               "uart_closed": false, "rx_maintenance": false, "maintenance_left_s": 0, "maintenance_chosen_s": 0,
               "next_detect_in_s": null},
  "casters": [
    {"slot": 0, "name": "Onocoy", "enabled": true, "protected": true, "host": "servers.onocoy.com",
     "state": "streaming", "reason": "none", "state_age_s": 3600, "connected_s": 3600,
     "today_online_s": 43200, "today_offline_s": 4, "graced_uptime_permille": 1000,
     "streak_ok": true, "graced_uptime_so_far_permille": 1000, "data_rate_permille": 1000, "budget_left_s": 469,
     "onocoy_ready": true, "bytes_sent": 74000000, "lag_ms_now": 40, "next_retry_s": 0, "unreached_s": 0, "hint": ""}
  ],
  "health": {"level": "degraded", "last_step": "none", "ladder_suspended": false,
             "factory_reset_refused": false, "recovery_mode": "full", "reboot_recheck": false,
             "reboot_hold": [], "reboot_hold_left_s": null, "reboot_cap_wait_s": null},
  "alerts": [{"id": "link_loss", "source": "health"}],
  "availability": {"protected_down_today_s": 4, "budget_left_s": 469},
  "update": {"app_available": true, "app_version": "v0.4.0",
             "app_reverted_version": "", "app_reverted_in_stage1": false,
             "gnss_available": true, "gnss_build": 19152, "gnss_version": "R4.10Build19152",
             "gnss_blocked_reason": "none", "gnss_blocked_left_s": 0,
             "gnss_flash_available": false, "gnss_flash_reason": "not_enabled_in_build",
             "checking": false, "last_check_unix": 1760080000}
}
```

| Field | Notes |
|---|---|
| `device.time_source` | `ledger_clock_source()`: `gnss`, `sntp` or `none` (then `time_unix` is `null`) |
| `safe_mode` | `lifecycle_get_boot_info()`: `retry_in_s` until the automatic normal-boot retry |
| `network.upstream` | last probe: `none`, `ok`, `no_ip`, `dns_failed`, `no_upstream` |
| `network.probe_age_s` | since that probe, in whole minutes (so the section changes at most once a minute; `/api/wifi` has the exact age); `null` = never |
| `network.mdns_host` | as in `GET /api/session`: the station answers `http://<mdns_host>.local/` on the home network and on its own access point; `null` while the responder does not run |
| `network.ap_close_in_s` | seconds until the station closes its access point, as `ap.close_in_s` in `/api/wifi` (section 11) |
| `network.assoc_no_ip_s` | associated with the access point without an IP for this long (0 = not associated, or it has an IP) |
| `network.ladder_triggers` | network ladder triggers holding (`network_ladder_trigger_t`): `protected` (the protected caster down while RTCM is healthy), `assoc_no_ip` (associated 60 s without an IP), `unreached` (a slot not reaching its caster for 180 s) |
| `network.cred_*`, `pending_ssid` | changed Wi-Fi credentials (section 11): `cred_trial` while they are tried (`cred_trial_left_s` until the station goes back), `cred_pending` with `pending_ssid` while they wait to be confirmed (`null` = none), `cred_reverted` when they did not work and the station is back on its previous credentials, `cred_reverted_why` why (`no_ip`: no address within the 90 s; `auth_failed`: the network refused the password; `null` unless reverted); `ssid` names the network in use, during a trial the one being tried |
| `receiver.state` | `station_state_t`; `idle` during receiver maintenance (`rx_maintenance` tells the two apart) |
| `receiver.uart_closed` | the receiver UART is closed and `gnss_link` reopens it on its own ("receiver UART closed (reopening)"; never in safe mode, where the link is not opened) |
| `receiver.rx_maintenance` | receiver maintenance is on (section 8): nothing is sent to the receiver; `maintenance_left_s` until it ends by itself, `maintenance_chosen_s` the chosen duration with extensions (both 0 when off) |
| `receiver.rtcm_age_ms` | since the last CRC-valid RTCM frame (`gnss_link`), `null` = never |
| `receiver.journal` | `none`, `running`, `waiting`, `unreadable`, `unknown` (section 9) |
| `receiver.next_detect_in_s` | no receiver found, or the station looking for it (`no_receiver`, `detecting`): seconds until the next detection pass, `0` while one runs; `null` in every other state. Read at the time of the request (a pass can take most of a minute) |
| `casters[]` | every slot; the full status is in `/api/casters`. Onocoy day figures (`graced_uptime_permille`, `streak_ok`, `graced_uptime_so_far_permille`, `data_rate_permille`, `budget_left_s`, section 10) are `null` for other slots, and for the protected slot too while it is disabled or not set up (`onocoy_ready` false); `onocoy_ready` is true for an Onocoy slot with the console's mountpoint, username and password; `unreached_s` = the current run of failures that never reached the caster (DNS, connect timed out or failed locally; 0 = none) |
| `alerts[]` | health's alerts (`source: "health"`) and the station's (`source: "station"`); `config_failed` carries `retry_in_s`; `no_receiver` (the station found no receiver: raised in `no_receiver`, not with `receiver_in_loader` or in receiver maintenance, cleared when the state ends) carries `saw_bytes` (the last detection pass received bytes it could not read: wiring or port speed rather than power) and `next_detect_in_s` (as `receiver.next_detect_in_s`) |
| `availability.budget_left_s` | protected slot's data-rate-adjusted `budget_left_s`, else `ledger_protected_budget_left_s()`; `null` without an enabled protected slot that is set up (`onocoy_ready`), or without a trusted clock. Information only, never a gate |
| `health.factory_reset_refused` | a BOOT-button factory reset was refused within the last 10 min because a receiver update was running (the UI shows a notice); admin form only |
| `health.recovery_mode` | automatic receiver recovery in effect (`gnss/recovery`, `health_recovery_mode_t`): `full`, `no_reboot` (everything but the ESP32 reboot), `alerts_only` (nothing is sent to the receiver: a stall is only reported and journalled, level `degraded`, `failed` once the ladder gives up); admin form only |
| `health.reboot_recheck` | no receiver found and an ESP32 reboot was blocked or held: it is looked at again after the next detection pass, when the reboot cap allows and nothing holds it; admin form only |
| `health.reboot_hold` | why the GNSS ladder's ESP32 restart is postponed right now (`[]` = nothing): `setup` (first setup not finished; no time limit), `ap_client` (someone is connected to the station's access point), `admin` (an admin session made a request or sent a WebSocket ping in the last 5 min), `power_on` (the first 60 min of station time since power-on while nothing valid was heard from the receiver in this boot). A held restart is never queued: it is journalled once (section 14) and looked at again later, like one the cap blocks; admin form only |
| `health.reboot_hold_left_s` | seconds until the 30 min limit on `ap_client` and `admin` together (per ladder episode) lets a held restart run anyway; `null` when neither holds |
| `health.reboot_cap_wait_s` | seconds until the reboot cap (60 min apart, at most 3 in 24 h) allows a ladder restart; `null` when it allows one now |
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
fifth concurrent client, and a third client without a session (public
viewers get at most 2 of the 4 slots, so the admin's pages always find
one), get `503 too_many_clients`. A handshake that carries a body, and a
plain `GET /ws` without the upgrade headers that carries one, get
`400 bad_request` with `Connection: close` (section 18); a plain `GET /ws`
without a body gets `400 bad_request` on a connection that stays open. A
handshake extends the session's idle timeout only when it succeeds.
Turning public status off closes every client without a session (`1008`).

**Client to server** (text frames, JSON, at most 256 bytes; larger closes
with `1009`):

| Message | Effect |
|---|---|
| `{"type": "subscribe", "topics": ["status", "progress", "log"], "log_cursor": 51200}` | replaces the topic set (admin topics are ignored for public clients); `log_cursor` (optional): send the log lines after this cursor (as `/api/logs?cursor=`), else, when `log` is newly subscribed, only lines logged from now on |
| `{"type": "ping"}` | `{"type": "pong"}`; also extends the session's idle timeout |

WebSocket control frames are answered by the server itself (a `PING` frame
gets a `PONG` with its payload, a `CLOSE` frame a `CLOSE`); they never
extend a session, and neither does the server's own bookkeeping of the
connection (section 18): only the page's own messages do.

Default topics after the handshake: `status` and `progress` (admin), `status`
(public). `event` messages go to every admin client.

**Server to client:**

| `type` | When | Body |
|---|---|---|
| `hello` | once, first | `{"type": "hello", "api": 1, "admin": true, "topics": ["status", "progress"], "status_period_ms": 1000}` |
| `status` | 1 Hz | `{"type": "status", "seq": 812, "full": false, "uptime_s": 86400, "data": {"casters": [...], "receiver": {...}}}` |
| `event` | at once, at most 10/s (coalesced) | `{"type": "event", "event": "caster_state", "slot": 0, "state": "backoff"}` |
| `progress` | at most 2 Hz while a job runs, plus one final message | `{"type": "progress", "job": "gnss_update", "phase": "downloading", "pct": 42, "done": false}` |
| `log` | every 500 ms when there are new lines, at most 4 KB per message | `{"type": "log", "from": 51200, "cursor": 51234, "gap": false, "text": "I (1234) station: ...\n"}` |
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
`ap_down`, `time_synced`), `scan_done`, `settings_changed` (`ns`, `client`;
advisory: reload that page's data), `receiver_restarted`, `rebooting`
(`reason`; sent from `LIFECYCLE_EVENT_PRE_REBOOT`, then the server closes
with `1012`).

`settings_changed.client` is the `X-Client-Id` request header (a random id
per tab, `[A-Za-z0-9_-]`, at most 32 characters; the UI sends it with every
request that is not a `GET`) of the request during which, or within 2 s
after which, the settings changed; `null` when no such request was running.
A page that finds its own id ignores the event as a warning (it made the
change) and only reloads its data when no form is being edited.

**Progress jobs** (`job`, `phase`; `pct` or `bytes`/`total` where known):

| `job` | Phases |
|---|---|
| `check` | `checking` |
| `app_update` | `downloading` → `rebooting` |
| `app_upload` | `receiving` → `verifying` → `rebooting` |
| `gnss_update` | `downloading`; when it ends with `ok` the station took the request and the `gnss_flash` job follows (without flashing in the build: `not_available_yet`, and a `gnss_check` job follows) |
| `gnss_check` | builds without receiver flashing: `checking`, then the final message with `result` `ok` (the package passes every rule; nothing was sent) or `package_rejected` (`failed_rule`, `reason`) |
| `gnss_upload` | `receiving` → `inspecting` |
| `gnss_flash` | the flash phases (`um98x_flash_phase_t`): `checking`, `entering`, `transferring`, `committing`, `finishing`, `done`, `failed`, `needs_user`, with `bytes_sent`, `bytes_total`, `retries`, `transfers`, `failure`, `frames_acked`, `frames_total`, `frames_sent`, `entry_attempts`, `journal_state` (`um98x_journal_state_t`, `null` before the journal opens). `finishing` covers leaving the loader, the version check and the profile's re-apply and its check; the journal closes after that. `failed` with `"done": false` is a transfer that failed after the point of no return while the session goes on (back to the loader, the same package again within the caps, or a stop for the user's decision); only the final message ends the job |

The final message has `"done": true` and `"result"`: `ok` or a result code
(section 12), plus `message` (the `esp_err_t` name) on failure. For
`gnss_flash` and `gnss_check` it is the station's own end of the job (a
session, a retry or restart, the journal's close or a package check ended),
never a phase on its way. The final
`gnss_flash` and `gnss_check` messages also carry the session's outcome:
`outcome` (`station_flash_outcome_t`), `firmware` and `build` (the
receiver's version log afterwards, `""`/`0` unknown), `failed_rule` (1-12,
0 none), `reason` (text); `result` is `ok` for `success` and `checked`,
else the outcome's name (`old_firmware`, `unexpected_build`, `not_done`,
`package_rejected` for `rejected`, `not_started`, `needs_user`).

**Close codes:** `1012` the device reboots, `4001` session ended (logout,
expiry, password change), `4003` client too slow (its queue stayed full for
30 s), `1009` client message too large, `1008` public status was turned off
(clients without a session), `1013` a public viewer made room when the
server ran out of sockets (section 18; reconnect after the usual backoff).
An admin's client is never closed for that.

**Log.** Each client reads the log from its own cursor: a `log` message
holds whole lines, those after `from` up to `cursor`, so a client that read
up to cursor N over HTTP and subscribed with `"log_cursor": N` gets the
following lines exactly once. A message whose `from` is not where the
client stands was sent before its subscription took effect (or the client
missed one): it reads the difference over HTTP and subscribes again.
`gap` is true when the ring overwrote lines before this client read them.

**Back-pressure.** Pushes go through `httpd_queue_work` and never block a
producer. Each client has a queue of at most 8 messages / 16 KB. When it is
full: a `status` message is dropped and the client gets a full snapshot
next time; log lines wait (the client's log cursor stays, so they go out
later, without a gap); `progress` keeps only the newest; `event` drops the
oldest.

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
  "link": {"baud": 115200, "mode": "stream", "rx_bytes": 123456789, "rx_age_ms": 12, "rtcm_age_ms": 180,
           "open": true, "open_failures": 0, "hands_off": false},
  "autocfg": true,
  "recovery": "full",
  "profile": {
    "signal_group_master": 3, "signal_group_slave": 6,
    "base_mode": "survey_in", "survey_in_s": 600, "fixed": null,
    "station_id": 1234, "com1_baud": 115200,
    "rtcm": [{"msg": 1005, "period_s": 1}, {"msg": 1033, "period_s": 1}, {"msg": 1077, "period_s": 1}],
    "sat_view_output": false,
    "antenna_model": "", "antenna_serial": "", "antenna_setup_id": 0
  },
  "rx_settings": {"valid": true, "age_s": 3600, "read_unix": 1760000000,
                  "b1cb2a": "on", "mmp": "off", "mask_deg": 10.0,
                  "antenna": {"model": "TRM59800.00     SCIS", "serial": "5000123", "setup_id": 0}},
  "apply_steps": {"antenna": {"sent": false, "ok": false, "reply": null},
                  "b1cb2a": {"sent": true, "ok": true, "reply": null}},
  "rtcm1033": {"valid": true, "station_id": 1234, "antenna": "TRM59800.00     SCIS", "setup_id": 0,
               "antenna_serial": "5000123", "receiver_type": "...", "receiver_fw": "...",
               "receiver_serial": "...", "age_ms": 400, "bad_frames": 0},
  "maintenance": {"on": false, "left_s": 0, "chosen_s": 0, "choices_s": [900, 1800, 3600, 7200, 14400],
                  "default_s": 1800, "max_left_s": 14400},
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
| `rtcm1230_supported` | `gnss/rtcm1230_ok`: `true` the receiver accepted message 1230 at the last apply, `false` it refused it, `null` never applied (the key is not stored: "not applied yet") |
| `link.mode` | `off`, `stream`, `command`, `raw` (`gnss_link`) |
| `link.rx_bytes` | bytes received from the receiver since boot (0 = nothing arrived yet) |
| `link.open`, `open_failures` | the UART driver is installed; failed installs since boot. `open: false` with a mode other than `off` = "receiver UART closed (reopening)": `gnss_link` reopens it on its own |
| `link.hands_off` | the link refuses every write and lease (receiver maintenance) |
| `recovery` | `gnss/recovery` as stored, `health_recovery_mode_t` (a stored value out of range reads as `full`, as health reads it) |
| `profile.antenna_*` | the base antenna descriptor the receiver publishes in 1033: `antenna_model` the IGS antenna name with its radome (`""` = leave the receiver's own value, the default), `antenna_serial`, `antenna_setup_id` 0-255; serial and setup ID only matter with a model (stored cleared without one) |
| `rx_settings` | what the receiver's configuration dump said at the last apply session this boot (`station_status_t.rx_settings`): `valid` false and every value unknown (`unknown` / `null`) after a reboot until the next apply. `b1cb2a`, `mmp` (`um98x_setting_t`): BeiDou B1C/B2a observations in RTCM, multipath mitigation; `mask_deg` the elevation mask (0.1°); `antenna` the descriptor the receiver holds (`null` = not read). `age_s` since the read-back, `read_unix` its time (`null` without a clock then). MMP and the mask are shown only, never sent (PROVISIONAL, to be confirmed on hardware) |
| `apply_steps` | outcome of the antenna and B1C/B2a steps of the last apply session (sent only when the read-back asked for them; a refusal does not fail the session): `sent`, `ok` (accepted), `reply` the receiver's refusal text or `"timeout"` (`null` unless refused). The UI words an antenna refusal "antenna model rejected: <reply>" |
| `rtcm1033` | the latest CRC-valid 1033 the receiver published (`gnss_obs_get_1033`), strings as received; `{"valid": false, "bad_frames": n}` before the first that decoded; `bad_frames` counts 1033 frames that did not decode since boot |
| `maintenance` | receiver maintenance (below): `left_s`, `chosen_s` 0 when off; `choices_s` the durations offered, `max_left_s` the most time left after extensions |
| `profile.fixed` | `{lat_deg, lon_deg, height_m}` in `fixed` mode, else `null`; fields a mode does not use are ignored on input and come back cleared (`survey_in_s` 0 in `fixed` mode) |
| `position` | locked or last surveyed (`station_status_t.position`); `source`: `none`, `survey`, `averaged`, `user`; `sigma_m` 0 = unknown |
| `arp` | latest CRC-valid 1005/1006 (`gnss_obs_get_arp`), converted to WGS84; `offset_m` = 3-D distance to the locked position, `null` when not locked; `{"valid": false}` before the first frame |

### `PUT /api/gnss/profile` (adm)

Every member optional: `profile` (the whole profile object as above),
`autocfg` (bool, `gnss/autocfg`) and `recovery` (`gnss/recovery`: `full`,
`no_reboot` or `alerts_only`; anything else `400 invalid_field`; health
reads it every tick, a step already running completes).

```json
{"profile": {"signal_group_master": 3, "signal_group_slave": 6, "base_mode": "fixed", "survey_in_s": 600,
             "fixed": {"lat_deg": 45.123456789, "lon_deg": -75.123456789, "height_m": 85.1234},
             "station_id": 1234, "com1_baud": 115200,
             "rtcm": [{"msg": 1005, "period_s": 1}], "sat_view_output": false,
             "antenna_model": "TRM59800.00     SCIS", "antenna_serial": "5000123", "antenna_setup_id": 0},
 "autocfg": true, "recovery": "full"}
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
`com1_baud` >= 460800; `antenna_model` and `antenna_serial` lose their
trailing spaces (the stored form) and must then be at most 31 printable
ASCII characters without `"` (inner spaces stay: the IGS name pads the
antenna type to 16 characters before the radome); `antenna_setup_id` 0-255.
The UI's hint: enter the IGS antenna name with its radome only if the
antenna has a published calibration; leave it empty to keep the receiver's
value; to clear a model the receiver already holds, enter its null-antenna
name (`ADVNULLANTENNA`).

`202 {"accepted": true, "saved": true, "apply": "queued"}`. `apply` is
`queued`, `unchanged` (same profile, or only `autocfg` changed),
`safe_mode` (stored; the station applies it after safe mode, when
`autocfg` is on), `unsupported_model` (stored; never applied to this
receiver), `maintenance` (stored; receiver maintenance is on, and its end
runs the stream-first check, which applies it when `autocfg` is on) or
`not_running` (stored; the station did not start this boot).
A fixed position entered here is stored as the locked position with
`source: "user"`; switching to `survey_in` forgets the locked position.
Errors: `400 invalid_field`, `409 journal_open` (any receiver update
journal open, for a body with `profile` or `autocfg`; the recovery mode
alone changes nothing on the receiver), `409 settings_unavailable`.

### `POST /api/gnss/apply` (adm)

Empty body. `202 {"accepted": true}`. Errors: `409 journal_open`,
`409 safe_mode`, `409 receiver_maintenance` (nothing is sent to the
receiver then), `409 unsupported_model`.

### `POST /api/gnss/position/lock` (adm)

Empty body: lock the latest surveyed position (the station switches the
profile to `fixed` and reconfigures). `202 {"accepted": true}`. Errors:
`409 no_position_yet` (no 1005 yet), `409 unsupported_model`,
`409 journal_open`, `409 safe_mode`, `409 receiver_maintenance`.

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
  "epoch_age_ms": 420, "epochs": 86400, "epoch_gaps": 0, "epoch_period_ms": 1000, "epoch_jumps": 0,
  "systems": [
    {"sys": "galileo", "satellites": 9, "bands": ["l1", "l5", "l6", "e5ab"],
     "signal_ids": [2, 15, 16, 22, 23], "cnr_dbhz": {"l1": 44, "l5": 47, "l6": null, "e5ab": 46}}
  ],
  "messages": [{"msg": 1077, "count": 86400, "frames": 86400, "period_ms": 1000, "age_ms": 420, "expected_period_s": 1}],
  "rtcm1230_supported": true
}
```

`rtcm1230_supported` as in `GET /api/gnss` (`null` = never applied).

`systems` lists only present constellations. `signal_ids` are the MSM
signal IDs that carried at least one cell in the last 10 s (a signal an
encoder announces in its signal mask without any cell is left out, and so
is its band); `satellites` likewise counts satellites with a cell;
`cnr_dbhz` is the mean per band over the latest epoch's cells (MSM4 to
MSM7; `null` = not available). `epochs` counts ended epochs: by their
closing MSM, or by the next epoch time when that MSM never came. `epoch_gaps`
counts epochs missing from the profile's shortest MSM period
(`epoch_period_ms`); both are `null` when the profile has no MSM message.
`epoch_jumps` counts outages over 60 s and epoch-time resets (not gaps).
Counters run since the last receiver configuration. `messages` lists the
profile's messages first (seen or not; `count` 0 = never seen), then every
other message observed (`expected_period_s` `null` = not in the profile).
For MSM types `count` counts epochs and `frames` the frames carrying them;
`period_ms` is the mean of the last 8 intervals and `age_ms` the time since
the last one (`null` = fewer than two / never). `messages` is `null` only
when the statistics cannot be read.

### `GET /api/sky` (pub\*/adm)

```json
{"available": true, "reason": null,
 "satellites": [{"sys": "gps", "prn": 12, "el_deg": 45, "az_deg": 210, "cnr_dbhz": 44}]}
```

At most 64 satellites, from the receiver's satellite-in-view output of the
last 30 s. `prn` is the satellite's PRN (GLONASS: its slot number).
`available: false` with `reason`: `sat_view_off` (the profile's
`sat_view_output` is off; the UI offers to turn it on) or `no_data` (it is
on, but nothing arrived in the last 30 s).

### Receiver maintenance

"Hands off" while the user works on the receiver through its other port (a
PC tool, a reflash): nothing is sent to the receiver (no detection, query,
apply, `RESET`, flash, ladder step or live-view toggle), reception goes on
and every CRC-valid RTCM frame still reaches the casters. The station
reports `idle` meanwhile (`rx_maintenance` tells it apart); the ledger
counts the time as planned. It ends by itself when its time is up, on "end
now", or with a power cut (an ESP32 software reset keeps it with its time
left); the end runs the station's stream-first check, as at boot. Entry and
exit are journalled (`rx_maintenance`). Whether input on the link can
disturb a receiver worked on through its other port is PROVISIONAL, to be
confirmed on hardware.

While it is on, the receiver actions answer `409 receiver_maintenance`
("receiver maintenance: nothing is sent to the receiver"): apply, lock,
re-survey, receiver updates (banner, upload, confirm), flash retry and
restart-receiver; a profile save is stored (`apply: "maintenance"`).

#### `POST /api/gnss/maintenance` (adm)

`{"duration_s": 1800}`: start for that long, or, while on, extend by it (at
most `max_left_s` left). `duration_s` one of `choices_s` (900, 1800, 3600,
7200, 14400); left out or 0 = the default (1800). Effective at once.
`200 {"on": true, "left_s": 1800, "chosen_s": 1800}`. Errors:
`400 invalid_field` (`duration_s`), `409 journal_open` (a receiver update
session), `409 busy` with `"reason": "receiver_session"` (a receiver session
or job runs, e.g. a configuration: offer it again when it ends),
`409 busy` with `"reason": "live_view_output"` (the live view's
satellite-in-view output may be on: in maintenance nothing would turn it
off, and a save made through the receiver's other port would keep it for
good, so the station turns it off first; an open view stays open without
it, `sky_why: "maintenance"`; offer it again in a few seconds),
`409 safe_mode`.

#### `DELETE /api/gnss/maintenance` (adm)

End now: the station leaves maintenance within a second and checks the
receiver as at boot. `202 {"accepted": true}`. Errors:
`409 not_in_maintenance`, `409 safe_mode`.

### Live view

A window the user opens on request (satellites first). It runs for
`default_min` minutes (10, `CONFIG_STATION_LIVE_VIEW_MIN`), can be extended
up to `max_min` (60) from now, and closes by itself when its time is up, at
the user's stop, when a receiver update begins, or when no admin client
asked for it for `idle_s` (120 s): every `GET /api/gnss/live` counts as
asking (`station_live_view_touch`), so a page polls it while it shows the
view. While it is open the receiver's satellite-in-view output is on,
unsaved (never saved in the receiver), every `sky_period_s`, for the
satellites' azimuth and elevation, and turned off when the window closes.
Each toggle is one configuring command under a short command lease (the
casters pause for it; journalled with its length), only while the station
streams with nothing else to do: never while it configures, detects,
recovers or flashes, never in maintenance, never with `autocfg` off, never
for a model the station does not configure, and never when the profile's
`sat_view_output` sends it permanently. A toggle sends its command at most
twice (about 7 s without an answer). An "on" without an answer may have
taken: `sky` is `blocked` with `sky_why: "no_answer"`, and it is turned off
when the window closes (extending sends it again). An ESP32-only reset
while it may be on (reboot, app update, crash) is remembered in RTC memory:
the next boot turns it off once the station streams. After a receiver
reconfiguration, reset or restart the output is off (`sky_why:
"reconfigured"`); opening or extending the view turns it on again. Everything else comes from the RTCM
stream without any receiver traffic. Whether toggling the output
interrupts RTCM is PROVISIONAL, to be confirmed on hardware.

#### `GET /api/gnss/live` (adm)

```json
{
  "window": {"open": true, "left_s": 540, "idle_left_s": 118, "open_s": 60, "max_s": 3600,
             "default_min": 10, "max_min": 60, "idle_s": 120,
             "sky": "on", "sky_why": "none", "sky_period_s": 5, "last_end": "none", "sky_reply": null},
  "receiver_state": "streaming", "rx_maintenance": false,
  "position": {"locked": true, "source": "survey", "lat_deg": 45.123456789, "lon_deg": -75.123456789,
               "height_m": 85.1234},
  "totals": {"satellites": 38, "tracked": 36, "signals": 112, "with_pos": 35, "dropped": 0,
             "cnr_dbhz": 43, "msm_age_ms": 400, "sky_age_ms": 1200,
             "bands": {"l1": {"satellites": 36, "cnr_dbhz": 44}, "l5": {"satellites": 20, "cnr_dbhz": 46}},
             "systems": [{"sys": "galileo", "satellites": 9, "bands": {"l1": 9, "l2": 9, "l5": 9, "l6": 7}}]},
  "satellites": [{"sys": "gps", "prn": 12, "in_msm": true, "msm_age_ms": 400, "signal_ids": [2, 15],
                  "cnr_dbhz": {"l1": 44, "l2": 38}, "el_deg": 45, "az_deg": 210, "sky_cnr_dbhz": 44,
                  "pos_age_ms": 1200}]
}
```

| Field | Notes |
|---|---|
| `window` | `station_status_t.live`, computed at the time of the request (not as the station task last published it: a start or stop shows at once, also while a detection pass runs); `left_s`, `idle_left_s`, `open_s` 0 while closed. `sky` (`station_live_sky_t`): `off`, `pending` (wanted, waits for a free moment: `sky_why`), `on`, `on_profile` (the profile sends it permanently), `blocked` (not in this window: `sky_why`), `stopping`. `sky_why` (`station_live_why_t`). `last_end` (`station_live_end_t`) why the last window closed. `sky_reply` the receiver's reply to the last failed toggle (`null` = none) |
| `receiver_state`, `rx_maintenance` | as in `/api/status`, for a page without the WebSocket (the setup wizard) |
| `position` | the station's locked or last surveyed position (`null` coordinates with `source: "none"`), for the optional station map |
| `totals` | `gnss_obs_get_live` over every satellite, also those beyond the list: `satellites` listed, `tracked` in an MSM within the last 10 s, `signals` their signals, `with_pos` with azimuth and elevation, `dropped` beyond the list; `cnr_dbhz` mean over every satellite band (`null` = none); `bands` per band with tracked satellites (count, mean C/N0); `systems` per constellation with tracked satellites, their count per band (only bands with any). `null` when the observations cannot be read |
| `satellites` | at most 96, by constellation and number. `prn` as in `/api/sky` (QZSS and SBAS by PRN, GLONASS by slot). `in_msm` and `msm_age_ms` (`null` = only the sky view knows it); `signal_ids` the MSM signal IDs; `cnr_dbhz` per band present: its best signal's C/N0 (`null` = no C/N0 in the MSM type). `el_deg`, `az_deg` (`null` until the satellite-in-view output reported it), `sky_cnr_dbhz` from that report, `pos_age_ms` its age |

The UI shows satellites first: a sky plot (satellites with a position,
coloured by C/N0, labelled), the per-satellite table, counts per
constellation and band with per-band totals, the mean C/N0 per band and
tiles (tracked satellites, signals, mean C/N0); then, last and optional, a
station map: the position with a small OpenStreetMap tile mosaic, fetched
only after the user asks for it, with the attribution and a plain link to
open the position in an external map (offline: the coordinates only).

#### `POST /api/gnss/live` (adm)

`{"minutes": 10}` (optional; 0 or left out = `default_min`; at most
`max_min`, else `400 invalid_field`): open the window, or extend an open one
by that much (at most `max_min` from now); either also turns the
satellite-in-view output on again after a receiver reconfiguration ended it.
`200` with the body of `GET /api/gnss/live`. Errors: `409 journal_open` (a
receiver update session), `409 safe_mode`.

#### `DELETE /api/gnss/live` (adm)

Close it now (the output is turned off at the next free moment).
`200 {"ok": true}`; `409 live_view_closed` when none is open.

### No receiver: `POST /api/gnss/detect` (adm)

`{}`. While the station found no receiver (`receiver.state`
`no_receiver`) the next detection pass starts now instead of at
`next_detect_in_s`; the search schedule goes on from where it was (passes
30 s apart for the first 5 min of the episode, then every 5 min; the full
baud list after two failed short passes and always after 5 min). A pass
that runs already counts. `202 {"accepted": true}`. Errors:
`409 not_no_receiver` (any other state, receiver maintenance included),
`409 safe_mode`. The UI's no-receiver card (dashboard alert, setup
wizard's reception step, Signals) offers it as "Check again".

---

## 9. Receiver update sessions

A receiver update session is recorded in a journal (`gnss/flash_job`) while
it is open. `journal` (here and in `/api/status`):

| Value | Meaning | Lockout |
|---|---|---|
| `none` | no session | — |
| `running` | a session or its boot-time recovery runs, or the user's retry or restart was accepted (its record still says `needs_user` until the session writes; it cannot be cleared meanwhile) | full: no app updates, reboots or receiver config changes |
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
               "transfers": 1, "failure": "transport", "last_err": "ESP_ERR_TIMEOUT",
               "frames_acked": 1984, "frames_total": 2957, "frames_sent": 1990, "entry_attempts": 1,
               "journal_state": "needs_user"},
  "receiver_in_loader": false,
  "receiver_text": "...last 4 KB of receiver text...",
  "actions": {"retry": {"allowed": false, "why": "app_trial"},
              "restart_receiver": {"allowed": true, "why": null},
              "clear": {"allowed": true, "why": null}},
  "last": {"outcome": "needs_user", "failed_rule": 0, "cross_layout": false, "check_only": false,
           "reapply_ok": false, "firmware": "", "build": 0,
           "reason": "waits for you: the loader prompt came back without the success text", "ended_unix": 1760000000,
           "ended_age_s": 120},
  "check_running": false,
  "flash_available": true,
  "flash_reason": null
}
```

`record` is `null` for `none`, `unreadable` and `unknown`. `receiver_in_loader`
is the station alert for a receiver found at its loader without a session
(then `restart_receiver` is offered with `journal: "none"`). `receiver_text`
comes from `station_get_flash_log()` (the last 4 KB the receiver sent during
the last session, non-printable bytes as `.`; empty until there is any).
`last` is the last session or package check this boot
(`station_flash_last_t`; `null` when there was none): `outcome`
(`station_flash_outcome_t`), `failed_rule` for `rejected`, `firmware` and
`build` the receiver reported afterwards, `reapply_ok` whether the station
profile came back after it. `check_running`: a package check runs (builds
without flashing). `flash_available` is `station_flash_available()`: `false`
in builds without receiver flashing (`CONFIG_UM98X_FLASH_ENABLE` off), then
`flash_reason` is `not_enabled_in_build` and the UI says that receiver
firmware updates are not enabled in this build yet; packages can still be
uploaded and checked.

`why` codes: `no_journal`, `not_waiting` (a session runs), `safe_mode`
(only `clear` there), `settings_unavailable`, `receiver_maintenance` (retry
and restart: nothing is sent to the receiver), `app_trial`, `app_update`,
`not_available_yet`, `unreadable` (a damaged journal: only `clear`; the
POST answers `409 journal_open`).

### `POST /api/gnss/flash/retry`, `/restart-receiver`, `/clear` (adm)

Request `{"confirm": true}` (else `400 confirm_required`); the UI shows each
action's warning first:
- retry: the same package is flashed again (needs the package still pinned);
- restart-receiver: the receiver leaves its loader and the version is
  checked; a broken application may only come back after a power cycle;
- clear: the session is forgotten and the package released; the receiver
  may not run valid firmware.

`202 {"accepted": true}` (`clear` completes at once: `200 {"ok": true}`);
retry and restart then report as progress job `gnss_flash`. Retry sends
only the package the journal pinned (its SHA-256 checked again on a fresh
RAM copy, the journalled model); it is one more transfer beyond the
automatic cap, confirmed by this request. Errors: `404 no_journal`, `409
not_waiting`, `409 safe_mode`, `409 receiver_maintenance`, `409 app_trial`,
`409 app_update`, `409 settings_unavailable`, `501 not_available_yet`
(retry and restart in builds without receiver flashing).

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
 "mountpoint": "<console mountpoint>", "username": "<console username>", "password_set": true,
 "label": "", "v2_raw_body": false, "backoff_profile": "fast", "protected": true,
 "dup_stream_ack": false}
```

Passwords are never returned (`password_set` only). Every entry
(`{"slot", "config", "status", "onocoy_ready"}`) says with `onocoy_ready`
whether it is an Onocoy slot that is set up: host in `onocoy.com`,
`mountpoint`, `username` and a stored password (false for every other
slot).

**Onocoy slots** hold exactly what the Onocoy console shows for a
station's NTRIP credentials, and the UI asks for those in the console's
words and order: host `servers.onocoy.com` and port 2101 (shown read-only),
`mountpoint`, `username`, `password`. On the wire
(`ntrip_wire_names()`): NTRIP 2.0 posts to `/<mountpoint>` with Basic
`<username>:<password>`; NTRIP 1.0 (the default) sends
`SOURCE <password> /<username>`, as Onocoy tells sources that speak only
1.0 to do. A slot stored before the username field existed (Onocoy host,
empty `username`, the old credential username in `mountpoint`) keeps
working with the old mapping (1.0: `mountpoint` as the mountpoint; 2.0:
`label` or `onocoydiy` as the mountpoint, `mountpoint` as the Basic-auth
user) but is not set up (`onocoy_ready` false); its next save while enabled
needs the username.

**`status`** (`ntrip_caster_status_t`):

```json
{"state": "streaming", "reason": "none", "state_age_s": 3600, "progress_age_ms": 120,
 "connects": 3, "failures": 1, "disconnects": 2, "restarts": 0,
 "failures_by_class": {"dns": 0, "connect": 1, "tls": 0, "timeout": 0, "remote_close": 0, "write_stall": 0,
                       "keepalive": 0, "auth": 0, "mount_busy": 0, "config": 0, "banned": 0},
 "connected_since_unix": 1760086400, "connected_s": 3600,
 "today_online_s": 43200, "today_offline_s": 4,
 "graced_uptime_permille": 1000, "streak_ok": true, "graced_uptime_so_far_permille": 1000,
 "data_rate_permille": 1000, "budget_left_s": 469,
 "bytes_sent": 74000000, "frames_sent": 900000, "epochs_sent": 86000, "epochs_sent_today": 43200,
 "frames_dropped": 0, "bytes_dropped": 0, "epochs_dropped": 0, "epochs_dropped_today": 0,
 "frames_skipped_idle": 12, "lag_drops": 0, "laps": 0, "lag_ms_now": 40, "max_lag_ms": 1000,
 "backoff_attempt": 0, "next_retry_s": 0,
 "last_status_code": 200, "last_status_line": "ICY 200 OK", "server_header": "",
 "resolved_ip": "35.207.172.57", "dns_cached_used": 0, "unreached_s": 0,
 "tls_version": null, "tls_cipher": null, "hint": "", "last_err": null}
```

"Today" is the current UTC day. The Onocoy day figures are `null` for
other slots, and for the protected slot while it is disabled or not set up
(`onocoy_ready` false): numbers for a slot that cannot stream would only
mislead. `graced_uptime_permille` is the day fraction (online plus the
300 s grant, of 86400 s; for diagnostics); `graced_uptime_so_far_permille`
= 1 - max(0, unplanned downtime today - 300 s) / time elapsed today, with
the downtime as the availability ledger counts it (planned time left out)
and the time counted from 00:00 UTC, or from when the slot was enabled and
set up during this boot when that came later (`null` without a trusted
clock); `data_rate_permille` is `null` until something was sent today;
`streak_ok` whether today's offline time is still within the budget for
that data rate (the UI never shows the day in red while it is true);
`tls_*` are `null` without TLS; `max_lag_ms` is the slot's lag
limit; `unreached_s` the current run of failures that never reached the
caster (DNS failures, connects that timed out or failed locally; 0 = none;
the UI says "caster not reached for N s").

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
  (`fast` is refused there: `400 invalid_field`), or, when left out, the
  stored profile (a `fast` one left from an Onocoy host becomes
  `standard`). These host rules belong to `ntrip_server`
  (`ntrip_server_apply_host_rules()`, which every save applies too); the
  server applies them to the request only to name a refused field.
- `dup_stream_ack`: see below.

Validation (`400 invalid_field` with `field`, and the rule in `message`):
no control characters anywhere; lengths in UTF-8 bytes: `name` <= 23,
`host` <= 63 (a DNS name or an IPv4 literal), `mountpoint` <= 47 of
`[A-Za-z0-9._-]`, `username` <= 47 without `:`, `password` <= 63 (NTRIP
1.0: no spaces), `label` <= 47 of `[A-Za-z0-9]`; `port` 1-65535;
`ntrip_version` 1 or 2; an enabled slot needs `host`, `port`, `mountpoint`,
a password (1.0) and a username (2.0). Onocoy slots: `username`
`[A-Za-z0-9._-]` (1.0 sends it as the mountpoint), `password` 1-63
characters without spaces in either version; an enabled Onocoy slot needs
`mountpoint`, `username` and a password.

**Caster policy** (`409 caster_policy` with `reason`). The rules belong
to `ntrip_server`: `ntrip_server_set_config()` checks them against the
stored slots under the same lock as the save and reports which one refused
(`ntrip_policy_t`); the server only names it:
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
  "mdns_host": "onocoydiy",
  "configured": true, "credentials_unreadable": false, "ssid": "Home",
  "online": true, "associated": true, "ip": "192.168.1.50", "rssi": -60, "channel": 6,
  "time_synced": true, "upstream": "ok", "probe_age_s": 120,
  "reconnects": 1, "connect_failures": 0, "offline_s": 0, "retry_in_s": 0,
  "last_disconnect_reason": 8, "fallback_dns": false, "assoc_no_ip_s": 0,
  "cred_trial": false, "cred_trial_left_s": 0, "cred_pending": false, "pending_ssid": null,
  "cred_reverted": false, "cred_reverted_why": null,
  "ladder": {"active": false, "rung": 0, "withheld": "none", "triggers": [], "actions": 0},
  "ap": {"active": false, "ssid": "onocoydiy-1A2B", "reasons": [], "clients": 0,
         "recovery_left_s": 0, "close_in_s": null, "password_set": false},
  "scan": {"running": false, "age_s": 30,
           "results": [{"ssid": "Home", "rssi": -55, "channel": 6, "secured": true}]}
}
```

`last_disconnect_reason` is the numeric `wifi_err_reason_t`. `scan.results`
are the cached ones (no radio activity; at most 24); `age_s` is `null`
before the first scan. `mdns_host`, `assoc_no_ip_s`, `cred_*`,
`pending_ssid` and `ladder.triggers` as in `/api/status` (section 6).
`ap.close_in_s`: seconds until the station closes its access point: the
linger after the station got its IP (`CONFIG_NETWORK_AP_LINGER_S`, 15 s:
the setup page counts it down), or the recovery AP's lifetime left; `null`
while the AP is down or nothing is scheduled (first setup without
credentials, the offline fallback, a recovery AP held past its lifetime by
a connected client).

### `PUT /api/wifi` (adm)

```json
{"ssid": "Home", "password": "...", "ap_password": ""}
```

Every member is optional.
- `ssid` 1-32 bytes; `password` `""` (open network), 8-63 characters or 64
  hex digits (only with `ssid`). A missing `password` keeps the stored one
  when `ssid` is unchanged, and then nothing changes and the station does
  not reconnect; for a new SSID it is required (`400 invalid_field`).
- `ap_password` (optional): 8-63 characters sets the AP password (WPA2),
  `""` removes it (the AP is then open; the UI warns); missing = unchanged.
  It takes effect the next time the AP comes up. The UI sends it alone, so
  changing it never touches the station's Wi-Fi connection.

`202 {"accepted": true, "persisted": true, "trial": true, "applied": true}`.
With new credentials the station switches about a second later; an AP that
is up stays up until 15 s after the station has an IP (`ap.close_in_s`
counts down), so the UI polls `GET /api/wifi` for `online` and `ip`.
Changing Wi-Fi drops every caster session (the UI warns). With the
settings unavailable the credentials are used for this boot only
(`"persisted": false`). `applied` is `false` when the credentials are only
stored: nothing changed (the same network and password), or the QEMU
development build, whose emulated Ethernet stays its link.

`trial` (`network_set_credentials`): with credentials stored before (and
different), the new ones are stored as **pending** and tried for 90 s.
They replace the current ones once they get an IP; otherwise the station
goes back to its previous credentials and keeps the new ones pending
(`cred_reverted`, `pending_ssid`), so a typo never strands a remote station.
A network that refuses the new password goes back sooner: three refused
attempts in a row before any association (wrong password, a handshake that
times out, an access point of that name only with another security
setting) end the trial at once (`cred_reverted_why: "auth_failed"`); a
network that is just out of reach keeps the 90 s.
Pending credentials are tried again first thing at the next boot, with
`POST /api/wifi/pending/retry`, or dropped with
`POST /api/wifi/pending/discard`. Without stored credentials (first setup)
`trial` is `false`: they are the station's at once.

### `POST /api/wifi/pending/retry` (adm)

Empty body. Try the pending credentials again (90 s, then back to the
current ones unless they got an IP). `202 {"accepted": true}`;
`404 nothing_pending`.

### `POST /api/wifi/pending/discard` (adm)

Empty body. Forget the pending credentials (a trial in progress ends: the
station goes back to its current credentials). `200 {"ok": true}`;
`404 nothing_pending`.

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

**Trust.** A release is trusted because of where it comes from and what it
lists. The manifest and every file are fetched over HTTPS, the server
certificate checked against the CA bundle and the dates of every certificate
in its chain against the station's clock (no transfer starts before the
clock is set: `download_failed`, and a check retries in 1 h), from the GitHub
releases of the repository set at build time (`release_repo`), and only from
GitHub's release hosts: `github.com`, `release-assets.githubusercontent.com`
and `objects.githubusercontent.com`. Every hop of a redirect must stay on
them, over https, without user info, and on `github.com` within
`/<release_repo>/releases/` (a renamed or moved repository is refused), or
the download stops before it connects (`untrusted_host`). The manifest
lists each file with its size and SHA-256, and a downloaded app image or
receiver package is used only if both match. No signing key is needed: a
build without one accepts the plain manifest (and reads a signed one without
checking its signature). A build with a manifest key (`signed_manifests`,
e.g. a fork that signs its releases) accepts only manifests signed with one
of its keys and refuses unsigned ones (`signature_failed`). Manual uploads
are not affected by any of this.

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
| `gnss_flash_available`, `gnss_flash_reason` | `station_flash_available()`; the reason is `not_enabled_in_build` when it is `false`, else `null` |
| `checking`, `last_check_unix` | state `checking`; `ota/last_check` |

### `GET /api/update` (adm)

```json
{
  "state": "idle", "progress_pct": 0, "checks_enabled": true,
  "release_repo": "Averyy/onocoydiy-esp-um982", "signed_manifests": false, "last_check_unix": 1760080000,
  "last_attempt_unix": 1760080000, "last_check_result": "ok", "last_http_status": 200, "next_check_in_s": 86100,
  "app_max_bytes": 4194304, "pkg_max_bytes": 6287360,
  "running_elf_sha256": "3f2a...64 hex...",
  "last_result": {"job": "gnss_update", "result": "not_available_yet", "message": "ESP_ERR_NOT_SUPPORTED"},
  "app": {"running": "v0.3.0", "dev_build": false, "latest": "v0.4.0", "available": true,
          "latest_rolled_back": false, "rolled_back": ["v0.3.1"], "reverted": "", "reverted_in_stage1": false,
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
| `checks_enabled` | `false` when the build cannot check: no valid release repository, or it requires signed manifests and none of its keys is usable (manual uploads still work) |
| `release_repo` | `owner/name` whose GitHub releases the station reads (`CONFIG_FW_UPDATE_GITHUB_REPO`; `null` if unset) |
| `signed_manifests` | `true` when the build has a manifest key and accepts signed manifests only |
| `last_result` | outcome of the last job the backend saw end (`job`: `check`, `app_update`, `app_upload`, `gnss_update`, `gnss_upload`, `rollback`), `null` this boot until one ended |
| `last_attempt_unix` | the last update check this boot, successful or not (`last_check_unix` stays the last success, which the daily schedule runs from); `null` before one, or without a trusted clock then |
| `last_check_result` | how that check went (`fw_update_check_result_t`): `ok`, `no_release` (HTTP 404: no public release with a manifest at `release_repo`), `http_error` (another status: `last_http_status`), `no_response` (offline, DNS, TLS, timeout, no trusted clock yet; retried after 1 h), `untrusted_redirect`, `bad_manifest` (an answer, but no manifest this build accepts); `null` before the first check of this boot. A field of its own because `last_result` already names job outcomes |
| `last_http_status` | the check's final HTTP status, `null` when no answer came |
| `next_check_in_s` | until the next automatic check is due (`0` = due, it waits for the network), `null` when this build cannot check |
| `app_max_bytes`, `pkg_max_bytes` | the largest app image (the update slot) and receiver package (`pkg_store_capacity()`) an upload may be; the UI checks a file against them before sending it |
| `running_elf_sha256` | the running app's ELF SHA-256 (`esp_app_desc_t.app_elf_sha256`): an app image with the same one is the code running now |
| `app.trial.stage` | `none`, `confirming`, `verifying`, `watching` |
| `app.latest_rolled_back` | the latest release is one that was rolled back here: not offered, installable with a warning |
| `app.rolled_back` | every version rolled back on this station, newest first (`fw_update_get_rolled_back()`: reverted by its trial, rolled back by the user, or rolled back twice in its first minute; the last 8, kept through a factory reset). The banner offers none of them, however many daily checks later; installing one, from the banner or as an upload, asks for `rolled_back_ack` first |
| `gnss.expected_downtime_s` | `station_expected_flash_downtime_s()` for the offered package (PROVISIONAL, to be confirmed on hardware); information, never a gate |
| `gnss.budget_left_s` | as in `/api/status`, for the dialog |

**Result codes** (`last_result.result`, final `progress` messages): `ok`,
`not_available_yet`, `refused` (the state changed between click and start),
`download_failed`, `checksum_mismatch`, `size_mismatch`, `invalid_image`,
`signature_failed` (check: a build with a manifest key got a manifest that
is unsigned or not signed with one of its keys), `untrusted_host` (check,
`app_update`, `gnss_update`: a redirect led off `release_repo`'s releases on
GitHub's release hosts, off https, or carried user info; nothing was fetched
from there), `package_rejected` (a receiver package failed a
pre-flight rule; with `failed_rule`, 1-12 as numbered in `um98x_flash.h`,
and `cross_layout`), `failed` (anything else; `message` has the
`esp_err_t`). An app upload that stops for `rolled_back_ack_required`
ends with `refused`.

### `POST /api/update/check` (adm)

Empty body. `202 {"accepted": true}` (also when a check already runs).
Errors: `501 checks_disabled`, `409 app_trial` (the new image's first
minute), `409 busy` (an install runs), `429 rate_limited` (one manual check
per 60 s).

### `POST /api/update/app` (adm)

`{"version": "v0.4.0", "rolled_back_ack": false}`: `version` is what the
banner showed. Downloads into the inactive slot, checks the image's size and
SHA-256 against the manifest (on the stream and again in flash) and that it
is that version of this project, activates, reboots, then runs the
two-stage trial.
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
receiver firmware updates are not enabled in this build yet.

`{"build": 19152, "cross_layout_ack": false}`. One click runs the chain:
download the manifest's package for the detected model into the package
slot (size and SHA-256 from the release manifest, checked on the stream and
on the stored copy), then start the session on the station. `202
{"accepted": true}`; progress job `gnss_update`, then `gnss_flash` (the
receiver identified, every package rule on the RAM copy, the transfer, the
version check, the profile re-applied and checked; the final message says
how it ended).

**In builds without receiver flashing** the chain ends after the download:
the start step answers `not_available_yet`, the package is checked against
every rule (progress job `gnss_check`), then released; nothing is sent to
the receiver. `last_result` says `not_available_yet`; the UI shows
"Receiver package downloaded and checked; receiver firmware updates are not
enabled in this build yet. Nothing on the receiver changed."

Errors (all before anything is downloaded): `409 not_offered` (the build
changed or nothing is offered; dev builds are offered nothing), `409
receiver_maintenance`, `409 app_trial` (`retry_in_s` = time left), `409
app_update`, `409 journal_open`, `409 receiver_not_streaming`, `409
unsupported_model`, `409 safe_mode`, `409 package_pinned`, `409 busy`.

### `POST /api/update/rollback` (adm)

`{"confirm": true}`. Marks the running version as rolled back and reboots
into the other slot (reboot reason `app_rollback`, planned downtime: a
failed trial's own revert is `app_revert` and is not). `202 {"accepted":
true}`. Errors: `409 no_rollback_image`, `409 reboot_inhibited`, `409
busy`, `409 journal_open`.

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
warning), and its version must not be one rolled back on this station
(`app.rolled_back` of `GET /api/update`), else `409
rolled_back_ack_required` (the query `?rolled_back_ack=1` overrides it after
the same warning an install from the banner gives; both queries combine as
`?any_project=1&rolled_back_ack=1`). A refused upload is read to its end
first, nothing is written. Then the image goes through
`fw_update_upload_app_*`, which claims the update slot and a reboot inhibit
before the first byte.

An image whose app ELF SHA-256 equals the running one (the code running
now) is refused the same way with `409 same_image`, unless the request
carries the header `X-Reinstall: 1` (the UI asks "This is the version
already running (v). Install it again?" first). A confirmed reinstall of
the identical image runs stage 1 of the trial only (the bootloader's
first-minute check): identical code needs no verify window and no crash
watch, so receiver updates are not held back for the watch's day.

`200 {"sha256": "...", "size": 1234567, "version": "v0.4.0+dev", "project": "onocoydiy-esp-um982", "reboot": true}`;
the device reboots into the image about a second after the response and
runs the trial. Errors: `400 invalid_image` (not a valid app for this chip),
`400 size_mismatch` (fewer bytes than announced), `409
rolled_back_ack_required`, `409 same_image`, `409 journal_open`, `409
busy`, `409 app_trial`.

The UI reads the file's first bytes before sending anything (image magic
`0xE9`; the app description at offset 32: magic `0xABCD5432`, `version` at
offset 48, `project_name` at offset 80, `app_elf_sha256` at offset 176) and
refuses another project or a file over `app_max_bytes` with the reason,
asks for the acknowledgement of a version on `app.rolled_back`, and asks
before reinstalling the running image.

### `POST /api/upload/gnss` (adm)

Size at most the package slot (`pkg_store_capacity()`, 6 MB minus 4 KB).
Refused before anything is stored: `409 receiver_maintenance`, `409
safe_mode`, `409 app_trial`, `409 app_update`, `409 journal_open`, `409
receiver_not_streaming`, `409 unsupported_model`, `409 package_pinned`,
`409 busy`.

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

`GET /api/upload/gnss` answers `200 {"staged": null}` when no uploaded
package waits for confirmation (nothing waiting is the resource's normal
state, not a missing one).

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
partition layout than the receiver's is refused unless the build allows it
(tested on hardware); then it needs `cross_layout_ack`.

**In builds without receiver flashing** the package is checked instead:
`202 {"accepted": true, "check_only": true}`, progress job `gnss_check`
(every rule on a RAM copy, against the receiver identity confirmed this
boot; nothing is sent to the receiver), then the package is released; the
result is in the final message and in `GET /api/gnss/flash` (`last`).
Other errors: `404 nothing_staged` and the `409` codes of the upload.

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
  "network_recoveries": 0, "rtcm_gap_ms": 230, "gnss_retry_in_s": null,
  "factory_reset_refused_age_s": null, "admin_resets": 0, "recovery_mode": "full", "reboot_recheck": false,
  "reboot_hold": ["setup", "power_on"], "reboot_hold_left_s": null, "reboot_cap_wait_s": null,
  "heap": {"internal_free": 120000, "internal_min": 98000, "internal_largest": 64000, "psram_free": 7000000,
           "psram_min": 6900000, "psram_largest": 4000000},
  "inhibited_by": null, "update_slot": "none",
  "boot": {"reset_reason": "sw", "reset_class": "planned", "prev_reboot_reason": "app_update",
           "boots_since_power_on": 3, "crashes": 0, "brownouts_since_power_on": 0,
           "safe_mode": false, "safe_mode_retry_in_s": null, "stable": true, "prev_session_uptime_s": 3600},
  "boot_history": [{"reset_class": "planned", "reset_reason": "sw", "planned_reason": "app_update",
                    "boot_no": 3, "prev_uptime_s": 3600, "version": "v0.3.0"}],
  "link": {"mode": "stream", "baud": 115200, "rx_bytes": 0, "tx_bytes": 0,
           "hw_fifo_overflows": 0, "rx_buffer_full": 0, "rx_events_dropped": 0, "line_errors": 0,
           "baud_switch_bytes": 0, "rtcm_held": 0, "flushed_bytes": 0,
           "last_rx_age_ms": 10, "last_rtcm_age_ms": 180, "open": true, "open_failures": 0, "hands_off": false,
           "framing": {"bytes_in": 0, "bytes_discarded": 0, "rtcm_frames": 0, "rtcm_crc_errors": 0,
                       "rtcm_reserved_nonzero": 0, "rtcm_bad_length": 0, "text_lines": 0,
                       "text_rejected": 0, "text_aborts": 0, "text_overflows": 0, "bin_skipped": 0,
                       "resyncs": 0, "prompts": 0, "prompts_ignored": 0}},
  "hub": {"frames_in": 0, "bytes_in": 0, "epochs_in": 0, "epochs_by_mmb": 0, "epochs_by_time": 0,
          "epochs_by_gap": 0, "publish_failures": 0, "consumers": 1, "capacity": 262144,
          "last_frame_age_ms": 180},
  "station": {"state": "streaming", "rtcm_excluded": 0, "detect_failures": 0, "detect_saw_bytes": false,
              "lease_held": false, "since_lease_release_ms": null, "rx_maintenance": false},
  "obs": {"epochs": 86400, "epoch_gaps": 0, "epoch_period_ms": 1000, "epoch_jumps": 0},
  "network": {"reconnects": 1, "connect_failures": 0, "offline_s": 0, "last_disconnect_reason": 8,
              "fallback_dns": false, "probe": "ok", "probe_age_s": 120, "assoc_no_ip_s": 0,
              "ladder": {"active": false, "rung": 0, "withheld": "none", "triggers": [], "actions": 0}},
  "journal": {"available": true, "capacity": 4064, "oldest_seq": 1, "newest_seq": 77, "sector_cycles": 0,
              "written": 12, "queued": 0, "refused_kind": 0, "refused_budget": 0, "queue_full": 0, "io_errors": 0},
  "log": {"lines": 812, "ring_overwritten": 0, "cut": 0, "console_dropped": 0, "console_dropped_bytes": 0,
          "prev_boot_available": true, "prev_boot_lines": 640},
  "coredump": {"present": false, "size": 0, "task": "", "pc": null, "backtrace": []},
  "byte_capture_built": false
}
```

`byte_capture_built`: this build records the receiver's bytes (bench builds,
`GET /api/diag/flash-capture`); the UI shows the download only then.

`rtcm_gap_ms` is the time since the last RTCM frame. `gnss_retry_in_s` is
the time until the receiver recovery's next retry pass (`null` = none
scheduled). `factory_reset_refused_age_s` is the time since a BOOT-button
factory reset was refused because a receiver update was running (`null` =
not this boot); `admin_resets` counts BOOT-button password resets this
boot. `recovery_mode`, `reboot_recheck`, `reboot_hold`,
`reboot_hold_left_s` and `reboot_cap_wait_s` as in `/api/status`.
`log.console_dropped` (and `_bytes`) count only lines the serial console
never sent because the log ring overwrote them first; a burst that the
console sends later, in order, is not a drop.
`link.open` and `link.open_failures`: the UART driver is installed, failed
installs since boot ("receiver UART closed (reopening)" while `open` is
false in a mode other than `off`); `link.hands_off`: receiver maintenance. `journal` and `log` are the event journal's and the log ring's
counters (`diag_journal_info()`, `diag_log_stats()`; `journal.capacity` in
records; `log.prev_boot_available` false and `log.prev_boot_lines` 0 when
the previous boot's lines are not readable, see `GET /api/logs`).
Per-caster loss counters are in `/api/casters`. `boot_history` is the last
16 boots, newest first (`prev_uptime_s` `null` = unknown). `boot` counts
since power-on from RTC memory; state written before another app image ran
(an update or a rollback, then back) is not used, so the counters start over
as after a power cut, and a reboot into another image that the bootloader
rolled back before the new image recorded a boot reads as `app_revert`. `inhibited_by` is
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
  "today": {"utc_day": 20736, "date": "2026-10-10", "down_s": [4, 0, 0, 0], "planned_s": [0, 0, 0, 0],
            "reconnects": [1, 0, 0, 0], "power_off_s": 0, "rx_overflows": 0},
  "history": []
}
```

Per-slot arrays have `max_slots` entries. `history` holds up to 30 past days
in the `today` shape, newest first. `down_s` counts seconds a slot should
have streamed but did not; `planned_s` the planned part of it: casters
paused for receiver configuration or a receiver update, and a restart that
is a direct consequence of a user action (Restart, an app update or
rollback, a factory reset, leaving safe mode, the BOOT-button resets;
`lifecycle_reboot_user_planned()`), from the moment it begins until each
caster streams again, at most `CONFIG_LEDGER_PLANNED_BOOT_S` (120 s) after
the boot. Recovery restarts (ladders, low memory, a failed app trial, the
safe-mode retry), crashes, brownouts and power cuts are never planned, and
neither is a crash that follows a planned restart: only the boot right
after the planned restart counts it, and the gap after any crash or reset
pin is unplanned. Planned time counts against the budget all the same. `clock` is `none` until a
trusted time exists (`utc_second_of_day` is then `null`). The budget is
information only.

### `GET /api/diag/tasks`

`{"tasks": [{"name": "gnss_rx", "priority": 18, "core": 1, "stack_free_min": 1234, "cpu_pct": 3}]}`.
`core` `null` = not pinned; `cpu_pct` since the previous call (poll every
2 s or slower).

### `GET /api/diag/flash-capture`

Bench builds only (the bench console, `CONFIG_UM98X_FLASH_BYTE_CAPTURE`):
every byte the receiver flasher and the bench console sent to and received
from the receiver, time-stamped (hex and text), as a `text/plain`
attachment (`flash-capture-<version>.txt`; at most the last 256 KB; data
frames as one summary line each). `404 not_found` in other builds. Read on
the worker like the other downloads.

### `PUT /api/diag/log-level`

`{"tag": "station", "level": "debug"}`: `tag` <= 15 characters or `"*"`;
`level` `none`, `error`, `warn`, `info`, `debug`, `verbose`. Not persisted.
`200 {"ok": true}`.

### `GET /api/logs?cursor=N[&boot=prev][&format=text]`

`{"cursor": 51234, "more": false, "gap": false, "text": "..."}` with at most
8 KB of whole lines newer than `cursor` (start with 0; pass the returned
`cursor` back; `more: true` = call again at once). `gap` = lines were
overwritten before they were read. `boot=prev` reads the previous boot's
ring. It is empty after a power cut or an app image change, and also when
the lines this image finds are older than the last boot: after another
image ran in between (an update or a rollback, then back), this image's own
lines from before that run are reported as unavailable, never shown as the
previous boot's or with an "older" label (checked against the image
generation at boot). Both flags come
from `diag_log_read_ex()`. `format=text`
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
| `ladder_step` | ladder (0 GNSS, 1 network, 2 caster) | `(outcome << 8) \| code` (outcome 0 started, 1 healthy again, 2 failed or refused, 3 abandoned, 4 not run because of the recovery mode, 5 held; network: `(reason << 16) \| ...`, and code 0 = a pass started with the trigger bits as the reason: 1 protected caster down, 2 associated without an IP, 4 a slot not reaching its caster). GNSS code 9 with outcome 5: the ESP32 restart is held back for now, `b = (hold << 16) \| (5 << 8) \| 9` with the hold bits 1 `setup`, 2 `ap_client`, 4 `admin`, 8 `power_on` (as `health.reboot_hold`); codes 6, 8 and 9 with outcome 0 keep their meaning; see `diag.h`, `network.h` |
| `app_update` | result (`esp_err_t`) | — |
| `app_revert` | trial stage that gave the image up (`fw_update_trial_t`; 0 = the user's rollback) | 1 = the user's rollback; a stage-1 rollback the bootloader did: the attempt (1, 2) |
| `gnss_flash` | the session's outcome (`station_flash_outcome_t`), written when its journal closes | transfers under the journal |
| `safe_mode` | episode | — |
| `wifi_disconnect` | `wifi_err_reason_t` | RSSI |
| `data_loss` | counter id (`diag_loss_id_t`) | increase since the previous record for that id |
| `caster_down` | slot (bits 0-7) \| end class (`ntrip_fail_class_t`, 0 = not a caster failure; bits 8-15) \| short sessions not journalled since the previous `caster_down` (bits 16-30) | session length s |
| `caster_up` | slot (bits 0-7) \| failed attempts since the previous session (bits 16-30) | outage s |
| `wifi_up` | connect attempts during the outage | outage s |
| `rx_maintenance` | 1 entry, 0 exit | chosen duration s (entry), time spent s (exit) |
| `live_view` | event (bits 0-7: 1 opened, 2 extended, 3 closed, 4 satellite-in-view output on, 5 off) \| detail << 8 (closed: end reason; on/off: 0 accepted, 1 rejected, 2 no answer, 3 not sent) | opened: window s; extended: s left; closed: s open; on/off: ms the lease held RTCM back |

Enum numbers are the C values; the UI decodes them with the appendix
tables.

### `GET /api/connections`

The connection history next to the ledger: the newest 64 records of
`caster_down`, `caster_up`, `wifi_up` and `wifi_disconnect`, newest first,
read backwards through the journal.

```json
{"complete": true,
 "entries": [{"seq": 812, "time_unix": 1760000000, "uptime_s": 3600, "kind": "caster_down",
              "slot": 0, "slot_name": "Onocoy", "class": "remote_close", "count": 0, "duration_s": 3600,
              "reason": null, "rssi": null}]}
```

| Field | Notes |
|---|---|
| `complete` | every journal record was looked at (the list holds every connection record the journal keeps) |
| `time_unix` | `null` without a trusted clock then: `uptime_s` dates it after its boot |
| `slot`, `slot_name` | caster records: the slot and its name now (`null` without one, and in safe mode); `null` for Wi-Fi |
| `class` | `caster_down`: why the session ended (`ntrip_fail_class_t`; `none` = not a caster failure: a pause, a configuration change, a health restart, the receiver's RTCM stopping, the network going down); `null` otherwise |
| `count` | `caster_down`: shorter sessions not journalled since the slot's previous `caster_down`; `caster_up`: failed attempts since the previous session; `wifi_up`: connect attempts during the outage; `null` for `wifi_disconnect` |
| `duration_s` | `caster_down`: session length; `caster_up`, `wifi_up`: outage length; `null` for `wifi_disconnect` |
| `reason`, `rssi` | `wifi_disconnect`: `wifi_err_reason_t` and the RSSI at the drop; `null` otherwise |

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

**Reaching the station.** On the home network the station answers
`http://<name>.local/` (mDNS; `mdns_host` in `GET /api/session`, usually
the station name `onocoydiy`, `onocoydiy-2` when another device holds that
name) and `http://<ip>/` (the address the router gave it; also in the
router's device list under the station name). On its own access point the
page is at `http://192.168.4.1/`, and the `.local` name answers there too.
The UI shows both, as links.

**First setup** (unconfigured station, open AP `onocoydiy-XXXX`):
1. `GET /api/session`: `admin_set: false`, `claim_allowed: true`.
2. `POST /api/setup/admin`: the admin password; the response logs in.
3. Wi-Fi: `POST /api/setup/step` `{"step": "reception"}` (the step the
   setup continues with on the home network), `GET /api/wifi` (cached
   scan), `POST /api/wifi/scan` if needed (nothing streams yet), then
   `PUT /api/wifi`. The UI polls `GET /api/wifi` every second until
   `online`, then shows the station's name and address and counts down
   `ap.close_in_s`: the AP closes 15 s after the station has its IP. The AP
   follows the station's channel once it connects, so phones may drop off
   it briefly; the UI keeps polling, and when the phone loses it for good
   tells the user how to go on (join the home Wi-Fi, open the `.local` name
   or the router's device list; the AP comes back if the station could not
   join; on a guest network the BOOT button opens the station's own
   network again).
4. The phone is back on the home network: the user opens
   `http://<mdns_host>.local/`, logs in, and the wizard resumes at
   `setup_step`.
5. Reception check, before any caster is enabled: `POST /api/gnss/live`
   opens the live view and the UI polls `GET /api/gnss/live`, with plain
   pass/warn hints (satellites tracked, mean C/N0, whether a constellation
   has 4 bands, the condition of Onocoy's top band reward; guide values,
   PROVISIONAL, to be confirmed on hardware) and "Continue anyway";
   `DELETE /api/gnss/live` when the user goes on.
6. Optional: the Onocoy slot, `PUT /api/casters/0` with `onocoy_preset`,
   the credential username and password; then optional further casters.
7. `POST /api/setup/done` (it also erases `setup_step`): the dashboard.

Every step posts the next one (`POST /api/setup/step`) before it shows it.
Wi-Fi credentials can never be saved before an admin password exists
(every Wi-Fi call needs a session). Logged in on the home network, the
wizard shows the Wi-Fi step only while the page is on the station's own
access point or the station has no working Wi-Fi.

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
During receiver maintenance (section 8) apply, lock, re-survey, receiver
updates (banner, upload, confirm), flash retry and restart-receiver answer
`receiver_maintenance`; a profile save is stored and checked when it ends;
everything else works as usual. Receiver maintenance itself is refused
while a receiver update session is open (`journal_open`) and while a
receiver session or job runs (`busy`).
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
  `max_uri_handlers` 64 with wildcard matching for `/api/casters/*`,
  `max_req_hdr_len` 2048 (cookies and long browser headers), receive
  timeout 4 s (a client that sends nothing that long is dropped while it
  sends headers; a body read gives up after three in a row), send timeout
  10 s. A request with a body that nobody reads (every `GET` and `DELETE`,
  anything outside the API routes, `/ws` and its handshake, a request
  refused before its body, such as a refused Host or Origin or a missing
  session) is answered and the connection closed, instead of reading the
  body away on the server task.
  WebSocket clients: at most 4, at most 2 of them without a session.
- **Request deadline:** the server reads every request on its one task, and
  the 4 s receive timeout alone would let a client that sends a byte every
  few seconds hold that task (and every status push, WebSocket message and
  session expiry queued behind it) for hours. So the first byte of a request
  arms a deadline of `CONFIG_WEB_REQUEST_DEADLINE_S` (6 s): the request
  line, the headers, a JSON body and a WebSocket frame must all be in by
  then, or the connection is closed (`408` while headers are read). A
  receive override on every connection enforces it (`web_conn.h`); the
  handler's end disarms it, and since no handler leaves a body for the
  server to read away afterwards, nothing re-arms it before the next
  request. Should a receive still come after a handler, the stale deadline
  is dropped at the server's next tick (250 ms) or new connection, so it
  never refuses that connection's next request or keeps it from being
  closed for a newcomer. A browser sends a request at once, so a slow
  phone on the setup AP needs a fraction of that, and 6 s still covers two
  lost and resent TCP segments. Uploads and downloads run on the worker,
  which reads a large body at the client's pace: they are not bounded by
  it, only by their own per-receive retries.
- **Socket limit:** `lru_purge_enable` is off. Its least-recently-used
  choice counts only requests received, so an admin's WebSocket, which only
  listens, would be the first connection closed whenever anyone opened
  sockets. Instead the open callback keeps one of the 7 sockets free: when a
  new connection takes the last one, another is closed at once, in this
  order: an idle connection whose last request had no admin session, an
  idle admin connection, a public viewer's WebSocket (`1013`); the oldest
  activity first within each. An admin's WebSocket and a connection whose
  upload or download runs on the worker are never chosen; when nothing else
  is left the new connection itself is closed. The choice runs on the
  server task between requests, so no connection is in the middle of a
  request on that task then. The chosen socket is shut down at once (after
  the `1013` close frame for a viewer), and the server deletes exactly that
  session at its next pass; a close queued to the server would name a
  session slot, which a new connection might have taken by then. Every
  other close of ours (a WebSocket client's, after its close code) works
  the same way. The trade-off: 6 connections stay open at a time instead
  of 7, and an idle keep-alive connection may be closed under a browser (it
  reopens one by itself); in exchange no client on the network can cut off
  the admin's live page by opening sockets. A client-side keep-alive frame
  was the alternative and was not taken: it only helps while the admin's
  frames outnumber the other client's requests, and the server would have
  to tell those frames from the page's own messages so as not to extend the
  session.
- **Refusals that close the connection** (a body that is too large, a
  missing session with a body, an upload refused before its body is read):
  the error goes out with `Connection: close`, the server ends its side
  (`shutdown(SHUT_WR)`), then reads and drops what the client still sends,
  at most 64 KB or 2 s, before it closes. A close with unread data in the
  socket would send a reset, and many clients then drop the answer still in
  their receive buffer (the browser shows "connection reset" instead of the
  413). The server's own warnings for these expected cases
  (`httpd_uri`, `httpd_txrx`, `httpd_ws`) are logged at error level only;
  the handlers log the refusals that matter.
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

**UI assets** (the files the UI provides): `ui/index.html`, `ui/app.css`,
`ui/favicon.svg` and the scripts `ui/js/*.js`, which the build
(`components/web/tools/ui_bundle.py`) joins in file-name order into one
`app.js` inside a strict-mode function. The build checks the page, styles
and scripts (structure, unique ids, and what the CSP of section 2 would
block: no inline scripts or styles, no `on*` or `style` attributes, no
other origins, no `eval`/`innerHTML`-style sinks), gzips every asset and
fails when the compressed total exceeds the budget in
`components/web/CMakeLists.txt` (150 KB). No other files are served.

---

## 19. Open items

Things this contract needs that the current headers do not provide yet:

1. **Boot history**: resolved; `lifecycle_get_boot_history()` reads
   `life/boots`.
2. **Caster policy reason**: resolved; `ntrip_server_set_config()` reports
   the rule it applied (`ntrip_policy_t`, out-parameter) and the server only
   names it (section 10).
3. **Receiver flashing available**: resolved; `flash_available` is
   `station_flash_available()` (`CONFIG_UM98X_FLASH_ENABLE`, off until the
   bench has confirmed the receiver side).
4. **Rolled-back versions**: resolved; `fw_update_get_rolled_back()` and
   `fw_update_is_rolled_back()` expose the bounded list fw_update keeps
   (`ota/bad_ver`, 8 versions, written only when one is added). The banner
   already suppressed every version on it; the API now lists them
   (`app.rolled_back`) and an app upload of one asks for the acknowledgement
   (section 13).
5. **Refused BOOT-button factory reset**: resolved;
   `health_status_t.factory_reset_refused_age_s`.
6. **Receiver text of a flash session**: resolved;
   `station_get_flash_log()` returns the last 4 KB the receiver sent during
   the last session.
7. **GNSS time**: resolved in part. The ledger takes the receiver's GPS
   time of week (`gnss_obs_get_gps_time()`, fed by the health monitor) and
   dates it against its running clock, or after a reset that was not a
   power cut against the previous session's last-alive mark, so `clock` is
   `gnss` while the receiver streams GPS MSM. MSM carries no week number:
   a station that never had a clock, or one that lost power, still needs
   one SNTP time first (`clock` is `none` until then).

---

## Appendix: enumerations

Each enum is listed in C order. Its C value is the position counted from
its first value: 0 for most, 1 for `diag_event_t`, `diag_loss_id_t` and
`um98x_journal_state_t` (`diag_event_t` has a gap: its row gives the values
after it). The journal's `a` and `b` carry these C values.

| Enum (C) | API strings |
|---|---|
| `station_state_t` | `idle`, `detecting`, `configuring`, `streaming`, `flashing`, `flash_recovery`, `recovering`, `no_receiver` |
| `station_alert_t` | `unsupported_model`, `config_failed`, `position_mismatch`, `antenna_moved`, `receiver_attention`, `receiver_in_loader`, `no_receiver` |
| `health_level_t` | `ok`, `degraded`, `recovering`, `failed` |
| `health_step_t` | `none`, `resync`, `link_restart`, `rescan`, `reconfigure`, `receiver_reset`, `reboot` |
| `health_alert_t` | `power_supply`, `receiver_power`, `bands`, `profile_drift`, `heap_fragmented`, `safe_mode`, `settings_degraded`, `no_upstream`, `link_loss`, `power_lost` |
| `health_recovery_mode_t` | `full`, `no_reboot`, `alerts_only` |
| `ntrip_state_t` | `disabled`, `wait_network`, `wait_data`, `connecting`, `streaming`, `backoff`, `auth_failed`, `paused`, `config_error`, `banned` |
| `ntrip_fail_class_t` | `none`, `dns`, `connect`, `tls`, `timeout`, `remote_close`, `write_stall`, `keepalive`, `auth`, `mount_busy`, `config`, `banned` |
| `ntrip_trust_t` | `bundle`, `custom_ca`, `insecure` |
| `ntrip_backoff_profile_t` | `fast`, `standard`, `polite` |
| `um98x_base_mode_t` | `survey_in`, `fixed` |
| `um98x_pos_source_t` | `none`, `survey`, `averaged`, `user` |
| `um98x_setting_t` | `unknown`, `on`, `off` |
| `station_live_sky_t` | `off`, `pending`, `on`, `on_profile`, `blocked`, `stopping` |
| `station_live_why_t` | `none`, `busy`, `not_streaming`, `maintenance`, `autocfg_off`, `unsupported`, `reconfigured`, `rejected`, `no_answer`, `flash`, `not_running` |
| `station_live_end_t` | `none`, `user`, `expired`, `idle`, `flash` |
| `um98x_flash_phase_t` | `idle`, `checking`, `entering`, `transferring`, `committing`, `finishing`, `done`, `failed`, `needs_user` |
| `um98x_journal_state_t` | `prepared`, `baud_changing`, `reset_sent`, `menu2_sent`, `transferring`, `eot_sent`, `verifying`, `failed`, `needs_user` |
| `um98x_flash_failure_t` | `none`, `transport`, `package`, `unconfirmed`, `no_answer`, `app_not_starting`, `entry`, `journal`, `receiver` |
| `station_flash_outcome_t` | `none`, `success`, `old_firmware`, `unexpected_build`, `not_done`, `rejected`, `not_started`, `needs_user`, `checked` |
| `fw_update_state_t` | `idle`, `checking`, `downloading_app`, `downloading_gnss`, `reboot_pending`, `failed`, `uploading_app` |
| `fw_update_trial_t` | `none`, `confirming`, `verifying`, `watching` |
| `fw_update_gnss_block_t` | `none`, `app_trial`, `journal_open`, `unsupported_model`, `safe_mode`, `app_update`, `receiver_not_streaming` |
| `fw_update_check_result_t` | (none: `null`), `ok`, `no_release`, `http_error`, `no_response`, `untrusted_redirect`, `bad_manifest` |
| `network_probe_t` | `none`, `ok`, `no_ip`, `dns_failed`, `no_upstream` |
| `network_ap_reason_t` | `setup`, `fallback`, `linger`, `recovery` |
| `network_cred_revert_t` | (none: `null`), `no_ip`, `auth_failed` |
| `health_reboot_hold_t` (bits) | `setup`, `ap_client`, `admin`, `power_on` |
| setup step (`sys/setup_step`) | (none: `null`), `wifi`, `reception`, `onocoy`, `casters` |
| `network_ladder_why_t` | `none`, `upstream_ok`, `no_upstream`, `journal_open`, `interval`, `inhibited`, `ap_in_use`, `no_reboot_trigger`, `ap_overridden` |
| `network_ladder_trigger_t` (bits) | `protected`, `assoc_no_ip`, `unreached` |
| `gnss_link_mode_t` | `off`, `stream`, `command`, `raw` |
| `lifecycle_reset_class_t` | `power_on`, `planned`, `crash`, `brownout`, `other` |
| `lifecycle_reboot_reason_t` | `none`, `user`, `factory_reset`, `admin_reset`, `app_update`, `app_revert` (a failed trial, also one the bootloader rolled back), `gnss_ladder`, `network_ladder`, `low_heap`, `safe_mode_retry`, `safe_mode_cleared`, `nvs_slot_switch`, `boot_failure`, `app_rollback` (the user's rollback) |
| `esp_reset_reason_t` | `unknown`, `poweron`, `ext`, `sw`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deepsleep`, `brownout`, `sdio`, `usb`, `jtag`, `efuse`, `pwr_glitch`, `cpu_lockup` |
| `ledger_clock_t` | `none`, `sntp`, `gnss` |
| `gnss_sys_t` | `gps`, `glonass`, `galileo`, `sbas`, `qzss`, `beidou`, `navic` |
| `gnss_band_t` | `l1`, `l2`, `l5`, `l6`, `e5ab` |
| `diag_event_t` | `boot`, `reboot`, `ladder_step`, `app_update`, `app_revert`, `gnss_flash`, `safe_mode`, `wifi_disconnect`, `data_loss`, `session`, `caster_down`, `caster_up`, `wifi_up` (1-13); `rx_maintenance` = 20, `live_view` = 21 |
| `diag_loss_id_t` | `hw_fifo`, `rx_buffer_full`, `line_errors`, `rtcm_crc`, `framer_discarded`, `hub_publish`, `baud_switch`, `rx_events_dropped`; 16 and up: caster slots |
