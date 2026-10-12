# Bench tools

Host-side scripts for bench work on the UM982 + ESP32-S3 board: find the
ports, capture both serial ports with timestamps, query the receiver from
the PC, decode RTCM captures, and pull diagnostics from the station
firmware's web API. The aim is that bench day is "plug in and iterate".

Everything is a [uv](https://docs.astral.sh/uv/) project. Run the scripts
from this directory:

```sh
cd tools/bench
uv sync                      # once: creates .venv with pyserial and httpx
uv run ports.py --help       # every script has --help
uv run pytest                # host unit tests (no hardware)
```

The tests also run the decoders over the real-world streams and receiver
output in `test/fixtures` (see its README) and compare every frame, MSM
cell, 1033 string, text line and satellite-in-view entry with independent
decodes (pyrtcm, pynmeagps).

Every run writes its logs to a new directory
`./bench-logs/<YYYYmmdd-HHMMSS>-<tool>[-<label>]/` (`--log-dir` or
`$BENCH_LOG_DIR` to move it, `--label` to tag it). `bench-logs/` is
gitignored.

## The board's two USB-C ports

| Silkscreen | Bridge | USB ID | What it is |
|---|---|---|---|
| ESP32S3 USB&&UART | CH343P | `1a86:55d3` | ESP32-S3 UART0 console, 115200; `idf.py flash` goes here |
| UM982 UART2 | CH340N | `1a86:7523` | UM982 **COM2**, 115200 by default |

The ESP32 talks to the receiver on UM982 **COM1** (GPIO1/GPIO2), which the
PC cannot see. COM2 is a separate port: what you capture there is only
what the receiver sends to COM2 (nothing, on a factory receiver) plus the
replies to commands sent on COM2. To look at the RTCM the station gets,
either ask the firmware (`station.py`, `/api/gnss/signals`) or mirror the
same messages to COM2 for a while (see "Seeing the RTCM on the PC" below).

On macOS the ports appear as `/dev/cu.usbmodem*`, `/dev/cu.usbserial-*` or
`/dev/cu.wchusbserial*` depending on the driver; `ports.py` sorts it out.

## What sends what

| Tool | Sends on a serial port | Notes |
|---|---|---|
| `ports.py` | nothing | listens only when probing |
| `capture.py` | nothing | `--reset-esp32` toggles RTS to reset the ESP32; still no bytes |
| `rtcm_inspect.py` | nothing | `--port` listens only |
| `um982.py` (default) | read-only queries only | the list is in `--help`: CR LF, `VERSIONA`, `CONFIG`, `MODE`, `MASK`, `UNILOGLIST`, `BESTNAVA`, `GPGSV`, `OBSVMA`, and a few more one-shot logs for `send` |
| `um982.py ... --write` | the commands you name, after you type `yes` | prints the exact text first; never resent automatically |
| `station.py` | (HTTP only) | GET requests plus log in/out; never changes settings |
| `bootprobe.py --i-understand` | `VERSIONA`, the soft restart, the bootloader stop key `T@`, then at the boot menu only `\r\n` and menu item `6` (soft reset cpu), after you type `yes` | restarts the receiver; every write passes a fixed allow-list |
| `rtcm_replay.py` | nothing (no serial port) | TCP only, local: serves a recorded stream to one client at a time (QEMU's UART1) and logs what the client sends back |
| `fake_caster.py` | nothing (no serial port) | TCP only, local: a scripted NTRIP caster listening on 127.0.0.1; it only answers |
| `ntrip_timing.py` | nothing (no serial port) | TCP only: one NTRIP client (GET) to the caster you name; there is no default host |

Never sent by any tool, even with `--write`: `FRESET`, `RESET` with
parameters and any firmware/bootloader command. Bootloader keys are sent
only by `bootprobe.py`, and only the stop key `T@` and menu item `6` (soft
reset cpu): no other menu item, no data frames, no erase or write command.
Receiver firmware updates stay with the station firmware (or the vendor
tool).

Modem lines: opening a port leaves DTR and RTS as the OS sets them (both
asserted), so attaching a capture does not reset the ESP32. Only
`capture.py --reset-esp32` pulses EN on purpose.

## The tools

### `ports.py`: which port is which

```sh
uv run ports.py                 # VID/PID first, then listen on anything unidentified
uv run ports.py --probe-all     # listen on every USB port (2.5 s per baud)
```

Probing looks for ESP-IDF log lines and ROM boot text (console) or NMEA,
RTCM 3 frames and receiver replies (UM982). The run directory gets
`ports.txt` and `ports.json`. The other tools accept `auto` instead of a
port and pick by VID/PID.

### `capture.py`: both ports, timestamped

```sh
uv run capture.py --auto
uv run capture.py --console auto --reset-esp32 --elf ../../build/onocoydiy-esp-um982.elf
uv run capture.py --receiver auto --duration 3600 --label survey
```

- Console: `console-NNNN.log` with a UTC timestamp per line (ANSI colours
  removed), `events.log` with panics, resets, watchdogs, brownouts and
  backtraces; with `--elf` each `Backtrace:` line is decoded with the
  Xtensa `addr2line` from your ESP-IDF install.
- Receiver: `receiver-NNNN.bin` (the exact byte stream), `receiver-NNNN.idx`
  (host time of every read, so the `.bin` can be timed later) and
  `receiver-NNNN.log` (one line per RTCM frame with type, length, decoded
  summary and CRC result; text lines verbatim with their checksum verdict;
  CRC failures; junk bytes).
- A status line every 10 s, a summary (with the RTCM report) at the end.
- Ctrl-C stops cleanly (twice to force). Files rotate at `--rotate-mb`.
  A port that disappears (unplug, re-enumeration) is reopened every second.
- The console port is opened exclusively: stop the capture before
  `idf.py flash`.

### `um982.py`: the receiver from the PC

```sh
uv run um982.py                        # = query: the day-one set, read-only
uv run um982.py send BESTNAVA GPGSV    # read-only one-shots need no flag
uv run um982.py send --write "CONFIG SIGNALGROUP 3 6"
uv run um982.py e6-check               # read-only part + the plan it would run
uv run um982.py e6-check --write       # try group 3 6 (Galileo E6)
uv run um982.py e6-check --write --group "7 0" --msm
```

`send --write` changes are not saved by themselves, but the station's next
configuration of the receiver (its first apply, a profile change, a re-apply
after drift, the recovery ladder's reconfigure step) ends with `SAVECONFIG`,
which saves the receiver's whole running configuration: a `MASK` or `MMP`
change made here stays then. Set back what should not stay.

`query` listens for 2 s first (to log anything the port streams on its
own), sends a bare CR LF, then `VERSIONA`, `CONFIG`, `MODE`, `MASK`,
`UNILOGLIST`, `BESTNAVA`, `GPGSV` and `OBSVMA`, one at a time, waiting for
each acknowledgement and its data. Every reply is saved verbatim under
`replies/`, every byte in and out in `session.log`/`session-rx.bin`, and the
summary (`summary.txt`, `summary.json`) shows the identity (with the CRC
check of `#VERSIONA`), signal group, port bauds, mode, masks, enabled logs,
position, the signals tracked per constellation (from GSV signal IDs and
from the observation log), and whether any rewarded constellation (GPS,
GLONASS, Galileo, BeiDou) has 4 bands; QZSS and NavIC never count, as on
the station.

`e6-check` answers "can this receiver reach 4 bands?". Without `--write`
it only reads (identity, current signal group, signals tracked now) and
prints the plan. With `--write` it:

1. asks, then sends `CONFIG SIGNALGROUP <group>` (the receiver restarts by
   itself and saves the group);
2. listens for the restart banner and polls `VERSIONA` until the receiver
   answers, and prints both delays;
3. reads `CONFIG` again to confirm the new group;
4. with `--msm`, asks, then enables `RTCM1097 COM2 1` (not saved) and
   decodes the Galileo MSM signal mask too;
5. every 15 s sends `GPGSV` and `OBSVMA` once and reports E6 (GSV signal
   ID 4/5, observation signal E6B/E6C, MSM signal 8-12) and the bands per
   constellation, stopping once E6 or a 4-band constellation shows in two
   rounds (`--full` to keep going, `--observe` for the limit);
6. asks before `UNLOG COM2 RTCM1097`, and offers to restore the previous
   signal group.

`--find-baud` tries the usual bauds (listening at each first, then
`VERSIONA` at each) when the receiver does not answer at 115200. Only for a
receiver whose application is known to run: never after a failed or
interrupted receiver update or a `bootprobe.py` run that did not end with
exit 0, because its queries at many bauds could reach a loader menu, where
garbled bytes may read as menu keys. Listen first instead (the
`bootprobe.py` section). Whatever the options, `um982.py` sends nothing at
all when its listen-first window (or the search's listening) hears the
receiver's loader (`boot>` or its menu text): it stops with exit status 3
and tells you to power-cycle the receiver. When the search's ask phase
hears the loader, it stops the same way and names every baud it had sent
`VERSIONA` at (in the output and `summary.txt`).

### `bootprobe.py`: the PC recovery channel (boot menu over COM2)

```sh
uv run bootprobe.py --port auto --i-understand               # boot menu at 460800
uv run bootprobe.py --port auto --i-understand --baud 115200
```

Before the station firmware ever drives the receiver's bootloader over COM1
(a receiver firmware update), the PC must be able to reach that bootloader
over COM2: it is the way back if an update over COM1 goes wrong. This tool
checks that and nothing else. It asks `VERSIONA` at the application baud
(nothing more if there is no answer), restarts the receiver with
`\r\nreset\r\nreset\r\n`, switches to `--baud` (460800 by default) and
streams the stop key `T@` in bursts (8 bytes at 115200, 32 at 460800, 64 at
921600), each one after the previous one has left the port plus about 5 ms
(the station firmware's cadence; reads never hold a burst back), until the
prompt `boot>`; it gives up 30 s after the restart, or earlier when data
still arrives 8 s after it without the prompt or the application's restart
banner shows up instead. At the menu it sends nothing until the port is
quiet, then `\r\n` (clears stop-key bytes left in the menu's input line;
a bootloader that prints its menu only on a key press prints it now) and
menu item `6` (soft reset cpu) only if the printed menu numbers the soft
reset as 6, waits for `resetting the cpu`, and asks `VERSIONA` at the
application baud until the application answers; the build must be the one
it had.

It never selects any other menu item, never sends data frames, erase or
write commands, `FRESET` or `SAVECONFIG`: every write passes the allow-list
in `allowed_tx()`. The run directory gets `transcript.log` (every byte in
and out with its UTC time and the port baud), `boot-menu.txt` (the
bootloader's banner, settings and menu, verbatim), `loader-rx.bin` and the
summary. When the prompt does not come, it says so, stops, and checks only
that the application is back; when the application does not come back it
tells you to power-cycle the receiver. When the menu cannot be confirmed
(no readable line names the soft reset, or it is not item 6) it sends
nothing more and tells you to power-cycle the receiver. Exit status 0 =
menu reached and the application back with the same build, 1 = no menu,
2 = application not back (or stopped at an unconfirmed menu), 3 = not
started, 4 = the application came back with another firmware. Only 0 lets
the next check on the bench go on.

Before its first `VERSIONA` it listens, sending nothing, 2 s at the boot
menu's baud and 2 s at the application baud; when it hears the receiver's
loader there (`boot>` or its menu text) the receiver already waits at its
loader: it sends nothing at all and exits with 3 (power-cycle the receiver,
then start again). After a failed or interrupted receiver update, a run that
did not end with 0, or no answer at all, listen before sending anything:

1. listen only: `uv run capture.py --receiver <port> --receiver-baud 460800
   --duration 20`, and the same with `--receiver-baud 115200`;
2. `boot>` seen: the receiver is at its loader: power-cycle it; run neither
   `um982.py` nor `bootprobe.py` until the application is heard
   (`bootprobe.py` needs the application: at a loader whose prompt it does
   not hear in its 2 s listen it would send `VERSIONA`, never `6`);
3. application output seen at a baud: `uv run um982.py --port <port> --baud
   <that baud>`;
4. nothing at either: power-cycle, then step 1 again; still silent: stop.

### `rtcm_inspect.py`: what is in the stream

```sh
uv run rtcm_inspect.py bench-logs/20261010-141500-capture
uv run rtcm_inspect.py stream.bin --frames
uv run rtcm_inspect.py --port auto --seconds 60      # live, listen only
```

Report: per message type the frames, MSM epochs, bytes, rate and interval
spread; per constellation and band the satellites, signals and mean C/N0;
the 1005/1006 position in ECEF and WGS84 and how far it moved (survey-in
still running?); 1033 strings; 1230 biases; CRC failures, text lines and
junk; and whether each epoch's MSMs end with the multiple-message bit
clear. Then the Onocoy checks:

- MSM7 at 1 Hz for GPS, GLONASS, Galileo and BeiDou (QZSS informational);
  MSM1-3 are not rewarded, MSM4-6 are accepted, faster than 1 Hz is
  ignored by Onocoy;
- 1005 present (and 1033, 1230);
- at least one rewarded constellation (GPS, GLONASS, Galileo, BeiDou) with
  4 bands (L1/L2/L5/L6), which moves the band reward from 0.80 to 0.95
  (the bands as the station buckets them: `benchlib/rtcm.py` equals the
  firmware's table, and a test compares the two);
- Galileo E6 seen or not, SBAS/1104/1105 numbers present (the station drops
  them), 1137 present.

`report.txt`/`report.json` land in the run directory. The exit status is 1
when any check is FAIL, so it can gate a script.

### `station.py`: our firmware over Wi-Fi

```sh
export STATION_HOST=192.168.1.50     # or onocoydiy.local, or 192.168.4.1 on the setup AP
export STATION_PASSWORD=...          # else it asks
uv run station.py                    # = dump
uv run station.py watch --logs
uv run station.py get /api/gnss/signals
```

`dump` saves every read-only endpoint of `components/web/API.md` as JSON
(status, receiver, signals, sky, receiver-update session, casters, Wi-Fi,
system, updates, health with boot history and core dump summary,
availability, task stats), the event journal, the log ring of this boot and
the previous one, and the core dump if there is one (decode it with
`idf.py coredump-info -c <file>`; it may contain secrets, so keep it out of
bug reports). `watch` prints one line per second (uptime, receiver state,
RTCM age, best constellation and bands, each caster with its lag, Wi-Fi
RSSI, health, alerts), marks reboots, rides out the station being
unreachable, and with `--logs` tails the device log ring. The password is
never written to the logs.

## Bench day

0. **Unbox, antenna on the master SMA with the board unpowered**, then plug
   in both USB-C cables. `uv run ports.py` should show one console and one
   UM982 port.
1. **Receiver first, still on the stock ESP32 firmware or none:**
   `uv run um982.py` and keep the run directory: it records the build,
   the factory signal group, the enabled logs and the bauds.
   `uv run um982.py e6-check` shows what is tracked now; then
   `uv run um982.py e6-check --write` (group 3 6) and, if E6 does not show,
   `--group "7 0"` for BeiDou B2a. Note the restart timings it prints.
   Then prove the PC recovery channel before anything touches the
   receiver's bootloader from the ESP32: `uv run bootprobe.py --port auto
   --i-understand` must reach `boot>` over COM2 and bring the application
   back (try `--baud 115200` once if 460800 does not reach the menu). Keep
   its `boot-menu.txt`.
2. **Build and flash our firmware** from the repository root:
   ```sh
   . ~/esp/esp-idf-v6.1/export.sh
   idf.py set-target esp32s3        # once per checkout
   idf.py build
   idf.py -p <console port> flash   # stop any capture on that port first
   ```
3. **Capture both ports from the first boot line:**
   ```sh
   cd tools/bench
   uv run capture.py --auto --reset-esp32 --elf ../../build/onocoydiy-esp-um982.elf --label first-boot
   ```
4. **See what the station gets:** join the station's setup AP or its
   Wi-Fi, then `uv run station.py watch --logs` while it configures the
   receiver, and `uv run station.py dump` once it streams (the signals
   endpoint is the firmware's own decode of COM1).
5. **Inspect the receiver side:** `uv run rtcm_inspect.py bench-logs/<capture dir>`
   (if RTCM is mirrored to COM2, below), or `uv run um982.py` again to see
   what the firmware configured (signal group, logs on COM1, mode).
6. **Iterate:** Ctrl-C the capture, edit, `idf.py build`, `idf.py -p <port> flash`,
   step 3 again. Each run is its own directory, so before/after comparisons
   are a `diff` away.

### Seeing the RTCM on the PC

COM1 goes only to the ESP32. To inspect the same messages on the PC,
mirror them to COM2 for a while (not saved by itself, but any configuration
the station runs later - its first apply, a profile change, a re-apply after
drift, the recovery ladder's reconfigure step - ends with SAVECONFIG, which
saves the receiver's whole running configuration, mirrors included: send
UNLOG COM2 as soon as you are done):

```sh
uv run um982.py send --write "RTCM1005 COM2 1" "RTCM1033 COM2 1" "RTCM1077 COM2 1" \
    "RTCM1087 COM2 1" "RTCM1097 COM2 1" "RTCM1117 COM2 1" "RTCM1127 COM2 1" "RTCM1230 COM2 1"
uv run rtcm_inspect.py --port auto --seconds 120
uv run um982.py send --write "UNLOG COM2"
```

The `send` replies also show whether each message type is accepted
(`OK`) or rejected (`PARSING FAILD ...`) by this firmware build.

## Layout

| File | Contents |
|---|---|
| `ports.py`, `capture.py`, `um982.py`, `rtcm_inspect.py`, `station.py`, `bootprobe.py`, `rtcm_replay.py`, `fake_caster.py`, `ntrip_timing.py` | the tools |
| `benchlib/rtcm.py` | RTCM 3 framing, CRC-24Q, MSM/1005/1006/1033/1230 decoders, band mapping |
| `benchlib/stream.py` | splits a byte stream into RTCM frames, text lines (checksum-checked), binary logs and junk |
| `benchlib/rtcmstats.py` | counts, rates, MSM content and the Onocoy checks |
| `benchlib/unicore.py` | receiver command rules (read-only list, block list), the command session, reply parsers |
| `benchlib/serialio.py` | port listing, identification, listen-only probing, safe opening |
| `benchlib/common.py` | run directories, rotating files, Ctrl-C handling |
| `benchlib/replay.py` | replay schedules: capture runs timed by their `.idx`, RTCM files grouped into epochs by MSM epoch time, looping on a continuous time base |
| `benchlib/ntrip.py` | NTRIP helpers for the local tools: chunked transfer coding, request parsing with the password masked, a frame recorder (CRC, bytes outside frames, cut end) |
| `docker/bkg-caster/` | Dockerfile, configuration and scripts for the optional BKG caster test peer |
| `tests/` | pytest; RTCM test frames are built field by field from the public layouts and cross-checked with pyrtcm; `bootprobe.py` runs against a scripted receiver over a socketpair; the replay, fake caster and timing tools run over local sockets; `test_docker_caster.py` only with `RUN_DOCKER_TESTS=1` |

`rtcm_replay.py`, `fake_caster.py`, `ntrip_timing.py` and the `benchlib`
modules they use (`rtcm`, `stream`, `common`, `replay`, `ntrip`) need only
the Python standard library, so `tools/qemu/e2e.py` can import them without
this project's environment.

## Replaying RTCM into QEMU

`rtcm_replay.py` stands in for the receiver on the firmware's UART1 when the
firmware runs in QEMU (`tools/qemu/README.md`). It serves either a
`capture.py` run (every `receiver-NNNN.bin` part in order, each read at its
`.idx` time) or a plain RTCM 3 file, grouped into epochs: a new epoch at the
first frame after an MSM with the multiple-message bit clear, or at an MSM
whose epoch time differs from the one its constellation already has; one
epoch per burst, timed by the MSM epoch times.

```sh
cd tools/bench
uv run rtcm_replay.py --listen 127.0.0.1:5600 ../../test/fixtures/rtcm/centipede_LAX_30ep.rtcm3 --loop
../qemu/run.sh --uart1 connect:5600            # in a second terminal, from the same directory
```

- One client at a time; it keeps listening after a client leaves (QEMU's
  `connect` mode reconnects every second) and starts the schedule from the
  beginning for each connection. Start the replay first so the firmware
  hears the stream from its first boot.
- `--loop` starts again after the last burst on a continuous time base (the
  gap at the seam is a normal one); `--speed 2` halves every gap; `--baud N`
  sends each burst at that line rate (10 bits per byte) instead of all at
  once.
- Everything the firmware sends back (its receiver queries) goes to
  `from-firmware.bin` and, timestamped, to `replay.log`, with each loop's
  counts. Ctrl-C stops it.

The replay has no receiver behind it: it never answers a query, so the
firmware streams what it hears and leaves the receiver's configuration
alone (an identity query that gets no answer pauses the casters for a few
seconds while it runs). The Centipede fixtures are test inputs, not a
correction service (see `test/fixtures/README.md`): replay them only to
local casters.

## A local fake caster

`fake_caster.py` is a caster on this computer for testing the station's
caster client: it answers each source with a reply row you pick and checks
what the source sends.

```sh
uv run fake_caster.py rows                                     # every reply row and its exact bytes
uv run fake_caster.py serve --port 2101 --mount BENCH --password bench-pass-1 --record
uv run fake_caster.py serve --port 2101 --reply naomi_error
uv run fake_caster.py serve --port 2102 --sequence "icy_ok:kick=5:text=onocoy_reconnected,onocoy_in_use"
uv run fake_caster.py report bench-logs/<serve run>            # connection table with the gaps
```

- Point a slot at it: host `10.0.2.2` from QEMU (the computer's
  127.0.0.1), or this computer's address from a board on the same network
  (then `--host 0.0.0.0`).
- For the Onocoy behaviour (fast retry profile, login guard, Onocoy wire
  layout), mark the slot `test_onocoy` (`PUT /api/casters/{n}`): bench and
  QEMU builds allow it (`CONFIG_NTRIP_TEST_ONOCOY_SLOT`), so no DNS entry
  for an onocoy.com name is needed. The station build refuses it
  (`400 invalid_field`).
- Rows: the success lines (`icy_ok`, `ok`, `http_200`, `http_1_0_200`; the
  session is checked frame by frame, NTRIP 2.0 chunked bodies are decoded),
  the HTTP errors, `sourcetable`, the `ERROR - ...` lines, refusals in plain
  text and HTML (`banned`, `sandbox`), misbehaving casters (`trickle`,
  `no_reply`, `close_no_newline`, `close_silent`, `overlong`, `garbage`) and
  Onocoy's termination reasons (`onocoy_login_blocked`, `onocoy_in_use`,
  `onocoy_parallel`, `onocoy_reconnected`, `onocoy_duplicate`,
  `onocoy_credential_banned`, `onocoy_auth_failed`, `onocoy_reauth_failed`,
  `onocoy_invalid_headers`). Those are sent as
  `Error <n> <reason> [onocoy naomi v2.5.95-fake]` and a close, the only
  error form seen from that caster so far (the numbers are made up);
  `--onocoy-form plain_line` sends the reason alone, `http_401` an HTTP 401
  with the reason as its body, `no_line_end` the error line without its
  CR LF before the close (the wire form is not known).
- `--sequence`: one entry per source connection, the last one repeating.
  `kick=S` ends an accepted session S seconds after the reply, after the
  line of the `text=` row if given; `record` saves that session.
- `--mount` and `--password` refuse other mountpoints and passwords
  (`bad_mountpoint`/`bad_password`, or HTTP 404/401 for NTRIP 2.0) before
  the sequence is consulted. `--relay` serves `GET /<mount>` clients
  (NTRIP 1.0 and 2.0) the frames of the current source.
- The run directory: `connections.jsonl` (one line per connection with
  unix and monotonic times, the request line with the password masked, the
  agent, the reply row and bytes, bytes in, frames, CRC failures, bytes
  outside frames, a partial frame at the end, who closed), `caster.log`,
  and with `--record` a `record-NNN.rtcm3` and `record-NNN.json` per
  session. No password is ever written.

## Measuring latency

`ntrip_timing.py capture` reads a mountpoint as an NTRIP client and saves
it in `capture.py`'s format (`stream-NNNN.bin`, `stream-NNNN.idx`, NTRIP 2.0
chunking removed) plus the caster's reply header; `report` then gives, for
every epoch's first MSM frame, its arrival time minus its MSM epoch time
per constellation and overall (p50, p95, max) and `PASS p95 < 1000 ms` or
`FAIL`. Keep this computer's clock synchronised: the figure is only as good
as its clock.

```sh
uv run fake_caster.py serve --port 2101 --mount BENCH --relay      # the station streams to it
uv run ntrip_timing.py capture --host 127.0.0.1 --port 2101 --mount BENCH --duration 120
uv run ntrip_timing.py report bench-logs/<timing run>
uv run ntrip_timing.py report bench-logs/<capture.py run> --json   # a COM2 mirror: the receiver's own delay
```

There is no default host: name the caster. For a caster that lets you
read your own mountpoint back, or any other mountpoint, give the user with
`--user` and the password in an environment variable (`--password-env
VAR`) or at a prompt (`--password-prompt`); it is never taken from the
command line, printed or written to the run directory. `--ntrip-version 2`
and `--tls` as the caster needs. Epoch times: GPS, Galileo, QZSS time of
week, BeiDou + 14 s, GLONASS Moscow day time, GPS - UTC = 18 s.

## A real caster in Docker

`docker/bkg-caster/` builds BKG's Standard NtripCaster (NTRIP 1.0) from
[goblimey/ntripcaster](https://github.com/goblimey/ntripcaster) at a pinned
commit, the way that project's README describes, with our own configuration
(mountpoint `BENCH`, source password `bench-src`, open to every client).
Optional: nothing else needs it.

```sh
docker/bkg-caster/build.sh                       # image onocoydiy-bench-bkg-caster
docker/bkg-caster/run.sh --port 2101             # on 127.0.0.1:2101; Ctrl-C stops it
docker/bkg-caster/run.sh --lan --detach          # on 0.0.0.0:2101, in the background
docker/bkg-caster/run.sh --lan --print           # the docker command only; nothing runs
docker/bkg-caster/run.sh --stop
RUN_DOCKER_TESTS=1 uv run --locked pytest tests/test_docker_caster.py
uv run ../qemu/e2e.py --scenario stream --caster docker
```

The caster is published on 127.0.0.1 by default: only this computer (and
QEMU's user network, as 10.0.2.2) reaches it. A station board reaches it
only with `--lan`, which publishes the port on every interface: then
everyone on your LAN reaches it too, and `run.sh` says so (`reachable from
your LAN: use the throwaway test passwords only`). Point the board's slot
at this computer's LAN address, port 2101, mountpoint `BENCH`, with the
bench source password; never a password you use anywhere else. `--print`
shows the `docker run` command (with the address it publishes on) without
running it, and needs no Docker; `tests/test_docker_run_script.py` checks
it on every run of the tests.

What this build does (recorded on 2026-10-12, `tests/test_docker_caster.py`):
an NTRIP 1.0 source gets `OK`; one whose agent is not an NTRIP agent (such
as `curl/8.5.0`) gets `Not authorized (no NTRIP source)`; a wrong password
`ERROR - Bad Password`; a second source on a held mountpoint
`ERROR - Mount Point Taken or Invalid` (each error line followed by an
empty line). Source passwords of 10 characters or more are always
refused with `ERROR - Bad Password`, hence the short one. A new client
starts inside older data and misses the first few frames after it joins.
Its log: `docker exec <container> tail -f /usr/local/ntripcaster/logs/ntripcaster.log`.
