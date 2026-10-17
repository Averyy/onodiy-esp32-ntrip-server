/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Reply line classification, checksums, echo matching, dump items and the
 * read-only allow-list.
 */
#include <stdio.h>
#include <string.h>

#include "um98x_reply.h"
#include "um98x_test_vectors.h"
#include "unity.h"

static void classify(const char *line, um98x_line_t *l)
{
    um98x_line_classify(line, strlen(line), l);
}

/* "$<content>*hh" with the '$'-inclusive XOR. */
static void make_dollar(char *out, size_t cap, const char *content)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "$%s", content);
    snprintf(out, cap, "%s*%02X", tmp, um98x_xor8(tmp, strlen(tmp)));
}

TEST_CASE("reply: public acknowledgement samples classify with echo and response", "[um98x_reply]")
{
    um98x_line_t l;
    classify(TV_ACK_UNLOG, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_TRUE(l.ok);
    TEST_ASSERT_EQUAL_size_t(5, l.echo_len);
    TEST_ASSERT_EQUAL_MEMORY("unlog", l.echo, 5);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "UNLOG"));

    classify(TV_ACK_VERSIONA, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "VERSIONA"));

    classify(TV_NACK_1230, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_FALSE(l.ok);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "RTCM1230 10"));
    TEST_ASSERT_EQUAL_STRING_LEN("PARSING FAILED NO MATCHING FUNC  RTCM1230", l.text, l.text_len);
    TEST_ASSERT_EQUAL_size_t(strlen("PARSING FAILED NO MATCHING FUNC  RTCM1230"), l.text_len);

    /* The older spelling is a refusal just the same: the text is never matched. */
    classify(TV_NACK_FAILD, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_FALSE(l.ok);
    TEST_ASSERT_EQUAL_STRING_LEN("PARSING FAILD NO MATCHING FUNC  RTCM1230", l.text, l.text_len);

    classify(TV_CONFIG_SG, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_CONFIG, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("SIGNALGROUP", l.item, l.item_len);
    TEST_ASSERT_EQUAL_STRING_LEN("CONFIG SIGNALGROUP 3 6", l.text, l.text_len);

    classify(TV_BANNER, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BANNER, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("COM1", l.text, l.text_len);
}

TEST_CASE("reply: the '$'-inclusive checksum is required for command-layer lines", "[um98x_reply]")
{
    um98x_line_t l;
    /* Same text with the NMEA rule (XOR without '$', 0x05): not an ack. */
    classify("$command,unlog,response: OK*05", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);
    classify("$command,unlog,response: OK", &l);   /* no checksum at all */
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);
    classify("$devicename,COM1*43", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);

    /* Lower-case hex digits are accepted too. */
    char buf[128];
    make_dollar(buf, sizeof(buf), "command,MODE,response: OK");
    for (char *p = buf; *p; p++) {
        if (*p >= 'A' && *p <= 'F' && p > strchr(buf, '*')) {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    classify(buf, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);

    /* An ack without ",response: " is malformed. */
    make_dollar(buf, sizeof(buf), "command,unlog OK");
    classify(buf, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);

    /* NMEA and other lines are not for the command layer. */
    classify("$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*48", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);
    classify("#BESTNAVA,COM1,0;SOL_COMPUTED*00000000", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);
    classify("", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);
    um98x_line_classify(NULL, 5, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);
}

TEST_CASE("reply: the echo is everything up to the last \",response: \"", "[um98x_reply]")
{
    char buf[160];
    um98x_line_t l;
    make_dollar(buf, sizeof(buf), "command,RTCM1077 COM1 1,response: OK");
    classify(buf, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "RTCM1077 COM1 1"));
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "  rtcm1077 com1 1\r\n"));
    TEST_ASSERT_FALSE(um98x_echo_matches(l.echo, l.echo_len, "RTCM1077 COM1 10"));
    TEST_ASSERT_FALSE(um98x_echo_matches(l.echo, l.echo_len, "RTCM1087 COM1 1"));
    TEST_ASSERT_FALSE(um98x_echo_matches(l.echo, l.echo_len, ""));

    /* A response that does not start with OK is a failure. */
    make_dollar(buf, sizeof(buf), "command,MODE BASE 1 TIME 60 0,response: ERROR");
    classify(buf, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_FALSE(l.ok);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "MODE BASE 1 TIME 60 0"));
}

