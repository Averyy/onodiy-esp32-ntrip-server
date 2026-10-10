/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Written from the public RTCM 10403.x frame and message definitions.
 */
#include "gnss_rtcm3.h"

#include <string.h>

#include "gnss_framing.h"

/* CRC-24Q of each single byte value (polynomial 0x1864CFB, initial value 0). */
static const uint32_t k_crc24q[256] = {
    0x000000u, 0x864CFBu, 0x8AD50Du, 0x0C99F6u, 0x93E6E1u, 0x15AA1Au, 0x1933ECu, 0x9F7F17u,
    0xA18139u, 0x27CDC2u, 0x2B5434u, 0xAD18CFu, 0x3267D8u, 0xB42B23u, 0xB8B2D5u, 0x3EFE2Eu,
    0xC54E89u, 0x430272u, 0x4F9B84u, 0xC9D77Fu, 0x56A868u, 0xD0E493u, 0xDC7D65u, 0x5A319Eu,
    0x64CFB0u, 0xE2834Bu, 0xEE1ABDu, 0x685646u, 0xF72951u, 0x7165AAu, 0x7DFC5Cu, 0xFBB0A7u,
    0x0CD1E9u, 0x8A9D12u, 0x8604E4u, 0x00481Fu, 0x9F3708u, 0x197BF3u, 0x15E205u, 0x93AEFEu,
    0xAD50D0u, 0x2B1C2Bu, 0x2785DDu, 0xA1C926u, 0x3EB631u, 0xB8FACAu, 0xB4633Cu, 0x322FC7u,
    0xC99F60u, 0x4FD39Bu, 0x434A6Du, 0xC50696u, 0x5A7981u, 0xDC357Au, 0xD0AC8Cu, 0x56E077u,
    0x681E59u, 0xEE52A2u, 0xE2CB54u, 0x6487AFu, 0xFBF8B8u, 0x7DB443u, 0x712DB5u, 0xF7614Eu,
    0x19A3D2u, 0x9FEF29u, 0x9376DFu, 0x153A24u, 0x8A4533u, 0x0C09C8u, 0x00903Eu, 0x86DCC5u,
    0xB822EBu, 0x3E6E10u, 0x32F7E6u, 0xB4BB1Du, 0x2BC40Au, 0xAD88F1u, 0xA11107u, 0x275DFCu,
    0xDCED5Bu, 0x5AA1A0u, 0x563856u, 0xD074ADu, 0x4F0BBAu, 0xC94741u, 0xC5DEB7u, 0x43924Cu,
    0x7D6C62u, 0xFB2099u, 0xF7B96Fu, 0x71F594u, 0xEE8A83u, 0x68C678u, 0x645F8Eu, 0xE21375u,
    0x15723Bu, 0x933EC0u, 0x9FA736u, 0x19EBCDu, 0x8694DAu, 0x00D821u, 0x0C41D7u, 0x8A0D2Cu,
    0xB4F302u, 0x32BFF9u, 0x3E260Fu, 0xB86AF4u, 0x2715E3u, 0xA15918u, 0xADC0EEu, 0x2B8C15u,
    0xD03CB2u, 0x567049u, 0x5AE9BFu, 0xDCA544u, 0x43DA53u, 0xC596A8u, 0xC90F5Eu, 0x4F43A5u,
    0x71BD8Bu, 0xF7F170u, 0xFB6886u, 0x7D247Du, 0xE25B6Au, 0x641791u, 0x688E67u, 0xEEC29Cu,
    0x3347A4u, 0xB50B5Fu, 0xB992A9u, 0x3FDE52u, 0xA0A145u, 0x26EDBEu, 0x2A7448u, 0xAC38B3u,
    0x92C69Du, 0x148A66u, 0x181390u, 0x9E5F6Bu, 0x01207Cu, 0x876C87u, 0x8BF571u, 0x0DB98Au,
    0xF6092Du, 0x7045D6u, 0x7CDC20u, 0xFA90DBu, 0x65EFCCu, 0xE3A337u, 0xEF3AC1u, 0x69763Au,
    0x578814u, 0xD1C4EFu, 0xDD5D19u, 0x5B11E2u, 0xC46EF5u, 0x42220Eu, 0x4EBBF8u, 0xC8F703u,
    0x3F964Du, 0xB9DAB6u, 0xB54340u, 0x330FBBu, 0xAC70ACu, 0x2A3C57u, 0x26A5A1u, 0xA0E95Au,
    0x9E1774u, 0x185B8Fu, 0x14C279u, 0x928E82u, 0x0DF195u, 0x8BBD6Eu, 0x872498u, 0x016863u,
    0xFAD8C4u, 0x7C943Fu, 0x700DC9u, 0xF64132u, 0x693E25u, 0xEF72DEu, 0xE3EB28u, 0x65A7D3u,
    0x5B59FDu, 0xDD1506u, 0xD18CF0u, 0x57C00Bu, 0xC8BF1Cu, 0x4EF3E7u, 0x426A11u, 0xC426EAu,
    0x2AE476u, 0xACA88Du, 0xA0317Bu, 0x267D80u, 0xB90297u, 0x3F4E6Cu, 0x33D79Au, 0xB59B61u,
    0x8B654Fu, 0x0D29B4u, 0x01B042u, 0x87FCB9u, 0x1883AEu, 0x9ECF55u, 0x9256A3u, 0x141A58u,
    0xEFAAFFu, 0x69E604u, 0x657FF2u, 0xE33309u, 0x7C4C1Eu, 0xFA00E5u, 0xF69913u, 0x70D5E8u,
    0x4E2BC6u, 0xC8673Du, 0xC4FECBu, 0x42B230u, 0xDDCD27u, 0x5B81DCu, 0x57182Au, 0xD154D1u,
    0x26359Fu, 0xA07964u, 0xACE092u, 0x2AAC69u, 0xB5D37Eu, 0x339F85u, 0x3F0673u, 0xB94A88u,
    0x87B4A6u, 0x01F85Du, 0x0D61ABu, 0x8B2D50u, 0x145247u, 0x921EBCu, 0x9E874Au, 0x18CBB1u,
    0xE37B16u, 0x6537EDu, 0x69AE1Bu, 0xEFE2E0u, 0x709DF7u, 0xF6D10Cu, 0xFA48FAu, 0x7C0401u,
    0x42FA2Fu, 0xC4B6D4u, 0xC82F22u, 0x4E63D9u, 0xD11CCEu, 0x575035u, 0x5BC9C3u, 0xDD8538u,
};

