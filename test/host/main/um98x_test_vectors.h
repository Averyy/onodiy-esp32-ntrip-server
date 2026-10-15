/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver reply vectors for the host tests.
 *
 * SYNTHETIC: the version and base-mode logs below were built by hand from
 * the documented field layout (header; quoted model, firmware,
 * authorisation, PN-SN, efuse/board ID, compile date), with made-up serial
 * numbers, and their checksums computed with an independent implementation
 * (the reflected CRC-32 0xEDB88320 with initial value 0 and no final XOR
 * equals zlib's crc32 of the same bytes with initial value 0xFFFFFFFF and a
 * final XOR of 0xFFFFFFFF). They are not captures from a receiver; replace
 * or extend them with captured lines once the bench runs.
 *
 * The acknowledgement, dump item and banner lines are the public reply
 * samples (their '$'-inclusive XOR checksums verified by hand). The refusal
 * lines and the restarting answer are UM982 build 25164's own (our bench,
 * 2026-10-14; test/fixtures/unicore/um982_build25164_replies.json); the
 * older "FAILD" spelling of other builds is kept beside them. The
 * signal-group logs are SYNTHETIC in their header (not captured yet) around
 * the bodies the bench recorded.
 */
#pragma once

/* Synthetic UM982 version log (CRC32 0c9293ad). */
#define TV_VERSIONA_UM982                                                                                         \
    "#VERSIONA,79,GPS,FINE,2350,349712000,0,0,18,975;\"UM982\",\"R4.10Build18975\",\"HRPT00-S10C-P\","         \
    "\"1234567890123-SYNTH00000001\",\"0123456789abcdef\",\"2025/04/18\"*0c9293ad"

/* Synthetic UM980 version log with a different header layout (CRC32 a2234a25). */
#define TV_VERSIONA_UM980                                                                                         \
    "#VERSIONA,COM1,0,0.0,UNKNOWN,0,0.000,0,0,0;\"UM980\",\"R4.10Build7923\",\"HRPT00-S10C-P\","                \
    "\"2310415000001-SYNTH00000002\",\"00000000000000ff\",\"2022/09/28\"*a2234a25"

/* Synthetic base-mode log (2-hex XOR including '#'). */
#define TV_MODE_SURVEY "#MODEA,COM1,0,0.0,FINE,0,0.000,0,0,0;MODE BASE 1234 TIME 600 0*13"

/* Public reply samples. */
#define TV_ACK_UNLOG    "$command,unlog,response: OK*21"
#define TV_ACK_VERSIONA "$command,VERSIONA,response: OK*45"
#define TV_CONFIG_SG    "$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 3 6*01"
#define TV_NACK_1230    "$command,rtcm1230 10,response: PARSING FAILED NO MATCHING FUNC  RTCM1230*24"
#define TV_NACK_FAILD   "$command,rtcm1230 10,response: PARSING FAILD NO MATCHING FUNC  RTCM1230*61"
#define TV_BANNER       "$devicename,COM1*67"

/* UM982 build 25164 (our bench, 2026-10-14). */
#define TV_NACK_FOOBAR  "$command,FOOBAR,response: PARSING FAILED NO MATCHING FUNC  FOOBAR*05"
#define TV_NACK_SG      "$command,CONFIG SIGNALGROUP 1,response: PARSING FAILED GRAMMAR ERROR,*60"
#define TV_NOTICE       "The Board is resetting,do not response any command!*39"
/* The reply to a signal group this build does not offer (after ",response: "). */
#define TV_SG_REFUSAL   "PARSING FAILED GRAMMAR ERROR,"

/* Signal-group logs: bench bodies in a made-up header (XOR and CRC32 forms). */
#define TV_GROUPS_36    "#ANT1GROUPA,COM1,0,0.0,FINE,0,0.000,0,0,0;3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35*1F"
#define TV_GROUPS_SLAVE "#ANT2GROUPA,COM1,0,0.0,FINE,0,0.000,0,0,0;6,4,GP,0x0D,GA,0x05,GB,0x05,GQ,0x05*5A"
#define TV_GROUPS_70    "#ANT1GROUPA,COM1,0,0.0,FINE,0,0.000,0,0,0;7,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15*2e149c7f"