TEST_CASE("reply: version and base-mode logs", "[um98x_reply]")
{
    um98x_line_t l;
    classify(TV_VERSIONA_UM982, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_VERSION, l.kind);
    TEST_ASSERT_EQUAL_STRING("UM982", l.version.model);

    char bad[512];
    strcpy(bad, TV_VERSIONA_UM982);
    bad[20] ^= 0x01;
    classify(bad, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);

    classify(TV_MODE_SURVEY, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_MODE, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("MODE BASE 1234 TIME 600 0", l.text, l.text_len);

    strcpy(bad, TV_MODE_SURVEY);
    bad[strlen(bad) - 1] = '4';
    classify(bad, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);

    /* The CRC32 form is accepted for the base-mode log as well. */
    static const char content[] = "MODE,COM1;MODE ROVER";
    uint32_t crc = 0;
    for (size_t i = 0; i < sizeof(content) - 1; i++) {
        crc ^= (uint8_t)content[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "#%s*%08x", content, (unsigned)crc);
    classify(buf, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_MODE, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("MODE ROVER", l.text, l.text_len);

    /* A base-mode log without a body reads as empty. */
    char nobody[64] = "#MODE,COM1,0";
    size_t n = strlen(nobody);
    snprintf(nobody + n, sizeof(nobody) - n, "*%02X", um98x_xor8(nobody, n));
    classify(nobody, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_MODE, l.kind);
    TEST_ASSERT_EQUAL_size_t(0, l.text_len);

    /* Other logs starting with MODE are not the base-mode log. */
    char other[64] = "#MODEX,COM1;x";
    n = strlen(other);
    snprintf(other + n, sizeof(other) - n, "*%02X", um98x_xor8(other, n));
    classify(other, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);
}

TEST_CASE("reply: dump items fold into the read-back", "[um98x_reply]")
{
    um98x_config_readback_t rb;
    memset(&rb, 0, sizeof(rb));
    um98x_line_t l;
    classify(TV_CONFIG_SG, &l);
    um98x_config_item(&rb, l.item, l.item_len, l.text, l.text_len);
    TEST_ASSERT_TRUE(rb.signal_group_known);
    TEST_ASSERT_EQUAL_UINT8(3, rb.signal_group_master);
    TEST_ASSERT_EQUAL_UINT8(6, rb.signal_group_slave);

    static const char com1[] = "CONFIG COM1 460800";
    um98x_config_item(&rb, "COM1", 4, com1, strlen(com1));
    TEST_ASSERT_TRUE(rb.com1_baud_known);
    TEST_ASSERT_EQUAL_UINT32(460800, rb.com1_baud);

    /* Case-insensitive; a single signal-group value reads as slave 0. */
    static const char sg1[] = "config signalgroup 7";
    um98x_config_item(&rb, "SignalGroup", 11, sg1, strlen(sg1));
    TEST_ASSERT_EQUAL_UINT8(7, rb.signal_group_master);
    TEST_ASSERT_EQUAL_UINT8(0, rb.signal_group_slave);

    /* Garbage values leave the read-back as it was. */
    static const char junk[] = "CONFIG SIGNALGROUP x y";
    um98x_config_item(&rb, "SIGNALGROUP", 11, junk, strlen(junk));
    TEST_ASSERT_EQUAL_UINT8(7, rb.signal_group_master);

    /* Other items only count. */
    static const char und[] = "CONFIG UNDULATION AUTO";
    um98x_config_item(&rb, "UNDULATION", 10, und, strlen(und));
    TEST_ASSERT_EQUAL_UINT16(5, rb.items);
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_UNKNOWN, rb.rx.b1cb2a);
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_UNKNOWN, rb.rx.mmp);
    TEST_ASSERT_FALSE(rb.rx.mask_known);
    TEST_ASSERT_FALSE(rb.rx.antenna_known);
}

static void fold(um98x_config_readback_t *rb, const char *line)
{
    um98x_line_t l;
    classify(line, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_CONFIG, l.kind);
    um98x_config_item(rb, l.item, l.item_len, l.text, l.text_len);
}

static void fold_item(um98x_config_readback_t *rb, const char *content)
{
    char line[256];
    make_dollar(line, sizeof(line), content);
    fold(rb, line);
}

TEST_CASE("reply: BeiDou B1C/B2a and multipath mitigation read back as on, off or unknown", "[um98x_reply]")
{
    um98x_config_readback_t rb;
    memset(&rb, 0, sizeof(rb));
    /* The public dump line, as it is. */
    fold(&rb, "$CONFIG,RTCMB1CB2A,CONFIG RTCMB1CB2A ENABLE*25");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_ON, rb.rx.b1cb2a);
    fold_item(&rb, "CONFIG,RTCMB1CB2A,CONFIG RTCMB1CB2A DISABLE");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_OFF, rb.rx.b1cb2a);
    /* Lower case, as the receiver echoes what it was given. */
    fold_item(&rb, "CONFIG,rtcmb1cb2a,config rtcmb1cb2a enable");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_ON, rb.rx.b1cb2a);
    /* A value that is neither, or none at all: unknown (the last line wins). */
    fold_item(&rb, "CONFIG,RTCMB1CB2A,CONFIG RTCMB1CB2A MAYBE");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_UNKNOWN, rb.rx.b1cb2a);
    fold_item(&rb, "CONFIG,RTCMB1CB2A,CONFIG RTCMB1CB2A DISABLE");
    fold_item(&rb, "CONFIG,RTCMB1CB2A,CONFIG RTCMB1CB2A");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_UNKNOWN, rb.rx.b1cb2a);

    fold_item(&rb, "CONFIG,MMP,CONFIG MMP ENABLE");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_ON, rb.rx.mmp);
    fold_item(&rb, "CONFIG,MMP,CONFIG MMP DISABLE");
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_OFF, rb.rx.mmp);

    /* An item missing from the dump stays unknown. */
    memset(&rb, 0, sizeof(rb));
    fold(&rb, TV_CONFIG_SG);
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_UNKNOWN, rb.rx.b1cb2a);
}

