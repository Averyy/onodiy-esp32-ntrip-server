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

| Port (silkscreen) | Bridge | USB ID | `ports.py` role | What it is |
|---|---|---|---|---|
| bottom edge (`ESP32S3`), one way round | CH343P | `1a86:55d3` | `esp32-console` | ESP32-S3 UART0 console, 115200; `idf.py flash` and bench work go here |
| bottom edge (`ESP32S3`), plug turned over | none: the chip's built-in USB Serial/JTAG | `303a:1001` | `esp32-native-usb` | ESP32-S3 built-in USB: ROM output, esptool, and in bench builds a copy of our firmware's console (output only) |
| top edge (no label) | CH340N | `1a86:7523` | `um982-com2` | UM982 **COM2**, 115200 by default |

The `ESP32S3` port is orientation-dependent: which of the two devices the
computer sees depends on which way round the plug goes in (the seller calls
the port "ESP32S3 USB&&UART"). The firmware's console and the bench console
are on UART0, so bench work uses the CH343P orientation: if `ports.py`
lists the port as `esp32-native-usb` (and the console as not found), unplug
and turn the plug over. `--console auto` picks the CH343P by VID/PID only;
in the other orientation name the port. A bench build (`sdkconfig.bench`)
copies its console's output to the built-in USB too
(`CONFIG_DIAG_CONSOLE_USB_MIRROR`, off by default), so a capture there shows
the log, but the bench console reads its input on the CH343P side only. The
copy keeps the USB PHY powered while Wi-Fi runs, which can lower Wi-Fi
performance (not measured on this board yet). The station and hold builds
leave it out and switch the built-in USB off while they run; flashing
through it then needs **BOT** held while pressing **RST**. A build directory
that had the copy on keeps the built-in USB and the USB PHY on after the
copy is turned off: set `CONFIG_DIAG_CONSOLE_USB_MIRROR`,
`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG` and `CONFIG_ESP_PHY_ENABLE_USB` to `n` in
its `sdkconfig`, or start a new one.

Neither port works on a USB-C-to-C cable, not even for power (the board
leaves out the USB-C CC resistors): plug both into USB-A ports, through a
USB-C-to-A adapter or a hub, with A-to-C data cables.

The ESP32 talks to the receiver on UM982 **COM1** (GPIO1/GPIO2), which the
PC cannot see. COM2 is a separate port: what you capture there is only
what the receiver sends to COM2 (nothing, on a factory receiver) plus the
replies to commands sent on COM2. To look at the RTCM the station gets,
either ask the firmware (`station.py`, `/api/gnss/signals`) or mirror the
same messages to COM2 for a while (see "Seeing the RTCM on the PC" below).

On macOS the ports appear as `/dev/cu.usbmodem*`, `/dev/cu.usbserial-*` or
`/dev/cu.wchusbserial*` depending on the driver; `ports.py` sorts it out.
Both sides of the `ESP32S3` port show up as `/dev/cu.usbmodem*`, so the name
alone does not say which way round the plug is.

## What sends what

| Tool | Sends on a serial port | Notes |
|---|---|---|
| `ports.py` | nothing | listens only when probing |
| `capture.py` | nothing | `--reset-esp32` toggles RTS to reset the ESP32; still no bytes |
| `rtcm_inspect.py` | nothing | `--port` listens only |
| `um982.py` (default) | read-only queries only | the list is in `--help`: CR LF, `VERSIONA`, `CONFIG`, `MODE`, `MASK`, `ANT1GROUPA`, `ANT2GROUPA`, `UNILOGLIST`, `BESTNAVA`, `GPGSV`, `OBSVMA`, and a few more one-shot logs for `send` |
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

Measured on the board (2026-10-14): opening and closing the console with
`capture.py --console`, serial-mcp or `idf.py monitor --no-reset` left the
ESP32 running on the built-in USB side; the CH343P side matched another
ESP32-S3 board with the same bridge. `capture.py --reset-esp32` resets the
chip on the built-in USB side too, without the port dropping, so the ROM
banner is captured from its first line. An ESP32 reset neither resets nor
reconfigures the UM982, and opening COM2 does not reset the receiver.

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

