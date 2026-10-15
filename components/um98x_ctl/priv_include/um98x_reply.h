/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver reply lines (pure logic, no IDF includes, host-testable).
 *
 * Classifies one framer-validated text line for the command layer and
 * re-checks its checksum under the rule its kind uses (the framer accepts a
 * '$' line under either XOR rule, so an acknowledgement that only passes the
 * NMEA rule is rejected here):
 *
 *   $command,<echo>,response: <text>*hh   acknowledgement; XOR from '$'
 *                                         (inclusive) to '*'. <text> starting
 *                                         with "OK" = accepted, anything else
 *                                         = rejected, the text being the
 *                                         reason: "PARSING FAILED NO MATCHING
 *                                         FUNC  <word>" for an unknown
 *                                         command, "PARSING FAILED GRAMMAR
 *                                         ERROR," for a signal group the
 *                                         firmware does not offer (UM982
 *                                         build 25164; older builds spell it
 *                                         "FAILD"; the spelling is never
 *                                         matched). The echo is everything up
 *                                         to the last ",response: ".
 *   $CONFIG,<item>,<text>*hh              one configuration dump item, same XOR
 *   $devicename,<port>*hh                 restart banner, same XOR
 *   The Board is resetting,<text>*hh      the receiver's answer to any command
 *                                         while it restarts: no lead
 *                                         character, the XOR of the text
 *                                         before '*' (gnss_framing emits it as
 *                                         a text line)
 *   #VERSIONA,...;<body>*crc32            version log (gnss_versiona_parse)
 *   #MODE...,...;<body>*hh                base-mode log; 2-hex XOR including
 *                                         '#', or an 8-hex CRC32 (either is
 *                                         accepted)
 *   #ANT1GROUPA,...;<body>*hh|crc32       the signals the running signal group
 *   #ANT2GROUPA,...;<body>*hh|crc32       configures on the main (1) or the
 *                                         second (2) antenna input, either
 *                                         checksum: <group>,<n>,<sys>,<mask>...
 *                                         (um98x_bands.h). The body is the
 *                                         text after the first ';', or after
 *                                         the log name when there is none
 *                                         (build 25164's form, read in a real
 *                                         apply on 2026-10-14)
 *
 * Anything else (NMEA sentences, other logs) is OTHER: not for this layer.
 *
 * Echo comparison is case-insensitive with surrounding spaces trimmed (the
 * receiver echoes the command as it was sent).
 *
 * Status: reply formats PROVISIONAL, to be confirmed on hardware (the
 * base-mode log header and the dump item texts in particular). The
 * acknowledgement and refusal lines, the banner, the restarting answer and
 * the signal-group log are as UM982 build 25164 sends them. Public
 * samples from a sibling receiver (test/fixtures/unicore) follow these
 * rules: the acknowledgement, error, dump and banner lines and a "#MODE,"
 * log with its 2-hex XOR including the lead character; the base-mode body
 * comes back as "MODE BASE TIME 60," (no station ID, trailing comma).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_framing.h"
#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest command text (without CR/LF) the layer sends or accepts, plus the
 *  terminator: the antenna descriptor command with two 31-character
 *  strings is about 100 characters. */
#define UM98X_CMD_MAX       128u
/** Longest failure reason kept (receiver text after "response: "). */
#define UM98X_REASON_MAX    64u
/** Longest base-mode log body kept. */
#define UM98X_MODE_BODY_MAX 128u

typedef enum {
    UM98X_LINE_OTHER = 0,   /**< not for the command layer (NMEA, other logs) */
    UM98X_LINE_ACK,         /**< command acknowledgement */
    UM98X_LINE_CONFIG,      /**< one configuration dump item */
    UM98X_LINE_BANNER,      /**< restart banner */
    UM98X_LINE_VERSION,     /**< version log, CRC-valid and parsed */
    UM98X_LINE_MODE,        /**< base-mode log */
    UM98X_LINE_BAD,         /**< has a command-layer prefix but a wrong checksum or form */
    UM98X_LINE_RESETTING,   /**< the receiver restarts: it answered a command with its restarting notice */
    UM98X_LINE_GROUPS,      /**< signal-group log: the signals the running group configures (ant: 1 or 2) */
} um98x_line_kind_t;

