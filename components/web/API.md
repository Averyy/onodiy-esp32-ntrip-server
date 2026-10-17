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
| 409 | state conflicts: `busy`, `journal_open`, `not_waiting`, `app_trial`, `app_update`, `safe_mode`, `not_in_safe_mode`, `receiver_not_streaming`, `unsupported_model`, `package_pinned`, `manifest_changed`, `not_offered`, `rolled_back_ack_required`, `reboot_inhibited`, `settings_unavailable`, `no_position_yet`, `caster_policy`, `sha256_mismatch`, `downgrade`, `no_rollback_image`, `admin_already_set`, `admin_not_set`, `receiver_maintenance`, `not_in_maintenance`, `live_view_closed`, `not_no_receiver`, `no_receiver`, `same_image`, `slot_disabled`, `signal_test_running`, `signal_test_idle`, `signal_test_not_deciding`, `first_apply_held`, `autocfg_off`, `identity_unconfirmed`, `config_failed`, `position_not_locked` |
| 411 | `length_required` (no `Content-Length`: an upload without one, or a body sent in chunks on any route that takes a body) |
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
   there. A name may also come fully qualified, with one trailing dot
   (`<mdns_host>.local.`, the target a DNS-SD browser opens for the
   `_http._tcp` service); an IP address may not. An IPv6 literal
   (`[fe80::...]`) is never accepted: a client that reaches the station
   over IPv6 (its link-local address, section 16) uses a name. Anything
   else on the station interface gets `421 bad_host` (DNS
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
5. **Bodies nobody reads.** A body is never read where nothing takes one:
   the answer goes out with `Connection: close` and the connection ends
   (section 18). A `GET` or `DELETE` route with a body:
   `400 bad_request` ("This request takes no body"). A request no route
   takes, with a body or not, gets the answer it would get without one: an
   `/api/` or `/ws` path that exists for other methods `405`, any other
   `/api/` path `404`, a UI asset `405` for a method other than `GET` and
   `HEAD`, on the AP interface the captive redirect, else `404`
   (`OPTIONS`: always `405`). A body sent with `Transfer-Encoding` (chunked)
   is never read anywhere: on a route that takes a body the answer is
   `411 length_required` (send `Content-Length`), elsewhere the answer
   above, always with `Connection: close`.
6. **Connection.** Connections are persistent (HTTP/1.1). A request with
   `Connection: close`, and every HTTP/1.0 request (also one with
   `Connection: keep-alive`: HTTP/1.0 connections are never kept), gets
   its answer with `Connection: close`, and the station closes the
   connection after it (also after an upload or a download the worker
   answers). Pipelined requests are answered in order. An answer that is
   streamed (JSON larger than the 2 KB response buffer, the upload
   results, and the downloads `/api/logs?format=text`, `/api/coredump`,
   `/api/diag/flash-capture` and `/api/diag/frame-times`) goes to an
   HTTP/1.0 request without `Transfer-Encoding`: with `Content-Length`
   when the size is known up front (the core dump), otherwise the body
   ends when the station closes the connection (section 18). HTTP/1.1 is
   unchanged: such answers go in chunks (`Transfer-Encoding: chunked`) and
   the connection stays open unless the request asked to close it.
7. **Response headers** on every page and API response:
   `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
   `Referrer-Policy: no-referrer`; HTML also gets a Content-Security-Policy
   that allows only the device's own origin (scripts, styles, images,
   connections including the WebSocket) and forbids framing, with one
   exception for images: `https://tile.openstreetmap.org`, the tiles of the
   Receiver page's optional station map (section 8). The page requests tiles
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
output, constant-time compare). A new password gets
`CONFIG_WEB_PBKDF2_ITERATIONS` iterations, 3000: measured at about 1.0 s
per 10 000 on the ESP32-S3, one check takes about 0.3 s (PROVISIONAL, to be
measured again on the station). The count is stored with each hash, so it
can change without a migration: a password set by an earlier build keeps
its count (10 000) until it is changed, and a login never re-hashes it
behind the user's back. The check runs on the server task (section 18).
Passwords are 8-64 bytes of UTF-8 without control characters.

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

**Login throttling** (per client, RAM): a client is its IPv4 address (also
when mapped into IPv6), an IPv6 link-local address (`fe80::/10`) as a whole
(every device on a link shares the `fe80::/64` prefix, so the prefix would
put every phone on the setup AP into one bucket), or another IPv6
address's /64 prefix (one host may use any address of its prefix),
together with the interface it came over (station or AP); the 16 clients
seen last are tracked. A client's first 5
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
password in `PUT /api/admin-password` counts the same way. Each counted
failure is logged with the client's address, at most one line per client
a minute: `wrong admin password from 192.168.1.23 (6 in a row from this
client, +4 not logged)` (the `+k` part only when lines were held back).

**First password ("claim").** `POST /api/setup/admin` works only while no
admin password is stored **and** the request arrived over the device's own
AP (the socket's local address is the AP's IPv4 address, or its IPv6
link-local address for a phone that reaches the AP over IPv6). The AP is up only while
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
| GET | `/api/gnss/apply/preview` | adm | What an apply would send (the held first configuration) | `um98x_ctl_apply_preview` |
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
| GET | `/api/gnss/signal-test` | adm | Signal-group comparison: state and results | `station_sg_test_get` |
| POST | `/api/gnss/signal-test` | adm | Compare two signal groups | `station_sg_test_start` |
| POST | `/api/gnss/signal-test/keep` | adm | Keep one of the two | `station_sg_test_keep` |
| DELETE | `/api/gnss/signal-test` | adm | Stop the comparison | `station_sg_test_stop` |
| GET | `/api/gnss/flash` | adm | Receiver update session, allowed actions | station |
| POST | `/api/gnss/flash/retry` | adm | Flash the same package again | `station_flash_user_action` |
| POST | `/api/gnss/flash/restart-receiver` | adm | Leave the loader, check the version | `station_flash_user_action` |
| POST | `/api/gnss/flash/clear` | adm | Forget the session | `station_flash_user_action` |
| GET | `/api/casters` | adm | All slots: config and status | ntrip_server |
| GET | `/api/casters/{n}` | adm | One slot | ntrip_server |
| PUT | `/api/casters/{n}` | adm | Edit slot n | `ntrip_server_set_config` |
| PUT, DELETE | `/api/casters/{n}/ca` | adm | Store / remove the slot's CA certificate | settings `cstN/ca_pem` |
| POST | `/api/casters/{n}/retry` | adm | Try a stopped (or waiting) slot once now | `ntrip_server_retry_slot` |
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
| GET | `/api/diag/frame-times` | adm | Receiver link frame and drain-pass times, CSV (bench builds) | `gnss_link_bench_timing_csv` |
| PUT | `/api/diag/log-level` | adm | Runtime log level per tag | `diag_set_log_level` |
| GET | `/api/logs` | adm | Log ring (this or previous boot) | `diag_log_read*` |
| GET | `/api/journal` | adm | Event journal | `diag_journal_read` |
| DELETE | `/api/journal` | adm | Clear the event journal | `diag_journal_clear` |
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
| `mdns_host` | the host label the mDNS responder answers, without `.local`: the hostname, or `<hostname>-2` (then `-3`, ...) after the responder renamed itself because another device answered the name; `null` while the responder does not run (and from the goodbye before a planned restart on). The renamed label is never stored: the next boot probes the hostname again. The web page's `_http._tcp` service carries the same label as its instance name and follows a rename; IPv6 (AAAA) queries for the name are answered too (section 16) |
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
               "responding": true, "restarting": false, "identity_confirmed": true, "profile_applied": true,
               "profile_acknowledged": true, "position_locked": true, "position_known": true, "surveying": false, "survey_window_left_s": 0,
               "survey_unfinished": false, "rtcm_age_ms": 180, "best_sys": "galileo", "best_bands": 4, "glonass": true,
               "rtcm1230_supported": true, "journal": "none",
               "uart_closed": false, "rx_maintenance": false, "maintenance_left_s": 0, "maintenance_chosen_s": 0,
               "next_detect_in_s": null, "first_apply_held": false},
  "casters": [
    {"slot": 0, "name": "Onocoy", "empty": false, "enabled": true, "protected": true, "host": "servers.onocoy.com",
     "state": "streaming", "reason": "none", "state_age_s": 3600, "connected_s": 3600,
     "today_online_s": 43200, "today_offline_s": 4, "graced_uptime_permille": 503,
     "streak_ok": true, "graced_uptime_so_far_permille": 1000, "data_rate_permille": 1000, "budget_s": 473,
     "budget_left_s": 469,
     "onocoy_ready": true, "bytes_sent": 74000000, "lag_ms_now": 40, "next_retry_s": 0, "unreached_s": 0, "hint": "",
     "stop_reason": "none"}
  ],
  "health": {"level": "degraded", "last_step": "none", "step": "none", "step_phase": "none", "ladder_suspended": false,
             "factory_reset_refused": false, "recovery_mode": "full", "reboot_recheck": false,
             "reboot_hold": [], "reboot_hold_left_s": null, "reboot_cap_wait_s": null, "reboot_cap_limit": null},
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
| `receiver.uart_closed` | the receiver UART is closed and `gnss_link` reopens it on its own (the UI's "Receiver port closed"; never in safe mode, where the link is not opened) |
| `receiver.rx_maintenance` | receiver maintenance is on (section 8): nothing is sent to the receiver; `maintenance_left_s` until it ends by itself, `maintenance_chosen_s` the chosen duration with extensions (both 0 when off) |
| `receiver.position_known` | the station has a base position, locked or surveyed so far (`position.source` in `GET /api/gnss` is not `none`); admin form only |
| `receiver.surveying`, `survey_window_left_s`, `survey_unfinished` | as `survey.active`, `survey.window_left_s` and `survey.unfinished` in `GET /api/gnss`; admin form only |
| `receiver.rtcm_age_ms` | since the last CRC-valid RTCM frame (`gnss_link`), `null` = never |
| `receiver.glonass` | the receiver's stream carried GLONASS signals in the last 10 s (the signal summary of `GET /api/gnss/signals`; `false` before the first summary); admin form only |
| `receiver.rtcm1230_supported` | as in `GET /api/gnss/signals`: the receiver accepted message 1230 (GLONASS biases) at the last configuration, `false` refused, `null` never configured. With `glonass` and `best_bands` it says whether the stream meets Onocoy's quality minimums (4 bands on one constellation, GLONASS with 1230) without polling the larger signals object; admin form only |
| `receiver.responding` | the receiver answers: it streams (a CRC-valid RTCM frame within the last 10 s), or it sent a checksum-valid line (a reply to a command, a log, an NMEA sentence) within the last 90 s and after its last RTCM frame, more than 1 s after it (any line, with none this boot); `false` while the station finds no receiver (`no_receiver`), and a line from before a detection pass that found nothing does not count. A receiver that sends no RTCM (none this boot, or none for 3 s) is asked (a version query without a lease, nothing paused, nothing configured; never in receiver maintenance): at once when nothing came since its last RTCM frame, then whenever no line came for 60 s. So one that answers when asked and sends nothing unasked (no antenna, no output configured, its RTCM stopped) stays `true`, and one that hung or was unplugged turns `false` 90 s after its last answer, in `alerts_only` too. Read at the time of the request, by the same rule as `responding` in `GET /api/gnss`, so the two agree; the same evidence as health's "answered after its last RTCM frame" (`no_rtcm`). Admin form only |
| `receiver.restarting` | the receiver said it is restarting (it answered a command with `The Board is resetting,do not response any command!`) within the last 20 s and its restart banner has not come yet: it answers, it is not silent; admin form only (also in `GET /api/gnss`) |
| `receiver.journal` | `none`, `running`, `waiting`, `unreadable`, `unknown` (section 9) |
| `receiver.first_apply_held` | the receiver's first configuration waits for the user (`gnss/hold`, section 8): only queries reach the receiver until the user applies; admin form only |
| `receiver.next_detect_in_s` | no receiver found, or the station looking for it (`no_receiver`, `detecting`): seconds until the next detection pass, `0` while one runs; `null` in every other state. Read at the time of the request (a pass can take most of a minute) |
| `receiver.signal_test` | the signal-group comparison (section 8) in short: `null` when none runs and none ended in the last 24 h of this boot; else `{"state", "phase", "group", "left_s", "decide_left_s", "outcome"}`: `state` as in `GET /api/gnss/signal-test`, `phase` 0 or 1 while switching, settling or measuring (else `null`), `group` the group being switched to, measured or (deciding) running, or the one restored to (`{"master", "slave"}`, else `null`), `left_s` the time left in the phase rounded up to 10 s, `decide_left_s` the time left to choose rounded up to a minute (`null` outside those states; rounded so the section changes rarely), `outcome` once decided (`null` while running). Admin form only |
| `casters[]` | every slot; the full status is in `/api/casters`. `empty`: the slot has no host and is not enabled (never set up, or removed); its `name` is then `""` whatever the stored name is. In safe mode (configs not loaded) no slot is empty and every `name` is `null`. `today_online_s` and `today_offline_s` are the availability ledger's (kept across restarts, section 10). Onocoy day figures (`graced_uptime_permille`, `streak_ok`, `graced_uptime_so_far_permille`, `data_rate_permille`, `budget_s`, `budget_left_s`, section 10) are `null` for other slots, and for the protected slot too while it is disabled or not set up (`onocoy_ready` false); `onocoy_ready` is true for an Onocoy slot with the console's mountpoint, username and password; `unreached_s` = the current run of failures that never reached the caster (DNS, connect timed out or failed locally; 0 = none); `stop_reason` why the Onocoy login guard stopped the slot (`ntrip_stop_t`; `none` unless `state` is `stopped`, section 10) |
| `alerts[]` | health's alerts (`source: "health"`, `health_alert_t`) and the station's (`source: "station"`, `station_alert_t`). A `failed` health level always comes with the alert that says why (below); `config_failed` carries `retry_in_s` and `auto_retry` (as `config_retry_in_s` and `config_auto_retry` in `GET /api/gnss`); `no_receiver` (the station found no receiver: raised in `no_receiver`, not with `receiver_in_loader` or in receiver maintenance, cleared when the state ends) carries `saw_bytes` (the last detection pass received bytes it could not read: wiring or port speed rather than power) and `next_detect_in_s` (as `receiver.next_detect_in_s`); `fixed_far` (the fixed base position looks wrong: more than 50 m from the receiver's own solution, horizontally or in height, or its height is the receiver's height above sea level: section 8, `fixed_check`) carries `horizontal_m`, `vertical_m`, `distance_m`, `height_only` and `sea_level` as `fixed_check` does, in whole metres |
| `availability.budget_left_s` | protected slot's data-rate-adjusted `budget_left_s`, else `ledger_protected_budget_left_s()`; `null` without an enabled protected slot that is set up (`onocoy_ready`), or without a trusted clock. Information only, never a gate |
| health alerts that say why there is no RTCM | set from the GNSS ladder's evidence while it is in its last step (`failed`), or while a stall is only reported (`alerts_only`, `degraded`; also the grace of a step the station ran before the user chose `alerts_only`); `no_satellites` and `no_rtcm` also while the recovery runs (`recovering`: a step in progress, a step whose grace waits while a session of the station's own runs, or between two of its steps), as soon as the receiver answered after its last RTCM frame within the last 90 s (as `receiver.responding` reads it), so the antenna hint comes with the recovery; a session of the station's own (an apply, the applied check's, one after a receiver restart) never drops the level or the reason meanwhile: `receiver_power` (bytes but no answer after the receiver reset: power-cycle the station), `receiver_silent` (nothing usable from the receiver and no answer to commands: power-cycle the station), `no_satellites` (the receiver answers, and its own position sentence says no fix with no satellites used: check that the antenna is on the socket marked M and sees the sky), `no_rtcm` (the receiver answers, by text lines or replies to the station, but sends no RTCM: most often the antenna; only while its last answer is at most 90 s old: older, it is `receiver_silent` in the last step and in `alerts_only`, and no reason while a step runs), `rx_held` (the first configuration is held, `first_apply_held`, and the receiver answers: no RTCM is expected yet; level `degraded`, never `failed`, and the ladder declares no stall for it). None of them while the station finds no receiver: its own `no_receiver` alert says that. With `no_satellites` or `no_rtcm` the ladder does not reset the receiver again on its 10-min retry pass (a silent receiver gets the link restart and the rescan only); the same after a receiver reset that brought RTCM back only briefly (the stall within 60 s of it) when the receiver answers |
| `health.step`, `health.step_phase` | what the GNSS recovery does now (`health_step_t`, `health_step_phase_t`): `step` is the step that is due or that the station runs (`step_phase` `running`), or the one it ran and whose grace runs, the station waiting for RTCM (`waiting`; also while a session of the station's own runs meanwhile: the grace waits for it with the time it had left, and the step is never run again for it, `ladder_suspended` `true` meanwhile); `reboot` while the station restarts itself for the receiver. `none` (both) outside a step, in the ladder's last step (`failed`), and in `alerts_only`, where nothing is fixed (a step the station ran before the user chose it is only watched: `none`, level `degraded`). `last_step` keeps the last step started; admin form only |
| `health.factory_reset_refused` | a BOOT-button factory reset was refused within the last 10 min because a receiver update was running (the UI shows a notice); admin form only |
| `health.recovery_mode` | automatic receiver recovery in effect (`gnss/recovery`, `health_recovery_mode_t`): `full`, `no_reboot` (everything but the ESP32 reboot), `alerts_only` (nothing is sent to the receiver: a stall is only reported and journalled, level `degraded`, `failed` once the ladder gives up); admin form only |
| `health.reboot_recheck` | no receiver found and an ESP32 reboot was blocked or held: it is looked at again after the next detection pass, when the reboot cap allows and nothing holds it; admin form only |
| `health.reboot_hold` | why the GNSS ladder's ESP32 restart is postponed right now (`[]` = nothing): `setup` (first setup not finished and no caster enabled; no time limit), `ap_client` (someone is connected to the station's access point), `admin` (an admin session made a request or sent a WebSocket ping in the last 5 min), `power_on` (the first 60 min of station time since power-on while nothing valid was heard from the receiver in this boot). Reported only during a ladder episode (a stall or no receiver, until the ladder resets) in full recovery (`recovery_mode` `full`), where a restart may be pending: `[]` otherwise, whatever holds (`no_reboot` and `alerts_only` never restart the ESP32 for the receiver). Not while the receiver answers either (the `no_rtcm` or `no_satellites` alert): the ladder restarts the ESP32 only for a receiver that sent nothing at all in the failed step's window (after the receiver reset: no answer to it), so nothing is pending until the evidence says it stopped answering (a rescan that finds nothing, a reset with no answer, or no answer for 90 s). A held restart is never queued: it is journalled once (section 14) and looked at again later, like one the cap blocks; admin form only |
| `health.reboot_hold_left_s` | seconds until every reason in `reboot_hold` has ended at the latest (`ap_client` and `admin`: the 30 min limit on both together per ladder episode; `power_on`: the end of its 60 min), so a restart still needed then runs, unless the reboot cap still holds then: the restart waits for the later of `reboot_hold_left_s` and `reboot_cap_wait_s`; `null` when nothing holds or `setup` holds (no time limit; the cap may still apply after it) |
| `health.reboot_cap_wait_s` | seconds until the reboot cap (60 min apart, at most 3 in 24 h) allows a ladder restart; `null` when it allows one now, outside a ladder episode, while the receiver answers (as `reboot_hold`), and when `recovery_mode` is not `full` |
| `health.reboot_cap_limit` | which limit of the cap sets `reboot_cap_wait_s`: `hour` (the last ladder restart was less than 60 min ago) or `day` (3 in the last 24 h); the later one when both hold; `null` when `reboot_cap_wait_s` is `null` |
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

