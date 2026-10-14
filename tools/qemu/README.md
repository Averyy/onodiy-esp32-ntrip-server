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
file, for scripts), `--console connect:PORT` (with `--headless`: the
console on a socket a script listens on, so it can type commands; the log
file still gets everything the firmware prints), `--seconds N`,
`--http-port N`, `--uart1 MODE`, `--no-wdt`, `--pcap FILE` (every Ethernet
frame between the station and QEMU's user-mode network, both ways, in pcap
form: QEMU's `filter-dump` on the NIC's netdev). `--overlay FILE` adds one more sdkconfig overlay after
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

**The clock keeps running across a restart** (QEMU only). On a board,
ESP-IDF's restart (`esp_restart_noos`, through
`esp_system_reset_modules_on_exit`) resets the system timer
(`SYSTEM_SYSTIMER_RST`), so every boot's `esp_timer` starts at 0. In this
QEMU only the CPUs are reset (`rst:0xc (RTC_SW_CPU_RST)`) and the system
timer keeps counting, so after a restart every value taken from uptime
carries the time before it: `lifecycle: boot stable after 704 s` 13 s into
the new boot, `uptime_s` in `/api/status`, the journal's start times
("Start at about ..." early by the previous boot's run time), today's
offline time of a caster slot (the outage looks as long as the whole
previous boot), and the GNSS ladder's first-hour hold after power-on
(counted from the first boot).
These are QEMU artefacts, not firmware bugs; only a fresh QEMU start (or a
board) shows them as a station would. `smoke.py` and `e2e.py` therefore
assert nothing taken from uptime after a restart they ask for: the checks
that span one look at stored values (the comparison's measured time, a
caster stop that survives the restart).

Expected log lines that a station does not print: `E` lines about the
multicast MAC filter (the emulated MAC has none: `Failed to add multicast
filter for IPv4` and `... for IPv6`, two of each at boot, and one `Failed to
remove multicast filter for IPv6` when mDNS stops before a planned restart)
and `QEMU development build: emulated Ethernet instead of Wi-Fi, no setup
AP`. The mDNS responder
runs as on a station, on the emulated Ethernet port, under the overlay's own
host name: `network: mDNS: answering as onocoydiy-qemu.local (web page as
service onocoydiy-qemu)`. QEMU's user-mode network sends the guest's
multicast announcements out of this computer onto its LAN, so under the
station's default name an emulated station would announce `onocoydiy.local`
beside a real one and could take the name from it; `sdkconfig.qemu` sets
`CONFIG_LWIP_LOCAL_HOSTNAME="onocoydiy-qemu"` to keep them apart. (Running
the user-mode network with `restrict=on` would keep the packets in, but it
also cuts the station's outbound update check and needs a `guestfwd` for
every local caster port.) Queries do not come back in, so the name cannot
be looked up from this computer, but a request with
`Host: onocoydiy-qemu.local` passes the Host check like on a board. The
emulated port also gets its IPv6 link-local address (`network: IPv6
link-local fe80::... on station`), as a station's interfaces do, and no
other: QEMU's user-mode network answers the guest's router solicitation
with a router advertisement for `fec0::/64`, and lwIP would form an
address from it on its own (lwIP turns stateless autoconfiguration on for
every interface it adds, whatever `CONFIG_LWIP_IPV6_AUTOCONFIG` says), so
the firmware turns it off per interface right before it creates the
link-local address. An address that is not link-local would show as a
warning, `network: IPv6 address fec0::... on station is not link-local`;
the smoke test fails on one in the 30 s after the link-local line.

With nothing answering on UART1 the firmware behaves as with a dead
receiver: detection gives up after about 26 s. The GNSS recovery ladder's
ESP32 restart is then held (`health: no receiver: restart to look for it
postponed (...)`): while first setup is not finished and no caster is
enabled, and for the first hour of station time after power-on when
nothing was heard from the receiver, so `lifecycle: rebooting (reason 6)`
does not appear in a QEMU run. During that no-receiver episode
`GET /api/status` shows the reasons in `health.reboot_hold` (outside a
ladder episode it names none).

**A planned restart on the APP CPU boots twice** (QEMU only). ESP-IDF's
restart (`esp_restart_noos`, `components/esp_system/port/soc/esp32s3/
system_internal.c`) first resets the other CPU and asks the RTC controller
to stall it (lines 123-124), so on a board the restarting CPU runs alone
until it resets both. Espressif's `esp32s3` machine works out the stall
(`hw/misc/esp32s3_rtc_cntl.c`, `esp32s3_rtc_update_cpu_stall`, lines
148-162) but its handler does nothing (`hw/xtensa/esp32s3.c`,
`esp32s3_cpu_stall`, lines 228-230; tag esp-develop-9.2.2-20260417). A
restart that runs on the APP CPU therefore starts the PRO CPU on the ROM
and the 2nd-stage bootloader at once while the APP CPU carries on, and the
APP CPU's own reset of the PRO CPU a moment later (lines 165-167) starts it
over: two `rst:0xc (RTC_SW_CPU_RST)` banners, the first boot stopped inside
the bootloader. How far that first boot gets depends on QEMU's round-robin
time slice (up to 100 ms, `TCG_KICK_PERIOD` in
`accel/tcg/tcg-accel-ops-rr.h`): on a slow host it reached about `SPI
Flash Size` (the CI log); on a fast one the bootloader loaded the app over
code the APP CPU was still running (`Guru Meditation Core 1 (BREAK)` in
`esp_restart_noos` right after the PRO CPU's reset, then QEMU hung for
good). Nothing else resets the CPU in the bootloader here: this machine has
no RTC watchdog, and the timer-group watchdog's reset output is not
connected. Measured with QEMU's `-d cpu_reset` and the lifecycle task
pinned to core 1: 4 of 4 reboots booted twice in the CI image (amd64, 2
CPUs), 2 of 2 hung on macOS; pinned to core 0, 8 of 8 booted once. The
unpinned task took the PRO CPU in 16 of 16 idle reboots: the APP CPU path
needs load, as during the e2e comparison. On the test board a software
restart prints one banner, and its power-on log shows the bootloader
reaching its RTC watchdog setup about 80 ms after the reset, well inside
the 1 s that `esp_restart_noos` arms that watchdog for.

The firmware runs every restart it plans on a task pinned to the PRO CPU:
the lifecycle service task, pinned to core 0, runs the queued ones and
every immediate one whose caller is not pinned there (`lifecycle.c`
`reboot_now`: leaving safe mode runs in the web server's worker, which has
no core of its own, and its restart is handed to the service task), a
caller pinned to core 0 runs its own (`app_main`, the boot-time trial
check), and the hook-overrun timer (`esp_timer` task) and the fatal boot
error retry (`app_main`) run there too. The one exception is a bench-only
test hook on a board (`CONFIG_UM98X_FLASH_BYTE_CAPTURE`, an abrupt restart
from the station task). A board restarts the same from either core. Which
core an unpinned task is on decides nothing here, and a clean boot proves
nothing either (the unpinned path needs load to land on the APP CPU), so
the checks look at the runner itself: the reboot line names it and its pin
(`lifecycle: rebooting (reason N) from lifecycle, pinned to core 0`), and
the boot checks of `smoke.py`, `e2e.py` and `bench_console.py` fail on a
planned restart from a task not pinned to core 0. `smoke.py` also checks
the service task's pin (`GET /api/diag/tasks`: `lifecycle` on core 0), and
`bench_console.py` leaves safe mode through the web API (the worker's
restart must come from the lifecycle task). The boot checks stay strict
too: a boot that ends in a second `rst:0xc` banner inside the bootloader
right after a planned restart still fails, named as "the restart ran on
the APP CPU, and QEMU has no CPU stall".

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
arriving on UART1 boots normally (with the system timer still counting
from the first boot, see above). `e2e.py` therefore pauses its replay
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
   `onocoydiy-qemu`, a request with `Host: onocoydiy-qemu.local` gets 200
   and one with `Host: evil.local` does not;
   HTTP connection handling: `POST /api/status` with `{}` gets `405` and
   `POST /api/nope` with a body `404`, both with `Connection: close` (not
   `413`); a refused 16 KB body sent whole before the client reads is
   drained after the answer: the station's packets (`qemu-net.pcap`)
   acknowledge every byte of the request and the client's FIN (a server
   that stopped reading would leave its receive window shut, the upload
   stalled in front of the answer); `GET /nope` with a body gets the setup AP's captive redirect
   (this build counts as the AP) with `Connection: close`; `GET
   /api/session` with a body gets `400 bad_request`; a request with
   `Connection: close`, a `GET / HTTP/1.0` and an HTTP/1.0 request with
   `Connection: keep-alive` (answered with `Connection: close`: HTTP/1.0
   connections are never kept) are answered and closed within 2 s; a
   chunked body is never read: `POST /api/nope` gets `404`, `POST
   /api/status` `405`, `POST /api/session` `411 length_required` and `GET
   /nope` the captive redirect, each one answer with `Connection: close`
   and the connection closed (the chunks never come back as a second
   answer); two pipelined HTTP/1.1 requests on one socket are both
   answered, and an HTTP/1.1 connection stays open for the next request;
   both ways a JSON answer goes out, on one HTTP/1.1 connection kept open
   throughout: `GET /api/session` (within the web server's response buffer,
   whose size the script reads from `components/web/web.c`) with its
   `Content-Length`, `GET /api/status` (larger) streamed, chunked; which
   way each must take follows from its measured size, both ways must have
   been taken, and the connection answers again after the streamed answer;
   answers that are streamed go to HTTP/1.0 without chunked framing (which
   HTTP/1.0 does not know): `GET /api/logs?format=text HTTP/1.0` and `GET
   /api/status HTTP/1.0` come without `Transfer-Encoding` (the status, being
   larger than the buffer, without a `Content-Length` either), the log's
   first line and JSON that parses as the body, and the connection closes
   within 2 s of the answer;
   a wrong admin password and a WebSocket handshake without a session
   (public status off) each give one console line naming `10.0.2.2` (how
   the guest sees this computer); after `PUT /api/diag/log-level` `*` at
   `info` a refused handshake prints no `W httpd_uri` line, `debug` is
   refused (`400 invalid_field`, `level`), `GET /api/logs` says
   `level_max` `info`, and `cursor=abc` on `/api/logs` and `/api/journal`
   is refused (`400 invalid_field`, `cursor`); `PUT /api/wifi` answers
   `applied: false, trial: false` (this build only stores Wi-Fi
   credentials) and the next `GET /api/wifi` agrees (no trial, nothing
   pending);
