# Development

Building, the tests, CI's jobs run locally, bench builds and the emulator.
Back to the [README](../README.md).

## Building

Requires ESP-IDF v6.1. From the repository root:

```sh
. $IDF_PATH/export.sh            # your ESP-IDF v6.1 checkout
idf.py set-target esp32s3
idf.py build
idf.py -p <port> flash monitor   # the USB-C printed "ESP32S3", CH343P way round (see [the two USB-C ports](hardware.md#the-two-usb-c-ports))
```

Host unit tests (Linux or macOS, no hardware needed):

```sh
cd test/host
idf.py --preview set-target linux
idf.py build
./build/onodiy_host_tests.elf
```

CI (`.github/workflows/build.yml`) runs both, with the bench tools' tests, for every push
to `main` (changes to documentation only run nothing). Release tags, pull requests, a
weekly run and **Actions > build > Run workflow** also run the bench-overlay build and
the two QEMU jobs (below). A release tag publishes the GitHub Release only once every
job has passed (see [releases](releases.md)).

CI builds the host tests on Linux with gcc and glibc, which find problems a macOS
(clang) build does not: other warnings under `-Werror`, archive order at link time, and
the test task's real stack size. To run CI's host-test job before pushing, in the same
pinned container, with Docker:

```sh
tools/ci/host_tests_linux.sh              # --keep-going lists every build error, not just the first
```

It reads the image and the commands from `build.yml`, builds a copy of the working tree
inside the container (nothing in the checkout is written) and fails on any compiler
warning in the project's own sources too. Whatever in the job it would not run as CI
does (an action other than checkout, setup-uv and upload-artifact, a matrix, a job
timeout, an expression to evaluate, a workflow-level `env:`) stops it before the first
step (`tools/ci/ci_job.py`).

CI's two QEMU jobs (the smoke test with the end-to-end run, and the bench console; see
[`tools/qemu/README.md`](../tools/qemu/README.md)) run the same way:

```sh
tools/ci/qemu_linux.sh                    # both jobs; --job smoke or --job bench-console for one
```

The container gets the repository's history too (the jobs name the build with `git
describe`) and, by default, 2 CPUs (`--cpus`), since the QEMU checks depend on timing:
the fewest a GitHub-hosted standard Linux runner has (2 for a private repository, 4 for
a public one). What a job uploads lands in `build-ci/<job>/`, with `steps.log` holding
every step's output. CI's runners are amd64 (the default `--platform`); on Apple
silicon Docker emulates them, and a run of both jobs took 39 minutes there, 21
natively (`--platform linux/arm64` is native and faster, but not what CI runs).

Notes:

- The console runs at 115200 baud on UART0, the same rate as the chip's ROM
  bootloader output. The computer reaches UART0 through the CH343P, that is the
  `ESP32S3` USB-C plugged in the CH343P way round (see
  [the two USB-C ports](hardware.md#the-two-usb-c-ports)).
- Only tag builds in CI are release versions (`vX.Y.Z`, passed as
  `-DRELEASE_VERSION=`; configure refuses it unless HEAD carries that tag). Every
  other build is named `<git describe>+dev` and is never offered updates by the daily
  check. The version is
  fixed when CMake configures; run `idf.py reconfigure` before a local build whose
  version matters.
- The partition table is permanent once a station is deployed (see the comments in
  `partitions.csv`). Boards with 8 MB flash (the NTRIP-X) run the same build with
  `partitions-8mb.csv`, whose board marker tells the app which board it is. Every board's
  install files from a build, checked (the app within the 2.5 MB slot of the 8 MB table,
  each bootloader valid for its flash size), as CI makes them for a release:

  ```sh
  tools/ci/install_files.sh build install   # install/onodiy-install-16mb.bin, ...-um980-ntrx.bin
  ```
- Receiver firmware updates are off in the default build until they have been tested
  on a bench unit: a package can be uploaded and checked against every rule, but
  nothing is sent to the receiver. A bench build turns them on together with a bench
  console on UART0 that runs the receiver tests one at a time and records every byte
  (`help` lists the commands), and copies the console to the built-in USB (see
  [the two USB-C ports](hardware.md#the-two-usb-c-ports) for its Wi-Fi cost):

  ```sh
  idf.py -B build-bench -D SDKCONFIG=build-bench/sdkconfig \
         -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench" build
  ```

  Never release a bench build: `tools/release_manifest.py` refuses one.

## Without hardware (QEMU)

The firmware also runs in Espressif's QEMU, with an emulated Ethernet port in place of
Wi-Fi and UART1 (the receiver link) on a local socket:

```sh
. $IDF_PATH/export.sh
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa   # once, then export.sh again
tools/qemu/run.sh               # builds into build-qemu/, then the web UI is at http://localhost:8080/
nc 127.0.0.1 5556               # in another terminal: the bytes the firmware sends to the receiver
```

Every run starts with empty settings, so the first page asks for the admin password.
`uv run tools/qemu/smoke.py` boots the image headless and checks the web server, the
status API and UART1 in both directions; CI runs it as well. The QEMU build uses
the overlay `sdkconfig.qemu` and is never a station image: configure refuses it for a
release version and the release job refuses to publish it. What differs from a station,
and why UART1 drops bytes while nothing is connected, is in
[`tools/qemu/README.md`](../tools/qemu/README.md), with each place where this QEMU
behaves unlike the chip and the measurements behind it (a restart run on the APP CPU
boots twice there, because the emulated chip never stalls a CPU, so the firmware runs
every planned restart on the PRO CPU). The checks never retry a request or wait longer
for one: a slow request is watched (`slow-requests.log`) and every Ethernet frame of a
run is kept (`qemu-net.pcap`), so a failure carries its own evidence.
