# Changelog

Changes per release, newest first. A release's GitHub notes start with its section here.
Changes since the last release go under `## Unreleased`; rename it to the new version
before tagging ([releases](docs/releases.md)).

## v0.1.8

- Web UI: shown passwords are cleared at logout; Hide empties a box Show filled.
- Status: Performance card: no axis line, rows centred, name and words on one line, Wi-Fi
  reading without dBm, no space before %, time covered as fine print.
- Updates: a dev build is offered releases newer than its base; receiver builds are offered
  whatever the app's version.
- QEMU test: the bench `crash` runs on core 0 (fixes an occasional 8 MB board failure).

## v0.1.7

- Web UI: password boxes have Show/Hide, except the joined Wi-Fi's. Show on an empty box
  fills the stored password (NTRIP servers, rover login, setup hotspot). The Local server
  card and the hotspot row have Show/Hide.
- API: new admin GETs `/api/casters/{n}/password`, `/api/local-caster/password`,
  `/api/wifi/ap-password`.
- Status: "Server ping" pings the internet (1.1.1.1) when no NTRIP server is on, labelled
  "Internet ping". Performance lines each have a scale and a rule between figures.
- Updates: the last receiver-update step shows a moving bar and "Finishing, please wait."
- Updates: no "update session is open" message while the update runs. Dismissable
  message text is centred beside its ×.
- Updates: "Finish checks now" ends a new app version's first checks (about 11 minutes)
  at once; the crash watch continues. UI says "checking the new version", not "on trial".
- Receiver: commands that older firmware refuses are not sent. UM980 before build 23980
  lacks message 1230 and `ANT1GROUPA`; both are skipped, and the Signals card, Onocoy
  card and Ask the receiver name the build that has them.

## v0.1.6

- Receiver firmware updates on in releases: the update banner offers a newer receiver
  build and installs it over the station's link, no PC. Tested on a UM980 (NTRIP-X,
  11833 to 23980); the UM982 uses the same steps, not yet tested on hardware. Releases
  carry UM980 packages 23980 and 25735 beside UM982 18975 and 23575.
- Network: station name is `onodiy-` plus the hotspot's four characters (hotspot
  `onodiy-C978` is `onodiy-c978.local`). Two stations on one network used to take
  `onodiy.local` from each other. A station with no name set answers at the new name after
  the update; a set name is kept. The local server's default mountpoint is the name.
- A new station name shows in the header and tab title when saved; the address changes
  at the next restart.
- Receiver settings: correction messages "Smaller (MSM4)" (about 40% less data) beside
  "Recommended (MSM7)", the default.
- Receiver settings: "Ask the receiver" sends one read-only command (VERSIONA, CONFIG,
  MODE, ANT1GROUPA, BASEPOSA, SPPNAVA) and shows the answer. CONFIG and ANT1GROUPA pause
  the NTRIP servers for about a second. Each question is listed on Logs.
- Updates: the last receiver update's result has a × to dismiss it.
- Updates: a receiver update can start after a new app version's first checks (about 11
  minutes), not after the 24-hour crash watch.
- Signals: when a UM980 refuses message 1230, the warning names firmware 23980 or later
  and the Onocoy card links to Updates. It also says rovers of other brands get less from
  GLONASS.
- Updates: a receiver build with another partition layout (UM980 11833 to 23980 or later)
  says so in its dialog and needs its own tick. Builds with receiver updates now allow
  these (before, the station refused them). Tested on the NTRIP-X, 11833 to 23980.
- Boards: the firmware knows its board and names it on Logs (Chip and Wi-Fi). NTRIP-X
  (GNS Electronics): 8 MB flash install (`onodiy-install-um980-ntrx.bin`), receiver on
  GPIO16/15, LEDs per GNS's quick guide (status LED codes: white, then blue for Wi-Fi and
  green for servers; fast red for a new alert; fast yellow and pink while the button is
  held 10 s and 20 s; Wi-Fi LED follows signal; GNSS LED shows a fix).
- Release: one image per board to write at 0x0 after an erase
  (`onodiy-install-16mb.bin` for the UM982 and UM980 boards); commands in the notes.