The public form lists only slots that are not empty, without the `empty`
field. A slot that becomes empty leaves it: public WebSocket clients get a
full status then, since a delta can only add or replace slots.

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

**Reconnecting (the UI).** After a close, a refused handshake or 5 s of
silence the page waits 1 s before the next handshake, doubling up to 15 s,
each wait drawn within ±20 % so pages that lost the station together do not
come back in the same second; only a connection that lived 10 s starts the
wait over. A refused or evicted page so makes at most 8 handshakes in its
first minute and then one every 12 to 18 s, never one a second. It asks
`GET /api/session` once per run of refusals (and again on a `1008`): a page
whose session is gone shows the login, and a public view whose public
status was turned off shows the login and stops (no more handshakes or
polls). While the socket is down the page polls `GET /api/status` every
2 s; a hidden tab with a session neither polls nor reconnects until it is
shown again.

**Events:** `station_state` (`state`), `caster_state` (`slot`, `state`),
`update_state` (`state`), `network` (`what`: `online`, `offline`, `ap_up`,
`ap_down`, `time_synced`), `scan_done`, `settings_changed` (`ns`, `client`;
advisory: reload that page's data), `receiver_restarted`, `rebooting`
(`reason`; sent from `LIFECYCLE_EVENT_PRE_REBOOT`, then the server closes
with `1012`).

`settings_changed` comes only when a setting changed: one a person set
(any page, the API, the BOOT button's password reset) or one the station
changed itself (the receiver profile at a position lock or a signal-group
switch, the held first configuration's settings). Device state the station
stores on its own (the availability ledger every minute, boot history, the
detected receiver, the applied state, update checks, journals, backoffs)
sends none: it is in the status stream and the pages' own reads.

`settings_changed.client` is the `X-Client-Id` request header (a random id
per tab, `[A-Za-z0-9_-]`, at most 32 characters; the UI sends it with every
request that is not a `GET`) of the request during which, or within 2 s
after which, the settings changed; `null` when no such request was running.
A page that finds its own id ignores the event as a warning (it made the
change) and only reloads its data when no form is being edited. One change
can come as a few events in a row (a batch, then a key erased): the UI
waits 300 ms for the rest and reloads, or warns, once.

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
  "responding": true, "restarting": false, "identity_confirmed": true,
  "alerts": [],
  "profile_applied": true, "profile_acknowledged": true, "rtcm_differs": false, "rtcm1230_supported": true,
  "config_failed": false, "config_retry_in_s": 0, "config_auto_retry": true, "reconfigure_refused": false,
  "detect_failures": 0,
  "link": {"baud": 115200, "mode": "stream", "rx_bytes": 123456789, "rx_age_ms": 12, "rtcm_age_ms": 180,
           "open": true, "open_failures": 0, "hands_off": false},
  "autocfg": true,
  "recovery": "full",
  "first_apply_held": false,
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
  "rx_signal_group": {"master": 3, "slave": 6},
  "rx_bands": {"state": "ok", "age_s": 3600, "read_unix": 1760000000, "group": 3, "profile_master": 3, "matches": true,
               "systems": [{"sys": "gps", "signals": ["L1C/A", "L2C", "L2P", "L5"], "bands": 3, "missing": []},
                           {"sys": "galileo", "signals": ["E1", "E5a", "E5b", "E6"], "bands": 4, "missing": []}],
               "reply": null},
  "signal_group_refused": null,
  "position": {"locked": true, "source": "survey", "lat_deg": 45.123456789, "lon_deg": -75.123456789,
               "height_m": 85.1234, "sigma_m": 0.0, "locked_unix": 1759000000},
  "survey": {"active": false, "window_left_s": 0, "unfinished": false},
  "fixed_check": {"state": "ok", "horizontal_m": 2, "vertical_m": -3, "distance_m": 4, "height_only": false,
                  "sea_level": false, "checked_age_s": 1200, "limit_m": 50},
  "arp": {"valid": true, "station_id": 1234, "ecef_m": [918129.1234, -4346071.2345, 4561977.3456],
          "lat_deg": 45.123456789, "lon_deg": -75.123456789, "height_m": 85.1251,
          "age_ms": 800, "offset_m": 0.0021}
}
```

| Field | Notes |
|---|---|
| `alerts` | station alerts only (`station_alert_t`): `unsupported_model`, `config_failed`, `position_mismatch`, `antenna_moved`, `receiver_attention`, `receiver_in_loader`, `no_receiver`, `signals_mismatch` (the signals the receiver reported for its group at the last configuration are not what the profile's group should give: `rx_bands.matches` false; cleared by a report that matches or by one that is not read), `fixed_far` (`fixed_check.state` `far`) |
| `responding` | the receiver answers, as `receiver.responding` in `GET /api/status` (one rule, read at the time of the request): it streams, or it sent a checksum-valid line within the last 90 s and after its last RTCM frame (the station asks a receiver that sends no RTCM once a minute while nothing comes); never while no receiver is found |
| `restarting` | the receiver answered a command with its restarting notice (`The Board is resetting,do not response any command!`, no leading `$`) within the last 20 s and its restart banner has not come yet: it answers, it is not silent. An unasked restart (no signal-group change or RESET of the station's own) counts as restart evidence once, like the banner that ends it |
| `config_failed`, `config_retry_in_s`, `config_auto_retry` | the last profile apply failed (`config_failed` alert). `config_auto_retry`: the station retries it on its own on its backoff (10 min, 30 min, 2 h, then every 6 h), which it does only with `autocfg` on and for a model it configures; `config_retry_in_s` is the time to that retry (0 when none is due, and always 0 when `config_auto_retry` is false). With `autocfg` off (the first-apply hold included) nothing is retried until the user applies again |
| `rtcm_differs` | the latest observation window saw RTCM flowing that differs from the profile: a message it checks was missing or came at another rate while others arrived. Never set without any RTCM (no antenna, a silent receiver: `no_rtcm`, `receiver_silent` or `no_satellites` say why); cleared by a window that passes, a window without RTCM and whenever the station starts watching again (a receiver restart, detection, a successful apply). Health raises `profile_drift` after 90 s of it |
| `profile_applied` | the applied check passed: the receiver acknowledged this profile on this firmware at its last apply (the stored applied state) and an observation window saw the profile's RTCM. A receiver whose applied state differs is configured at once (`autocfg` on). Without any RTCM (no antenna, a silent receiver) it stays `false` and the check waits for the first window with RTCM; nothing is re-applied for a window without RTCM, which could not show whether a re-apply worked (the stall is the recovery's, whose reconfigure step re-applies once under the user's recovery mode) |
| `profile_acknowledged` | the receiver acknowledged this profile on the firmware it runs at its last apply: the stored applied state matches (the profile's hash, the firmware, the message 1230 answer stored), confirmed by this boot's version query or by an apply that worked since. `profile_applied` adds the check against the stream, so `true` with `profile_applied` `false` means applied but not checked yet (no RTCM: no antenna; or the first window still runs). `false` while the identity is not confirmed, while the station looks for the receiver or updates it, for a model the station does not configure, and from a saved profile that differs until it is applied (the user's, or the station's own for a lock, a re-survey or a signal-group switch, also when that apply fails). Also in `GET /api/status` (`receiver.profile_acknowledged`) |
| `rtcm1230_supported` | `gnss/rtcm1230_ok`: `true` the receiver accepted message 1230 at the last apply, `false` it refused it, `null` never applied (the key is not stored: "not applied yet") |
| `link.mode` | `off`, `stream`, `command`, `raw` (`gnss_link`) |
| `link.rx_bytes` | bytes received from the receiver since boot (0 = nothing arrived yet) |
| `link.open`, `open_failures` | the UART driver is installed; failed installs since boot. `open: false` with a mode other than `off` = the UI's "Receiver port closed": `gnss_link` reopens it on its own |
| `link.hands_off` | the link refuses every write and lease (receiver maintenance) |
| `recovery` | `gnss/recovery` as stored, `health_recovery_mode_t` (a stored value out of range reads as `full`, as health reads it) |
| `first_apply_held` | `gnss/hold` is stored: the receiver's first configuration is held until the user applies. A build with `CONFIG_STATION_HOLD_FIRST_APPLY` (bench builds, the `sdkconfig.hold` overlay) writes it once at boot on a device whose profile was never applied and whose `gnss/autocfg` was never set, together with `gnss/autocfg` = 0 and `gnss/recovery` = 2 (`alerts_only`, if unset), before anything reaches the receiver; it stays in NVS whatever build runs later. While it is stored the station sends the receiver only queries (version, configuration dump, base mode, its own position) and a loader prompt met during detection gets no restart key (`receiver_in_loader`, listen-only; the user's receiver restart still works). The first successful apply erases it, and so does `PUT /api/gnss/profile` with `autocfg: true`; a factory reset erases it with the other user settings. When the hold is due but cannot be stored (settings unusable this boot, or the write failed), the station holds the first configuration for this boot only, as the batch would have stored it: `first_apply_held` true, `autocfg` false, nothing stored. A successful apply ends the hold (automatic configuration stays off for the boot); `autocfg: true` ends both. `GET /api/gnss/apply/preview` lists what the apply would send |
| `profile.antenna_*` | the base antenna descriptor the receiver publishes in 1033: `antenna_model` the IGS antenna name with its radome (`""` = leave the receiver's own value, the default), `antenna_serial`, `antenna_setup_id` 0-255; serial and setup ID only matter with a model (stored cleared without one). UM982 build 25164 stores the name with every run of spaces collapsed to one: the IGS form `TRM59800.00     SCIS` (the name padded to 16 characters, then the radome) reads back as `TRM59800.00 SCIS` (our bench, 2026-10-14). The station therefore compares names (and serial numbers) with runs of spaces read as one and leading and trailing spaces dropped, so a receiver that holds the collapsed form is not sent the antenna again on every apply. The 1033 the receiver then sends may carry the collapsed name rather than the IGS padded form, which rovers and casters look up by; `rtcm1033.antenna` shows what it sends (to be checked with sky before a real antenna name goes to a caster) |
| `rx_settings` | what the receiver's configuration dump said at the last apply session this boot (`station_status_t.rx_settings`): `valid` false and every value unknown (`unknown` / `null`) after a reboot until the next apply. `b1cb2a`, `mmp` (`um98x_setting_t`): BeiDou B1C/B2a observations in RTCM, multipath mitigation; `mask_deg` the elevation mask (0.1°); `antenna` the descriptor the receiver holds, as it reads it back (`null` = not read): when the session sent the antenna, the dump is read again right after that step, so this is the receiver's own form (collapsed spaces on build 25164), never the profile's text; `null` if that read failed. `age_s` since the read-back, `read_unix` its time (`null` without a clock then). MMP and the mask are shown only, never sent (PROVISIONAL, to be confirmed on hardware) |
| `apply_steps` | outcome of the antenna and B1C/B2a steps of the last apply session (sent only when the read-back asked for them; a refusal does not fail the session): `sent`, `ok` (accepted), `reply` the receiver's refusal text or `"timeout"` (`null` unless refused). The UI words an antenna refusal "antenna model rejected: <reply>" |
| `rtcm1033` | the latest CRC-valid 1033 the receiver published (`gnss_obs_get_1033`), strings as received; `{"valid": false, "bad_frames": n}` before the first that decoded; `bad_frames` counts 1033 frames that did not decode since boot |
| `maintenance` | receiver maintenance (below): `left_s`, `chosen_s` 0 when off; `choices_s` the durations offered, `max_left_s` the most time left after extensions |
| `rx_signal_group` | the signal group the receiver runs as its last configuration read-back this boot showed (`CONFIG SIGNALGROUP`; after a change, the new one); `null` before an apply session read it |
| `rx_bands` | what the receiver's running signal group configures on its main antenna input, from the last apply session this boot: every session asks once, after its signal-group step (`ANT1GROUPA`, one-shot without a rate; it answers without sky). `state` (`um98x_bands_state_t`): `unknown` (not asked this boot, or no answer; `age_s` `null`), `ok`, `refused` (the receiver refused the query: a firmware without it; `reply` its text; never a failed apply), `unread` (an answer this firmware does not read). With `ok`: `group` as the receiver named it, `profile_master` the profile's main-antenna group it was compared with, `matches` (the report names that group and lists every signal it should configure; extra signals are fine) and `systems` per constellation (`gps`, `glonass`, `galileo`, `beidou`, `qzss`): `signals` configured, `bands` the frequency bands they cover (L1, L2, L5, L6 buckets, as `GET /api/gnss/signals` counts them), `missing` the signals the profile's group should give and the report lacks. What a group should give (main input, UM982 build 25164, our bench 2026-10-14; the manual's table): `4` GPS L1C/A L2C L2P L5, GLONASS G1 G2, Galileo E1 E5a E5b, BeiDou B1I B2I B3I, QZSS L1C/A L2C L5; `3` the same with Galileo E6 and QZSS L6, BeiDou B1I B3I B1C B2b; `7` the same as `4` with every BeiDou signal (B1C, B2a, B2b added); `5` no L5 at all. Other groups: only the number is compared. The log's whole form (header and checksum) is confirmed on build 25164: the station read it in a real apply on 2026-10-14 (`state` `ok`, `group` 3, `matches` true) |
| `signal_group_refused` | the receiver refused a signal group (any apply, the comparison's switches included): `{"master", "slave", "reply", "back_to"}` with the receiver's reply (UM982 build 25164: `PARSING FAILED GRAMMAR ERROR,` for every pair but `4 5`, `3 6`, `5 0` and `7 0`; `"signal group rejected"` when it acknowledged the pair but its read-back after the restart still showed another group) and the group the stored profile went back to (`{"master", "slave"}`: the group the session read back before the change, else the one of the last successful apply, `gnss/sg_ok`). The station re-applies that group at once; a refused group never raises `config_failed` and is never retried on the backoff. A plain refusal (a negative reply: nothing changed on the receiver and it did not restart) with such a group to go back to is not rolled back with `RESET`: the re-apply restores the outputs the session quieted, so the receiver is not restarted (nor its survey) for it. `back_to` `null`: neither was known, and the refusal is an ordinary failed apply (`config_failed`, rolled back with `RESET`). Kept until the next `PUT /api/gnss/profile` with a profile; `null` otherwise. The UI says "The receiver refused signal group 7 1. It is back on 3 6 (Galileo E6) and configured again. Choose a group from the list and save." with "Receiver reply: <reply>" under it |
| `profile.fixed` | `{lat_deg, lon_deg, height_m}` in `fixed` mode, else `null`; fields a mode does not use are ignored on input and come back cleared (`survey_in_s` 0 in `fixed` mode) |
| `position` | locked or last surveyed (`station_status_t.position`); `source`: `none`, `survey`, `averaged`, `user`; `sigma_m` 0 = unknown |
| `survey` | `active`: inside the survey window, the survey's length plus 120 s from its start (the receiver acknowledged the base mode, restarted, or was found surveying at boot); `window_left_s` until it ends (0 outside). The window times the station's base-mode read-back (every 60 s), the station messages (1005/1006/1033) the observed set may miss, and the survey rule of the recovery (`GET /api/health`). `unfinished`: the survey started and has not finished (the read-back no longer reports a survey, or its time is up and 60 consecutive 1005 frames lie within 1 mm): the receiver may still be surveying, inside the window and after it, since a survey makes no progress without satellites (no antenna). The station locks it once it finishes. The UI says "Surveying, paused while no corrections arrive." for an unfinished survey without corrections |
| `arp` | latest CRC-valid 1005/1006 (`gnss_obs_get_arp`), converted to WGS84; `offset_m` = 3-D distance to the locked position, `null` when not locked; `{"valid": false}` before the first frame |
| `fixed_check` | is the fixed position (typed in or locked) where the receiver is? A fixed base sends the position it was given in every 1005, so the stream cannot tell; the receiver's own single-point solution can. With a fixed profile and a model the station configures, the station asks it without a lease (`BASEPOSA`, the base's position as a fixed base, or `SPPNAVA` when the receiver refuses that, only echoes the set position or gives nothing usable three times; one-shot, one line, the casters never wait): 60 s after streaming began, a receiver restart or an apply, then every 20 s until three usable solutions (computed, single-point, at least 6 satellites, standard deviations at most 5 m horizontally and 10 m in height) agree, at most 15 tries; again every hour. `state` (`station_fix_state_t`): `none` (the profile surveys, or not a model the station configures), `checking` (no verdict yet), `ok` (within 50 m horizontally and vertically, and not a sea-level height), `far` (the position looks wrong: beyond the limit in all three, or `sea_level`: the `fixed_far` alert, journalled; it clears once three solutions are within 40 m, or at once when the fixed position changes; streaming is never held back), `no_solution` (no verdict: the last check got no usable solution, for example without sky), `refused` (no verdict: the receiver refused both logs). With `ok` and `far`: `horizontal_m`, `vertical_m` (the fixed height minus the receiver's ellipsoidal height, its height above sea level plus the geoid separation it reports: > 0 the fixed position is above) and `distance_m` (3-D), the medians of the three solutions that decided (else `null`); `height_only` with `far`: only the height is wrong (beyond the limit, or `sea_level`). `sea_level` with `far`: the fixed height is the receiver's height above sea level, not its ellipsoidal height, a common mistake that is under 50 m off in most places: the medians put the fixed height within 5 m of the receiver's height above sea level (as it prints it, so the separation's sign plays no part) and more than 10 m from its ellipsoidal height, and more than four times the height deviation the solutions report (their median), with the horizontal distance within the limit. 5 m is twice the receiver's vertical RMS (three solutions 20 s apart share most of their error, so their median scatters about as much as one); 10 m is four times that RMS and the largest height deviation a counted solution may report, and where the separation is about 10 m or less the two heights are too close to tell apart (only the 50 m limit applies). The solutions' own deviation keeps a right height that a poor sky puts 10 to 20 m off from reading as a sea-level height where the separation is about that: a solution that reports 8 m needs the fixed height more than 32 m off. Once raised it holds within 10 m of the sea-level height and beyond 5 m from the ellipsoidal one, so an hourly check does not flip it. The UI says "The fixed height looks like a height above sea level. Enter the height above the ellipsoid." `checked_age_s` since the latest usable solution (`null` = none this boot); `limit_m` the limit (50). The receiver's single-point accuracy is 1.5 m horizontal and 2.5 m vertical (RMS), so 50 m is far beyond its own scatter and well inside the about 300 m at which a receiver stops its corrections (`antenna_moved`). PROVISIONAL, to be confirmed on hardware: that `BASEPOSA` reports the receiver's own solution while it works as a fixed base |

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

`autocfg: true` also ends the first-apply hold (`gnss/hold` erased, see
`first_apply_held`): the station configures the receiver as usual from
then on.

The UI's Receiver settings show `autocfg` and `recovery` as one choice,
"Receiver management": "The station" (`true`, `full`), "The station, but
never restart itself" (`true`, `no_reboot`), "Me (alerts only)" (`false`,
`alerts_only`); any other pair shows as "Custom" with the nearest choice
offered. The Status page (with a Change link to that card) and Logs show it
as "Receiver management" from the station's side: "The station", "The
station, no restarts", "You (alerts only)" (Logs: "Paused" while the
station holds the recovery). A
save sends only the members that change, one per request since they are
stored one after the other: `recovery` first when `autocfg` goes off, else
`autocfg` first, so a refusal halfway never leaves automatic recovery on
for a receiver the station does not configure. The Position card ("Save
position") and the Receiver profile card ("Save profile") each send the
whole profile: their own fields over the profile as last read. Both read
"Save and apply" while `autocfg` is off (outside the first-apply hold), as
a changed profile is applied all the same. Each button shows once its own
card changed, so an edit in the other card stays on the page with its
button.

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
The UI's hint: only for an antenna in the IGS list, its name and radome as
listed; empty keeps the receiver's value; `ADVNULLANTENNA` (the null-antenna
name) clears a model the receiver already holds.

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
alone changes nothing on the receiver), `409 signal_test_running` (a
signal-group comparison runs, for a body with `profile` or `autocfg`),
`409 settings_unavailable`. A body with `profile` clears
`signal_group_refused`.

### `POST /api/gnss/apply` (adm)

Empty body. `202 {"accepted": true}`. Errors: `409 journal_open`,
`409 signal_test_running`, `409 safe_mode`, `409 receiver_maintenance`
(nothing is sent to the receiver then), `409 unsupported_model`. With the first configuration
held (`first_apply_held`) this is the user's confirmation: the apply runs
once the receiver streams, and its success erases `gnss/hold`
(`gnss/autocfg` and `gnss/recovery` stay as they are; the UI turns them on
afterwards when the user asked for it).

### `GET /api/gnss/apply/preview` (adm)

What an apply session would send to the receiver, in order, for the user
to confirm the held first configuration. Static: it is built from the
stored profile (the default one when none is stored) and the link's
current baud, without a read-back (the conditional steps depend on one,
and reading it needs a configuring session: `UNLOG COM1` is itself a
write). Nothing is sent.

```json
{"steps": [{"text": "UNLOG COM1", "kind": "write", "when": "always", "saved": false},
           {"text": "VERSIONA", "kind": "query", "when": "always", "saved": false},
           {"text": "CONFIG", "kind": "query", "when": "always", "saved": false},
           {"text": "MODE", "kind": "query", "when": "always", "saved": false},
           {"text": "CONFIG SIGNALGROUP 3 6", "kind": "write", "when": "if_differs", "saved": true},
           {"text": "ANT1GROUPA", "kind": "query", "when": "always", "saved": false},
           {"text": "MODE BASE 1234 TIME 600 0", "kind": "write", "when": "if_differs", "saved": false},
           {"text": "CONFIG RTCMB1CB2A ENABLE", "kind": "write", "when": "if_off", "saved": false},
           {"text": "RTCM1005 COM1 1", "kind": "write", "when": "always", "saved": false},
           {"text": "SAVECONFIG", "kind": "write", "when": "always", "saved": true}],
 "saveconfig": true}
```

| Field | Notes |
|---|---|
| `steps[].text` | the command as the session writes it (the plan builder's own text); `ANT1GROUPA` is the signal-group query after the signal-group step (`rx_bands`), sent in every session; `RTCM<msg> COM1 <period>` once per profile message, `CONFIG COM1 <baud>` only when the profile's baud is not the link's, `CONFIG BASEANTENNAMODEL ...` only with an antenna model, followed by the dump query `CONFIG` again (`if_differs`: only when the antenna was sent and accepted; the name as the receiver stores it, for `rx_settings.antenna`), `GPGSV COM1 10` only with satellite-in-view output in the profile; a fixed position is sent as ECEF with 4 decimals |
| `steps[].kind` | `um98x_preview_kind_t`: `query` (reads the receiver), `write` (changes its running configuration) |
| `steps[].when` | `um98x_preview_when_t`: `always`; `if_differs` (only when the read-back differs from the profile); `if_off` (only when the read-back shows the setting off) |
| `steps[].saved` | the command saves to the receiver's stored configuration by itself: `CONFIG SIGNALGROUP` (the receiver saves it and restarts) and `SAVECONFIG` |
| `saveconfig` | the list ends with `SAVECONFIG`, which saves the receiver's whole running configuration (also whatever was changed on it from elsewhere) |

Errors: `409 safe_mode`.

### `POST /api/gnss/position/lock` (adm)

Empty body: lock the latest surveyed position (the station switches the
profile to `fixed` and reconfigures). `202 {"accepted": true}`. Errors:
`409 no_position_yet` (no 1005 yet), `409 unsupported_model`,
`409 journal_open`, `409 signal_test_running`, `409 safe_mode`,
`409 receiver_maintenance`.

### `DELETE /api/gnss/position/lock` (adm)

Forget the locked position and survey again. The UI warns first: the
position sent to casters changes, and Onocoy may validate the station
again. `202 {"accepted": true}`. Errors as for apply.

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

`best_sys` and `best_bands`: the constellation with the most bands among
those Onocoy rewards (GPS, GLONASS, Galileo and BeiDou; the first in
message order on a tie), `null` and 0 when none of them is present. QZSS
and NavIC are listed with their bands but never count as best, so a
4-band QZSS cannot hide a degraded rewarded constellation.

`systems` lists only present constellations. Bands follow one table (the
firmware's `k_band_sigs`): NavIC signal 22 (L5 SPS) is band `l5`. SBAS is
never listed: its message numbers 1101-1107 are ones the receiver reuses
for its own messages, kept out of the hub and of this view. `signal_ids` are the MSM
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
`sat_view_output` is off) or `no_data` (it is on, but nothing arrived in
the last 30 s). The UI draws its sky from the live view (`GET
/api/gnss/live`), not from this endpoint.

### Receiver maintenance

"Hands off" while the user works on the receiver through its other port (a
PC tool, a reflash): nothing is sent to the receiver (no detection, query,
apply, `RESET`, flash, ladder step or live-view toggle), reception goes on
and every CRC-valid RTCM frame still reaches the casters. The station
reports `idle` meanwhile (`rx_maintenance` tells it apart); the ledger
counts the time as planned. It ends by itself when its time is up, on "end
now", with a power cut or a factory reset. Any other ESP32 reset keeps it
with its time left, the boot only listening: a software restart, a crash or
a watchdog (kept in the ESP32's RTC memory, exact), and a reset by the
ESP32's EN pin (the USB auto-reset that opening the ESP32's port or
flashing can cause, the RST button), which reads as power-on and loses the
RTC memory while the receiver keeps running and may be mid-work (kept as a
copy in settings, stored at the start, at each extension and every minute:
up to 70 s less time left then). The time across the reset comes off: it
never ends later than the time chosen (with the trusted clock, also when
the ESP32 was held in reset for a while) and ends at once when that time
ran out meanwhile. A power-on or brownout reset during which the receiver
restarted too (`station.rx_boot` `restarted` in `GET /api/health`) lost the
power for both: maintenance resumed at that boot ends then, unless the user
extended it since; `kept_running` or `unknown` leave it on (a brownout keeps
the RTC memory and its timer: it resumes from there, exact). Any other restart banner does not
end it (the work may restart the receiver). The end runs the station's
stream-first check, as at boot. Entry and exit are journalled
(`rx_maintenance`; an exit also when its time ran out across a reset).

Nothing is checked during maintenance, so the receiver's fields keep what
the station knew at entry. The copy in settings carries that too, and a
maintenance resumed after a reset (either way) reports it until it ends, as
one without a reset does: in `GET /api/status` and `GET /api/gnss` the
receiver's identity (`model`, `firmware`, `receiver`, `identity_confirmed`;
the receiver's own word wins if it repeats it) and the signal group it runs
(`rx_signal_group`), and when the profile is
still the one it was about, `profile_applied`, `profile_acknowledged`, `fixed_check` and the
station's alerts (`config_failed` with them); `surveying`,
`survey_window_left_s`, `survey_unfinished` and `survey` run on with the survey window, its end
as far from maintenance's end as before (so the reset's time comes off both
alike), and after maintenance the station goes on with that window instead
of starting a new one. The rest of what an apply session read back
(`rx_settings`, `rx_bands`, `apply_steps`) stays this boot's: unknown until
the next configuration, as after any reset. Without that knowledge (a
copy from an older firmware, settings unusable) `identity_confirmed` is
false during maintenance and the UI reads such rows as "Not checked during
maintenance" instead of "not yet" (the station will not look before the
end); a fixed-position check without a verdict reads so in any
maintenance.
Whether input on the link can disturb a receiver worked on through its
other port is PROVISIONAL, to be confirmed on hardware. The UI starts it in
Settings, next to the receiver update; while it is on, that row has Add 30
min and End now, and every other page a one-line notice with End now (the
Status page's top card).

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
`409 signal_test_running` (a signal-group comparison runs), `409 safe_mode`.

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
while it may be on (reboot, app update, crash, and the reset pin, which
reads as power-on on the ESP32-S3) is remembered in RTC memory: the next
boot turns it off once the station streams (a receiver that restarted
after all clears it with its banner). After a receiver
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
| `position` | the station's locked or last surveyed position (`null` coordinates with `source: "none"`), as in `GET /api/gnss` |
| `totals` | `gnss_obs_get_live` over every satellite, also those beyond the list: `satellites` listed, `tracked` in an MSM within the last 10 s, `signals` their signals, `with_pos` with azimuth and elevation, `dropped` beyond the list; `cnr_dbhz` mean over every satellite band (`null` = none); `bands` per band with tracked satellites (count, mean C/N0); `systems` per constellation with tracked satellites, their count per band (only bands with any). `null` when the observations cannot be read |
| `satellites` | at most 96, by constellation and number. `prn` as in `/api/sky` (QZSS and SBAS by PRN, GLONASS by slot). `in_msm` and `msm_age_ms` (`null` = only the sky view knows it); `signal_ids` the MSM signal IDs; `cnr_dbhz` per band present: its best signal's C/N0 (`null` = no C/N0 in the MSM type). `el_deg`, `az_deg` (`null` until the satellite-in-view output reported it), `sky_cnr_dbhz` from that report, `pos_age_ms` its age |

The UI opens it from the Receiver page's Signals card ("Show the sky"):
the time left with Extend and Close, the sky plot (satellites with a
position, coloured by C/N0, labelled) beside the card's band table, and
every satellite under that table, one line each (best C/N0 and the C/N0
per band). The station map sits in the Receiver page's Position card: the
position with a small OpenStreetMap tile mosaic, fetched only after the
user asks for it, with the attribution and a plain link to open the
position in an external map (offline: the coordinates only).

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
`409 safe_mode`. The UI offers it as "Check again" wherever it says no
receiver was found (the Status page; Logs, Overview; the setup
wizard's antenna check; the Receiver page).

### Signal-group comparison

The user compares two signal groups on this antenna: the station measures
each for `phase_s` seconds, then waits for the user to keep one. Each group
in turn: a switch (the station writes the group into the stored profile and
re-applies it, its own reconfiguration: the casters pause for it and the
ledger counts the gap as planned downtime; the receiver restarts for a new
signal group), a minute of settling, then `phase_s` seconds of good samples
(one every 10 s while the station streams with an epoch younger than 3 s;
samples taken otherwise do not count, so the phase takes longer). The group
running now, if it is one of the two, is measured first, which saves a
switch: at most three switches per test (to the first group, to the second,
and back or to the kept one). After both: deciding, for up to 24 h of
station running time, with the receiver on the group measured last; keeping
that one switches nothing, keeping the other switches once. No choice
within 24 h brings back the group from before the test (`timed_out`).
A group the receiver refuses ends the test (`rejected`) and brings back the
group from before; another failed switch, a switch not done within 10 min,
or 10 min without streaming while settling or measuring ends it as `failed`
the same way. Time counts in good samples and station running time, never
in wall-clock time, and the test is stored (`gnss/sg_test`, at every
transition, every 5 min while measuring, every 10 min while deciding and
before a planned restart):
after a restart a switch is asked for again, settling or measuring settles
again and keeps what was measured, deciding keeps its countdown; a record
that cannot be read ends the test as `interrupted`. A factory reset erases
it. Bands follow the signal view's table (`GET /api/gnss/signals`); a band
counts for a group when it was there in at least half of its samples.

#### `GET /api/gnss/signal-test` (adm)

```json
{"state": "measuring", "groups": [{"master": 3, "slave": 6}, {"master": 7, "slave": 0}],
 "before": {"master": 3, "slave": 6}, "phase": 0, "phase_s": 1800, "settle_s": 60,
 "left_s": 1230, "decide_left_s": null, "running_group": {"master": 3, "slave": 6},
 "results": [
   {"group": {"master": 3, "slave": 6}, "status": "measuring", "measured_s": 570, "samples": 57,
    "epoch_gaps": 0, "best_sys": "galileo", "best_bands": 4,
    "systems": [{"sys": "galileo", "satellites": 9.4, "bands": ["l1", "l2", "l5", "l6"],
                 "per_band": [{"band": "l1", "satellites": 9.4, "cnr_dbhz": 44.6, "signal_ids": [2]}]}]},
   {"group": {"master": 7, "slave": 0}, "status": "pending", "measured_s": 0, "samples": 0,
    "epoch_gaps": 0, "best_sys": null, "best_bands": 0, "systems": []}],
 "outcome": null, "kept": null, "reason": "", "ended_unix": null}
```

| Field | Notes |
|---|---|
| `state` | `idle`, `switching` (the receiver is being switched to `running_group`), `settling`, `measuring`, `deciding`, `restoring` (switching to the group the test ends on: back to `before`, or to the kept one) |
| `groups`, `before` | the two groups as chosen (A, B) and the group running when the test started; `null` before the first test (and after `interrupted`) |
| `phase` | 0 for the group measured first, 1 for the second, while switching, settling or measuring; else `null` |
| `phase_s`, `settle_s` | measuring time per group (the last test's, else the default 1800) and the settling time (60) |
| `left_s` | settling or measuring: seconds left in the phase (settling plus the good samples still missing); else `null` |
| `decide_left_s` | deciding: station running time left to choose; else `null` |
| `running_group` | the group the phase switches to or measures, the one running while deciding, the one restored to; `null` when idle |
| `results[]` | one per group in `groups` order (`null` with `groups`): `status` `pending`, `measuring`, `done` or `rejected` (refused by the receiver); `measured_s` (good samples x 10 s), `samples`, `epoch_gaps` (epochs missing while measuring); `best_sys`, `best_bands` the rewarded constellation (GPS, GLONASS, Galileo, BeiDou) with the most bands, as in the signal view (`null`, 0 = none); `systems[]` the constellations with at least one band (never SBAS): `satellites` the mean count (1 decimal), `bands`, and per band the mean satellites with a signal there (1 decimal), the mean C/N0 (`cnr_dbhz`, 1 decimal, `null` when none was measured: MSM4-7 only) and the MSM `signal_ids` seen there |
| `outcome` | `null` while running, decided once restoring: `kept`, `timed_out`, `cancelled`, `rejected`, `failed`, `interrupted` |
| `kept` | the group kept (`kept` only) |
| `reason` | what happened, in words (`""` = nothing to say) |
| `ended_unix` | when the last test ended, `null` without a clock then (or while one runs) |

#### `POST /api/gnss/signal-test` (adm)

```json
{"groups": [{"master": 3, "slave": 6}, {"master": 7, "slave": 0}], "phase_s": 1800}
```

`202 {"accepted": true}`; `GET` shows the test. `groups`: two different
pairs of 0-255 (else `400 invalid_field` `groups`); `phase_s` 60-7200,
left out = 1800 (else `400 invalid_field` `phase_s`). Refused, in this
order: `409 signal_test_running` (one runs already),
`409 settings_unavailable` (the test could not be stored),
`409 first_apply_held`, `409 autocfg_off` (automatic configuration is
off: the user configures the receiver), `409 receiver_maintenance`,
`409 journal_open`, `409 safe_mode`, `409 no_receiver` (no receiver is
connected: `GET /api/gnss` `state` is `no_receiver`),
`409 unsupported_model` (an identified receiver model this firmware does
not configure, or while the `unsupported_model` alert is raised),
`409 identity_unconfirmed` (the receiver has not answered its version
query this boot: a connected receiver that is not identified yet gets
this one), `409 receiver_not_streaming`, `409 config_failed` (the
last apply failed), `409 position_not_locked` (a surveying base would
survey again at every receiver restart: lock the position first).

While a test runs, these answer `409 signal_test_running`, so nothing else
changes the receiver underneath it: `PUT /api/gnss/profile` with `profile`
or `autocfg`, `POST /api/gnss/apply`, `POST` and `DELETE
/api/gnss/position/lock`, `POST /api/gnss/maintenance`, `POST
/api/update/gnss`, `POST /api/upload/gnss/confirm` and the
`/api/gnss/flash/*` actions. The health ladder's own steps still run.

#### `POST /api/gnss/signal-test/keep` (adm)

`{"group": 0}` (0 = the first of `groups`, 1 = the second; else
`400 invalid_field` `group`). Deciding only: `202 {"accepted": true}`;
the test ends (`kept`) once the group runs. `409 signal_test_not_deciding`
otherwise.

#### `DELETE /api/gnss/signal-test` (adm)

Stop a running test: back to `before` (`cancelled`). `202 {"accepted":
true}`; `409 signal_test_idle` when none runs.

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
`409 app_update`, `409 settings_unavailable`, `409 signal_test_running` (a
signal-group comparison runs; it cannot start while a journal is open),
`501 not_available_yet` (retry and restart in builds without receiver
flashing).

---

## 10. Casters

Slots `0` to `max_slots - 1` (`CONFIG_NTRIP_MAX_CASTERS`).

### `GET /api/casters` (adm)

```json
{
  "max_slots": 4,
  "onocoy_preset": {"host": "servers.onocoy.com", "port": 2101, "tls_port": 2121, "ntrip_version": 1,
                    "backoff_profile": "fast"},
  "slots": [{"slot": 0, "config": {}, "status": {}}],
  "warnings": [{"id": "onocoy_open_caster", "onocoy_slots": [0], "open_slots": [1], "rtk2go": true}]
}
```

`config` and `status` are the objects below.

`warnings` (an empty list most of the time; also in safe mode, where the
configs are not loaded): `onocoy_open_caster` while an enabled slot streams
to Onocoy and another enabled slot to an open caster (RTK2go, Centipede):
`onocoy_slots` and `open_slots` name them, `rtk2go` says whether one of the
open slots is RTK2go (`ntrip_server_open_caster_pair()`). Streams on open
casters are public, so anyone can take the station's stream there and submit
it to Onocoy as well, and Onocoy's FAQ says: "Please note that multiple
submissions of the same streams, e.g. from free or public networks such as
IGS or RTK2GO, even when done by separate users in the onocoy platform, will
result in banning." The UI warns about it on the card of each open slot and
in one line on the Status page, and links that FAQ. It blocks nothing (RTK2go slots stay off unless the user
turns one on); saving such a pair still asks for `dup_stream_ack` (below).

`GET /api/casters/{n}` returns one `{"slot", "config", "status"}` entry. In
safe mode `config` is `null` (the slots were never loaded) and every status
is `disabled`.

An **empty slot** is one with no `host` that is not enabled: never set up,
or removed (`DELETE /api/casters/{n}`). Its `config` keeps whatever is
stored, but the status summaries (`/api/status` `casters[]`, section 6, and
`/api/availability` `slots[]`, section 14) mark it `empty` with the name
`""`, and the public status leaves it out, so a removed slot's old name
never shows.

**`config`** (`ntrip_caster_cfg_t`):

```json
{"enabled": true, "name": "Onocoy", "host": "servers.onocoy.com", "port": 2101,
 "ntrip_version": 1, "tls": false, "trust": "bundle", "ca_pem_set": false,
 "mountpoint": "<console mountpoint>", "username": "<console username>", "password_set": true,
 "label": "", "v2_raw_body": false, "backoff_profile": "fast", "protected": true,
 "dup_stream_ack": false, "test_onocoy": false}
```

Passwords are never returned (`password_set` only). Every entry
(`{"slot", "config", "status", "onocoy_ready"}`) says with `onocoy_ready`
whether it is an Onocoy slot that is set up: a slot that counts as Onocoy
(host in `onocoy.com`, or a test Onocoy slot), `mountpoint`, `username` and
a stored password (false for every other slot).

`test_onocoy` (always present) marks a **test Onocoy slot**, possible only in
bench and QEMU builds (`CONFIG_NTRIP_TEST_ONOCOY_SLOT`, which depends on the
bench console or the QEMU Ethernet option); in any other build it is always
`false`, a stored mark is ignored (the slot is judged by its host) and the
next save drops it. A test Onocoy slot counts as Onocoy in every respect
whatever its host: `protected`, the `fast` profile, the Onocoy fields and
wire names, `onocoy_ready`, the one-Onocoy rules (`second_onocoy`,
`dup_stream_ack_required`) and the login guard below. So a local test caster
(the Mac at `10.0.2.2` from QEMU, a PC on the bench) sees what Onocoy would.

**Onocoy slots** hold exactly what the Onocoy console shows for a
station's NTRIP credentials, and the UI asks for those in the console's
words and order: host `servers.onocoy.com` and port 2101 (shown read-only),
`mountpoint`, `username`, `password`. On the wire
(`ntrip_wire_names()`): NTRIP 2.0 posts to `/<mountpoint>` with Basic
`<username>:<password>`; NTRIP 1.0 (the default) sends
`SOURCE <password> /<username>`, as Onocoy tells sources that speak only
1.0 to do. A slot stored in the older layout (an Onocoy slot whose stored
record has no layout byte, the credential username in `mountpoint`) is loaded
with an empty `username`, whatever its record held: a slot switched to
Onocoy in place kept the username of the caster it was before, which meant
nothing for Onocoy. It keeps working with the old mapping (1.0:
`mountpoint` as the mountpoint; 2.0: `label` or `onocoydiy` as the
mountpoint, `mountpoint` as the Basic-auth user) but is not set up
(`onocoy_ready` false); its next save while enabled needs the username, and
any save stores it in the current layout.

**`status`** (`ntrip_caster_status_t`):

```json
{"state": "streaming", "reason": "none", "state_age_s": 3600, "progress_age_ms": 120,
 "connects": 3, "failures": 1, "disconnects": 2, "restarts": 0,
 "failures_by_class": {"dns": 0, "connect": 1, "tls": 0, "timeout": 0, "remote_close": 0, "write_stall": 0,
                       "keepalive": 0, "auth": 0, "mount_busy": 0, "config": 0, "banned": 0,
                       "login_blocked": 0, "in_use": 0, "replaced": 0, "local": 0},
 "connected_since_unix": 1760086400, "connected_s": 3600,
 "today_online_s": 43200, "today_offline_s": 4,
 "graced_uptime_permille": 503, "streak_ok": true, "graced_uptime_so_far_permille": 1000,
 "data_rate_permille": 1000, "budget_s": 473, "budget_left_s": 469,
 "bytes_sent": 74000000, "bytes_sent_today": 37000000, "frames_sent": 900000, "epochs_sent": 86000,
 "epochs_sent_today": 43200,
 "frames_dropped": 0, "bytes_dropped": 0, "epochs_dropped": 0, "epochs_dropped_today": 0,
 "frames_skipped_idle": 12, "lag_drops": 0, "laps": 0, "lag_ms_now": 40, "max_lag_ms": 1000,
 "backoff_attempt": 0, "next_retry_s": 0,
 "last_status_code": 200, "last_status_line": "ICY 200 OK", "server_header": "",
 "resolved_ip": "35.207.172.57", "dns_cached_used": 0, "unreached_s": 0,
 "tls_version": null, "tls_cipher": null, "hint": "", "last_err": null,
 "stop_reason": "none", "login_rejects": 0, "login_blocks": 0, "kicked_sessions": 0, "last_session_text": ""}
```

"Today" is the current UTC day. `today_online_s` and `today_offline_s` are
the availability ledger's figures for the slot (section 15:
`/api/availability` `today.up_s` and `down_s`): seconds it streamed with
fresh RTCM, and seconds it was enabled but did not (planned time
included); they are kept across restarts, so every "today" figure agrees
with the others. `epochs_sent_today`, `epochs_dropped_today` and
`bytes_sent_today` (RTCM bytes, without chunk framing) are this start's
counts for the current UTC day (RAM; they start again at a restart and at
the UTC day change). The Onocoy day figures are `null` for
other slots, and for the protected slot while it is disabled or not set up
(`onocoy_ready` false): numbers for a slot that cannot stream would only
mislead. `graced_uptime_permille` is the share of the whole day covered so
far (online today plus the 300 s grant, of 86400 s: what the day reaches
if nothing more is sent); `data_rate_permille` = this start's epochs sent
today over its seconds online today (a ratio of this start only, shown
as the current data rate; online as the availability ledger counts it:
streaming with fresh RTCM from the receiver, from the caster's OK on, so
a session left open through a pause for receiver work is offline time,
never lost epochs as well, and one left open through a receiver stall is
online for its first 10 s, until the station sees the RTCM go stale (those
seconds are lost epochs), and offline time after that); `budget_s` the offline seconds the day allows at
the best data rate it can still reach (473 at 100 %): 1 - lost / (86400 -
`today_offline_s`), with lost the epochs the slot should have sent while
online (one a second) but did not, today, across restarts (the
availability ledger keeps the count), so one epoch lost right after a
start costs about nothing instead of the whole budget; `budget_left_s` =
`budget_s` minus `today_offline_s`, 0 once used up; `graced_uptime_so_far_permille`
= 1 - max(0, unplanned downtime today - 300 s) / time elapsed today, with
the downtime as the availability ledger counts it (planned time left out)
and the time counted from 00:00 UTC, or from when the slot was enabled and
set up during this boot when that came later (`null` without a trusted
clock); `data_rate_permille` is `null` until something was sent today;
`streak_ok` whether that best rate still reaches 98 % and today's offline
time is still within `budget_s` (the UI shows "Missed" exactly while it
is false);
`tls_*` are `null` without TLS; `max_lag_ms` is the highest
`lag_ms_now` seen since the start; `unreached_s` the current run of
failures that never reached the caster (DNS failures, connects that timed
out or failed locally; 0 = none; the UI says "caster not reached for N
s"); `last_session_text` the newest
line the caster sent during a session (nothing is expected there), with
the password, the username and an Onocoy slot's mountpoint masked (`""` =
none since the last save).

**Onocoy login guard** (slots that count as Onocoy only; standard and polite
slots keep the rows above). A refused login from Onocoy may block logins
from the station's whole internet connection for a while (every other
station on it too), and two devices streaming with one credential keep
knocking each other off, so such a slot never loops a failing login.
Onocoy's console reasons (`too many unsuccessful logins`, `already in use`,
`parallel connections`, `device reconnected`, `duplicate of another
stream`, `credential is banned`, `authentication failed`, `invalid request
in headers`) are matched case-insensitively in the reply line, in the body
of every non-success HTTP reply and in every line received during a
session; a match decides over the reply's generic class (the wire form of
these texts is unknown: PROVISIONAL, like every value here). "Within the
window" means within 300 s of this station's previous session (or start) on
credentials the caster accepted before. Every guard delay is its entry
x 1.0-1.2.

- The counts below last for an incident: only a healthy session (60 s and
  5 KB sent) or the user ends it, not the caster's acceptance alone (Onocoy
  may accept a login and end the session at once with a reason). They are
  kept across restarts.
- A refused login (`auth` or `config` reply, `authentication failed` in the
  reply or during a session, or an unrecognised `Error <n>` line after the
  first one in the incident): retry after 300 s (30 s within the window,
  once: a caster deploy), then 1800 s; the third stops the slot
  (`rejected`). `login_rejects` counts them, the hint says "Onocoy refused
  the login (n of 3)", the state is `auth_failed`. `500`, `503` and `ERROR -
  Too many sources` are the caster's own trouble: the `fast` transient row,
  never a refused login.
- `too many unsuccessful logins` (class `login_blocked`): 3600 s, then
  7200 s; the third stops the slot (`login_blocked`). `login_blocks` counts
  them; the state is `auth_failed`.
- The credential in use (`already in use`, `parallel connections`, or
  `409`, `Mount Point Taken`, `Already Connected`; class `in_use`): outside
  the window, or on credentials never accepted, the slot stops at once
  (`in_use`); within the window (the caster may still hold this station's
  own dead session) it retries after 15, 30, 60 and 120 s, and the fifth
  stops it.
- Take-over: a session the caster ends within 120 s of its start counts 1,
  a `device reconnected` or `in use` line during a session counts 2 (class
  `replaced`); at 3 the slot stops (`kicked`). The reconnect after such a
  session waits by the count it reached: 5 s at 1, 10 s at 2 (so 10 s
  after a `device reconnected` line). A session of 10 min or more resets the
  count (`kicked_sessions`, at most 3). Only an end from the caster's side
  counts (EOF or a reset, class `remote_close`); write stalls, keepalive
  aborts, failures on this station's side (class `local`, or a `tls` error
  inside TLS), network loss, source stalls, pauses and edits never count.
- `duplicate of another stream`, `credential is banned`, or any other
  `banned` reply: the slot stops at once (`duplicate`, `banned`).
- Failures without a reply (nothing in 10 s, a close before the line ends
  that names none of the texts above) stay on the `fast` transient row, but
  after 10 of them in the incident every delay is at least 300 s, and a
  restart waits 300 s before its first attempt. Failures that never reached
  the caster keep the flat 5 s retry; TLS failures keep their rows.

A stopped slot has `state` `stopped` and a `stop_reason` (`ntrip_stop_t`;
`none` otherwise) and makes no attempt at all, across restarts and power
cuts (stored with its backoff record), until the user saves it, presses
retry (`POST /api/casters/{n}/retry`) or disables it. Each stop is logged
once and journalled (`caster_stop`, section 14). A stopped protected slot
counts as offline in the ledger and as down for the health level, but it
does not hold the network ladder's `protected` trigger: a Wi-Fi restart or
a reboot cannot fix a credential.

### `PUT /api/casters/{n}` (adm)

Body: the `config` object. Fields left out keep their stored value (a form
that does not show a field cannot reset it). Output-only fields
(`password_set`, `ca_pem_set`, `protected`) are ignored. Special fields:
- `password`: a non-empty value replaces the stored one; empty or missing
  keeps it, but only while `host` stays the same (pointed at another host the
  slot never sends the old password there); `"clear_password": true` removes
  it.
- `protected` is set by the server: true exactly for a slot that counts as
  Onocoy (a host in the `onocoy.com` domain, or a test Onocoy slot).
  `backoff_profile` is forced to `fast` for such a slot and to `polite` for
  `rtk2go.com`; elsewhere `standard` or `polite` as sent
  (`fast` is refused there: `400 invalid_field`), or, when left out, the
  stored profile (a `fast` one left from an Onocoy host becomes
  `standard`). These host rules belong to `ntrip_server`
  (`ntrip_server_apply_host_rules()`, which every save applies too); the
  server applies them to the request only to name a refused field.
- `dup_stream_ack`: see below.
- `test_onocoy`: `true` only in a build with `CONFIG_NTRIP_TEST_ONOCOY_SLOT`
  (bench and QEMU builds); elsewhere `400 invalid_field` with `field`
  `test_onocoy` and the message `only in bench and QEMU builds`. `false` is
  always accepted.
- Any save clears a stopped slot's stop and the login guard's counts.

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

**RTK2go mountpoints**, checked when a slot is saved with an `rtk2go.com`
host that is new (no stored host, or another one) or with a changed
`mountpoint`, in this order (`400 invalid_field`, `field` `mountpoint`):
anything but letters, digits, `_` and `-` (`RTK2go mountpoints use letters,
digits, "_" and "-" only (no ".")`); a first character that is not a letter
(`RTK2go mountpoints start with a letter`); an uppercase `A` first (`RTK2go
no longer accepts new mountpoints that start with "A"`; a lowercase `a` is
not refused); fewer than 4 or more than 20 characters (`RTK2go mountpoints
have 4 to 20 characters`). These are the rules of RTK2go's reservation form
for new names. A slot stored before them keeps streaming and can be
enabled and disabled unchanged; only a new name has to follow them.
`ntrip_server_set_config()` refuses the same saves (`ntrip_server_rtk2go_check()`).

**Caster policy** (`409 caster_policy` with `reason`). The rules belong
to `ntrip_server`: `ntrip_server_set_config()` checks them against the
stored slots under the same lock as the save and reports which one refused
(`ntrip_policy_t`); the server only names it:
- `second_onocoy`: another enabled slot already streams to Onocoy;
- `second_rtk2go`: another enabled slot already streams to RTK2go;
- `dup_stream_ack_required`: enabling this slot would stream to Onocoy and
  to an open caster (RTK2go, Centipede) at the same time. Streams on open
  casters are public, anyone can submit them to Onocoy as well, and Onocoy's
  FAQ says multiple submissions of the same stream, e.g. from RTK2go, "will
  result in banning" (the rule quoted under `warnings` above). The UI shows
  that, and on the user's tick sends the same body again with
  `"dup_stream_ack": true`. The acknowledgement is stored with the slot
  being saved.

`200` with the slot's `{"slot", "config", "status"}`; only that slot
reconnects. A session inside an epoch first sends the rest of it (at most
1 s), so the caster's last epoch of that session is a whole one; a reboot
ends sessions the same way. Also `409 safe_mode`,
`409 settings_unavailable`.

### `POST /api/casters/{n}/retry` (adm)

The user's retry: the slot's stop (or a backoff it waits out) and the login
guard's counts are cleared as by a save with the same settings, and it may
try at once (the UI asks first: only after making sure no other device
streams with the credential and the credentials match the Onocoy console,
since every failed login counts toward Onocoy's block). Body `{}`. `202
{"accepted": true}`; `409 slot_disabled` when the slot is not enabled;
`409 safe_mode`. Any other suffix after the slot: `404 not_found` (`405`
for the slot and its `ca`).

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
  "bssid": "AA:BB:CC:DD:EE:FF", "tx_power_dbm": 19.5, "ap_steering": true,
  "time_synced": true, "upstream": "ok", "probe_age_s": 120,
  "reconnects": 1, "connect_failures": 0, "roams": 0, "offline_s": 0, "retry_in_s": 0,
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

`bssid` is the access point the station is associated with (one of
several that may serve the SSID: a mesh or a repeater), `tx_power_dbm`
the station's Wi-Fi transmit power limit now (`esp_wifi_get_max_tx_power`,
read in steps of 0.25 dBm); both refreshed with `rssi` every 5 s, and
like `rssi` and `channel` `null` while not associated (and in the QEMU
build, whose emulated link has no access point). Diagnostics for a weak
link (the UI's Logs page shows them under Chip and Wi-Fi, with the
channel); only in this admin route, never in the public status.
`ap_steering`: the access point takes part in 802.11v BSS transition
management, so it can ask the station to move to another access point of
the network (a mesh steering it); read once, a few seconds into each
association; `null` before that and while not associated. Left out in a
build that never reads it (roaming, `CONFIG_NETWORK_ROAM`, or 802.11v
support, `CONFIG_ESP_WIFI_WNM_SUPPORT`, off; the QEMU build): the UI then
shows no row for it. `roams`: moves to another
access point of the network since boot, the ones the access point asked
for and the station's own (to a clearly stronger access point while its
signal is weak); each is a `wifi_roam` journal entry (so is an own
reconnect that stayed on the same access point, not counted here), and
none counts in `reconnects` (links re-established after a drop). A move
that is associated again within a few seconds keeps `online`, `ip` and
every TCP session (`associated` is `false` meanwhile); the UI's
Logs page shows `roams` and `ap_steering` under Chip and Wi-Fi.
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

`trial` is `network_set_credentials()`'s plan: `true` exactly when the
credentials are stored as pending and tried (`NETWORK_CRED_PLAN_TRIAL`);
`applied` is `false` for the store-only plan. From the first
`GET /api/wifi` (or `/api/status`, or the WebSocket's network section)
after the `202` on, the `cred_*` fields agree with it: `cred_trial` is
`true` from the moment the trial is queued, with `cred_trial_left_s` 91
(the 90 s of the trial plus the second until the station switches) and
counting down from 90 once it runs; `cred_pending` and `pending_ssid`
name the new network and `cred_reverted` is `false`. The same holds after
`POST /api/wifi/pending/retry` (a trial again: `cred_trial` true,
`cred_reverted` false) and `POST /api/wifi/pending/discard` (nothing
pending, no trial, no revert).

With credentials stored before (and different), the new ones are stored
as **pending** and tried for 90 s.
They replace the current ones once they get an IP; otherwise the station
goes back to its previous credentials and keeps the new ones pending
(`cred_reverted`, `pending_ssid`), so a typo never strands a remote station.
A network that refuses the new password goes back sooner: three refused
attempts in a row before any association (wrong password, a handshake that
times out, an access point of that name only with another security
setting) end the trial at once (`cred_reverted_why: "auth_failed"`); a
network that is just out of reach keeps the 90 s. During such a trial a
refused attempt is retried without the reconnect backoff (each attempt
still scans first), so the three refusals come back to back and the
station is on its previous network about 18 s after the change (measured
parts: 3.6 s to the first attempt, about 3.1 s of authentication per
attempt, about 2.7 s of scan before each further one; PROVISIONAL, the
target is under 20 s).
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
One asked for during the station's own short scan for its network
(roaming, `GET /api/wifi` `roams`) starts right after that one.
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

**Banner** (`update` in `/api/status`, admin sessions only): while
`app_available`, `gnss_available` or `app_reverted_in_stage1`, a dot on the
Settings tab and a one-line notice on every other page, "Update available:
app vX and receiver build Y. Later / Update". "Later" hides the notice, kept
in the browser (`localStorage`, keyed by `app_version` and `gnss_build`)
until a newer version appears; the server keeps no "later" state.

| Field | Source |
|---|---|
| `app_available`, `app_version` | `app_update_available`, `latest_version` |
| `app_reverted_version`, `app_reverted_in_stage1` | the last version rolled back; true when it rolled back in its first minute (offer "install again?") |
| `gnss_available`, `gnss_build`, `gnss_version` | `gnss_update_available`, `gnss_latest_build`, `gnss_latest` |
| `gnss_blocked_reason`, `gnss_blocked_left_s` | `fw_update_gnss_block_t`; `left_s` for `app_trial`: the rest of the trial (confirmation plus crash watch; for a reinstall of the running image the confirmation only) |
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
           "expected_downtime_s": 252, "budget_left_s": 469, "flash_available": false,
           "flash_reason": "not_enabled_in_build"}
}
```

| Field | Notes |
|---|---|
| `state` | `fw_update_state_t` |
| `checks_enabled` | `false` when the build cannot check: no valid release repository, or it requires signed manifests and none of its keys is usable (manual uploads still work) |
| `release_repo` | `owner/name` whose GitHub releases the station reads (`CONFIG_FW_UPDATE_GITHUB_REPO`; `null` if unset) |
| `signed_manifests` | `true` when the build has a manifest key and accepts signed manifests only |
| `last_result` | outcome of the last job the backend saw end (`job`: `check`, `app_update`, `app_upload`, `gnss_update`, `gnss_upload`, `rollback`), `null` this boot until one ended; `message` the `esp_err_t` name; with `failed_rule` and `cross_layout` only when `result` is `package_rejected` (result codes below) |
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
| `gnss.flash_available`, `gnss.flash_reason` | as `gnss_flash_available` and `gnss_flash_reason` in `/api/status`: `station_flash_available()`; the reason is `not_enabled_in_build` when it is `false`, else `null` |

**Result codes** (`last_result.result`, final `progress` messages): `ok`,
`not_available_yet`, `refused` (the state changed between click and start),
`download_failed`, `checksum_mismatch`, `size_mismatch`, `invalid_image`,
`signature_failed` (check: a build with a manifest key got a manifest that
is unsigned or not signed with one of its keys), `untrusted_host` (check,
`app_update`, `gnss_update`: a redirect led off `release_repo`'s releases on
GitHub's release hosts, off https, or carried user info; nothing was fetched
from there), `package_rejected` (a receiver package failed a
pre-flight rule; with `failed_rule`, 1-12 as numbered in `um98x_flash.h`,
and `cross_layout`), `same_image` (an app upload of the image already
running, sent without `X-Reinstall`: nothing was written),
`rolled_back_ack_required` (an app upload of a rolled-back version sent
without `rolled_back_ack=1`: nothing was written), `failed` (anything else;
`message` has the `esp_err_t`).

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
"Receiver package downloaded and verified. Receiver firmware updates are
not enabled in this build yet. The station now checks the package against
every rule; nothing is sent to the receiver."

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
watch, so receiver updates are not held back for the watch's day: during
that stage `gnss_blocked_left_s` (section 12) counts the confirmation
alone (at most a minute).

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
Other errors: `404 nothing_staged`, `409 signal_test_running` (a
signal-group comparison runs) and the `409` codes of the upload.

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
  "level": "ok", "alerts": [], "last_step": "none", "step": "none", "step_phase": "none", "ladder_suspended": false,
  "uptime_s": 86400, "gnss_stalls": 0, "recoveries": 0, "caster_restarts": 0,
  "network_recoveries": 0, "rtcm_gap_ms": 230, "gnss_retry_in_s": null,
  "factory_reset_refused_age_s": null, "admin_resets": 0, "recovery_mode": "full", "reboot_recheck": false,
  "reboot_hold": [], "reboot_hold_left_s": null, "reboot_cap_wait_s": null, "reboot_cap_limit": null,
  "heap": {"internal_free": 120000, "internal_min": 98000, "internal_largest": 64000, "psram_free": 7000000,
           "psram_min": 6900000, "psram_largest": 4000000},
  "inhibited_by": null, "update_slot": "none",
  "boot": {"reset_reason": "sw", "reset_class": "planned", "prev_reboot_reason": "app_update",
           "boots_since_power_on": 3, "crashes": 0, "brownouts_since_power_on": 0,
           "safe_mode": false, "safe_mode_retry_in_s": null, "stable": true, "prev_session_uptime_s": 3600},
  "boot_history": [{"reset_class": "planned", "reset_reason": "sw", "planned_reason": "app_update",
                    "boot_no": 3, "prev_uptime_s": 3600, "version": "v0.3.0", "start_unix": 1760086400}],
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
              "lease_held": false, "since_lease_release_ms": null, "rx_maintenance": false,
              "rx_boot": "none", "rx_boot_by": "none"},
  "obs": {"epochs": 86400, "epoch_gaps": 0, "epoch_period_ms": 1000, "epoch_jumps": 0},
  "network": {"reconnects": 1, "connect_failures": 0, "offline_s": 0, "last_disconnect_reason": 8,
              "fallback_dns": false, "probe": "ok", "probe_age_s": 120, "assoc_no_ip_s": 0,
              "ladder": {"active": false, "rung": 0, "withheld": "none", "triggers": [], "actions": 0}},
  "journal": {"available": true, "capacity": 4064, "oldest_seq": 1, "newest_seq": 77, "sector_cycles": 0,
              "written": 12, "queued": 0, "refused_kind": 0, "refused_budget": 0, "queue_full": 0, "io_errors": 0},
  "log": {"lines": 812, "ring_overwritten": 0, "cut": 0, "console_dropped": 0, "console_dropped_bytes": 0,
          "prev_boot_available": true, "prev_boot_lines": 640, "usb_mirror": false, "usb_connected": false,
          "usb_dropped": 0, "usb_dropped_bytes": 0},
  "coredump": {"present": false, "size": 0, "task": "", "pc": null, "backtrace": []},
  "byte_capture_built": false
}
```

`byte_capture_built`: this build records the receiver's bytes (bench builds,
`GET /api/diag/flash-capture`); the UI shows the download only then.

`alerts` are health's alerts with the names of `GET /api/status` (the
reasons for no RTCM among them). `rtcm_gap_ms` is the time since the last RTCM frame. `gnss_retry_in_s` is
the time until the receiver recovery's next retry pass (`null` = none
scheduled; also with `recovery_mode` `alerts_only`, where the pass sends
nothing). `factory_reset_refused_age_s` is the time since a BOOT-button
factory reset was refused because a receiver update was running (`null` =
not this boot); `admin_resets` counts BOOT-button password resets this
boot. `step`, `step_phase`, `recovery_mode`, `reboot_recheck`, `reboot_hold`,
`reboot_hold_left_s`, `reboot_cap_wait_s` and `reboot_cap_limit` as in `/api/status`.
`log.console_dropped` (and `_bytes`) count only lines the serial console
never sent because the log ring overwrote them first; a burst that the
console sends later, in order, is not a drop. `log.usb_mirror`: the console
is also copied to the ESP32's built-in USB (the `ESP32S3` USB-C port with
the plug turned over; a build with `CONFIG_DIAG_CONSOLE_USB_MIRROR`, which
bench builds turn on); `log.usb_connected`: a USB host is attached there now
(it may not have the port open); `log.usb_dropped` (and `_bytes`): lines not
copied there although a host was attached, because it did not read and the
USB buffer was full. With no host attached nothing is copied and nothing is
counted.
`station.rx_boot`: after a power-on or brownout reset, whether the
receiver kept running (`station_rx_boot_t`): `none` (neither), `pending`
(judged within 30 s of the start), `kept_running` (only the ESP32 was
reset, or only its supply dipped), `restarted` (the receiver restarted with
it: the power was cut, or dipped for both), `unknown` (nothing the receiver
sent told). On the ESP32-S3 a reset by its EN pin (the USB serial chip's
auto-reset that every flash and capture uses, the RST button) reads as
`power_on` (`boot.reset_class`) while the receiver, on the same supply,
keeps running; a `brownout` says only that the ESP32's own 3.3 V rail
dipped below its detector's level (about 2.44 V): on our board each half
has a regulator of its own on the shared USB 5 V (inferred from the board),
so a dip of that input takes both down while one the ESP32's own load
causes (Wi-Fi transmit peaks) leaves the receiver running. So the station
asks the receiver's own evidence, listening only (`rx_boot_by`): `restart` its
restart banner before any data (restarted); `uptime` its version log,
whose header's millisecond field counts from its restart while it has no
time (restarted if that is shorter than the ESP32's uptime, kept running if
it began at least 2 s before the ESP32 started); `stream` RTCM or NMEA
within 3.5 s of the start, before any restart evidence (kept running: a
receiver that just got power needs longer). The earliest decides; it is
journalled once (`rx_boot`). Only a power-on boot's `restarted` counts
toward the `power_lost` alert (three within 24 h; a brownout raises
`power_supply`), and only a power-on boot's `kept_running` makes its gap no
power-off time in the availability ledger (`power_off_s`; a gap counted
before the verdict, after the first trusted clock, has it taken back; the
gap stays offline time; a brownout is the ESP32's own power dip whatever
the receiver did). A `restarted` verdict ends a receiver maintenance resumed
at that boot. PROVISIONAL, to be confirmed on hardware (a real power cut's
timing was not measured).
`link.framing` counts what the framer made of the receiver's bytes
(`gnss_framing_stats_t`): `bytes_discarded` are bytes that became no RTCM
frame and no checksum-valid line (line noise, a unit cut short by a baud
switch, a reopen or the end of a lease, skipped binary logs); input dropped
on purpose (the end of a lease, a resync) counts in `flushed_bytes`, the
first 20 ms after a baud change in `baud_switch_bytes`. A boot that has to
look for the receiver no longer adds to `bytes_discarded`: the detection
probe waits up to 1 s for the version log that follows its acknowledgement
before its lease ends, so that log is read whole (it gives the identity)
instead of being cut.
`link.open` and `link.open_failures`: the UART driver is installed, failed
installs since boot (the UI's "Receiver port closed" while `open` is
false in a mode other than `off`); `link.hands_off`: receiver maintenance. `journal` and `log` are the event journal's and the log ring's
counters (`diag_journal_info()`, `diag_log_stats()`; `journal.capacity` in
records; `log.prev_boot_available` false and `log.prev_boot_lines` 0 when
the previous boot's lines are not readable, see `GET /api/logs`).
`log.lines` counts the lines logged this boot (as the text download has
them); `log.ring_overwritten`, `log.cut` and `log.prev_boot_lines` count
the ring's records, each a line or a part of one.
Per-caster loss counters are in `/api/casters`. `boot_history` is the last
16 boots, newest first (`prev_uptime_s` `null` = unknown). `start_unix`:
when that boot started (UTC s), stored once it has a trusted clock; `null`
when it never had one, for a boot recorded by an older build, and when long
version names leave no room in `life/boots` (it keeps its old maximum size).
`boot` counts
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
  "slots": [{"slot": 0, "empty": false, "name": "Onocoy", "protected": true}],
  "protected": {"enabled": true, "onocoy_ready": true, "down_today_s": 4, "budget_left_s": 469, "budget_s": 473,
                "data_rate_permille": 1000, "graced_uptime_permille": 503, "streak_ok": true,
                "graced_uptime_so_far_permille": 1000},
  "today": {"utc_day": 20736, "date": "2026-10-10", "down_s": [4, 0, 0, 0], "planned_s": [0, 0, 0, 0],
            "reconnects": [1, 0, 0, 0], "up_s": [43196, 0, 0, 0], "power_off_s": 0, "rx_overflows": 0,
            "lost_epochs": 0},
  "history": []
}
```

Per-slot arrays have `max_slots` entries. `history` holds up to 30 past days
in the `today` shape, newest first. `power_off_s`: gaps before a boot after
a power cut (not one whose receiver kept running: the ESP32's reset pin,
`station.rx_boot`). `up_s` counts seconds a slot streamed
with fresh RTCM behind it (online; 0 for days stored by a build that did
not count it); `down_s` seconds it should have streamed but did not;
`planned_s` the planned part of it: casters
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
trusted time exists (`utc_second_of_day` is then `null`). `slots[].empty`
as `casters[].empty` in `/api/status` (no host, not enabled; its `name` is
then `""`). `protected`: the day of the Onocoy slot in use, which is the
first enabled protected slot, else the first protected one (a disabled
Onocoy slot left over in a lower slot never hides the enabled one;
`null` without any protected slot): `down_today_s` its offline time
today, `budget_s` the day's offline allowance at the best data rate the
day can still reach, `budget_left_s` what is left of it, and the other
figures as in the caster status (section 10). The budget is information
only. `today.lost_epochs` (only in `today`: a finished day does not keep
it) the protected slot's epochs lost while online, across restarts:
the losses that budget is figured from.

In `protected`, `enabled` and `onocoy_ready` (the slot holds the console's
mountpoint, username and password) are as in the caster status, and so is
`graced_uptime_so_far_permille`. Every figure but `down_today_s` is `null`
while the slot is off or not set up, or while the day's figures are not
known yet.

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

### `GET /api/diag/frame-times`

Bench builds only (the receiver link's bench hooks,
`CONFIG_GNSS_LINK_BENCH_HOOKS`): the last recording of the bench console's
`ftimes` (frame and drain-pass times of the receiver link) as a
`text/plain` CSV attachment (`frame-times-<version>.csv`). `404 not_found`
in other builds. Read on the worker like the other downloads. Times are
`esp_timer` microseconds; the first line pairs them with the system clock
(`unix_ms` empty and `clock=none` while SNTP has not set it; the clock is
never the one derived from the receiver's own epochs):

```
# esp_timer_us=123456789,unix_ms=1760000000000,clock=sntp,records=5021,full=0,running=0
# F,arrival_us,read_us,published_us,msg,len,sys,epoch,mmb,held,exact
# P,begin_us,end_us,bytes,capped,next_wait_ms,gap_us
# S,begin_us,end_us,where
P,120400118,120400410,1180,0,20,941204
F,120396877,120400150,120400221,1077,402,gps,270524000,1,0,1
S,130000000,135000012,rx
```

`F` one RTCM frame: when its last byte arrived (`arrival_us`), when the
read that completed it returned, when the sinks returned (read to
published), message number, length, for an MSM the constellation
(`gnss_sys_t`) and the epoch time field as sent, its multiple-message bit,
`held` (a lease held it back, not published) and `exact`. `arrival_us` is
the time at which the UART interrupt moved the chunk holding the frame's
last byte into the driver's ring buffer (`exact` 1); its resolution is one
FIFO chunk, about 10 ms at 115200 Bd. When no interrupt time is known for
the frame, `arrival_us` is `read_us` and `exact` is 0.
`P` one drain pass that read data: begin, end, bytes, whether it stopped
at the drain cap (the next event wait is then 0), that next wait, and the
time since the previous pass ended (the reader was away). `S` a bench
stall: `rx` (the RX task) or `raw` (inside a RAW-lease read). The
console's `ftimes summary` line gives, right after its read-to-published
percentiles, the arrival-to-read percentiles (`arrival to read p50 A p95 B
p99 C p99.9 D max E us (n N)`, over the frames with `exact` 1, or `arrival
to read: none` without one) and how many frames had no interrupt time
(`M frames without an interrupt time`); its epoch delay per constellation
counts from the arrival of each epoch's first frame (frames with `exact` 1
only).

### `PUT /api/diag/log-level`

`{"tag": "station", "level": "debug"}`: `tag` <= 15 characters or `"*"`;
`level` `none`, `error`, `warn`, `info`, `debug`, `verbose`. Not persisted.
`200 {"ok": true}`. A level above what this build logs
(`CONFIG_LOG_MAXIMUM_LEVEL`; `level_max` in `GET /api/logs`, `info` in
release builds) is refused: `400 invalid_field` (`level`, message "This
build logs at most info"), since those lines are not in the image. `"*"`
sets every tag; a few tags keep a fixed level through it: the HTTP
server's own (`httpd_uri`, `httpd_txrx`, `httpd_ws`) stay at `error`
(section 18). Setting such a tag by name holds until the next `"*"` or
restart.

### `GET /api/logs?cursor=N[&boot=prev][&format=text]`

`{"cursor": 51234, "more": false, "gap": false, "level_max": "info", "text": "..."}`
with at most 8 KB of whole lines newer than `cursor` (start with 0; pass the
returned `cursor` back; `more: true` = call again at once). `level_max` is
the most detailed level this build logs (`CONFIG_LOG_MAXIMUM_LEVEL`): the
log-level picker offers nothing above it. `gap` = lines were
overwritten before they were read. `boot=prev` reads the previous boot's
ring. It is empty after a power cut or an app image change, and also when
the lines this image finds are older than the last boot: after another
image ran in between (an update or a rollback, then back), this image's own
lines from before that run are reported as unavailable, never shown as the
previous boot's or with an "older" label (checked against the image
generation at boot). Both flags come
from `diag_log_read_ex()`. `format=text`
streams the whole ring as `text/plain` with
`Content-Disposition: attachment; filename="log-<version>.txt"`. The
cursor counts the ring's records, not lines (a line can take more than one
record). Control characters other than tab and the line end are stored as
`?` (the Wi-Fi driver prints a NUL inside its country line), so the
download is plain text.
A `cursor` that is not a number answers `400 invalid_field` with
`"field": "cursor"`.

### `GET /api/journal?cursor=N`

```json
{"cursor": 77, "more": true,
 "entries": [{"seq": 77, "time_unix": 1760000000, "uptime_s": 3, "kind": "boot", "a": 1, "b": 4}]}
```

At most 64 entries per call, oldest first, `seq > cursor`; a cursor past
the newest, or from before the journal started over, starts again from the
oldest. Pass the returned `cursor` back (`more: true` = call again at once).
A `cursor` that is not a number answers `400 invalid_field` with
`"field": "cursor"`. `time_unix` `null` = no trusted clock then. `a` and `b`
by `kind` (integers; only a `wifi_disconnect` entry's `b` can be `null`):

| `kind` | `a` | `b` |
|---|---|---|
| `boot` | reset class | planned reboot reason |
| `session` | previous session uptime s (-1 unknown) | raw reset reason |
| `reboot` | reason | — |
| `ladder_step` | ladder (0 GNSS, 1 network, 2 caster) | `(outcome << 8) \| code` (outcome 0 started, 1 healthy again, 2 failed or refused, 3 abandoned, 4 not run because of the recovery mode, 5 held; network: `(reason << 16) \| ...`, and code 0 = a pass started with the trigger bits as the reason: 1 protected caster down, 2 associated without an IP, 4 a slot not reaching its caster). GNSS code 9 with outcome 5: the ESP32 restart is held back for now, `b = (hold << 16) \| (5 << 8) \| 9` with the hold bits 1 `setup`, 2 `ap_client`, 4 `admin`, 8 `power_on` (as `health.reboot_hold`). GNSS code 9 with outcome 0: the restart was blocked, `b = (why << 16) \| 9` with `why` 1 the hourly limit of the reboot cap (at most one an hour), 2 its daily limit (at most 3 in 24 hours), 3 a restart lock (another job, such as an update, holds restarts back), 0 not recorded (a record from an older build); codes 6 and 8 with outcome 0 keep their meaning; see `diag.h`, `network.h` |
| `app_update` | result (`esp_err_t`) | — |
| `app_revert` | trial stage that gave the image up (`fw_update_trial_t`; 0 = the user's rollback) | 1 = the user's rollback; a stage-1 rollback the bootloader did: the attempt (1, 2) |
| `gnss_flash` | the session's outcome (`station_flash_outcome_t`), written when its journal closes | transfers under the journal |
| `safe_mode` | episode | — |
| `wifi_disconnect` | `wifi_err_reason_t` | RSSI at the drop in dBm (-127 to -1); `null` when unknown (stored as 0; also the -128 of records from earlier builds) |
| `data_loss` | counter id (`diag_loss_id_t`) | increase since the previous record for that id |
| `caster_down` | slot (bits 0-7) \| end class (`ntrip_fail_class_t`, 0 = not a caster failure; bits 8-15) \| short sessions not journalled since the previous `caster_down` (bits 16-30) | session length s |
| `caster_up` | slot (bits 0-7) \| failed attempts since the previous session (bits 16-30) | outage s |
| `caster_stop` | slot (bits 0-7) \| stop reason (`ntrip_stop_t`, bits 8-15): the Onocoy login guard stopped the slot until the user acts (once per stop) | 0 |
| `wifi_up` | connect attempts during the outage | outage s |
| `wifi_roam` | the station moved to another access point of its network and got its address there (no `wifi_disconnect` or `wifi_up` for it): channel before (bits 0-7) \| channel after (bits 8-15) \| who started it (bits 16-23: 1 the access point asked, 2 the station found a clearly stronger one, 3 asked for on the bench console) \| bit 24: its reconnect found no stronger one after all and it stayed \| bit 25: the address and the connections were kept (a channel of 0 = unknown) | RSSI before (bits 0-7) \| after (bits 8-15), each a signed byte in dBm, 0 = unknown \| seconds without an address, with bit 25 without a link (bits 16-30) |
| `rx_maintenance` | 1 entry, 0 exit | chosen duration s (entry), time spent s (exit) |
| `rx_boot` | after a power-on or brownout reset, whether the receiver kept running (`diag_rx_boot_t`): 1 kept running (only the ESP32 was reset, its reset pin, or only its supply dipped), 2 restarted with it (the power was cut, or dipped for both), 3 unknown; written once per power-on or brownout boot, when the station knows (`station.rx_boot` in `/api/health`); only a power-on boot's 2 counts toward `power_lost` | what told it (`diag_rx_boot_by_t`): 0 nothing, 1 its restart banner before any data, 2 its version log's time since its restart, 3 RTCM or NMEA at once |
| `live_view` | event (bits 0-7: 1 opened, 2 extended, 3 closed, 4 satellite-in-view output on, 5 off) \| detail << 8 (closed: end reason; on/off: 0 accepted, 1 rejected, 2 no answer, 3 not sent) | opened: window s; extended: s left; closed: s open; on/off: ms the lease held RTCM back |
| `sg_test` | signal groups: event (bits 0-7: 1 a comparison started, 2 it switched the receiver, 3 the receiver refused a signal group, in a comparison or any apply, 4 a comparison ended) \| master << 8 \| slave << 16 (started: the group measured first; switched: the group now running; refused: the refused group; ended: the group it ended on) | started: s per group; switched: the phase (0, 1); refused: `(master << 8) \| slave` of the group the profile went back to (-1 none); ended: the outcome (1 kept, 2 timed out, 3 cancelled, 4 rejected, 5 failed, 6 interrupted) |
| `fixed_check` | the fixed-position check: event (bits 0-7: 1 the fixed position is more than 50 m from the receiver's own solution, horizontally or in height: the `fixed_far` alert raised; 2 within the limits again: cleared; 3 the fixed position changed, or the profile surveys now, while it stood: cleared; 4 the fixed height is the receiver's height above sea level: the `fixed_far` alert raised, or its reason changed from event 1; event 1 after event 4 likewise) \| the fixed height minus the receiver's in m << 8 (signed, bits 8-31) | the horizontal distance in m (0 for event 3) |

Enum numbers are the C values; the UI decodes them with the appendix
tables.

### `DELETE /api/journal`

Empty body. Clears the event journal: `200 {"ok": true}` once every record
written or queued before the request is gone. Later records go on with the
next `seq` (never reused), so a client's cursor stays valid and simply
finds nothing older. It erases each journal sector once (32 sectors of 4 KB;
PROVISIONAL: a second or two, to be measured on hardware, longer on a worn
flash). The clear runs on the server's worker task (like the uploads and
the file downloads), so other requests and the WebSocket's status pushes
go on meanwhile; a concurrent `GET /api/journal` or `/api/connections`
waits for one sector erase at most and never sees part of the old records,
and `GET /api/health` keeps `journal.available` true (held longer than a
second, its counts are those of the previous request).
Errors: `404 not_found` (no usable journal partition:
`health.journal.available` false), `409 busy` (another clear is waiting
or runs, or an upload is being received: it holds the worker), `500
internal` (a flash error; `message` says which: `ESP_FAIL` the clear could
not start, the sector it opens first could not be erased or written: not
cleared, the records stay, except in a full journal the oldest sector's
(127 records), which the ring was about to drop anyway; `ESP_ERR_NOT_FINISHED` a later
erase failed: the records are gone from every read all the same; a sector
that could not be erased brings its records back at the next boot, as
does a power cut during the clear).

What clearing resets: the journal's records (`GET /api/journal`), the
connection history (`GET /api/connections`, empty and `complete`), the
`power_lost` alert's count of power losses (it starts again from the
records written after the clear, so the alert goes off), and
`health.journal.oldest_seq` and `newest_seq` (0 until the next record).
What it keeps: the start history (`health.boot_history`, settings
`life/boots`), the availability ledger (`GET /api/availability`), the logs,
the `seq` numbering, `health.journal.sector_cycles` (it counts the clear's
erases too) and the journal counters since boot.

### `GET /api/connections`

The connection history next to the ledger: the newest 64 records of
`caster_down`, `caster_up`, `caster_stop`, `wifi_up` and `wifi_disconnect`,
newest first, read backwards through the journal.

```json
{"complete": true,
 "entries": [{"seq": 812, "time_unix": 1760000000, "uptime_s": 3600, "kind": "caster_down",
              "slot": 0, "slot_name": "Onocoy", "class": "remote_close", "count": 0, "duration_s": 3600,
              "reason": null, "rssi": null, "stop_reason": null}]}
```

| Field | Notes |
|---|---|
| `complete` | every journal record was looked at (the list holds every connection record the journal keeps) |
| `time_unix` | `null` without a trusted clock then: `uptime_s` dates it after its boot |
| `slot`, `slot_name` | caster records: the slot and its name now (`null` without one, and in safe mode); `null` for Wi-Fi |
| `class` | `caster_down`: why the session ended (`ntrip_fail_class_t`; `none` = not a caster failure: a pause, a configuration change, a health restart, the receiver's RTCM stopping, the network going down); `null` otherwise |
| `count` | `caster_down`: shorter sessions not journalled since the slot's previous `caster_down`; `caster_up`: failed attempts since the previous session; `wifi_up`: connect attempts during the outage; `null` for `caster_stop` and `wifi_disconnect` |
| `duration_s` | `caster_down`: session length; `caster_up`, `wifi_up`: outage length; `null` for `caster_stop` and `wifi_disconnect` |
| `reason`, `rssi` | `wifi_disconnect`: `wifi_err_reason_t` and the RSSI at the drop in dBm (-127 to -1; `rssi` `null` when unknown, as the journal's `b`); both `null` otherwise |
| `stop_reason` | `caster_stop`: why the Onocoy login guard stopped the slot (`ntrip_stop_t`); `null` otherwise |

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

**mDNS details.** The responder answers A and AAAA queries for the name:
each interface (station, AP) gets an IPv6 link-local address when its link
comes up, and no other IPv6 address (no SLAAC), so a dual-stack phone or
computer gets its AAAA answer at once instead of waiting out a timeout; the
web server answers on that address too (a phone on the AP that reaches it
over IPv6 counts as on the AP, section 3). The web page is advertised as
an `_http._tcp` service (TXT `path=/`) whose instance name is the host
label (`mdns_host`), so it follows a rename after a name conflict. Before a
planned restart (while the network is still up) or after a rename, the
station withdraws its web service's PTR record with TTL 0, so DNS-SD
browsers drop the entry at once. The pinned mDNS component does not
withdraw the service's SRV and TXT records or the old host name's A/AAAA
records: they expire from caches within their 120 s TTL, and until then a
client using the old name reaches the station and gets `421 bad_host`.
The QEMU development build answers as `onocoydiy-qemu` (its own host name:
QEMU's user-mode network carries its announcements onto the developer's
LAN).

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
   while the mDNS responder runs (`mdns_host`), else the router's device
   list; the AP comes back if the station could not join; on a guest
   network the BOOT button opens the station's own network again). A
   `PUT /api/wifi` that fails, and a join that keeps failing or goes back
   to the previous credentials, post `{"step": "wifi"}` again; a station
   that loses its address during the countdown keeps the AP up (fallback),
   and the UI says so and waits again instead of counting down.
4. The phone is back on the home network: the user opens
   `http://<mdns_host>.local/`, logs in, and the wizard resumes at
   `setup_step`.
5. Antenna check, before any caster is enabled. The UI reads
   `GET /api/status` first: with no receiver found (alert `no_receiver`,
   section 6) or while the station looks for one it shows the no-receiver
   card (what to check, the next search; "Check again" is
   `POST /api/gnss/detect`) and opens nothing. Once the receiver streams,
   `POST /api/gnss/live` opens the live view and the UI polls
   `GET /api/gnss/live`, with plain pass/warn hints (satellites tracked,
   mean C/N0, whether a constellation has 4 bands, which Onocoy's quality
   score needs and the most this receiver reaches (the fifth band is
   Galileo E5 AltBOC, which it lacks); guide values, PROVISIONAL, to be confirmed on
   hardware) and "Continue anyway"; `DELETE /api/gnss/live` when the user
   goes on.
6. Optional: the Onocoy slot, `PUT /api/casters/0` with `onocoy_preset`
   (host `servers.onocoy.com` and port 2101, shown, not editable) and what the
   Onocoy console gives: `mountpoint`, `username` and `password`
   (section 10).
7. The last screen (`setup_step` `casters`): the UI polls `GET /api/status`
   and shows the Onocoy slot's state as it happens (connecting, a refused
   login with `login_rejects` from `GET /api/casters`, streaming once
   corrections flow), the next steps in the Onocoy console, and a link to
   the Casters page for more casters.
8. `POST /api/setup/done` (it also erases `setup_step`): the dashboard, or
   the Casters page from that link.

Every step posts the next one (`POST /api/setup/step`) before it shows it.
Wi-Fi credentials can never be saved before an admin password exists
(every Wi-Fi call needs a session). Logged in on the home network, the
wizard shows the Wi-Fi step only while the page is on the station's own
access point or the station has no working Wi-Fi. A wizard that starts
(a reload, a new login) reads `GET /api/wifi` first: while the station
has no working Wi-Fi (none stored, or not connected) it resumes at the
Wi-Fi step whatever `setup_step` says. A stored `casters` step resumes at
the last screen, which has no fields; the UI never stores another value for
it.

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
| Caster edit, retry | ✓ | ✓ | `safe_mode` | ✓ | ✓ |
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
`unsupported_model`, `package_pinned`) whenever those hold.
While a signal-group comparison runs (section 8), every change of the
receiver answers `signal_test_running`: a profile save with `profile` or
`autocfg`, apply, lock, re-survey, receiver maintenance, receiver updates
(banner, confirm) and the flash actions. The comparison itself is refused
while the first configuration is held, automatic configuration is off,
during receiver maintenance, an open receiver update session or safe mode,
without a receiver (`no_receiver`), for an unsupported or unidentified
receiver, while the receiver does not stream, after a failed apply and
while the position is not locked. With the settings unavailable this boot,
every call that would persist something answers `settings_unavailable`,
except Wi-Fi credentials and the first admin password (both kept for this
boot), and a journal in state `unknown` allows no receiver update action.

---

## 18. Server limits and implementation notes

- **Server:** `esp_http_server`, task priority 5, 8 KB stack in internal
  RAM; `max_open_sockets` 7 (the lwIP socket budget counts on it),
  `max_uri_handlers` 72 (`WEB_MAX_URI_HANDLERS`; a static assertion keeps
  the API routes, `/ws` and the catch-all within it) with wildcard
  matching for `/api/casters/*`,
  `max_req_hdr_len` 2048 (cookies and long browser headers), receive
  timeout 4 s (a client that sends nothing that long is dropped while it
  sends headers; a body read gives up after three in a row), send timeout
  10 s. A request with a body that nobody reads (every `GET` and `DELETE`,
  anything outside the API routes, `/ws` and its handshake, a request
  refused before its body, such as a refused Host or Origin or a missing
  session) is answered and the connection closed, instead of reading the
  body away on the server task. The answer is the one the request would
  get without its body (section 2: `404`/`405`, the captive redirect, an
  asset; `400 bad_request` on a route that takes no body), never `413`
  for a body nobody would read.
- **Connection: close and HTTP/1.0:** `esp_http_server` parses each
  request's HTTP version but does not keep it, so the receive override
  feeds what it reads to a small per-connection scanner (`web_conn.h`): it
  follows request lines, header lines and `Content-Length` bodies and
  notes, in order, whether each request line was HTTP/1.0, so pipelined
  requests keep their own. A handler that starts takes its request's note
  and reads the `Connection` header (a token list: `close`, `keep-alive`,
  without case). A request that asked to close, and every HTTP/1.0
  request, gets `Connection: close` on its answer, and then the server
  ends its side, drains briefly and drops the session; the worker does the
  same for a request it took over. HTTP/1.0 keep-alive is not offered: the
  client would need the `keep-alive` token echoed on every answer, so
  HTTP/1.0 connections are never kept. An answer that is streamed (JSON
  larger than the 2 KB response buffer, the upload results, and the
  downloads `/api/logs?format=text`, `/api/coredump`,
  `/api/diag/flash-capture` and `/api/diag/frame-times`) goes to an
  HTTP/1.0 request without `Transfer-Encoding`, which HTTP/1.1 forbids
  toward a 1.0 client. It carries the status line `HTTP/1.1 <code> <text>`,
  like the server's other answers; `Content-Type`,
  `X-Content-Type-Options`, `X-Frame-Options` and `Referrer-Policy`;
  `Cache-Control: no-store` on API answers; `Content-Disposition` on
  downloads; `Connection: close`; and `Content-Length` when the size is
  known up front (the core dump). Otherwise the body ends when the station
  closes the connection. To an HTTP/1.1 request the same answers go in
  chunks, as before, and the connection stays open unless the request
  asked to close it. WebSocket upgrades are not affected (the scanner stops
  at the handshake).
  WebSocket clients: at most 4, at most 2 of them without a session.
- **Chunked request bodies:** `esp_http_server` reports a request with
  `Transfer-Encoding: chunked` as one without a body (`content_len` 0) and
  would parse the chunks as the next request, answering that with a second
  `400`. Every handler therefore counts any `Transfer-Encoding` header as a
  body nobody reads (`web_req_body_of`): `411` on routes that take a body,
  `400` on routes that take none, the request's own answer in the
  catch-all and on refused Host, Origin or session checks, and always
  `Connection: close` with the connection ended.
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
  at most 64 KB or 2 s, before it closes. Without that read lwIP's receive
  window would stay shut: the client's upload stalls in front of the
  answer, and the station resets the connection at the client's next
  window probe; many clients then drop the answer still in their receive
  buffer (the browser shows "connection reset" instead of the 413). Only
  the bytes the socket reports queued (`FIONREAD`, which needs
  `CONFIG_LWIP_SO_RCVBUF`) are read, never the client's FIN behind them:
  the drain is for the body, and readable with nothing queued is that FIN,
  after which the close is a full one. An earlier drain of this server
  read until `recv()` returned 0, which took the FIN as well; when the
  client's FIN had come before ours, that shut lwIP's receive side while
  the connection waited in `LAST_ACK` for the client's last ACK, and lwIP
  then freed the connection on that ACK without telling the socket (lwIP
  bug #62141, open); the close after it worked on freed memory. Reading
  the FIN was this server's defect, fixed in the drain (`components/web/web.c`,
  `web_linger_close`; host-tested in `test/host/main/test_web_conn.c`
  against a model of the socket). The server's own warnings for these
  expected cases (`httpd_uri`, `httpd_txrx`, `httpd_ws`) are logged at
  error level only, as fixed levels (`diag_log_fix_level`) that a `"*"`
  change of the log level keeps; the handlers log the refusals that
  matter.
- **Connections the station ends first** (`Connection: close`, HTTP/1.0, the
  refusals above, idle ones it evicts) stay in TIME_WAIT on the station for
  10 s (2 x MSL; `CONFIG_LWIP_TCP_MSL` is 5 s instead of lwIP's 60 s). A new
  connection from the same client port within that time is not taken at
  once: lwIP answers its SYN with a bare ACK (not reopening from TIME_WAIT
  is upstream lwIP's choice, and optional: RFC 9293 section 3.6.1), and the
  client answers that ACK with a reset. lwIP alone ignores that reset (RFC
  1337's fix against TIME-WAIT assassination, an Informational RFC); RFC
  9293 section 3.10.7.4 ends TIME_WAIT on it, as Linux does by default, and
  so does this firmware: an lwIP hook ends the TIME_WAIT on a reset that
  carries exactly the next sequence number the station expects (RFC 5961's
  check), once the station's socket for that connection is closed
  (`components/network/net_tw.c`), and the client's next SYN is taken. Linux
  (5.10 and later) sends it a few milliseconds after its reset: a Linux 7.0
  client back on its port 1 to 9 s after the station's close, measured
  against this firmware in QEMU on a tap network, connected within 0.04 s,
  where it waited 2.1 to 10.2 s without the hook. A client whose reset does
  not come waits until TIME_WAIT ends: up to 15 s with Linux's SYN retries.
  (A SYN that falls inside the old connection's window gets a reset instead,
  and the client's `connect()` fails at once; a new initial sequence number
  lands there only by rare chance.) The fault was this firmware's: it held
  such ports for lwIP's default 120 s, and nothing in it acted on the
  client's reset. Linux clients (since 2022) take the local ports of their
  connections to one server from a starting point drawn anew every 10 s, so
  this happens now and then, at random: for each port the station holds, at
  most about one new connection in 14,000 lands on it (Linux's default range
  has 14,116 even ports, the ones `connect()` takes first). macOS hands its
  ports out in order across the whole system and was not seen to come back
  on one. Other systems, Windows among them, were not checked (neither their
  ports nor whether they send that reset); one lwIP user reports clients of
  a web server on lwIP coming back on their port within 24 s, more often
  Chrome on Windows 11 than others (lwip-users list, "Bug in TCP connect", 4
  November 2025). With lwIP's 120 s and no hook, a Linux script that polled
  with `Connection: close` would now and then wait up to two minutes, or see
  its `connect()` fail.
- **Client addresses in the log:** a wrong admin password (section 3) and a
  refused WebSocket handshake name the client: `WebSocket refused for
  192.168.1.23: no session, and public status is off` (also `Host or Origin
  not accepted`, `a body with the handshake`, `every WebSocket slot is
  taken`, `every public viewer slot is taken`). Each kind of line comes at
  most once a minute per client and reason (the 8 client and reason pairs
  seen last are tracked, so one tab's `no session` lines never hide a full
  slot met by another tab of the same client); the next line of that pair
  says how many it held back (`+k not logged`). IPv4 addresses (also
  v4-mapped) are written dotted, IPv6 in the compressed form.
- **Handlers never block for long.** Station, caster, network and update
  calls are asynchronous; a handler reads snapshots and returns. The
  password check (about 0.3 s at the default 3000 iterations; about 1 s
  for a password kept from a build with 10 000, PROVISIONAL) runs on the
  server task: the per-client throttle bounds how often one client can
  make it run, and moving it to the worker would make login asynchronous
  and queue it behind uploads. Uploads and
  downloads run as async requests (`httpd_req_async_handler_begin`) on one
  worker task with an internal-RAM stack (flash writes), so status pushes
  continue meanwhile.
- **Memory:** large snapshots (`station_status_t`, caster status) are read
  into PSRAM or static storage, never onto the server stack. JSON is
  written by a streaming writer into a 2 KB PSRAM buffer and sent with
  `httpd_resp_send_chunk` (to an HTTP/1.0 request: raw on the socket after
  a head of the station's own, the body ending at the close, section 2
  item 6), so no response needs a large contiguous block
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
  captive redirects): the AP's IPv4 address (also v4-mapped) or its IPv6
  link-local address.
- The web component also needs `um98x_ctl` (profile codec, validation and
  WGS84/ECEF conversion) and `um98x_flash` (`um98x_flash_inspect_staged`)
  in its `PRIV_REQUIRES`; both are lower layers.

**UI assets** (the files the UI provides): `ui/index.html`, `ui/app.css`,
`ui/favicon.svg` and the scripts `ui/js/*.js`, which the build
(`components/web/tools/ui_bundle.py`) joins in file-name order into one
`app.js` inside a strict-mode function, without their comments. The build
checks the page, styles and scripts (structure, unique ids, and what the
CSP of section 2 would block: no inline scripts or styles, no `on*` or
`style` attributes, no other origins, no `eval`/`innerHTML`-style sinks),
gzips every asset and fails when the compressed total exceeds the budget
in `components/web/CMakeLists.txt` (150 KB). No other files are served.

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
7. **Signal-group comparison**: resolved; `station_sg_test_*()` (station.h,
   `station_sgt.h`) and the four `/api/gnss/signal-test` routes (66 API
   routes, 68 of the 72 URI handlers with `/ws` and the catch-all).
8. **GNSS time**: resolved in part. The ledger takes the receiver's GPS
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
| `station_alert_t` | `unsupported_model`, `config_failed`, `position_mismatch`, `antenna_moved`, `receiver_attention`, `receiver_in_loader`, `no_receiver`, `signals_mismatch`, `fixed_far` |
| `station_fix_state_t` | `none`, `checking`, `ok`, `far`, `no_solution`, `refused` |
| `health_level_t` | `ok`, `degraded`, `recovering`, `failed` |
| `health_step_t` | `none`, `resync`, `link_restart`, `rescan`, `reconfigure`, `receiver_reset`, `reboot` |
| `health_step_phase_t` | `none`, `running`, `waiting` |
| `health_alert_t` | `power_supply`, `receiver_power`, `bands`, `profile_drift`, `heap_fragmented`, `safe_mode`, `settings_degraded`, `no_upstream`, `link_loss`, `power_lost`, `receiver_silent`, `no_satellites`, `no_rtcm`, `rx_held` |
| `health_recovery_mode_t` | `full`, `no_reboot`, `alerts_only` |
| `ntrip_state_t` | `disabled`, `wait_network`, `wait_data`, `connecting`, `streaming`, `backoff`, `auth_failed`, `paused`, `config_error`, `banned`, `stopped` |
| `ntrip_fail_class_t` | `none`, `dns`, `connect`, `tls`, `timeout`, `remote_close`, `write_stall`, `keepalive`, `auth`, `mount_busy`, `config`, `banned`, `login_blocked`, `in_use`, `replaced`, `local` |
| `ntrip_trust_t` | `bundle`, `custom_ca`, `insecure` |
| `ntrip_backoff_profile_t` | `fast`, `standard`, `polite` |
| `ntrip_stop_t` | `none`, `rejected`, `login_blocked`, `in_use`, `kicked`, `duplicate`, `banned` |
| `um98x_base_mode_t` | `survey_in`, `fixed` |
| `um98x_pos_source_t` | `none`, `survey`, `averaged`, `user` |
| `um98x_setting_t` | `unknown`, `on`, `off` |
| `um98x_bands_state_t` | `unknown`, `ok`, `refused`, `unread` |
| `um98x_preview_kind_t` | `query`, `write` |
| `um98x_preview_when_t` | `always`, `if_differs`, `if_off` |
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
| `lifecycle_reset_class_t` | `power_on` (on the ESP32-S3 also a reset by its EN pin: the USB auto-reset, the RST button), `planned`, `crash`, `brownout`, `other` |
| `lifecycle_reboot_reason_t` | `none`, `user`, `factory_reset`, `admin_reset`, `app_update`, `app_revert` (a failed trial, also one the bootloader rolled back), `gnss_ladder`, `network_ladder`, `low_heap`, `safe_mode_retry`, `safe_mode_cleared`, `nvs_slot_switch`, `boot_failure`, `app_rollback` (the user's rollback) |
| `esp_reset_reason_t` | `unknown`, `poweron`, `ext`, `sw`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deepsleep`, `brownout`, `sdio`, `usb`, `jtag`, `efuse`, `pwr_glitch`, `cpu_lockup` |
| `ledger_clock_t` | `none`, `sntp`, `gnss` |
| `gnss_sys_t` | `gps`, `glonass`, `galileo`, `sbas`, `qzss`, `beidou`, `navic` |
| `gnss_band_t` | `l1`, `l2`, `l5`, `l6`, `e5ab` |
| `diag_event_t` | `boot`, `reboot`, `ladder_step`, `app_update`, `app_revert`, `gnss_flash`, `safe_mode`, `wifi_disconnect`, `data_loss`, `session`, `caster_down`, `caster_up`, `wifi_up`, `caster_stop` (1-14); `rx_maintenance` = 20, `live_view` = 21, `sg_test` = 22, `rx_boot` = 23, `fixed_check` = 24, `wifi_roam` = 25 |
| `station_sgt_state_t` | `idle`, `switching`, `settling`, `measuring`, `deciding`, `restoring` |
| `station_sgt_outcome_t` | (none: `null`), `kept`, `timed_out`, `cancelled`, `rejected`, `failed`, `interrupted` |
| `station_sgt_res_status_t` | `pending`, `measuring`, `done`, `rejected` |
| `diag_loss_id_t` | `hw_fifo`, `rx_buffer_full`, `line_errors`, `rtcm_crc`, `framer_discarded`, `hub_publish`, `baud_switch`, `rx_events_dropped`; 16 and up: caster slots |