3. UART1 both ways: the detection queries arrive on the host, and text the
   host sends makes detection end with "bytes but nothing valid"; with the
   station then without a receiver (`GET /api/gnss` state `no_receiver`),
   `POST /api/gnss/signal-test` is refused with `409 no_receiver` ("No
   receiver is connected") and `GET /api/gnss/signal-test` stays `idle`;
   then 400 refused requests (`POST /api/nope` with a 2-byte body) whose
   client ends its sending side right after the body: each one answered
   `404` with `Connection: close`, no crash and no restart, and 100 more
   that a route refuses itself (`GET /api/session` with a 1 KB body:
   `400 bad_request`; a refusal must end its request with `ESP_FAIL`, or
   the server would read away the rest of the body and take the client's
   FIN after the drain, see `web_internal.h`). The client's
   FIN is then queued before the answer goes out, and the lingering close
   reads only the bytes `FIONREAD` counts, never that FIN
   (`components/web/web.c`, `web_linger_close`). The drain before the fix
   read on until `recv()` returned 0 and so took the FIN after our own FIN
   was out: that shut lwIP's receive side in `LAST_ACK`, where lwIP frees
   the connection on the client's last ACK without telling the socket
   (lwIP bug #62141), and the `close()` that followed worked on freed
   memory. Reading the FIN was our defect (the drain only needs the body);
   this check guards the whole path, and `test/host/main/test_web_conn.c`
   holds the drain itself to never reading the FIN, against a model of
   lwIP's socket. How often the old image failed in QEMU is no rate a
   station would see: here it lost the 22nd request of this check (the
   pcap shows the station's FIN right after the SYN-ACK, before the
   request) and crashed after 72 and after 165 in two earlier runs, while
   on a test board (ESP32-S3 on Wi-Fi) the old drain answered 4400 such
   requests, one at a time and six at once, without a fault; built with
   `CONFIG_HEAP_POISONING_COMPREHENSIVE` it aborted three times in 4000
   (freed memory written, found when the block was next allocated), and
   the fixed drain in that build answered all 4000; the current code with
   only the drain put back as it was aborted six times in 8000, and the
   same build with the drain as it is answered all 8000; last, a client back on
   its local port 15 s after the station ended its connection first
   (`Connection: close`) gets a SYN-ACK at once (the station's packets in
   the pcap show one per connection, told apart by their initial sequence
   numbers: lwIP sends a SYN-ACK again with the same one when the client's
   ACK is lost, as when QEMU's MAC drops a received frame, and that is no
   third connection). lwIP never takes a new SYN on a
   connection it holds in TIME_WAIT (`tcp_timewait_input`: a reset, or a
   bare ACK the client answers with a reset), and with lwIP's default MSL
   that lasts 120 s; Linux hands the same port out again well within that
   (in a CI-parity run on arm64 the 47th half-closing request reused the
   port of a connection the station had closed 21 s before, and every SYN
   for 40 s got a bare ACK). `sdkconfig.defaults` sets MSL to 5 s
   (TIME_WAIT 10 s); with lwIP's 60 s this check fails (one SYN-ACK);
4. over 75 s (`--observe`, at least 45): no `Guru Meditation`, `abort()`,
   stack overflow, failed assert, backtrace or task watchdog report on the
   console, and no reboot loop (a restart without the firmware's own
   `rebooting (reason N)` line before it is a crash; more than two boots is
   a loop); no GNSS ladder restart (`rebooting (reason 6)`), no console
   note about dropped lines (`[diag:`), and the mDNS line, the mDNS task's
   stack in PSRAM (`mDNS task will be created from SPIRAM`; the console
   writer's too, no `console task stack in internal RAM`) and the
   station's IPv6 link-local line in the boot log, with no `is not
   link-local` warning in the 30 s after it; at the end of the window `GET /`
   and `GET /api/status` still answer, the console writer task runs (`GET
   /api/diag/tasks`), the `lifecycle` task is pinned to core 0 and every
   planned restart's reboot line names a task pinned to core 0 (see "A
   planned restart on the APP CPU" above), and `GET /api/health` counts no
   console drop; then
   `POST /api/system/reboot`: `network: mDNS: goodbye sent` is on the
   console before the next boot, with no shutdown-budget warning, and the
   station answers again. QEMU's UART sends every byte at once, so a log
   burst cannot back the console up here; the writer's ordering and drop
   rules are host-tested (`test/host/main/test_diag_console.c`).

Every HTTP answer is read to the end its framing gives (`Content-Length`,
the last chunk, or the end of the connection), waiting up to 30 s for it,
and a "closed within" time counts from the end of the answer. The emulated
Ethernet MAC drops a received frame when the firmware has no free receive
buffer (`opencores.emac: ... RX frame dropped (0x14)` on the console), and
TCP sends it again only seconds later: in CI an answer once came 5.5 s
late that way, which a check reading for 5 s took for an answer without
chunked framing. `sdkconfig.qemu` gives the MAC 16 receive buffers instead
of IDF's 4, so a burst of frames waits for the receive task instead of
being dropped.

Every request is watched rather than retried or given a longer timeout
(all but the polls that wait for the station to come up: QEMU's network
holds those connections until the guest listens): one still unanswered
after 5 s gets a line every second in
`slow-requests.log` with a probe on another connection (does the server
answer meanwhile?), each QEMU thread's CPU time (is the emulator running?)
and every console line printed since the request began, all on the host's
clock; `run.sh --pcap` puts every frame of the run in `qemu-net.pcap`, so
the request's own packets show whether it reached the station and what
came back. A request lost without a firmware log line was seen once in CI;
two causes were found and fixed since (the drain above, and a client port
reused while the station held it in TIME_WAIT, which the pcap of a parity
run showed), and the next one, if any, leaves this evidence. The summary
names the requests that were watched.

QEMU always stops with it: on a normal end, Ctrl-C, SIGTERM or SIGHUP the
script stops QEMU on the way out, and `run.sh` gets `--seconds` (observe or
timeout, whichever is longer, plus 180 s for the half-close, drain and port
reuse checks and 60 s) as a backstop should the script be killed outright.

The `qemu-smoke` CI job runs the same in the pinned ESP-IDF image (which
ships qemu-xtensa) with `--out qemu-smoke` and uploads that directory and
the build log as an artifact. `tools/ci/qemu_linux.sh --job smoke` runs that
job on this computer, in the same image with its steps read from
`build.yml` (README, Building), and copies what it uploads to
`build-ci/qemu-smoke/`.

## Bench console

The bench overlay (`sdkconfig.bench`) runs in QEMU too, built into
`build-qemu-bench/`. Its command line shares the console UART with the log:
the log's console writer installs the UART driver at boot, and the command
line reads through that same driver. Building the overlay cannot show that
both still work; `bench_console.py` runs it:

```sh
tools/qemu/run.sh --overlay sdkconfig.bench --build-only
uv run tools/qemu/bench_console.py    # logs in build-qemu-bench/bench-console/
```

It listens for the console (`--console connect:<port>`, UART1 not
connected: a station without a receiver), boots the image headless and
reports PASS or FAIL for each of these (exit status 1 if any failed):

1. the start line (`bench console on the UART0 console`) and the prompt
   (`bench> `) are on the console;
2. typed commands are answered: `help` lists `help` and every command
   `components/bench/bench.c` registers (the receiver link's hooks only when
   the build has them), `ftimes status` answers `not running, 0 records`,
   `ftimes summary` prints the summary line in its form for a recording
   without a frame (`...; read to published: none; arrival to read: none;
   0 frames without an interrupt time; longest reader gap ...`) with the
   `interrupt times since boot` line after it, and an unknown command
   answers `Unrecognized command`;
3. the log still reaches the console after the command line started (a log
   line at least 5 s after the start line);
4. `GET /` answers; with the first admin password set, the web log (`GET
   /api/logs?format=text`) has the start line and no console driver error,
   and `GET /api/health` counts no console drop;
5. replies stay whole while another task logs: `logflood 15 2` makes a
   task above the command line's log a line every 2 ms for 15 s, and
   meanwhile `status`, `race_result`, `heapfrag free`, a second `logflood`
   (refused), `ftimes status`, `ftimes summary` and `help` are typed round
   after round; every console line with `BENCH` in it must be one of those
   replies in its whole form, none with a log line's time and tag inside,
   `help` still lists every command, every flood line arrives whole and
   once, flood lines did arrive while the replies were printed, and `GET
   /api/health` counts no console drop afterwards (this runs after the web
   checks: the flood overwrites the web log's ring);
6. safe mode, entered and left: `safemode ENTER-SAFE-MODE` restarts as a
   failed boot at the crash limit (`lifecycle_bench_reboot_into_safe_mode`,
   bench builds only), the next boot runs in safe mode (`SAFE MODE` on the
   console, `GET /api/status` `safe_mode.active`), and `POST
   /api/system/safe-mode/clear` leaves it: the request is served by the
   web server's worker, a task with no core of its own, and its reboot line
   must read `(reason 10) from lifecycle, pinned to core 0` (the restart
   handed to the lifecycle task); the boot after it is a normal one;
7. throughout: no `UART driver already installed` or `uart driver error`
   line, no `[diag:` note, no panic, and three boots: the two restarts
   above, each with the firmware's own reboot line from a task pinned to
   core 0, and no other.

The bench console's replies, its prompt in the dumb-terminal mode and the
receiver flasher's byte echo go to the console through the log's console
writer (`diag_console_write`), a whole line at a time under the lock the
log lines take, so a log line never lands inside one. What still goes out
a byte at a time is the echo of what is typed (linenoise's), so a log line
can land inside an echoed command.

The firmware's dumb-terminal mode applies here (the script does not answer
the terminal status request), so a log line that comes while the prompt
waits follows it on the same line (`bench> I (2014) network: ...`). QEMU
stops with the script as with `smoke.py`. The `qemu-bench-console` CI job
runs it with `--out qemu-bench-console` and uploads that directory and the
build log; `tools/ci/qemu_linux.sh --job bench-console` runs that job on this
computer.

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
uv run tools/qemu/e2e.py                       # every scenario; logs in build-qemu/e2e/
uv run tools/qemu/e2e.py --scenario stream --caster docker
uv run tools/qemu/e2e.py --scenario signal-test
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
and, as soon as the station streams, configures slot 1: `Bench`, host
`10.0.2.2`, the caster's port, NTRIP 1.0, mountpoint `BENCH`, a made-up
password, the standard profile. Until the `signal-test` scenario nothing
answers the station's receiver queries: its identity query goes unanswered
and, while RTCM flows, never takes the line (the casters are never paused
for it). PASS or FAIL for each of these (exit status 1 if any failed):

- scenario `stream`: slot 1 streaming within 60 s (`ICY 200 OK` from the
  fake caster, `OK` from the BKG caster with `--caster docker`) and never
  `paused` while the scenario runs, with no `COMMAND lease taken by
  identify` on the console; `/api/gnss/signals` lists the fixture's
  constellations with their bands, as computed from the fixture with
  `benchlib`'s band table (the firmware's own: NavIC signal 22 is L5; SBAS
  never listed); `/api/health` `hub.frames_in` grows by at least five
  epochs' worth of frames in 10 s and the slot's `epochs_sent` rises; the
  caster's record holds at least 60 s of frames (`--observe`, default 75),
  checked strictly: every one CRC-valid and byte-identical to a fixture
  frame; after the hub's replay of its sticky station frames
  (1005/1006/1033/1230) the record starts at a fixture epoch's first frame,
  never inside an epoch; then fixture order (modulo the loop; fixture frames
  the station never forwards skipped, the SBAS numbers 1101-1107); any gap
  falls on an epoch boundary (an epoch's last frame, then sticky copies and
  the next epoch from its first frame, whole: frames carry the UART
  interrupt's arrival time, so a firmware reader stalled by flash writes
  no longer makes the hub start an epoch at its first MSM; only the
  record's very first epoch, and only without sticky copies in front of it,
  may lack its leading station messages, never its MSMs; anywhere else a
  missing station message fails the check); nothing between frames, a cut frame
  only at the very end; receiver maintenance answers its start and
  extension with the time it set (900 s, then 1800 s left), and
  `GET /api/gnss` agrees. With
  `--caster docker` the record is what a `GET /BENCH` client receives from
  the BKG caster (`tools/bench/docker/bkg-caster`, built on first use);
  bytes in front of its first frame are allowed there, because that caster
  starts a new client inside older data;
- scenario `onocoy-guard`: slot 0 as a test-Onocoy slot (`test_onocoy`,
  only in builds with test-Onocoy slots: on a firmware without them it
  prints SKIP and passes) against a second fake caster scripted with
  Onocoy's termination reasons: a credential in use on credentials that
  never worked stops the slot at once, and so do `409`, `ERROR - Mount
  Point Taken or Invalid` and `ERROR - Already Connected` (no refused login
  counted); three `503` replies keep the fast transient row; a refused
  login whose line a third caster closes before its line end counts as a
  refused login (300 s); the waits after a rejected login,
  a login block and an in-use reply (each delay within [entry, 1.2 x entry]
  plus 2 s for QEMU, read from `next_retry_s` and from the connection
  times); sessions the caster ends quickly (and a `Device reconnected` line)
  stop the slot after a score of 3, waiting 5 s at a score of 1 and 10 s at
  2 (a `Device reconnected` line scores 2 at once); banned
  and duplicate reports stop it at once; a save and
  `POST /api/casters/0/retry` clear a stop, a reboot does not; the journal
  has the `caster_stop` records. `--long` adds the 5-minute wait after a
  rejected login and the whole in-use retry ladder (about 8 minutes more).
  While the guard waits after a rejected login, a login block or an in-use
  reply, `/api/health` `network.ladder.triggers` has no `protected` (a
  Wi-Fi restart or a reboot cannot fix a caster's reply); pointed at a
  closed port (the connect refused) it has. It runs before `stream`; a wait
  that a receiver lease interrupted is noted and not judged;
- scenario `signal-test` (runs last, under 15 minutes on this computer): the replay gets a
  scripted UM982 behind it (`tools/bench/benchlib/um982_sim.py`, see
  `tools/bench/README.md`, `--respond`): it answers the identity query, the
  configuration dump and base mode, takes the configuration and switches
  signal groups (4 5, 3 6, 5 0 and 7 0, less those it is told to refuse),
  withholding its RTCM for about 3 s per restart. A fixed profile (the
  stream's own position when it carries one, else a made-up one; the
  responder takes any) through `PUT /api/gnss/profile` makes the station
  identify the receiver, switch it from 4 5 to 3 6 and lock; the comparison
  is refused with automatic configuration off (`autocfg_off`) and in
  receiver maintenance (`receiver_maintenance`); with 7 0 refused by the
  receiver a comparison ends as `rejected`, the profile back on 3 6 and no
  `config_failed`; 3 6 against 7 0 with `phase_s` 60 is restarted in the
  middle of measuring (the replay paused across the reboot, as in the
  guard's reboot) and resumes with its measured time kept, then runs
  through settling, measuring, switching, settling, measuring and deciding
  in that order within their time bounds, shows both results, and keep 1
  leaves the profile on 7 0; then 12 applies of the stored profile
  (`POST /api/gnss/apply`), each after the slot streamed two more epochs
  and a random wait of up to 1 s, so the station decides to pause at any
  moment of the receiver's second (after an epoch, between its station
  messages and its first MSM, inside its MSMs), and 12 more with each
  epoch's last MSM sent 0.7 s after the rest of it
  (`ReplayServer.set_last_frame_delay`; the rest takes about 0.3 s to
  reach the station, so the last MSM comes about 0.4 s after the one
  before it): longer than the hub's 250 ms gap rule, so a pause decided in
  that time must wait for the last MSM rather than take the quiet line for
  the epoch's end (as a stalled line or reader makes it look: under load
  in CI's container an epoch's last MSM reached the station that late, and
  the caster record lost it; the station logs a hold that waited 300 ms or
  more, and warns when it had to hold without a boundary, which the
  scenario prints); the configured receiver sends the station messages
  the profile asks for (its own 1005, 1033 and 1230 open every epoch, so
  the station judges the profile applied); then, still with each epoch's
  last MSM 0.7 s late, 6 saves of slot 1's settings (each closes the
  session for the new settings and reconnects; every other one 0.1 s after
  a burst began going out, inside its epoch, the others at a random moment)
  with at least one close that waited for the end of the epoch it was
  sending (`ending the session after the epoch it was sending` on the
  console) and none that gave up on it (`closing inside an epoch`), and a
  restart asked for 0.1 s after a burst began (the web server restarts 1 s
  later, so the shutdown comes early in the next one) with the replay
  streaming until that session has ended (then paused, a waiting late MSM
  dropped too: the next boot must not see RTCM arrive across the restart,
  see the UART1 section): the session the restart ends finishes its epoch
  first (tried again, up to 3 restarts, when the request came late and the
  shutdown found no epoch in flight). Before this a planned close (reboot,
  new settings, a restart by the health monitor) closed every session at
  once, and the last chunk a caster got ended inside an epoch whenever the
  close fell in the receiver's MSM burst; the restart in the comparison
  above, with the replay paused 2 s before it, could not show that. And
  every caster record of the scenario (the slot is paused for each switch
  and each apply) passes the strict epoch check above, against the fixture
  as that receiver sends it; a record that ended (the station closed the
  session during a pause, for new settings, or a restart did) and the part
  of the first session before the configuration's pause also end on a
  whole epoch: their
  last frame that is no station message is its epoch's last, with nothing
  cut after it (the next epoch's station messages may follow: a caster can
  send them before the pause, and they never belong to an MSM run). Before
  the hub's hold point (`rtcm_hub_hold`, see `components/rtcm_hub`) a
  record could end `1005, 1033, 1230, 1077` with the rest of that epoch
  never sent: the station paused by a timer while RTCM kept arriving until
  the receiver session took the line, and a caster waiting for data sent
  what came. The replay cannot change bands, so this checks the flow, its
  timing, the refusal, the restart and the records, not which group is
  better;
- throughout: `smoke.py`'s console checks (no panic, abort, stack overflow,
  failed assert, backtrace or task watchdog report, no reboot loop, no GNSS
  ladder restart; the reboot the scenario asks for is allowed).

Everything lands in `--out`: `console.log`, `e2e.log` (the run's events),
`replay/` (what the firmware sent on UART1 and, in `replay.log`, the
responder's replies), `caster/` and `guard/` (each caster's
`connections.jsonl` and records), the HTTP answers (`sgt-*.json` for the
comparison), `qemu-net.pcap` and `slow-requests.log` (as `smoke.py` writes
them) and `summary.txt`. The `qemu-smoke` CI job runs it after the smoke test with
`--out qemu-e2e --scenario all` and uploads that directory too.
