# Running the firmware in QEMU

Espressif's QEMU emulates the ESP32-S3 well enough to boot the station
firmware, serve the web UI and talk on UART1, the receiver link. That is
enough to work on the UI, the web API, settings, the update check and the
receiver detection path without a board. Nothing here replaces bench tests:
there is no radio, no receiver and no real timing.

## Quick start

```sh
. $IDF_PATH/export.sh                                  # ESP-IDF v6.1
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa   # once, then export.sh again
tools/qemu/run.sh
```

`run.sh` builds `sdkconfig.defaults` plus the overlay `sdkconfig.qemu` into
`build-qemu/` (the station build in `build/` is untouched), writes a fresh
flash image and starts QEMU with the console on your terminal (Ctrl-A X
quits). It prints where things are:

```
Web UI:  http://localhost:8080/  (empty NVS: set the admin password on the first page)
UART1:   tcp 127.0.0.1:5556 (QEMU listens)
```

Every run starts with empty NVS: no admin password, default settings (the
flash image is written to a new file each run, so a QEMU left over from an
earlier run can never write into it). Other options (`tools/qemu/run.sh
--help`): `--no-build`, `--build-only`, `--headless LOG` (console to a
file, for scripts), `--seconds N`, `--http-port N`, `--uart1 MODE`,
`--no-wdt`.

The emulated watchdogs are live: unlike `idf.py qemu`, `run.sh` does not
turn off the timer groups' watchdogs, so the task watchdog (TIMG0, 10 s,
both IDLE tasks watched, panic) and the interrupt watchdog (TIMG1) fire as
on a board. `--no-wdt` turns them off, for a debugger session that stops
the CPUs only.

Never flash anything from `build-qemu/` to a board. Configure refuses a
release version with the overlay, the CI firmware job fails if the station
build has its options on, and the release job refuses to publish such a
build.

## What is different from a station

`CONFIG_NETWORK_QEMU_ETHERNET` (set by the overlay; it depends on the
OpenCores Ethernet driver, which exists only in QEMU) changes exactly this,
each inside an `#if` on that option:

- **Network.** Wi-Fi is never initialised or started. QEMU's emulated
  OpenCores Ethernet port is the station interface instead (same interface
  key and IP events, so the web server, the casters and SNTP work
  unchanged), with an address from QEMU's user-mode network (10.0.2.15).
  There is no setup AP, no scan, and the network recovery steps are skipped.
  Wi-Fi credentials saved in the UI are stored as the station's, not used
  and never tried (no pending credentials, no trial, no revert). The
  station's status shows the SSID `QEMU Ethernet`.
