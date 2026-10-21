# UM980 GNSS firmware

Unicore UM980 receiver firmware packages. The station checks them by the same rules as
the UM982's, with the UM980's own marks, and takes one only for a UM980 receiver. None
has been flashed on hardware yet, and receiver flashing stays off in station builds
until it has (as for the UM982). Releases carry 11833 and 23980
([`firmware/release-packages.txt`](../release-packages.txt)). 11833 is the build the
NTRIP-X ships with: an update never goes down a build, so the first test installs the same
build again (uploaded by hand) and keeps 23980 for later. The update banner offers the
highest build above the receiver's, so a receiver on 11833 is offered 23980. That is a
cross-layout update (another partition table: the type-4 backup address), which the
station refuses until such updates have been tested on a board
(`CONFIG_UM98X_FLASH_CROSS_LAYOUT`) and the user confirms it.

| Build | Built (the package's block date stamps) | SHA-256 |
|---|---|---|
| R4.10 Build 10110 | 2023-06-13 | `6dee458e454c72f6ac4a3347bf39e8ba90172872490e82e0732b391a25c2e410` |
| R4.10 Build 11833 | 2023-11-24 | `dbfa083c9889349f3aac157b14e6e4f0dc78a9c934e5ac7037ab1cc662df2988` |
| R4.10 Build 23980 | 2026-05-09 | `afa20d837047ef010f1ac36d4c84e3f45363ad7fa324e35cd51c3331feb9adc2` |
| R4.10 Build 25735 | 2026-07-07 | `bddfd8ceb17b9270074b26911135faef9c99f4dceff9694863efbbaa9c0e9493` |

Verify with `shasum -a 256 -c SHA256SUMS`. "R4.10" in the names follows the UM982
packages; the files do not spell their version in readable form, and the station reads
only the build number.

**Format.** The same block format as the UM982 packages
([`firmware/um982/README.md`](../um982/README.md)): five blocks, a bootloader block
among them. All four pass every package rule on a UM980 (host-tested: test/host/main/test_pkg_check.c). What marks them as
UM980 packages: the model string `UM980`, product tag `0x41` (UM982: `0x40`) and a
zero at file word `0xAC` (UM982: 660000000).

**Partition tables.** Builds 10110 and 11833 carry the table assumed for UM982
build 13495 (the type-4 backup at the primary address); 23980 and 25735 carry the
table of UM982 builds 18975 and 23575, and the same bootloader block as UM982
build 23575.

**Still to do:** flash one on hardware (the NTRIP-X, then the UM980 board) before
receiver flashing is turned on for station builds.
