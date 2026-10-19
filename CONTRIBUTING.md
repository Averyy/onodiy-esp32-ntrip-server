# Contributing

## Clean room

All code here is written from scratch. Never copy code from other firmware
projects.

## Running the checks

Python runs through [uv](https://docs.astral.sh/uv/) (`uv run`), never pip.

- Firmware: `idf.py set-target esp32s3` and `idf.py build` with ESP-IDF v6.1
  (see "Building" in the [README](README.md#building)).
- Host unit tests: in `test/host`, `idf.py --preview set-target linux`,
  `idf.py build`, then `./build/onodiy_host_tests.elf`.
- Bench tool tests: `cd tools/bench && uv run --locked pytest`.
- CI's host-test job in its Linux container (Docker): `tools/ci/host_tests_linux.sh`.
- CI's QEMU jobs (Docker): `tools/ci/qemu_linux.sh`.

## Changes

Keep changes small and focused: one fix or feature per pull request.
