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
`--no-wdt`. `--overlay FILE` adds one more sdkconfig overlay after
`sdkconfig.qemu` and builds into `build-qemu-<name>/` (`--overlay
sdkconfig.hold` builds into `build-qemu-hold/`, where the receiver's first
configuration waits for the user).

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

**A restart while bytes arrive on UART1 hangs the next boot** (QEMU only).
ESP-IDF restarts an ESP32-S3 by resetting both CPUs (`esp_restart_noos`);
UART1 and the interrupt matrix keep their state, and on a board the first
`uart_param_config()` of the next boot resets UART1 through its
peripheral reset bit while the startup's interrupt-matrix clear drops the
old CPU interrupt line at once. This QEMU does neither: UART1 keeps its
registers (interrupt enables and a full RX FIFO), and remapping a source in
the interrupt matrix (`hw/xtensa/esp32s3_intc.c`) does not lower the CPU
line that source drove before. RTCM pending at the moment of the restart
leaves UART1's old CPU interrupt (APP CPU, level 1) raised; when the next
boot's `uart_driver_install()` allocates that interrupt again for UART1,
its handler is entered without end while UART1's status reads 0, and the
boot stops before `gnss_link: UART1 open` (seen with gdb: `-s` on the
QEMU command line, every entry `INT_ST` 0, `INT_ENA` still holding the
previous boot's bits at `call_start_cpu0`). A restart with nothing
arriving on UART1 boots normally. `e2e.py` therefore pauses its replay
(`ReplayServer.pause()`: bursts that fall due are dropped) from 2 s before
the reboot it asks for until the web server answers again.

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

## Replayed RTCM and a local caster

`e2e.py` runs the whole data path in QEMU: recorded RTCM into UART1, through
the firmware, out to a caster on this computer.

```sh
tools/qemu/run.sh --build-only
uv run tools/qemu/e2e.py                       # both scenarios; logs in build-qemu/e2e/
uv run tools/qemu/e2e.py --scenario stream --caster docker
```

Like `smoke.py` it needs the ESP-IDF environment and uses only the standard
library (it imports `tools/bench/rtcm_replay.py`, `fake_caster.py` and their
`benchlib` modules by path). Nothing leaves this computer: the replay and
the casters listen on 127.0.0.1, which the emulated station reaches as
10.0.2.2.

It replays the fixture (`--fixture`: an RTCM file or a `capture.py` run;
default `test/fixtures/rtcm/centipede_LAX_30ep.rtcm3`, looped at its own one
epoch per second) into UART1 from the first boot (`--uart1 connect:PORT`), so the
station finds a streaming receiver by listening, sets the admin password
and, once the station's start-up receiver query is over (it pauses the
casters while it runs), configures slot 1: `Bench`, host `10.0.2.2`, the
caster's port, NTRIP 1.0, mountpoint `BENCH`, a made-up password, the
standard profile. PASS or FAIL for each of these (exit status 1 if any
failed):

- scenario `stream`: slot 1 streaming within 60 s (`ICY 200 OK` from the
  fake caster, `OK` from the BKG caster with `--caster docker`);
  `/api/gnss/signals` lists the fixture's constellations with their bands,
  as computed from the fixture with `benchlib` (NavIC listed without a band:
  the firmware maps none for it); `/api/health` `hub.frames_in` grows by at
  least five epochs' worth of frames in 10 s and the slot's `epochs_sent`
  rises; the caster's record holds at least 60 s of frames (`--observe`,
  default 75): every one CRC-valid and byte-identical to a fixture frame, in
  fixture order from the first epoch boundary on (modulo the loop), fixture
  frames the station never forwards skipped (the SBAS numbers 1101-1107),
  nothing between frames, a cut frame only at the very end. With
  `--caster docker` the record is what a `GET /BENCH` client receives from
  the BKG caster (`tools/bench/docker/bkg-caster`, built on first use);
  bytes in front of its first frame are allowed there, because that caster
  starts a new client inside older data;
- scenario `onocoy-guard`: slot 0 as a test-Onocoy slot (`test_onocoy`,
  only in builds with test-Onocoy slots: on a firmware without them it
  prints SKIP and passes) against a second fake caster scripted with
  Onocoy's termination reasons: a credential in use on credentials that
  never worked stops the slot at once; the waits after a rejected login,
  a login block and an in-use reply (each delay within [entry, 1.2 x entry]
  plus 2 s for QEMU, read from `next_retry_s` and from the connection
  times); sessions the caster ends quickly (and a `Device reconnected` line)
  stop the slot after a score of 3, waiting 5 s at a score of 1 and 10 s at
  2 (a `Device reconnected` line scores 2 at once); banned
  and duplicate reports stop it at once; a save and
  `POST /api/casters/0/retry` clear a stop, a reboot does not; the journal
  has the `caster_stop` records. `--long` adds the 5-minute wait after a
  rejected login and the whole in-use retry ladder (about 8 minutes more).
  It runs before `stream`: with nothing answering, the station repeats its
  receiver query every 10 minutes and the casters pause while it runs; a
  wait that such a query interrupted is noted and not judged;
- throughout: `smoke.py`'s console checks (no panic, abort, stack overflow,
  failed assert, backtrace or task watchdog report, no reboot loop, no GNSS
  ladder restart; the reboot the scenario asks for is allowed).

Everything lands in `--out`: `console.log`, `e2e.log` (the run's events),
`replay/` (what the firmware sent on UART1), `caster/` and `guard/` (each
caster's `connections.jsonl` and records), the HTTP answers and
`summary.txt`. The `qemu-smoke` CI job runs it after the smoke test with
`--out qemu-e2e --scenario all` and uploads that directory too.