TEST_CASE("reply: the elevation mask reads back in tenths of a degree", "[um98x_reply]")
{
    um98x_config_readback_t rb;
    memset(&rb, 0, sizeof(rb));
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK 5.0");
    TEST_ASSERT_TRUE(rb.rx.mask_known);
    TEST_ASSERT_EQUAL_INT16(50, rb.rx.mask_ddeg);
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK 0");
    TEST_ASSERT_EQUAL_INT16(0, rb.rx.mask_ddeg);
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK 12.25");
    TEST_ASSERT_EQUAL_INT16(123, rb.rx.mask_ddeg);   /* rounded to 0.1 degree */
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK -5");
    TEST_ASSERT_EQUAL_INT16(-50, rb.rx.mask_ddeg);
    /* A system mask line (no number) leaves the elevation mask alone. */
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK GPS");
    TEST_ASSERT_TRUE(rb.rx.mask_known);
    TEST_ASSERT_EQUAL_INT16(-50, rb.rx.mask_ddeg);
    /* Out of range or malformed: ignored too. */
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK 95");
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK 1.2.3");
    TEST_ASSERT_EQUAL_INT16(-50, rb.rx.mask_ddeg);

    memset(&rb, 0, sizeof(rb));
    fold_item(&rb, "CONFIG,MASK,CONFIG MASK BDS");
    TEST_ASSERT_FALSE(rb.rx.mask_known);

    /* The receiver's own dump form for MASK has no CONFIG word, six decimals,
     * and per-satellite lines ("<system>MaskPrn:<n>,") that are no elevation. */
    memset(&rb, 0, sizeof(rb));
    fold_item(&rb, "CONFIG,MASK,MASK 10.000000");
    TEST_ASSERT_TRUE(rb.rx.mask_known);
    TEST_ASSERT_EQUAL_INT16(100, rb.rx.mask_ddeg);
    fold_item(&rb, "CONFIG,MASK,GPSMaskPrn:12,");
    fold_item(&rb, "CONFIG,MASK,QZSSMaskPrn:194,");
    fold_item(&rb, "CONFIG,MASK,MASK GPS");
    TEST_ASSERT_EQUAL_INT16(100, rb.rx.mask_ddeg);
    TEST_ASSERT_EQUAL_UINT16(4, rb.items);
}