uint32_t gnss_rtcm3_crc24q(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = ((crc << 8) & 0xFFFFFFu) ^ k_crc24q[((crc >> 16) ^ data[i]) & 0xFFu];
    }
    return crc;
}

/* Payload of a structurally sound frame (preamble, length field equal to
 * len - 6); NULL otherwise. Reserved bits and CRC are not checked here. */
static const uint8_t *payload_of(const uint8_t *frame, size_t len, size_t *plen)
{
    if (frame == NULL || len < GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN || frame[0] != GNSS_RTCM3_PREAMBLE) {
        return NULL;
    }
    size_t n = ((size_t)(frame[1] & 0x03u) << 8) | frame[2];
    if (len != n + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN) {
        return NULL;
    }
    *plen = n;
    return frame + GNSS_RTCM3_HEADER_LEN;
}

/* Unsigned field of n <= 64 bits starting at bit `pos` (bit 0 = MSB of p[0]). */
static uint64_t bits_u(const uint8_t *p, size_t pos, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++, pos++) {
        v = (v << 1) | ((p[pos >> 3] >> (7u - (pos & 7u))) & 1u);
    }
    return v;
}

/* Two's complement field of 2 <= n <= 63 bits. */
static int64_t bits_s(const uint8_t *p, size_t pos, unsigned n)
{
    uint64_t v = bits_u(p, pos, n);
    if (v & ((uint64_t)1 << (n - 1))) {
        return (int64_t)v - ((int64_t)1 << n);
    }
    return (int64_t)v;
}

bool gnss_rtcm3_frame_ok(const uint8_t *frame, size_t len)
{
    size_t plen;
    if (payload_of(frame, len, &plen) == NULL || (frame[1] >> 2) != 0 || plen < 2) {
        return false;
    }
    return gnss_rtcm3_crc24q(frame, len) == 0;
}

uint16_t gnss_rtcm3_msg_type(const uint8_t *frame, size_t len)
{
    size_t plen;
    const uint8_t *p = payload_of(frame, len, &plen);
    if (p == NULL || plen < 2) {
        return 0;
    }
    return (uint16_t)(((unsigned)p[0] << 4) | (p[1] >> 4));
}

