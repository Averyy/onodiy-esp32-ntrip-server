# Reusing the receiver components

How another ESP-IDF application uses this repository's receiver components
without the rest of the station. Back to [development](development.md).

The reference is [`examples/external-receiver`](../examples/external-receiver):
a minimal application that CI copies out of the tree and builds against the
checkout (`external-app` job, `tools/ci/external_app.sh`) with the other
CI jobs (pushes to `main`, pull requests, tags); a release needs it. It is
build-tested only: this repository does not run it on hardware.

## What is supported

Eight components, used unchanged from a pinned revision:

| Component | What it is | Needs (from here) |
|---|---|---|
| `gnss_types` | Types only (`um98x_info_t`, ...) | nothing |
| `gnss_framing` | Byte stream to RTCM3 frames and checksum-valid text lines; the loader prompt detector; an RTCM3-only scanner for untrusted input (`gnss_rtcm_scan.h`). Pure C, no ESP-IDF | nothing |
| `board` | Board table, pin validation, saved receiver wiring | nothing |
| `settings` | NVS store (generic get/set/blob/batch) and the station's settings schema | nothing |
| `gnss_link` | Sole owner of the receiver UART: RX task, sinks, STREAM/COMMAND/RAW leases, writes | `gnss_framing`, `board`, `settings` |
| `gnss_obs` | Observations from the stream (MSM, 1005/1006, 1033, GSV) and the receiver's solution sentences field by field (GGA, GST: `gnss_obs_get_solution`) | `gnss_framing`, `gnss_types` |
| `um98x_ctl` | Unicore N4 command layer: detection, identity, commands, mode read-back, the station's base configuration | `gnss_link`, `gnss_types`, `settings`, `gnss_obs`, `gnss_framing` |
| `leds` | The board's LEDs (status RGB, Wi-Fi, GNSS): the station's blink codes, or a product's own patterns (`leds_set_pattern`) | `board` |

`um98x_ctl` therefore needs the first seven; `leds` only `board`. The ESP-IDF
components they use are listed in each `CMakeLists.txt` (`esp_driver_uart`,
`esp_driver_gpio`, `esp_driver_ledc`, `esp_partition`, `spi_flash`,
`nvs_flash`, `esp_event`, `esp_timer`). Nothing else in `components/` is
part of this contract: the station, web, casters, hub, updaters and
diagnostics.

None of them starts station behaviour on its own: the station supervisor,
casters and web server live in other components. `um98x_ctl` holds the
station's base profile and apply session, which run only when called
(`um98x_ctl_apply_profile`, `um98x_ctl_restore_output`).

## Pinning a revision and upgrading

- Take the components from a clean checkout at a commit (a release tag), each
  by its path, and keep the rest of the tree out of the build:

  ```cmake
  foreach(c board settings gnss_types gnss_framing gnss_link gnss_obs um98x_ctl leds)
      list(APPEND EXTRA_COMPONENT_DIRS "${STATION_DIR}/components/${c}")
  endforeach()
  set(COMPONENTS main)
  ```

- Before moving to a newer revision, read `CHANGELOG.md` from the pinned
  version up: a change that breaks a public API of these components is called
  out there.
- `tools/ci/external_app.sh` fails when the example build takes any other
  component from this repository than these eight.

## Build requirements

- **ESP-IDF 6.1**, target `esp32s3`: what CI builds with (the pinned
  `espressif/idf` v6.1 image). The example warns on any other version.
- **sdkconfig** ([`sdkconfig.defaults`](../examples/external-receiver/sdkconfig.defaults)):
  - `CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2`: required.
    `gnss_link` parks and wakes its RX task on notification index 1 and
    refuses to build with fewer (a static assertion).
  - `CONFIG_FREERTOS_HZ=1000` and `CONFIG_UART_ISR_IN_IRAM=y`: as the station
    builds them, the configuration the components are tested in. With the
    UART interrupt in IRAM, `gnss_link` requires `CONFIG_ESP_TIMER_IN_IRAM`
    (a build error otherwise).
  - The components' own options keep their defaults: `Station: GNSS link`
    (baud, RX buffer, RX task priority 18, core 1, stack) and the board's
    pins. `CONFIG_GNSS_LINK_BENCH_HOOKS` depends on the station's
    `CONFIG_BENCH_CONSOLE`, which an outside build does not have: it stays off
    and no Kconfig warning appears (CI checks both).