TEST_CASE("reply: the base antenna descriptor reads back, or unknown", "[um98x_reply]")
{
    um98x_config_readback_t rb;
    memset(&rb, 0, sizeof(rb));
    fold_item(&rb, "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"TRM59800.00     SCIS\" \"5000112233\" 3 USER");
    TEST_ASSERT_TRUE(rb.rx.antenna_known);
    TEST_ASSERT_EQUAL_STRING("TRM59800.00     SCIS", rb.rx.antenna_model);   /* inner spaces kept */
    TEST_ASSERT_EQUAL_STRING("5000112233", rb.rx.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(3, rb.rx.antenna_setup_id);

    /* Build 25164's read-back on the bench, after it was sent
     * "TRM59800.00     SCIS": runs of spaces collapsed. Read as it is. */
    fold_item(&rb, "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"TRM59800.00 SCIS\" \"5000123\" 0 USER");
    TEST_ASSERT_TRUE(rb.rx.antenna_known);
    TEST_ASSERT_EQUAL_STRING("TRM59800.00 SCIS", rb.rx.antenna_model);
    TEST_ASSERT_EQUAL_STRING("5000123", rb.rx.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(0, rb.rx.antenna_setup_id);

    /* The receiver's null antenna with an empty serial. */
    fold_item(&rb, "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"ADVNULLANTENNA\" \"\" 0 USER");
    TEST_ASSERT_TRUE(rb.rx.antenna_known);
    TEST_ASSERT_EQUAL_STRING("ADVNULLANTENNA", rb.rx.antenna_model);
    TEST_ASSERT_EQUAL_STRING("", rb.rx.antenna_serial);

    /* 31 characters fit. */
    fold_item(&rb, "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"1234567890123456789012345678901\" \"S\" 255 USER");
    TEST_ASSERT_TRUE(rb.rx.antenna_known);
    TEST_ASSERT_EQUAL_UINT8(255, rb.rx.antenna_setup_id);

    /* Anything that does not fit: unknown (and the last line wins). */
    static const char *const bad[] = {
        "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL TRM59800.00 SCIS 5000112233 3 USER",   /* no quotes */
        "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"TRM59800.00\" 5000112233 3",          /* serial unquoted */
        "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"TRM59800.00\" \"1\"",                 /* no setup ID */
        "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"TRM59800.00\" \"1\" 256 USER",        /* ID out of range */
        "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"12345678901234567890123456789012\" \"\" 0",   /* 32 characters */
        "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"TRM59800.00",                         /* unterminated */
        "CONFIG,BASEANTENNAMODEL,BASEANTENNA",                                                   /* no item name in the text */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fold_item(&rb, "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL \"A\" \"B\" 1 USER");
        TEST_ASSERT_TRUE(rb.rx.antenna_known);
        fold_item(&rb, bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(rb.rx.antenna_known, bad[i]);
        TEST_ASSERT_EQUAL_STRING("", rb.rx.antenna_model);
    }
}

TEST_CASE("reply: only the read-only queries are allowed without a lease", "[um98x_reply]")
{
    TEST_ASSERT_TRUE(um98x_stream_query_allowed("VERSIONA"));
    TEST_ASSERT_TRUE(um98x_stream_query_allowed("versiona"));
    TEST_ASSERT_TRUE(um98x_stream_query_allowed("\r\nMODE\r\n"));
    TEST_ASSERT_TRUE(um98x_stream_query_allowed(" mode "));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("CONFIG"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("MODE BASE 1 TIME 60 0"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("VERSION"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("UNLOG COM1"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("SAVECONFIG"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed(""));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed(NULL));
}

TEST_CASE("reply: allow-list per link mode", "[um98x_reply]")
{
    /* Streaming, no lease: the read-only queries only. */
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, "VERSIONA"));
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, "MODE"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, "CONFIG"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, "UNLOG COM1"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, NULL));   /* no session */
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_STREAM, true, "VERSIONA"));   /* stale lease */

    /* COMMAND: only the lease holder, anything. */
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_COMMAND, true, "SAVECONFIG"));
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_COMMAND, true, NULL));
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_COMMAND, true, "CONFIG"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_COMMAND, false, "VERSIONA"));   /* someone else's lease */

    /* RAW belongs to the flasher; OFF sends nothing. */
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_RAW, true, "VERSIONA"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_RAW, false, "VERSIONA"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_OFF, false, "MODE"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_OFF, true, NULL));
}

