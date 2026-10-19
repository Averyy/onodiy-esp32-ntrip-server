# Changelog

What each release changes, newest first. A release's notes on GitHub start with its
section here. Changes since the last release collect under Unreleased; before you tag,
rename that heading to the new version ([releases](docs/releases.md)).

## Unreleased

- Settings, Updates: the trial line names its stage and gives the whole trial's time
  left. The receiver line says "Up to date" when the receiver runs a newer build than the
  release has.
- A restart keeps what the last update check found, so the card still knows the latest
  release after an update or a power cut.
- Times read 17s, 4min 10s, 23h 39min.
- Receiver, Signals: the checks lead the card, the bands one of them, naming every
  constellation with the most bands.

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