- Logs: a crash's core dump names the task, address and backtrace (they were empty:
  ESP-IDF v6.1 no longer defines the setting the firmware checked).
- Status: the "Update available" banner shows here too; its button says "Settings page".
- The dot beside a tab (update waiting) lines up with the tab name.
- Receiver: elevation mask setting under Signals. Default 0° (receiver default was 5°).
  Existing stations switch to 0° and configure the receiver once after the update.
- Local server: up to 4 rovers (was 2).
- Receiver: UM980 support, in testing on the NTRIP-X. Configured with signal group 2 by
  default (both GPS L2 signals, L1C, GLONASS G3, Galileo E6, NavIC), multipath mitigation
  on. The Signals card shows the UM980's groups.
- Receiver firmware updates for the UM980 (receiver flashing stays off in station builds
  until tested on a board, as for the UM982): same checks with the UM980's own marks;
  releases carry builds 11833 and 23980. A board without spare flash (NTRIP-X) keeps the
  package in memory; if a restart cuts the update short, Retry downloads it again (or asks
  for the same file).

## v0.1.5

- Wi-Fi: removed the "sta is connecting, cannot set config" log error at each join
  (a supplicant call the driver refuses at that moment; no effect).
- NTRIP: Delete in a server's editor (asks first) removes the server and erases its
  settings, password and certificate; the slot is free.
- NTRIP: a slot given another server (deleted, or edited to another host) restarts its
  figures: online/offline today, availability history, bytes, connects, last reply.
- Status: Performance card. Ping to the router and to Onocoy (or the first server on),
  Wi-Fi signal, CPU load, free memory: latest reading (every 30 s), 24 h average and
  worst, and a line over that time. Two small pings every 30 s; history is in memory, a
  restart starts it again.

## v0.1.4

- Receiver: signal checks side by side on one banner; tidier sky key; "Reset location"
  (was Survey again); OpenStreetMap link beside "Hide the map".
- Receiver settings: defaults say "(default)", not "(recommended)"; station-restart
  option reworded.
- Status, Onocoy: today's online share is the headline; offline time is a row.
- Onocoy setup: the username is labelled the reference station credential.
- Receiver firmware: UM982 build 23575 (February 2026) in the release for receivers on
  older builds (receiver flashing still off: stations download and check it only).
- Settings, Danger zone: "Receiver wiring" sets the ESP32 pins and receiver port for a
  self-wired board (from the next start; kept across updates and factory reset).
  "Revert firmware" moved here from Updates and names the version it starts.
- Buttons no longer end in "…".
- NTRIP: the health monitor no longer restarts a server that streams fine now and then
  (a timing race made its progress look 49 days old).

## v0.1.3

- Receiver data no longer lost while Logs reads the firmware's task list (it briefly
  blocked the receiver's serial port).
- Pages no longer fail with an empty answer when a browser has several requests open:
  the station never closes a connection with a request waiting, has room for 10
  connections, and the page retries a lost read once.

## v0.1.2

- Settings, Updates: the trial line names its stage and the whole trial's time left; the
  receiver line says "Up to date" when the receiver runs a newer build than the release;
  a restart keeps the latest release.
- Status: the Receiver card shows signal strength.
- Receiver: the Signals card leads with its checks; the satellite table is half as tall.
- Logs: event list in time order, each start below what happened after it; "Previous
  uptime"; a UM982's own descriptor no longer shows as differing.
- NTRIP: "More servers" counts the slots left beside Onocoy.
- Times read 17s, 4min 10s, 23h 39min.

## v0.1.1

Same firmware as v0.1.0, released to try an update from the web UI (Settings, Updates).

## v0.1.0

The first release.

- Sets up the UM982 by itself: 4-band signal group, survey-in, the RTCM messages Onocoy
  asks for (1005, 1033, MSM7 for six constellations and 1230, every second).
- Streams to Onocoy and up to 3 more NTRIP servers at once, and to your own rover on the
  home network.
- Web UI: setup, status with a sky map, receiver settings with a signal-group
  comparison, logs, updates.
- Watches its own health and recovers by itself.
- Updates from GitHub Releases, installed only on Update. A new version runs on trial and
  reverts to the previous one if it fails.
