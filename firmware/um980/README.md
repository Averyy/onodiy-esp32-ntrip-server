# UM980 GNSS firmware

Unicore UM980 receiver firmware packages, kept for UM980 support later. **The station
does not use them yet:** it supports the UM982 only and refuses a package for any
other model. Releases do not include them (the release attaches `firmware/um982/`
only), and none has been flashed on hardware.

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
among them. All four pass the station's package rules 1-7 (structure,
header checksums, addresses, payload sums, trailers, every compressed block
decoded, a known partition table) and stop at rule 8, the model. What marks them as
UM980 packages: the model string `UM980`, product tag `0x41` (UM982: `0x40`) and a
zero at file word `0xAC` (UM982: 660000000).

**Partition tables.** Builds 10110 and 11833 carry the table assumed for UM982
build 13495 (the type-4 backup at the primary address); 23980 and 25735 carry the
table of UM982 builds 18975 and 23575, and the same bootloader block as UM982
build 23575.

**Before a station can use them:** the package checks would have to accept each
model's own marks and tables, the release would have to attach this folder, and a
UM980 board has to be tested first.
