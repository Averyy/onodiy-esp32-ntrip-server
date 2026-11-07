# LG290P GNSS firmware

Quectel LG290P (03) receiver firmware packages, kept for a possible LG290P board (see
[`docs/todo-lg290p.md`](../../docs/todo-lg290p.md)). The station does not support the
LG290P yet: nothing here is flashed by it or published with a release, and neither
package has been flashed on hardware here.

| Package | Release notes | SHA-256 |
|---|---|---|
| `LG290P03AANR01A06S.pkg` | adds `$PQTMENV` | `411b8e5bd6ccd7bf2100e86ecbd2d2980d531ae86a873efaf8a4cf46e79072de` |
| `LG290P03AANR02A02S.pkg` | RTCM 1230 in the standard format; notes dated 2026-05-18 | `cf9e02b4166bb6b592da61ed5d7d02f81c2eb2abba230fe084284eb3ca200f3d` |

Verify with `shasum -a 256 -c SHA256SUMS`. Each file carries its version string
(`LG290P03AANR01A06S`, `LG290P03AANR02A02S`) in readable form.

**Source.** Both are byte-for-byte the packages in SparkFun's
[RTK Postcard repository](https://github.com/sparkfun/SparkFun_RTK_Postcard/tree/main/Firmware)
(`LG290P03AANR01A06S.zip`, `LG290P03AANR02A02S.zip`), named as there. Those zips
also hold Quectel's release notes (R02A02S: English and Chinese), which list every
version from R01A03S.

**Installing.** The receiver's boot loader takes the `.pkg` as it is at 460800 baud,
by the protocol in Quectel's
[LG290P(03) Firmware Upgrade Guide](https://docs.sparkfun.com/SparkFun_RTK_Postcard/assets/component_documentation/Quectel_LG290P%2803%29_Firmware_Upgrade_Guide_V1.0.pdf).
