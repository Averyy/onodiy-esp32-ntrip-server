# Hardware details

The UM982 + ESP32-S3 board: its two USB-C ports, the console, cables and power, and the board as delivered.
Back to the [README](../README.md).

## The two USB-C ports

| Port | Printed on the board | What the computer sees |
|---|---|---|
| Top edge | nothing | a CH340N USB-serial bridge (`1a86:7523`) on the receiver's COM2, 115200 baud |
| Bottom edge | `ESP32S3` | **depends on which way round the plug goes in:** the ESP32-S3's built-in USB (USB Serial/JTAG, `303a:1001`), or, with the plug turned over, a WCH CH343P bridge (`1a86:55d3`) on the ESP32's UART0 |

The `ESP32S3` port carries both (the seller calls it "ESP32S3 USB&&UART"). On the
CH343P side the bridge drives the usual auto-reset transistor pair (EN and IO0), as on
most ESP32 dev boards. On macOS both sides show up as `/dev/cu.usbmodem*`;
`tools/bench/ports.py` tells them apart (see [`tools/bench/README.md`](../tools/bench/README.md)).
If you get the wrong one, unplug and turn the plug over.

Our firmware's console is on UART0, so plug the `ESP32S3` port in the CH343P way
round to see its log. Either side should flash the ESP32: esptool works over the
built-in USB, and the CH343P side has the auto-reset pair. The station build (and
the `sdkconfig.hold` one) turns the built-in USB off while it runs
(`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n`) and lets Wi-Fi power the USB PHY down
(`CONFIG_ESP_PHY_ENABLE_USB=n`): the ESP-IDF documentation says a powered USB PHY can
lower Wi-Fi performance. That side then shows only the chip's ROM output, and to flash
through it you hold **BOT** and press **RST** first.

Bench builds (`sdkconfig.bench`) copy the console to the built-in USB as well
(`CONFIG_DIAG_CONSOLE_USB_MIRROR`, off by default), so either way round shows the
log; the bench console reads its input on the CH343P side only. The built-in USB then
keeps running, so esptool can reset the chip into download mode through it, as it
does through the CH343P side's auto-reset pair (to be confirmed on the board), and the
USB PHY stays powered while Wi-Fi runs, with or without a host on that side. How much
Wi-Fi that costs on this board has not been measured; compare signal strength and
throughput with and without the copy before turning it on for a station. The copy
never holds the console up: with no host on the built-in USB nothing is sent there,
and lines that a host that does not read has no room for are dropped and counted
(`GET /api/health`, `log.usb_dropped`). It starts with the app's first step: the
startup lines before that, and panic output, are not copied.

Turning the copy off in a build directory that had it on leaves the built-in USB and
the USB PHY on there: its `sdkconfig` keeps them at `y`. Set all three to n in it (or
start a new build directory); the configure step warns when the USB PHY is on outside
a bench build, or without the copy:

```
CONFIG_DIAG_CONSOLE_USB_MIRROR=n
CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n
CONFIG_ESP_PHY_ENABLE_USB=n
```

## Cables and power

A USB-C-to-C cable gives neither power nor data on either port:
the board leaves out the USB-C CC resistors. Plug each port into a USB-A port instead,
through a USB-C-to-A adapter or a hub, with an A-to-C data cable. Power an installed
station from a plain 5 V USB-A charger with an A-to-C cable, not a USB-C or PD
charger.

## Console notes

Opening or closing the console port with `idf.py monitor
--no-reset`, `tools/bench/capture.py --console` or another pyserial terminal does not
reset the ESP32 (measured on the built-in USB side; the CH343P side matched another
ESP32-S3 board with the same bridge). An ESP32 reset neither resets nor reconfigures
the receiver, and opening the receiver's USB-C does not reset it either.

## As delivered (one board, October 2026)

- ESP32: a small Arduino SD-card test sketch from 2022 that prints `error opening
  test.txt` on UART0 every 400 ms; no Wi-Fi or Bluetooth, and it leaves the receiver
  alone. 4 MB Arduino partition table. Secure Boot and flash encryption are off and
  download mode is on, so the board takes an unsigned image as it is. To keep a copy
  before the first flash: `esptool --chip esp32s3 -p <port> read-flash 0 0x1000000
  stock-esp32.bin`.
- Receiver: UM982 firmware R4.10 Build 25164 (compiled 2026-05-21), newer than the
  package in [`firmware/um982/`](../firmware/um982/README.md). Signal group `4 5`, rover
  mode, COM1/COM2/COM3 at 115200, and COM1 sending a position line and a status report
  at 1 Hz. This build accepts the signal groups `4 5`, `3 6`, `5 0` and `7 0`; `3 6`
  (Galileo E6) and `7 0` (BeiDou B2a) each give 4 bands on one constellation, which
  Onocoy rewards most. With an antenna, this build tracks Galileo E6 in `3 6`.
