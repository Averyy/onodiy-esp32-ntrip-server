# Changelog

What each release changes, newest first. A release's notes on GitHub start with its
section here: add it before you tag ([releases](docs/releases.md)).

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