uint16_t gnss_rtcm3_station_id(const uint8_t *frame, size_t len)
{
    size_t plen;
    const uint8_t *p = payload_of(frame, len, &plen);
    if (p == NULL || plen < 3) {
        return 0;
    }
    return (uint16_t)(((unsigned)(p[1] & 0x0Fu) << 8) | p[2]);
}

bool gnss_rtcm3_msm_type(uint16_t msg_type, gnss_sys_t *sys, uint8_t *msm)
{
    unsigned digit = msg_type % 10u;
    if (msg_type < 1071 || msg_type > 1137 || digit < 1 || digit > 7) {
        return false;
    }
    /* 107x GPS, 108x GLONASS, 109x Galileo, 110x SBAS (not MSM here),
     * 111x QZSS, 112x BeiDou, 113x NavIC. */
    static const gnss_sys_t k_sys[] = {
        GNSS_SYS_GPS, GNSS_SYS_GLONASS, GNSS_SYS_GALILEO, GNSS_SYS_SBAS,
        GNSS_SYS_QZSS, GNSS_SYS_BEIDOU, GNSS_SYS_NAVIC,
    };
    gnss_sys_t s = k_sys[(msg_type - 1070u) / 10u];
    if (s == GNSS_SYS_SBAS) {
        return false;
    }
    if (sys != NULL) {
        *sys = s;
    }
    if (msm != NULL) {
        *msm = (uint8_t)digit;
    }
    return true;
}

bool gnss_rtcm3_msm_decode(const uint8_t *frame, size_t len, gnss_rtcm3_msm_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t plen;
    const uint8_t *p = payload_of(frame, len, &plen);
    if (p == NULL || plen * 8u < GNSS_RTCM3_MSM_HEADER_BITS) {
        return false;
    }
    gnss_rtcm3_msm_t h = { 0 };
    h.msg_type = (uint16_t)bits_u(p, 0, 12);
    if (!gnss_rtcm3_msm_type(h.msg_type, &h.sys, &h.msm)) {
        return false;
    }
    h.station_id = (uint16_t)bits_u(p, 12, 12);
    h.epoch_time = (uint32_t)bits_u(p, 24, 30);
    h.multiple_message = bits_u(p, 54, 1) != 0;
    h.iods = (uint8_t)bits_u(p, 55, 3);
    /* 7 reserved bits, clock steering (2), external clock (2), smoothing
     * indicator (1) and interval (3) up to bit 72 are not needed. */
    for (unsigned i = 0; i < 64; i++) {
        if (bits_u(p, 73u + i, 1)) {
            h.sat_mask |= (uint64_t)1 << i;
            h.num_sats++;
        }
    }
    for (unsigned i = 0; i < 32; i++) {
        if (bits_u(p, 137u + i, 1)) {
            h.sig_mask |= (uint32_t)1 << i;
            h.num_sigs++;
        }
    }
    unsigned cells = (unsigned)h.num_sats * h.num_sigs;
    if (cells > GNSS_RTCM3_MSM_MAX_CELLS || plen * 8u < GNSS_RTCM3_MSM_HEADER_BITS + cells) {
        return false;
    }
    for (unsigned k = 0; k < cells; k++) {
        if (bits_u(p, GNSS_RTCM3_MSM_HEADER_BITS + k, 1)) {
            h.cell_mask |= (uint64_t)1 << k;
            h.num_cells++;
        }
    }
    *out = h;
    return true;
}

bool gnss_rtcm3_arp_decode(const uint8_t *frame, size_t len, gnss_rtcm3_arp_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t plen;
    const uint8_t *p = payload_of(frame, len, &plen);
    if (p == NULL || plen < 2) {
        return false;
    }
    uint16_t type = (uint16_t)bits_u(p, 0, 12);
    size_t need_bits = (type == 1005) ? 152u : (type == 1006) ? 168u : 0u;
    if (need_bits == 0 || plen * 8u < need_bits) {
        return false;
    }
    gnss_rtcm3_arp_t a = { 0 };
    a.msg_type = type;
    a.station_id = (uint16_t)bits_u(p, 12, 12);
    a.itrf_year = (uint8_t)bits_u(p, 24, 6);
    a.gps = bits_u(p, 30, 1) != 0;
    a.glonass = bits_u(p, 31, 1) != 0;
    a.galileo = bits_u(p, 32, 1) != 0;
    a.ref_station = bits_u(p, 33, 1) != 0;
    a.ecef_01mm[0] = bits_s(p, 34, 38);
    /* bit 72: single receiver oscillator indicator, bit 73: reserved */
    a.ecef_01mm[1] = bits_s(p, 74, 38);
    /* bits 112-113: quarter cycle indicator */
    a.ecef_01mm[2] = bits_s(p, 114, 38);
    if (type == 1006) {
        a.height_01mm = (uint16_t)bits_u(p, 152, 16);
    }
    *out = a;
    return true;
}