The `ESP32S3` USB-C shows up as `esp32-console` (the CH343P, `1a86:55d3`)
or as `esp32-native-usb` (the built-in USB, `303a:1001`), depending on which
way round the plug is (see "The board's two USB-C ports"); the receiver's
port is `um982-com2`.

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
  summary and CRC result; text lines verbatim with their checksum verdict,
  the receiver's `The Board is resetting` answer marked `restarting`; CRC
  failures; junk bytes).
- A status line every 10 s, a summary (with the RTCM report) at the end.
- Every file is flushed to the disk at least once a second and on each
  status line, the console files also on each event, so `tail -f` follows
  a running capture and one that is killed loses at most about a second.
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
`ANT1GROUPA`, `ANT2GROUPA`, `UNILOGLIST`, `BESTNAVA`, `GPGSV` and `OBSVMA`,
one at a time, waiting for each acknowledgement and its data. Every reply
is saved verbatim under `replies/`, every byte in and out in
`session.log`/`session-rx.bin`, and the summary (`summary.txt`,
`summary.json`) shows the identity (with the CRC check of `#VERSIONA`),
signal group, port bauds, mode, masks, the signals the signal group sets up
on each antenna (from `ANT1GROUPA`/`ANT2GROUPA`, known without sky), enabled
logs, position, the signals tracked per constellation (from GSV signal IDs
and from the observation log), and whether any rewarded constellation (GPS,
GLONASS, Galileo, BeiDou) has 4 bands, set up and tracked; QZSS and NavIC
never count, as on the station.

