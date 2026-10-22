# Changelog

What each release changes, newest first. A release's notes on GitHub start with its
section here. Changes since the last release collect under `## Unreleased`; before you
tag, rename that heading to the new version ([releases](docs/releases.md)).

## Unreleased

- Network: each station has its own name, `onodiy-` and its hotspot's four characters
  (`onodiy-c978.local` for the hotspot `onodiy-C978`). Two stations on one network
  took `onodiy.local` from each other and both ended up under other names. A station
  without a name set in Settings answers at its new name after the update; one with a
  name keeps it. The local server's default mountpoint is the name, as before.
- A new station name shows in the header and the tab title as soon as it is saved; the
  station answers at the new address after the next restart, as before.
- Receiver settings: Onocoy's message set with MSM4 observations (about 40% less data,
  for a slow link or a server with a rate limit) next to the default MSM7 set.
- Boards: the firmware knows which board it runs on and names it on Logs (Chip and
  Wi-Fi). NTRIP-X (GNS Electronics) support, in testing: its own install with 8 MB
  flash (`onodiy-install-um980-ntrx.bin` in the release), its receiver on GPIO16/15,
  and its LEDs: the status LED blinks the codes of GNS's quick guide (white, then blue
  for Wi-Fi and green for the servers; fast red for a new alert; fast yellow and pink
  while the button is held to 10 s and 20 s), the Wi-Fi LED brightens with the signal
  and the GNSS LED shows a fix.
- Release: one image per board to write at 0x0 after an erase
  (`onodiy-install-16mb.bin` for the UM982 and UM980 boards), with the commands in the
  notes.
- Logs: a crash's core dump names the crashed task, its address and a backtrace (they
  were always empty: ESP-IDF v6.1 no longer defines the setting the firmware checked).
- Status: the "Update available" banner shows here too, and its button says where it
  goes: "Settings page".
- The dot beside a tab (an update is waiting) lines up with the tab's name.
- Receiver: an elevation mask setting under Signals. The default is 0°, so the receiver
  tracks every satellite above the horizon (its own default was 5°). Existing stations
  switch to 0° and configure the receiver once after the update.
- Local server: up to 4 rovers at once (was 2).
- Receiver: UM980 support, in testing on the NTRIP-X. The station configures a
  UM980 with its own signal group, by default 2 (every signal: both GPS L2 signals, L1C,
  GLONASS G3, Galileo E6, NavIC), and turns on its multipath mitigation. The Signals card
  shows the UM980's groups when one is connected.
- Receiver firmware updates for the UM980 too (receiver flashing itself stays off in
  station builds until it has been tested on a board, as for the UM982): its packages pass
  the same checks with the UM980's own marks, and releases carry builds 11833 and 23980.
  A board without room in flash (the NTRIP-X) keeps the package in memory; if a restart
  cuts the update short, Retry downloads the package again (or asks for the same file).

## v0.1.5

- Wi-Fi: no more "sta is connecting, cannot set config" error in the log at every join
  (ESP-IDF's supplicant made a call the Wi-Fi driver refuses at that moment; it changed
  nothing).
- NTRIP: Delete in a server's editor (asks first) removes the server and erases its
  settings, password and certificate; the slot is free again.
- NTRIP: a slot given another server (deleted, or edited to another host) starts its
  figures over: online and offline today, the availability history, bytes, connects
  and the last reply no longer carry over from the server before.
- Status: a Performance card. Ping times to your router and to Onocoy (or the first
  server that is on), the Wi-Fi signal, CPU load and free memory: the latest reading
  (every 30s), the last 24h's average and worst, and a line over that time. Two small
  pings every 30s; the history stays in memory, so a restart starts it again.

## v0.1.4

- Receiver: the signal checks side by side on one banner; a tidier sky key; "Reset
  location" (was Survey again); the OpenStreetMap link beside "Hide the map".
- Receiver settings: the defaults say "(default)" instead of "(recommended)"; the
  station-restart option reworded.
- Status, Onocoy: today's online share is the headline, the offline time left a row.
- Onocoy setup: the username says it is the reference station credential.
- Receiver firmware: UM982 Build 23575 (February 2026) is in the release for receivers on
  older builds (receiver flashing is still off: stations download and check it only).
- Settings, Danger zone: "Receiver wiring" sets the ESP32 pins and receiver port for a
  board you wired yourself (from the next start; updates and a factory reset keep it);
  "Revert firmware" moved here from Updates and names the version it starts.
- Buttons no longer end in "…".
- NTRIP: a server that streams fine is no longer restarted now and then by the health
  monitor (a timing race made its progress look 49 days old).

## v0.1.3

- Receiver data no longer lost while the Logs page reads the firmware's task list (it
  briefly blocked the receiver's serial port).
- Pages no longer fail now and then with an empty answer when a browser has several
  requests open: the station never closes a connection that has a request waiting, has
  room for 10 connections, and the page retries a lost read once.

## v0.1.2

- Settings, Updates: the trial line names its stage and gives the whole trial's time
  left; the receiver line says "Up to date" when the receiver runs a newer build than the
  release has; a restart no longer forgets the latest release.
- Status: the Receiver card shows the signal strength (the Sky card already shows the
  bands).
- Receiver: the Signals card leads with its checks; the satellite table is half as tall.
- Logs: the event list is in time order, each start below what happened after it;
  "Previous uptime"; a UM982's own descriptor no longer shows as differing.
- NTRIP: "More servers" counts the slots left beside Onocoy.
- Times read 17s, 4min 10s, 23h 39min.

## v0.1.1

The same firmware as v0.1.0, released to try an update from the web UI (Settings,
Updates).

## v0.1.0

The first release.

- Sets up the UM982 by itself: the 4-band signal group, a survey-in, and the RTCM
  messages Onocoy asks for (1005, 1033, MSM7 for six constellations and 1230, every
  second).
- Streams to Onocoy and up to 3 more NTRIP servers at once, and to your own rover on
  your home network.
- Web UI for setup, status with a sky map, receiver settings with a signal-group
  comparison, logs and updates.
- Watches its own health and recovers by itself.
- Updates from GitHub Releases, installed only when you click Update. A new version runs
  on trial and goes back to the previous one if it fails.

Not tested yet: streaming to Onocoy live, and a real rover.