/* Signal IDs lo..hi as mask bits (bit id-1). */
#define SIG_IDS(lo, hi) ((0xFFFFFFFFu >> (32u - ((hi) - (lo) + 1u))) << ((lo) - 1u))

/* Signal IDs of each band bucket, per constellation. */
static const uint32_t k_band_sigs[GNSS_SYS_COUNT][GNSS_BAND_COUNT] = {
    [GNSS_SYS_GPS] = {
        [GNSS_BAND_L1] = SIG_IDS(2, 4) | SIG_IDS(30, 32),       /* 1C 1P 1W; L1C 1S 1L 1X */
        [GNSS_BAND_L2] = SIG_IDS(8, 10) | SIG_IDS(15, 17),      /* 2C 2P 2W; 2S 2L 2X */
        [GNSS_BAND_L5] = SIG_IDS(22, 24),                       /* 5I 5Q 5X */
    },
    [GNSS_SYS_GLONASS] = {
        [GNSS_BAND_L1] = SIG_IDS(2, 3),                         /* G1: 1C 1P */
        [GNSS_BAND_L2] = SIG_IDS(8, 9),                         /* G2: 2C 2P */
    },
    [GNSS_SYS_GALILEO] = {
        [GNSS_BAND_L1] = SIG_IDS(2, 6),                         /* E1: 1C 1A 1B 1X 1Z */
        [GNSS_BAND_L6] = SIG_IDS(8, 12),                        /* E6: 6C 6A 6B 6X 6Z */
        [GNSS_BAND_L2] = SIG_IDS(14, 16),                       /* E5b: 7I 7Q 7X */
        [GNSS_BAND_E5AB] = SIG_IDS(18, 20),                     /* E5 AltBOC: 8I 8Q 8X */
        [GNSS_BAND_L5] = SIG_IDS(22, 24),                       /* E5a: 5I 5Q 5X */
    },
    [GNSS_SYS_QZSS] = {
        [GNSS_BAND_L1] = SIG_IDS(2, 2) | SIG_IDS(30, 32),       /* 1C; L1C 1S 1L 1X */
        [GNSS_BAND_L6] = SIG_IDS(9, 13),                        /* L6: 6S 6L 6X (+ 6E 6Z, tentative IDs) */
        [GNSS_BAND_L2] = SIG_IDS(15, 17),                       /* 2S 2L 2X */
        [GNSS_BAND_L5] = SIG_IDS(22, 24),                       /* 5I 5Q 5X */
    },
    [GNSS_SYS_BEIDOU] = {
        [GNSS_BAND_L1] = SIG_IDS(2, 4) | SIG_IDS(30, 32),       /* B1I 2I 2Q 2X; B1C 1D 1P 1X */
        [GNSS_BAND_L6] = SIG_IDS(8, 10),                        /* B3I: 6I 6Q 6X */
        [GNSS_BAND_L2] = SIG_IDS(14, 16) | SIG_IDS(25, 25),     /* B2I 7I 7Q 7X; B2b 7D */
        [GNSS_BAND_L5] = SIG_IDS(22, 24),                       /* B2a: 5D 5P 5X */
    },
    /* SBAS and NavIC: not mapped. */
};

gnss_band_t gnss_rtcm3_msm_band(gnss_sys_t sys, unsigned sig_id)
{
    if ((unsigned)sys >= GNSS_SYS_COUNT || sig_id < 1 || sig_id > 32) {
        return GNSS_BAND_NONE;
    }
    const uint32_t bit = (uint32_t)1 << (sig_id - 1);
    for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
        if (k_band_sigs[sys][b] & bit) {
            return (gnss_band_t)b;
        }
    }
    return GNSS_BAND_NONE;
}

uint8_t gnss_rtcm3_msm_band_mask(gnss_sys_t sys, uint32_t sig_mask)
{
    if ((unsigned)sys >= GNSS_SYS_COUNT) {
        return 0;
    }
    uint8_t bands = 0;
    for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
        if (k_band_sigs[sys][b] & sig_mask) {
            bands |= (uint8_t)(1u << b);
        }
    }
    return bands;
}