`ANT1GROUPA` (master antenna) and `ANT2GROUPA` (slave) go out alone: with a
period (`ANT1GROUPA 1`, the manual's example) they would become a periodic
log on COM2, so that form needs `--write` and says why. Each answer is
`<group>,<n systems>`, then a constellation and a signal mask per system
(`3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35`), decoded with this bit order,
lowest bit first (inferred: it matches the manual's signal-group table for
every mask build 25164 reported):

| System | Bits |
|---|---|
| `GP` GPS | L1C/A, L1C, L2C, L2P, L5 |
| `GL` GLONASS | G1, G2, G3 |
| `GA` Galileo | E1, E5a, E5b, E6 |
| `GB` BeiDou | B1I, B2I, B3I, B1C, B2a, B2b |
| `GQ` QZSS | L1C/A, L1C, L2C, (bit 3 never seen), L5, L6 |

A slave group of 0 means the slave antenna is off; the masks reported with
it then are not read.

A receiver that is restarting answers any command with `The Board is
resetting,do not response any command!` (no `$`). The tool reports
"receiver restarting", never resends on it (even a read-only query) and
logs the line as `RX[restarting]`, not as an unknown line.

`e6-check` answers "can this receiver reach 4 bands?". Without `--write`
it only reads (identity, current signal group, the signals that group sets
up on the master antenna from `ANT1GROUPA`, signals tracked now) and prints
the plan. With `--write` it:

1. asks, then sends `CONFIG SIGNALGROUP <group>` (the receiver restarts by
   itself and saves the group);
2. listens for the restart banner (up to 20 s) and then polls `VERSIONA`
   once a second (up to 20 s) until the receiver answers, and prints both
   delays (build 25164: the banner 8.0-9.2 s after the `OK`, answers within
   4 s of the banner; the group it already has gives `OK` and no restart);
   a poll answered with `The Board is resetting` is reported as "still
   restarting";
3. reads `CONFIG` again to confirm the new group, and `ANT1GROUPA` to
   confirm, before any sky, that the new group sets up Galileo E6 (`3 6`)
   or BeiDou B2a (`7 0`) and a 4-band constellation;
4. with `--msm`, asks, then enables `RTCM1097 COM2 1` (not saved) and
   decodes the Galileo MSM signal mask too;
5. every 15 s sends `GPGSV` and `OBSVMA` once and reports E6 (GSV signal
   ID 4/5, observation signal E6B/E6C, MSM signal 8-12) and the bands per
   constellation, stopping once E6 or a 4-band constellation shows in two
   rounds (`--full` to keep going, `--observe` for the limit);
6. asks before `UNLOG COM2 RTCM1097`, and offers to restore the previous
   signal group.

What build 25164 does (measured 2026-10-14, no antenna): it accepts exactly
`CONFIG SIGNALGROUP 4 5`, `3 6`, `5 0` and `7 0`; any other pair gets
`PARSING FAILED GRAMMAR ERROR,` and no restart; one number is taken
when it makes a listed pair (`5` is `5 0`). A command sent while it
restarts gets the line `The Board is resetting,do not response any
command!` (no leading `$`). Before there is any sky,
`uv run um982.py send ANT1GROUPA` lists the signals the current group sets
up on the master antenna: `3 6` gives Galileo E1/E5a/E5b/E6 (4 bands), `7 0`
all six BeiDou signals including B2a (4 bands), `4 5` and `5 0` at most 3
bands.

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
   both USB-C ports into USB-A ports (A-to-C data cables; C-to-C does not
   work), the `ESP32S3` one the CH343P way round. `uv run ports.py` should
   show `esp32-console` and `um982-com2`; `esp32-native-usb` instead of the
   console means that plug is the other way round.
1. **Receiver first, still on the stock ESP32 firmware or none** (the stock
   firmware is an SD-card test sketch that leaves the receiver alone):
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
   idf.py -p <console port> flash   # the CH343P port; stop any capture on it first
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
(`OK`) or rejected (`PARSING FAILED ...` on build 25164; some builds spell
it `FAILD`) by this firmware build.

### Frame times on the bench console

Bench builds (the `sdkconfig.bench` overlay) have a console on UART0 (the
`ESP32S3` USB-C, CH343P way round) with
the receiver link's timing recorder. `ftimes <s>` starts a recording of
frame and drain-pass times and returns at once, so a stall can be put
inside it; the recording ends by itself after `<s>` seconds (or when its
ring is full):

```
ftimes 120              BENCH ftimes: recording frame and drain-pass times for 120 s in the background ...
stall 2000              BENCH stall 2000 ms done after 2003 ms: +... buffer-full, ...
ftimes status           BENCH ftimes status: running, 4210 records, 95 s left
ftimes summary          BENCH ftimes 120 s (still recording, 95 s left): ... 1 stalls; read to published ...; arrival to read ...; after a stall: ...
ftimes stop             BENCH ftimes 120 s (stopped after 31 s): ... (the summary line)
ftimes dump             the whole recording as CSV, ending with "BENCH ftimes dump end (...)"
```

- `ftimes summary` prints the summary line of the latest recording, also
  after it ended by itself; `ftimes stop` ends a running one early and
  prints it. The line names the records, frames, passes and stalls, the
  read-to-published percentiles, right after them the arrival-to-read
  percentiles over the frames with an interrupt time (`; arrival to read
  p50 A p95 B p99 C p99.9 D max E us (n N)`, or `; arrival to read: none`
  when no frame had one), always followed by the count of frames without
  one (`; M frames without an interrupt time`, also when it is 0 or every
  frame), the longest reader gap and, when a stall is in the recording,
  the zero-wait capped passes after it, when the ring was empty again and
  when the first frame was published after it; with the clock set by
  SNTP also the epoch delays per constellation, counted
  from the arrival of each epoch's first frame (frames with an interrupt
  time only). A second line always follows it with the link's pairing of
  interrupt times since boot: `BENCH ftimes interrupt times since boot: X
  frames with one, Y without, R resyncs, M mismatches, D pairings dropped
  by a known cause`.
- `ftimes dump` (and `GET /api/diag/frame-times`) writes one CSV line per
  record. A frame is `F,arrival_us,read_us,published_us,msg,len,sys,epoch,mmb,held,exact`
  (header `# F,arrival_us,read_us,published_us,msg,len,sys,epoch,mmb,held,exact`;
  the `P` and `S` lines are as before). `arrival_us` is the `esp_timer`
  time at which the UART interrupt moved the chunk holding the frame's last
  byte into the driver's ring buffer, `exact` 1: one FIFO chunk of
  resolution, about 10 ms at 115200 Bd. Without an interrupt time
  `arrival_us` is `read_us` and `exact` is 0.
- A second `ftimes <s>` while one runs answers `BENCH REFUSED`. `stall`,
  `rawstall` and `listen` run during a recording (with their usual
  refusals: receiver maintenance, an open update journal, another receiver
  job).