- **Partition table**: the station's, `partitions.csv` for the 16 MB boards
  or `partitions-8mb.csv` for the NTRIP-X (8 MB), at offset `0x10000`, with
  the matching flash size. `board_init()` picks the board from a
  `board.<id>` marker partition in the table (`board.um980-ntrx` in the 8 MB
  table; none means the original UM982 board, `um982-alix`) and checks it
  against the chip's flash size. `settings` uses the `nvs` partition, where a
  station install keeps the shared keys below. The example's `CMakeLists.txt`
  generates these four options from `STATION_FLASH` (16MB or 8MB).
- **Image size**: the example is about 246 KiB, far below both app slots
  (4 MiB in the 16 MB table, 2.5 MiB in the 8 MB one).

## Start-up order

From the example's `app_main` and `receiver_start`:

1. `nvs_flash_init()`, `esp_event_loop_create_default()`, `settings_init()`.
   Never erase NVS on an init error: it holds the shared device keys.
   `settings_init()` checks the stored settings schema version and migrates
   the station's keys in place, as the station does; when it fails, every
   getter fails and the components run on their defaults.
2. The saved wiring: `settings_get_blob("gnss", "wiring", ...)`,
   `board_wiring_decode()`, `board_use_wiring()`, all before `board_init()`.
3. `board_init()`; the link may open only when `board_link_off_reason()` is
   `NULL`.
4. `gnss_obs_init()`, `um98x_ctl_init()`, `um98x_ctl_set_restart_cb()`,
   `um98x_ctl_set_port(board_get()->gnss_port)`.
5. `gnss_link_init()`. An error here still leaves the link initialised but
   closed: once started, it reopens the UART on its own schedule.
6. `gnss_link_set_sinks()`, then `gnss_link_start()`.
7. The product's own task for receiver transactions.

## Tasks, callbacks and ownership

**Sinks** (`gnss_link_sinks_t`) run on the link's RX task. They must not
block, allocate or log; from a sink only `gnss_link_get_stats`, `_get_mode`,
`_get_baud` and `gnss_link_prompt_seen` work. Wire them as the example does:

- `on_line`: `um98x_ctl_on_line()` first; a line it returns `false` for is
  receiver output (NMEA and the like) for the product, and for
  `gnss_obs_feed_line()`.
- `on_rtcm`: `gnss_obs_feed_rtcm()` and the product's own use. Frames are
  delivered in STREAM only; under a lease they are held back and counted.