/** A classified line. Pointers point into the line and live only as long as it. */
typedef struct {
    um98x_line_kind_t kind;
    const char *echo;       /**< ACK: echoed command */
    size_t echo_len;
    const char *text;       /**< ACK: response text; CONFIG: item text; MODE, GROUPS: body; BANNER: port */
    size_t text_len;
    bool ok;                /**< ACK: the response starts with "OK" */
    const char *item;       /**< CONFIG: item name */
    size_t item_len;
    gnss_versiona_t version;/**< VERSION: parsed body */
    uint8_t ant;            /**< GROUPS: 1 the main antenna input, 2 the second */
} um98x_line_t;

/** What the receiver's configuration dump said about the items the apply needs. */
typedef struct {
    uint16_t items;                 /**< dump lines seen */
    bool signal_group_known;
    uint8_t signal_group_master;
    uint8_t signal_group_slave;
    bool com1_baud_known;
    uint32_t com1_baud;
    um98x_rx_settings_t rx;         /**< B1C/B2a, multipath mitigation, elevation mask, antenna descriptor */
} um98x_config_readback_t;

/** XOR of n bytes (the receiver's 2-hex checksums). */
uint8_t um98x_xor8(const char *s, size_t n);

/** Classify one line (without CR/LF). Never fails: unknown lines are OTHER. */
void um98x_line_classify(const char *line, size_t len, um98x_line_t *out);

/** True if an acknowledgement's echo is this command (case-insensitive, trimmed). */
bool um98x_echo_matches(const char *echo, size_t echo_len, const char *cmd);

/**
 * Fold one dump item into the read-back. Item names compare
 * case-insensitively; the values follow the item's own name in the item
 * text (public dump form "$CONFIG,<item>,CONFIG <item> <values>*hh"):
 *   SIGNALGROUP       "<master> [<slave>]"; a missing slave value reads as 0
 *   COM1              "<baud> ..."
 *   RTCMB1CB2A, MMP   the first value: "ENABLE" = on, "DISABLE" = off,
 *                     anything else or none = unknown; a repeated item:
 *                     the last line wins
 *   MASK              the first value, if it is a number of degrees
 *                     (-90..90, up to one decimal kept): the elevation mask;
 *                     a line whose first value is not a number (a system
 *                     mask) leaves it alone
 *   BASEANTENNAMODEL  "\"<name>\" \"<serial>\" <setup id> ...": the first
 *                     quoted string is the descriptor, the second the
 *                     serial number, the next integer (0..255) the setup
 *                     ID; anything that does not fit (no quotes, a string
 *                     over 31 characters, no ID) = unknown; the last line
 *                     wins
 * Other items only count. PROVISIONAL, to be confirmed on hardware: the
 * value forms of RTCMB1CB2A, MMP, MASK and BASEANTENNAMODEL.
 */
void um98x_config_item(um98x_config_readback_t *rb, const char *item, size_t item_len, const char *text,
                       size_t text_len);

/**
 * True if cmd may be sent without a COMMAND lease while streaming: exactly
 * "VERSIONA" or "MODE" (case-insensitive, surrounding CR/LF/spaces ignored).
 * Everything else, the configuration dump "CONFIG" included, needs a lease.
 */
bool um98x_stream_query_allowed(const char *cmd);

/** The GNSS link's ownership mode, as this layer sees it. */
typedef enum {
    UM98X_LINK_OFF = 0,
    UM98X_LINK_STREAM,
    UM98X_LINK_COMMAND,
    UM98X_LINK_RAW,
} um98x_link_mode_t;

/**
 * Allow-list per link mode. STREAM without a lease: the read-only queries
 * only (um98x_stream_query_allowed). COMMAND with the lease: anything,
 * including whole sessions (cmd == NULL: apply, detection, restore). A lease
 * in STREAM, no lease while someone holds COMMAND, RAW (the flasher owns the
 * bytes) and OFF: nothing.
 */
bool um98x_cmd_allowed(um98x_link_mode_t mode, bool have_lease, const char *cmd);

/**
 * Normalise a caller's command: drop surrounding CR/LF/spaces, reject empty,
 * over-long (>= UM98X_CMD_MAX) or text with control characters inside.
 * @return false if the command cannot be sent.
 */
bool um98x_cmd_normalise(const char *cmd, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
