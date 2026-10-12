# Third-party notices

The source code in this repository is original and MIT-licensed (see
[`LICENSE`](LICENSE)). The firmware image built from it (`onocoydiy-esp-um982.bin`,
and the bootloader and partition table that come with it) also contains code
from the projects below, linked in from ESP-IDF v6.1 and the ESP Component
Registry. Their licenses apply to those parts of the image. This file ships with
every build artifact and release.

| Component | Used for | License | Copyright |
|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | SDK: drivers, Wi-Fi, HTTP client/server, OTA, NVS, bootloader | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. |
| [FreeRTOS kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) (ESP-IDF port) | RTOS | MIT | Amazon.com, Inc. or its affiliates |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | TCP/IP stack | BSD-3-Clause | Swedish Institute of Computer Science |
| [Mbed TLS / TF-PSA-Crypto](https://github.com/Mbed-TLS/mbedtls) | TLS, PSA Crypto | Apache-2.0 | ARM Limited and the Mbed TLS contributors |
| [wpa_supplicant](https://w1.fi/wpa_supplicant/) (ESP-IDF port) | Wi-Fi security | BSD-3-Clause | Jouni Malinen and contributors |
| FreeBSD net80211 (in the ESP-IDF Wi-Fi libraries) | Wi-Fi | BSD | Sam Leffler, Errno Consulting |
| [Picolibc](https://keithp.com/picolibc/) (ESP-IDF `esp_libc`) | C library | BSD (various) | The Picolibc and Newlib authors |
| Xtensa HAL headers | CPU support | MIT | Tensilica Inc. |
| [TLSF allocator](https://github.com/espressif/tlsf) | Heap | BSD-3-Clause | Matthew Conte |
| [HTTP parser](https://github.com/nodejs/http-parser) (ESP-IDF `http_parser`) | HTTP | MIT | Joyent, Inc. and other Node contributors; Igor Sysoev |
| [cJSON](https://github.com/DaveGamble/cJSON) (`espressif/cjson`) | JSON | MIT | Dave Gamble and cJSON contributors |
| [mDNS](https://github.com/espressif/esp-protocols/tree/master/components/mdns) (`espressif/mdns` 1.13.1) | local name `<host>.local` and service advertising | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. |

The authoritative list for ESP-IDF, including components this project does not
link, is ESP-IDF's
[Copyrights and Licenses](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/COPYRIGHT.html)
page; each component's license file in the ESP-IDF source tree takes precedence
over the summary above. When a dependency is added, add it here in the same
change.

The test data under `test/fixtures/` (receiver captures and example streams
from other projects and from the Centipede-RTK network) is not part of any
build artifact; its sources, licenses and attributions are listed in
[`test/fixtures/README.md`](test/fixtures/README.md) and
[`test/fixtures/LICENSES/`](test/fixtures/LICENSES).

## cJSON

```
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## FreeRTOS kernel

Original parts Copyright (C) 2017 Amazon.com, Inc. or its affiliates.

```
MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## lwIP

```
Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
```

## Apache-2.0 components (ESP-IDF, Mbed TLS, mDNS)

These are distributed under the Apache License, Version 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>). They are used unmodified, as
shipped in ESP-IDF v6.1 and, for mDNS, as published in the ESP Component
Registry (`espressif/mdns` 1.13.1, pinned in `dependencies.lock`). ESP-IDF's and
Mbed TLS's own license and NOTICE files are in the ESP-IDF source tree
(`LICENSE`, `components/mbedtls/mbedtls/LICENSE`); the mDNS component's
`LICENSE` is the same Apache-2.0 text as ESP-IDF's.