- `on_prompt`: `um98x_ctl_notify_prompt()`.
- `on_reset`: the framer was reset (overflow, buffer full, baud change,
  resync, restart, lease release): what came before is discontinuous. For a
  lease release it runs on the RX task before `gnss_link_release()` returns
  (before `gnss_link_start()`, on the caller's task); the mode is still the
  lease's at that moment.

**The restart callback** (`um98x_ctl_set_restart_cb`) runs on the RX task
too, inside `um98x_ctl_on_line()`, once for each receiver restart counted:
its banner, its answer "The Board is resetting,..." and a version log nobody
asked for are one restart when they come within 20 s of each other. Restarts
the command layer causes itself (a signal-group change, `RESET`) are not
reported. To wait a reported restart out without a detection pass:
`um98x_ctl_query_version(lease, true, &info)` polls every second until 20 s
after the restart evidence.

**Receiver transactions** (`um98x_ctl_*`) run on the product's own task,
never in a sink. Calls are serialised: a second caller gets `ESP_ERR_TIMEOUT`
after 1 s. A full baud scan takes about 44 to 50 s, one command up to
15 s with its retries; every wait is cut into slices of at most 1 s and
feeds the task watchdog only if the calling task is subscribed to it.

**Leases**: transactions that configure the receiver need a COMMAND lease
(`gnss_link_acquire`). Without a lease, in STREAM, `um98x_ctl` sends only its
read-only queries (VERSIONA, MODE, BASEPOSA, SPPNAVA). RAW belongs to the
receiver updater. A stale lease handle is refused everywhere.

**Loader prompt**: arm the detector under the lease
(`gnss_link_set_prompt_detect(lease, true)`). Once it fires, the hit is
latched for the rest of the lease (`gnss_link_prompt_seen`) and no
`um98x_ctl` call under that lease writes anything more: the call in flight
ends at once and it and every later call return `ESP_ERR_NOT_ALLOWED`. The
latch is read right before each write, so a write already being handed to
the UART when the prompt comes still goes out. The
exceptions: `um98x_ctl_detect` keeps reporting it as `ESP_ERR_INVALID_RESPONSE`
with `out->prompt`, and `um98x_ctl_leave_loader` still sends the loader's
restart key. Details in `um98x_ctl.h`, "Loader prompt".

**Mode read-back**: `um98x_ctl_query_mode` returns the mode log's body as
the receiver sent it, rover modes included. The public rover sample gives
`MODE ROVER SURVEY,` with a trailing comma. Details in `um98x_ctl.h`.

**Correction ingress**: complete RTCM frames, one per call, with
`gnss_link_write_ex(NULL, frame, len, timeout_ms, &tx)` while the link
streams. The contract (task context, serialisation with commands and lease
grants, TX done versus receiver acknowledgement, timeouts, the
`GNSS_LINK_TX_NONE` / `_DONE` / `_UNCERTAIN` outcome) is in `gnss_link.h`,
"Writing". Writes are refused while a lease is held or hands off is on;
nothing is queued or replayed. Before taking a lease, the product closes its
own admission of corrections and lets a write already under way finish;
`gnss_link_acquire` also waits for it.

**Untrusted RTCM input** (a phone app): `gnss_rtcm_scan.h` finds the same
frames as the framer on real streams, at a bounded cost per byte whatever
the input, and reports the input it holds (`gnss_rtcm_scan_held`) and each
frame's stream offset (`gnss_frame_t.start`). The framer reports the same
for its own input (`gnss_framer_held`). Details in the headers.

## LEDs

`leds_init()` after `board_init()` (a board without LEDs is a no-op), then
either the station's codes (`leds_set`) or the product's own feedback:
`leds_set_pattern()` takes one looping pattern per LED (status colour steps,
Wi-Fi brightness steps, and the GNSS LED either following the receiver's
status input or its own on/off steps; `leds_pattern.h`). One driver owns the
pins, the LEDC timer and channels and the 25 ms timer, so never configure
those pins elsewhere. Setting an equal pattern again keeps its phase, so a
product can set it on every status update; what each pattern means (phone
connected, subscribed, a fresh or stale fix) is the product's policy. The
GNSS status input says the receiver has a position fix, not which kind.

## Receiver firmware updates (`um98x_flash`)

Optional, on top of the eight: `um98x_flash` with the two components it
needs, `diag` and `pkg_store`. The example builds this set with
`-D STATION_UPDATER=ON` (`tools/ci/external_app.sh` checks that build too;
its `example_update_receiver` is the sequence below, build-tested only). It
needs octal PSRAM (the package is checked and sent from a PSRAM copy) and
FreeRTOS run-time statistics for `diag`; both are set by the example when
the option is on. A product doesn't have to run `diag_init`.

At start, after `settings_init`: `pkg_store_init()`, `um98x_flash_init()`,
and **before any ordinary write to the receiver** read the journal:

| `um98x_flash_journal_get` | Meaning | What to do |
|---|---|---|
| `ESP_ERR_NOT_FOUND` | No update open | Normal start. Release a package left over (`pkg_store_release`), pinned or not |
| `ESP_OK` | An update is open | Send nothing ordinary. `um98x_flash_journal_plan` gives the recovery plan; without recovery actions the journal waits for the user (`um98x_flash_user_retry`, `um98x_flash_user_restart`) |
| `ESP_ERR_INVALID_SIZE` | Unreadable (damaged, read error) | Counts as open and waiting: send nothing. Nothing erases it for you; only the user's decision clears it (`um98x_flash_journal_close`) |
| `ESP_ERR_INVALID_VERSION` | From a newer schema | The same; it can be cleared the same way |
| `ESP_ERR_INVALID_STATE` | Settings unavailable this boot | State unknown: send nothing, nothing to clear |

A session, as the station runs it, all under one RAW lease
(`gnss_link_acquire(GNSS_LINK_MODE_RAW, ...)`, held throughout; the caller's
task, which the updater's waits feed if it is on the task watchdog):

1. `um98x_flash_identify`. `UM98X_FLASH_ID_NO_ANSWER` (receiver absent) or
   `_AT_LOADER`: no session, no journal, nothing written.
2. `um98x_flash_prepare` with the size and SHA-256 the user confirmed and the
   live identity: every pre-flight rule on a RAM copy. A wrong model, a wrong
   package, a downgrade or a bad checksum is refused here (`failed_rule`,
   `why`): nothing has been sent. Release the package.
3. `pkg_store_set_pinned(true)`, then `um98x_flash_run` with the journal's
   identity fields. It opens the journal and writes it before every state.
4. `um98x_flash_cancel` is honoured only before the receiver's reset.

How it ends (`um98x_flash_result_t.end`):

- `DONE` or `ABORTED`: `receiver` is the application's own version log after
  the session (model, firmware, build). The product compares that with what
  it expected and verifies its own profile **before** reopening corrections,
  then calls `um98x_flash_journal_close` and `pkg_store_release`.
- `NEEDS_USER`: the journal and the pinned package stay for the user.
- `NOT_STARTED`: nothing was sent and no journal exists. Unpin and release.

Recovery actions (`um98x_flash_recover`, the boot race `um98x_flash_race`)
run only in a build with `CONFIG_UM98X_FLASH_RECOVERY_ACTIONS` (off by
default). Without it both return `ESP_ERR_NOT_SUPPORTED` at once and touch
nothing: not the receiver, the staged copy or the journal.

On the 8 MB layout (no `gnss_pkg` partition) the package lives in PSRAM only:
after a restart it is gone and an open journal waits until the same package
is staged again.

Tested in `test/host` against a simulated receiver: wrong model and the
package rules (`test_pkg_check.c`), interruption and retry transitions, the
point of no return, needs-user and recovery plans (`test_fl_session.c`).

## Shared NVS keys

Namespace `gnss`, written as device state (no `SETTINGS_EVENT_CHANGED`):

| Key | Read by | Written by |
|---|---|---|
| `wiring` | the application at start (`board_use_wiring`) | the station's setup page only |
| `baud` | `gnss_link_init`, `gnss_link_restart`, detection (its first candidate) | `um98x_ctl_detect` when found at another baud; a successful apply; a failed apply where the receiver last answered |
| `model`, `fw` | the station's status | every successful identity: `um98x_ctl_detect` with the version log, `um98x_ctl_query_version`, an apply; only when they change |
| `applied_hash`, `applied_fw`, `rtcm1230_ok`, `sg_ok` | the station | a successful `um98x_ctl_apply_profile`, in one batch |
| `hold` | the station | written by the station; erased by a successful `um98x_ctl_apply_profile` |

These keys, and every other key of the station's namespaces
(`settings_ns_is_station()`: sys, wifi, gnss, ota, life, ledger, web,
lcast, cst0 to cst7), stay the station's: their layout and their migrations
belong to the station (`settings_init` stamps and migrates its schema). A
product reads and writes them only as the table says.

## Product preferences and resets

- Keep them in namespaces of your own (NVS names have at most 15
  characters). The station's factory reset (`settings_factory_reset`) never
  touches them, and nothing in these components writes or erases them.
- Version them yourself, with a version key in your namespace. The station's
  rule for its own stamp is a reasonable one to follow: a version newer than
  the code knows (after a rollback) is read as it is and nothing is deleted.
- Reset them with `settings_erase_product_namespace(ns)`. It refuses a
  station namespace (`ESP_ERR_INVALID_ARG`, nothing erased), so a product
  reset keeps the receiver wiring and identity, the receiver update journal,
  the app trial records and the device history (host-tested).
- A record of yours that cannot be read is yours to handle: nothing here
  erases it for you.

## Host tests

`gnss_framing.c`, `gnss_rtcm3.c` and `gnss_rtcm_scan.c` build with a host C
compiler alone, no ESP-IDF: [`host/framing_check.c`](../examples/external-receiver/host/framing_check.c)
feeds a recorded RTCM3 file through the framer in pieces of 1 to 97 bytes
and checks every frame's CRC-24Q again:

```sh
tools/ci/external_host_framing.sh    # cc, then the check on test/fixtures/rtcm/centipede_LAX_30ep.rtcm3
```

The pure cores of all eight components are covered by the station's own
host tests (`test/host`, ESP-IDF's linux target).

## Licence

All eight components are this project's original code under the MIT licence
(SPDX headers, [`LICENSE`](../LICENSE)), with no third-party code inside. An
image built from them also contains the ESP-IDF parts they link (Apache-2.0,
FreeRTOS under MIT, the C library under BSD licences, ...); the product
lists those for its own image, as [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)
does for the station's.

## Not established

- The example has not run on hardware here. The components' hardware status
  is in their headers (PROVISIONAL items).
- Other ESP-IDF versions, other targets and other partition layouts are not
  built or tested.
