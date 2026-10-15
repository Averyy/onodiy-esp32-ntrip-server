# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""What UM982 build 25164 wrote on the bench (2026-10-14, no antenna), for the tests.

The lines are byte for byte as received. Of the ANT1GROUPA / ANT2GROUPA
answers only the bodies were kept; group_log() gives them the header layout
of the receiver's other ASCII logs, which is an assumption."""

from __future__ import annotations

from benchlib.stream import crc32_unicore

LINES = {
    "refused": "$command,FOOBAR,response: PARSING FAILED NO MATCHING FUNC  FOOBAR*05",
    "group 1": "$command,CONFIG SIGNALGROUP 1,response: PARSING FAILED GRAMMAR ERROR,*60",
    "group 99 99": "$command,CONFIG SIGNALGROUP 99 99,response: PARSING FAILED GRAMMAR ERROR,*71",
    "group 3 6": "$command,CONFIG SIGNALGROUP 3 6,response: OK*30",
    "group 7 0": "$command,CONFIG SIGNALGROUP 7 0,response: OK*32",
    "group 5 0": "$command,CONFIG SIGNALGROUP 5 0,response: OK*30",
    "group 4 5": "$command,CONFIG SIGNALGROUP 4 5,response: OK*34",
    "group 5": "$command,CONFIG SIGNALGROUP 5,response: OK*20",
    "banner": "$devicename,COM2*64",
    "restarting": "The Board is resetting,do not response any command!*39",
    "no satellites": "$GPGSV,1,1,00,,,,,*55",
}

# Signal group -> (ANT1GROUPA body, ANT2GROUPA body).
GROUP_BODIES = {
    (4, 5): ("4,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x07,GQ,0x15", "5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05"),
    (3, 6): ("3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35", "6,4,GP,0x0D,GA,0x05,GB,0x05,GQ,0x05"),
    (7, 0): ("7,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15", "0,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15"),
    (5, 0): ("5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05", "0,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15"),
}


def group_log(name: str, body: str) -> str:
    """`#<name>,<header>;<body>*<crc32>` with a version-log style header (assumed)."""
    text = f"{name},97,GPS,UNKNOWN,1,1000,25164,0,18,975;{body}"
    return f"#{text}*{crc32_unicore(text.encode('latin-1')):08x}"