### Reading the bench console's replies

Every reply line (`BENCH ...`, `help`, the usage lines), the flasher's byte
echo and, for a script (a terminal that does not answer linenoise's status
request), the prompt reach the console whole: they go through the log's
console writer a line at a time, so a log line from another task comes
before or after a reply line, never inside it, and a script can take each
console line as it is. Only the echo of what is typed can still be split by
a log line. `status` names what its numbers mean:

```
status   BENCH state 3 (streaming), receiver UM982 R4.10Build18975, journal none (state 0 none, phase 0 none, 0 transfer(s)), flashing built in
         BENCH progress: phase 0 (idle), 0/0 bytes, 0/0 frames, 0 retries; last: outcome 0 (none), rule 0, reason none
```

The station state, update phase and outcome names are the web API's; the
journal states are the ones `jstate` and `reset_at` take; a receiver not
identified yet is `receiver none`. `logflood <s> [ms]` logs a line every
`ms` (default 10) for `s` seconds from a task of its own, to see that
replies stay whole while the log is busy (`tools/qemu/bench_console.py`
does that in QEMU).
- The same recording is `GET /api/diag/frame-times` (CSV).

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
| `benchlib/um982_sim.py` | the scripted UM982 command responder (test tooling) behind `rtcm_replay.py --respond`, which the replay and the QEMU e2e use: version, configuration dump, signal-group switches with restarts, the station messages as configured |
| `docker/bkg-caster/` | Dockerfile, configuration and scripts for the optional BKG caster test peer |
| `tests/` | pytest; RTCM test frames are built field by field from the public layouts and cross-checked with pyrtcm; `bootprobe.py` runs against a scripted receiver over a socketpair; the replay, fake caster and timing tools run over local sockets; `test_ui_text.py` runs sentences the web UI builds from the station's status (`components/web/ui/js`) in Node.js (skipped without it), among them the station state every Status card reads (each condition, their order, the Onocoy verdict), the page notices (order, folding, the page that leaves each out), where the old addresses land, and the receiver pages (the Receiver card's chip, the paused survey, the Signals headline and hints, the position message that waits for the survey, receiver management and Custom) Settings (the chip of the station's own Wi-Fi, the age of the last update check, receiver maintenance started in one place) and Diagnostics (its three parts and where its old addresses land, the health chip that follows the Status page, the event list that pairs a drop with its return and makes a sky session one row, one-line log and event rows), the short restart line and the setup's last screen (Onocoy's state as it happens); `test_ui_casters.py` drives the caster editor over a small DOM in Node.js and checks the body its Save sends and what it shows where, a caster card's address line, chip, duplicate-stream warning and connection details, the Status page's receiver rows and caster lines, the no-receiver panel, receiver management's save order, and the setup's Onocoy step answering the duplicate-stream question (skipped without it); `test_bench_console_checks.py` checks the pure parts of `tools/qemu/bench_console.py` (the commands it expects from `components/bench/bench.c`, the `ftimes summary` form); `test_smoke_boot.py` checks `tools/qemu/smoke.py`'s boot accounting (a planned restart cut short in the bootloader named as QEMU's APP-CPU artefact, never a crash), its slow-request watch over a local server (`request()` and a socket exchange of its own, each named with its client port) and its reading of a pcap (the station's packets, the client SYNs it turned away with a bare ACK or a reset, the client's resets, and which of the client's SYNs it took); `test_ci_job.py` checks `tools/ci/ci_job.py` (`build.yml` read as Actions reads it, what it refuses, continue-on-error, timeout-minutes and the upload stand-in over small jobs run with bash); `test_docker_run_script.py` checks the options of `docker/bkg-caster/run.sh` (`--print`, no Docker needed); `test_docker_caster.py` only with `RUN_DOCKER_TESTS=1`; `test_readme_layout.py` checks that this table names every `benchlib` module; `test_um982.py` runs `um982.py` against scripted ports (a loader, a silent port, and the replay's scripted receiver with `ANT1GROUPA`/`ANT2GROUPA` answers, for `query`, `e6-check --write` and a restart); `receiver_25164.py` holds what build 25164 wrote on the bench, byte for byte; `test_capture.py` checks that a running capture's files are on the disk within about a second |

`rtcm_replay.py`, `fake_caster.py`, `ntrip_timing.py` and the `benchlib`
modules they use (`rtcm`, `stream`, `common`, `replay`, `ntrip`, `um982_sim`
and the `unicore` it builds its replies with) need only
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

By default the replay has no receiver behind it: it never answers a
query, so the firmware streams what it hears and leaves the receiver's
configuration alone (its identity query goes unanswered and is asked again
every 10 min, without taking the line while RTCM flows: the casters are not
paused for it).

`--respond` puts a scripted UM982 behind it (`benchlib/um982_sim.py`, test
tooling): it answers what the station's receiver sessions send, with the
reply formats of `benchlib/unicore.py` (the public reply samples and the
host-test vectors), so the firmware identifies the receiver, configures it
and can switch its signal group:

```sh
uv run rtcm_replay.py --listen 127.0.0.1:5600 ../../test/fixtures/rtcm/centipede_LAX_30ep.rtcm3 --loop \
    --respond --respond-group 4,5 --respond-reject 7,0
```

- `VERSIONA` gets a UM982 `R4.10Build25164` version log; `CONFIG` the dump
  (SIGNALGROUP, COM1, MMP, RTCMB1CB2A, MASK, BASEANTENNAMODEL); `MODE` the
  base mode that `MODE BASE ...` set; `UNLOG COM1`, `RTCMxxxx COM1 n`,
  `CONFIG COM1`, `CONFIG RTCMB1CB2A`, `CONFIG BASEANTENNAMODEL`, the
  satellite-view toggles, `SAVECONFIG` and `RESET` an `OK`.
- `CONFIG SIGNALGROUP m s` is taken for 4 5, 3 6, 5 0 and 7 0, less every
  `--respond-reject M,S` (repeatable), and `CONFIG SIGNALGROUP m` as `m 0`;
  any other group gets `PARSING FAILED GRAMMAR ERROR,` as on build 25164,
  and nothing changes or restarts; any other command gets
  `PARSING FAILED NO MATCHING FUNC  <word>`. The group saves itself.
  `--respond-group` is the group it starts on (default 4,5, the factory one).
- `ANT1GROUPA` and `ANT2GROUPA` (the signal-group query, one-shot) get an
  `OK` and a log with the body build 25164 gave for the running group, in a
  made-up header (the bench recorded the bodies only).
- A signal-group change or `RESET` restarts it: for `--respond-restart`
  seconds (default 3; a real build 25164 takes 8.0-9.2 s to its banner) no
  RTCM goes out and every command is answered with `The Board is
  resetting,do not response any command!*39` (no leading `$`), as build
  25164 answered one 0.4 s into a restart (later in a restart it was not
  tried); then the restart banner and the saved configuration (outputs,
  base mode). The RTCM output also stops after
  `UNLOG COM1` until an `RTCMxxxx COM1` command. Bursts that fall due while
  it sends no RTCM are dropped (counted as `bursts_withheld`), never sent
  late. Replies go out between whole frames, never inside one.
- The station messages are its own, as configured: `RTCM1005`, `RTCM1006`,
  `RTCM1033` and `RTCM1230 COM1 n` put them at the start of every n-th
  epoch, the recording's own of those types left out. 1005/1006 carry the
  position `MODE BASE <id> <X> <Y> <Z>` set (a fixed base only: none while
  it surveys or as a rover), 1033 the `CONFIG BASEANTENNAMODEL` descriptor
  with its own receiver type, firmware and serial number, 1230 no biases;
  all with the station ID of `MODE BASE`. So a station that configured it
  sees what it asked for and judges its profile applied.
- `tools/qemu/e2e.py` (scenario `signal-test`) uses it in-process
  (`ReplayServer(..., responder=Um982Responder(...))`).

The Centipede fixtures are test inputs, not a correction service (see
`test/fixtures/README.md`): replay them only to local casters.

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
chunking removed, flushed to the disk at least once a second) plus the
caster's reply header; `report` then gives, for every epoch's first MSM
frame, its arrival time minus its MSM epoch time per constellation and
overall (p50, p95, max) and `PASS p95 < 1000 ms` or `FAIL`. Keep this
computer's clock synchronised: the figure is only as good as its clock.

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