TEST_CASE("reply: commands are normalised and checked before sending", "[um98x_reply]")
{
    char out[UM98X_CMD_MAX];
    TEST_ASSERT_TRUE(um98x_cmd_normalise("  UNLOG COM1\r\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("UNLOG COM1", out);
    TEST_ASSERT_FALSE(um98x_cmd_normalise("\r\n", out, sizeof(out)));
    TEST_ASSERT_FALSE(um98x_cmd_normalise("UNLOG\r\nSAVECONFIG", out, sizeof(out)));
    TEST_ASSERT_FALSE(um98x_cmd_normalise("RTCM1005\tCOM1 1", out, sizeof(out)));
    TEST_ASSERT_FALSE(um98x_cmd_normalise(NULL, out, sizeof(out)));

    char longcmd[UM98X_CMD_MAX + 1];
    memset(longcmd, 'A', sizeof(longcmd) - 1);
    longcmd[sizeof(longcmd) - 1] = '\0';
    TEST_ASSERT_FALSE(um98x_cmd_normalise(longcmd, out, sizeof(out)));
    longcmd[UM98X_CMD_MAX - 1] = '\0';
    TEST_ASSERT_TRUE(um98x_cmd_normalise(longcmd, out, sizeof(out)));
}

TEST_CASE("reply: build 25164's refusals, the restarting answer and the signal-group logs", "[um98x_reply]")
{
    um98x_line_t l;
    classify(TV_NACK_FOOBAR, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_FALSE(l.ok);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "FOOBAR"));
    TEST_ASSERT_EQUAL_STRING_LEN("PARSING FAILED NO MATCHING FUNC  FOOBAR", l.text, l.text_len);

    /* A refused signal group: a trailing comma, no echo of the group in the text. */
    classify(TV_NACK_SG, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_FALSE(l.ok);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "CONFIG SIGNALGROUP 1"));
    TEST_ASSERT_EQUAL_STRING_LEN(TV_SG_REFUSAL, l.text, l.text_len);

    /* The restarting answer: no lead character, the XOR of the text before '*'. */
    classify(TV_NOTICE, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_RESETTING, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("The Board is resetting,do not response any command!", l.text, l.text_len);
    classify("The Board is resetting,do not response any command!*38", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);
    classify("The Board is resetting,do not response any command!", &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);
    classify("The Board", &l);   /* not the whole prefix: nothing for this layer */
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);

    /* Signal-group logs, either checksum; the body follows the first ';'. */
    classify(TV_GROUPS_36, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_GROUPS, l.kind);
    TEST_ASSERT_EQUAL_UINT8(1, l.ant);
    TEST_ASSERT_EQUAL_STRING_LEN("3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35", l.text, l.text_len);
    classify(TV_GROUPS_70, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_GROUPS, l.kind);
    TEST_ASSERT_EQUAL_UINT8(1, l.ant);
    classify(TV_GROUPS_SLAVE, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_GROUPS, l.kind);
    TEST_ASSERT_EQUAL_UINT8(2, l.ant);
    char bad[sizeof(TV_GROUPS_36)];
    memcpy(bad, TV_GROUPS_36, sizeof(bad));
    bad[sizeof(bad) - 2] = '0';   /* checksum changed */
    classify(bad, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);
    /* No header: the body follows the log name. */
    char nohdr[128];
    snprintf(nohdr, sizeof(nohdr), "#ANT1GROUPA,5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05");
    char line[160];
    snprintf(line, sizeof(line), "%s*%02X", nohdr, um98x_xor8(nohdr, strlen(nohdr)));
    classify(line, &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_GROUPS, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05", l.text, l.text_len);
}
