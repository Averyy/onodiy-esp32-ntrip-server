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
    TEST_ASSERT_EQUAL_STRING_LEN("PARSING FAILD NO MATCHING FUNC  RTCM1230", l.text, l.text_len);
    TEST_ASSERT_EQUAL_size_t(strlen("PARSING FAILD NO MATCHING FUNC  RTCM1230"), l.text_len);

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
    static const char mmp[] = "CONFIG MMP ENABLE";
    um98x_config_item(&rb, "MMP", 3, mmp, strlen(mmp));
    TEST_ASSERT_EQUAL_UINT16(5, rb.items);
}

TEST_CASE("reply: only VERSIONA and MODE are allowed without a lease", "[um98x_reply]")
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
    /* Streaming, no lease: the two read-only queries only. */
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