- **Web requests.** The Host names `localhost` and `127.0.0.1` are accepted
  (QEMU's port forward), and every request counts as coming over the setup
  AP: the first admin password can be set from the browser, and a request
  with a foreign Host header gets the setup AP's captive-portal redirect
  (to 192.168.4.1, which is not reachable here).

The overlay also changes what QEMU cannot emulate (reasons in its comments):
quad instead of octal PSRAM, DIO instead of QIO flash, and software instead
of hardware SHA/AES/MPI.

Expected log lines that a station does not print: three `E` lines about the
multicast MAC filter (the emulated MAC has none) and `QEMU development
build: emulated Ethernet instead of Wi-Fi, no setup AP`. The mDNS responder
runs as on a station (`network: mDNS: answering as onocoydiy.local`), on
the emulated Ethernet port; QEMU's user-mode network carries no multicast
to the host, so the name itself cannot be looked up from there, but a
request with `Host: onocoydiy.local` passes the Host check like on a board.

With nothing answering on UART1 the firmware behaves as with a dead
receiver: detection gives up after about 26 s. The GNSS recovery ladder's
ESP32 restart is then held (`health: no receiver: restart to look for it
postponed (...)`): while first setup is not finished and no caster is
enabled, and for the first hour of station time after power-on when
nothing was heard from the receiver, so `lifecycle: rebooting (reason 6)`
does not appear in a QEMU run. During that no-receiver episode
`GET /api/status` shows the reasons in `health.reboot_hold` (outside a
ladder episode it names none).

## UART1 (the receiver link)

In Espressif's `esp32s3` machine the second `-serial` is UART1:
`hw/xtensa/esp32s3.c` connects `uart[1]` straight to `serial_hd(1)`. The
GPIO matrix and the IO MUX play no part (the IO MUX is an unimplemented
device there), so GPIO1/GPIO2 in the firmware's pin map make no difference.
`run.sh` passes that second serial port; `--uart1` picks its backend:

| `--uart1` | What it is |
|---|---|
| `listen[:PORT]` (default `listen:5556`) | QEMU listens on 127.0.0.1:PORT; connect any time, e.g. `nc 127.0.0.1 5556` |
| `connect:PORT` | QEMU connects at start to a program already listening on 127.0.0.1:PORT, so that program sees every byte from the first boot |
| `pty` | a pseudo-terminal (QEMU prints its path); open it with `screen` or pyserial |
| `file:PATH` | only what the firmware sends, written to PATH |
| `none` | not connected |

Both directions work. The firmware's detection queries arrive on the host
side (`\r\nVERSIONA\r\n`, a few per baud it tries, from about 4 s after
boot), and bytes the host writes reach `gnss_link`: send any text during
detection and it ends with `receiver not found (bytes but nothing valid)`
instead of `(no bytes at all)`. The firmware's baud changes have no effect
on the socket (the model passes bytes through at any baud).

**Bytes sent while nothing is connected are lost.** The UART model
(`hw/char/esp32_uart.c`, `uart_transmit`) drops its TX FIFO when the
character backend is not open ("drain the fifo instantly, if the char
device backend is not connected"), as a wire that is not plugged in would.
With `listen`, a client that connects after the firmware has sent
something never sees it; an earlier attempt to capture UART1 got 0 bytes
for that reason. Measured with the same QEMU arguments: a client connected
1 s after start received the whole first detection pass (132 bytes, 11
`VERSIONA`); one connected after 28 s lost that pass and saw only the next
one. Use `connect:PORT` (or `file:`) when every byte matters.

A real receiver behind a USB-serial adapter cannot stand in for the socket:
the model never passes the firmware's baud changes to a host serial port,
so detection at other bauds would not match the wire.

## Smoke test

```sh
tools/qemu/run.sh --build-only
uv run tools/qemu/smoke.py            # logs in build-qemu/smoke/
```

It listens for UART1 (`--uart1 connect:<port>`), boots the image headless
and reports PASS or FAIL for each of these (exit status 1 if any failed):

1. HTTP 200 with the UI page on `/`;
2. the first admin password can be set (`POST /api/setup/admin`) and
   `GET /api/status` with that session is valid JSON, API version 1, the
   station online over the emulated Ethernet, `health.reboot_hold` with
   `setup`; `GET /api/session` has `hostname` and `mdns_host`
   `onocoydiy`, a request with `Host: onocoydiy.local` gets 200 and one
   with `Host: evil.local` does not;
3. UART1 both ways: the detection queries arrive on the host, and text the
   host sends makes detection end with "bytes but nothing valid";
4. over 75 s (`--observe`, at least 45): no `Guru Meditation`, `abort()`,
   stack overflow, failed assert, backtrace or task watchdog report on the
   console, and no reboot loop (a restart without the firmware's own
   `rebooting (reason N)` line before it is a crash; more than two boots is
   a loop); no GNSS ladder restart (`rebooting (reason 6)`), no console
   note about dropped lines (`[diag:`), and the mDNS line in the boot log;
   at the end of the window `GET /` and `GET /api/status` still answer, the
   console writer task runs (`GET /api/diag/tasks`) and `GET /api/health`
   counts no console drop. QEMU's UART sends every byte at once, so a log
   burst cannot back the console up here; the writer's ordering and drop
   rules are host-tested (`test/host/main/test_diag_console.c`).

QEMU always stops with it: on a normal end, Ctrl-C, SIGTERM or SIGHUP the
script stops QEMU on the way out, and `run.sh` gets `--seconds` (observe or
timeout, whichever is longer, plus 60 s) as a backstop should the script be
killed outright.

The `qemu-smoke` CI job runs the same in the pinned ESP-IDF image (which
ships qemu-xtensa) with `--out qemu-smoke` and uploads that directory and
the build log as an artifact.

## One host thread for both cores

`run.sh` passes `-accel tcg,thread=single`. With QEMU's default for this
machine (one host thread per emulated core), this QEMU build
(esp_develop_9.2.2_20260417) crashed on Linux within seconds of boot: host
heap corruption, reported by glibc as `malloc(): unsorted double linked
list corrupted`, or a SIGSEGV whose core dump stops in QEMU's MMIO dispatch
(`int_ld_mmio_beN`, a read of the system timer at 0x60023004) while the
other core's thread was translating code. Measured in the CI image on
aarch64, one 30 s run at a time with UART1 listening and no client: 5 of 13
runs crashed with one thread per core, none of 13 with one thread for both;
the smoke test then passed in each of its 5 runs there. The crash moved
with timing (none when four QEMUs shared the machine, none under gdb),
which points at a race inside QEMU rather than at anything the firmware
does: the guest code is the same in every run. On macOS no crash was seen
either way.
