# Changelog

What each release changes, newest first. A release's notes on GitHub start with its
section here. Changes since the last release collect under `## Unreleased`; before you
tag, rename that heading to the new version ([releases](docs/releases.md)).

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
