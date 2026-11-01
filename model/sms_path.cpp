// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sms_path.cpp - f286::sms implementation. See sms_path.h for grounding and tags.
//
// Conventions: all PDU fields are octet streams, LSB-first bit order inside
// septets (TS 23.038: septet s occupies stream bits 7s..7s+6, bit 0 first).
// No heap, no exceptions. Every stock VA cited is in kernel.bin space
// (file_off = VA - 0x80a05dc8 for the rodata tables).

#include "sms_path.h"

#include <cstring>

namespace f286 {
namespace sms {

// ---------------------------------------------------------------------------
// 1. GSM 7-bit tables - byte-exact copies from kernel.bin
// ---------------------------------------------------------------------------

// TS 23.038 default alphabet -> Latin-1 bytes. Stock @0x8152C10E. Stock
// deviations kept: [0x10..0x1A] identity/underscore (Greek unrepresentable),
// [0x24] = '$' (spec says 0xA4).
extern const uint8_t kGsm7ToByte[128] = {
    0x40, 0xa3, 0x24, 0xa5, 0xe8, 0xe9, 0xf9, 0xec, 0xf2, 0xc7, 0x0a, 0xd8, 0xf8, 0x0d, 0xc5, 0xe5,
    0x10, 0x5f, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0xc6, 0xe6, 0xdf, 0xc9,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0xa1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,
    0xbf, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xe4, 0xf6, 0xf1, 0xfc, 0xe0,
};

// 0x1B-escaped byte extension. Stock @0x8152C08E. NO form-feed at 0x0A, NO euro
// at 0x65 (both verified absent); placeholders are 0x20.
extern const uint8_t kGsm7ExtToByte[128] = {
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x5e, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x7b, 0x7d, 0x20, 0x20, 0x20, 0x20, 0x20, 0x5c,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x5b, 0x7e, 0x5d, 0x20,
    0x7c, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
};

// GSM 7-bit code -> UCS2. Stock @0x8152C18E. Spec-correct at 0x24 (0x00A4),
// standard Greek block at 0x10..0x19, 0x1B escape -> 0x0020 placeholder.
// NOTE indices 0x1B and 0x20 BOTH decode to 0x0020 - the 0x1B slot is the escape
// code and must never be chosen by the encoder (see find_in_map).
extern const uint16_t kGsm7ToUcs2[128] = {
    0x0040, 0x00a3, 0x0024, 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec,
    0x00f2, 0x00c7, 0x000a, 0x00d8, 0x00f8, 0x000d, 0x00c5, 0x00e5,
    0x0394, 0x005f, 0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8,
    0x03a3, 0x0398, 0x039e, 0x0020, 0x00c6, 0x00e6, 0x00df, 0x00c9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00a4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
    0x00a1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005a, 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7,
    0x00bf, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007a, 0x00e4, 0x00f6, 0x00f1, 0x00fc, 0x00e0,
};

// 0x1B-escaped UCS2 extension. Stock @0x8152C28E - same mapping as the byte ext
// table PLUS the euro U+20AC at index 0x65 (== 0x8152C358).
extern const uint16_t kGsm7ExtToUcs2[128] = {
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x005e, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x007b, 0x007d, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x005c,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x005b, 0x007e, 0x005d, 0x0020,
    0x007c, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x20ac, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
};

// Latin-1/byte -> septet (canonical mapping wins on the '$' double-map; 0xFF =
// not representable in a single septet - extension chars go through
// ucs2_to_gsm7_septets, which emits 0x1B pairs).
extern const uint8_t kGsm7ByteToSeptet[256] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0a, 0xff, 0xff, 0x0d, 0xff, 0xff,
    0x10, 0xff, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0xff, 0xff, 0xff, 0xff,
    0x20, 0x21, 0x22, 0x23, 0x02, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x00, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xff, 0xff, 0xff, 0xff, 0x11,
    0xff, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x40, 0xff, 0x01, 0xff, 0x03, 0xff, 0x5f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x60,
    0xff, 0xff, 0xff, 0xff, 0x5b, 0x0e, 0x1c, 0x09, 0xff, 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x5d, 0xff, 0xff, 0xff, 0xff, 0x5c, 0xff, 0x0b, 0xff, 0xff, 0xff, 0x5e, 0xff, 0xff, 0x1e,
    0x7f, 0xff, 0xff, 0xff, 0x7b, 0x0f, 0x1d, 0xff, 0x04, 0x05, 0xff, 0xff, 0x07, 0xff, 0xff, 0xff,
    0xff, 0x7d, 0x08, 0xff, 0xff, 0xff, 0x7c, 0xff, 0x0c, 0x06, 0xff, 0xff, 0x7e, 0xff, 0xff, 0xff,
};

// ---------------------------------------------------------------------------
// GSM7 packing / expansion - stock semantics, roles SPLIT (0x81246560/0x81246666)
// ---------------------------------------------------------------------------

size_t gsm7_pack(uint8_t* septets, uint16_t* n, uint32_t fill_flag) {
    if (!septets || !n) return 0;
    const uint32_t nsept = *n;
    const uint32_t total = nsept * 7;
    const uint32_t nbytes = (total + 7) / 8;
    // The air bitstream is LSB-first: septet s occupies stream bits 7s..7s+6
    // (septet bit 0 first); stream bit p lives in byte p>>3, bit p&7. Rotation
    // form: byte j = (sept[i] >> shift) | (sept[i+1] << (7 - shift)), i = 8j/7,
    // shift = 8j - 7*i. Verified against the canonical vector "hellohello" ->
    // E8 32 9B FD 46 97 D9 EC 37 (equivalent to the stock rotator 0x81246560).
    for (uint32_t j = 0; j < nbytes; ++j) {
        uint8_t out = 0;
        for (uint32_t b = 0; b < 8; ++b) {
            const uint32_t p = j * 8 + b;
            uint8_t bit;
            if (p < total) {
                bit = (uint8_t)((septets[p / 7] >> (p % 7)) & 1);
            } else if (fill_flag == 0 && (total & 7) == 1) {
                bit = (uint8_t)((0x0D >> (p - total)) & 1);  // stock: 7 spare bits -> CR fill
            } else {
                bit = 0;
            }
            out |= (uint8_t)(bit << b);
        }
        septets[j] = out;
    }
    *n = (uint16_t)nbytes;
    return nbytes;
}

void gsm7_expand(const uint8_t* src, uint16_t n_bytes, uint8_t* dest,
                 uint16_t* out_len, int skip134) {
    if (!src || !dest || !out_len) return;
    const uint32_t total = ((uint32_t)n_bytes * 8) / 7;
    uint32_t bitpos = 0, di = 0;
    for (uint32_t p = 0; p < total; ++p) {
        if (skip134 && (p % 134) == 0) {  // stock: consumed but NOT packed
            bitpos += 7;
            continue;
        }
        uint8_t s = 0;
        for (uint32_t b = 0; b < 7; ++b) {
            const uint32_t k = bitpos + b;
            s |= (uint8_t)(((src[k >> 3] >> (k & 7)) & 1) << b);  // LSB-first stream
        }
        bitpos += 7;
        dest[di++] = s;
    }
    *out_len = (uint16_t)total;  // stock sets n*8/7 regardless of skips
}

static int find_in_map(const uint16_t* map, uint16_t c) {
    for (int i = 0; i < 128; ++i) {
        if (i == 0x1B) continue;  // escape slot: never emitted as a literal char
        if (map[i] == c) return i;
    }
    return -1;
}

size_t ucs2_to_gsm7_septets(const uint16_t* ucs2, size_t nchars, uint8_t* septets,
                            size_t cap_septets) {
    if (!ucs2 || !septets) return SIZE_MAX;
    size_t out = 0;
    for (size_t i = 0; i < nchars; ++i) {
        const uint16_t c = ucs2[i];
        const int s = find_in_map(kGsm7ToUcs2, c);
        if (s >= 0) {
            if (out + 1 > cap_septets) return SIZE_MAX;
            septets[out++] = (uint8_t)s;
            continue;
        }
        const int e = find_in_map(kGsm7ExtToUcs2, c);
        if (e >= 0 && kGsm7ExtToUcs2[e] != 0x20) {  // 0x1B + extension septet
            if (out + 2 > cap_septets) return SIZE_MAX;
            septets[out++] = 0x1B;
            septets[out++] = (uint8_t)e;
            continue;
        }
        return SIZE_MAX;  // not representable -> caller falls back to UCS2
    }
    return out;
}

size_t gsm7_septets_to_ucs2(const uint8_t* septets, size_t nseptets,
                            uint16_t* ucs2, size_t cap_chars) {
    if (!septets || !ucs2) return 0;
    size_t out = 0;
    for (size_t i = 0; i < nseptets; ++i) {
        uint16_t c;
        if (septets[i] == 0x1B && i + 1 < nseptets) {
            c = kGsm7ExtToUcs2[septets[++i]];
        } else {
            c = kGsm7ToUcs2[septets[i] & 0x7F];
        }
        if (out < cap_chars) ucs2[out] = c;
        ++out;
    }
    return out;
}

size_t gsm7_ud_decode(const uint8_t* ud, size_t ud_bytes, uint8_t udhl,
                      uint16_t* ucs2, size_t cap_chars) {
    uint8_t septets[kUdMaxBytes * 8 / 7 + 8];
    uint8_t tmp[kUdMaxBytes];
    uint16_t n = (uint16_t)ud_bytes;
    if (n > kUdMaxBytes) n = kUdMaxBytes;
    memcpy(tmp, ud, n);
    gsm7_expand(tmp, n, septets, &n, 0);
    const size_t fill = udhl ? ((size_t)udhl * 8 + 6) / 7 : 0;  // UDH padding septets
    if ((size_t)n <= fill) return 0;
    return gsm7_septets_to_ucs2(septets + fill, (size_t)n - fill, ucs2, cap_chars);
}

// ---------------------------------------------------------------------------
// 2. Addresses
// ---------------------------------------------------------------------------

static uint8_t digit_to_semi(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    switch (c) {
        case '*': return 0x0A;
        case '#': return 0x0B;
        case 'a': case 'A': return 0x0C;
        case 'b': case 'B': return 0x0D;  // stock decode maps 0x0D -> '?'
        case 'c': case 'C': return 0x0E;
        default: return 0x0F;             // filler
    }
}

static char semi_to_digit(uint8_t s) {
    static const char t[16] = {'0','1','2','3','4','5','6','7','8','9','*','#','a','?','c','?'};
    return t[s & 0x0F];  // stock 0x80a67344: 0x0D -> '?'
}

bool addr_decode(const uint8_t* buf, size_t cap, size_t* consumed, SmsAddress* out) {
    if (!buf || cap < 2 || !out) return false;
    const uint8_t len = buf[0];                 // semi-octets [V 0x80a673e8]
    const size_t nocts = ((size_t)len + 1) >> 1;
    if (nocts >= 0x0D || nocts > cap - 2) return false;  // stock: nocts < 0xD required
    out->toa = buf[1];
    size_t d = 0;
    for (size_t i = 0; i < nocts; ++i) {
        if (d < kAddrMaxDigits) out->digits[d++] = semi_to_digit(buf[2 + i] & 0x0F);
        if (d < kAddrMaxDigits && (i * 2 + 1) < len)
            out->digits[d++] = semi_to_digit(buf[2 + i] >> 4);
    }
    out->digits[d] = 0;
    out->len = (uint8_t)d;
    if (consumed) *consumed = 2 + nocts;
    return true;
}

size_t addr_encode(const SmsAddress& a, uint8_t* out, size_t cap) {
    if (!out || cap < 2) return 0;
    size_t n = 0;
    while (n < kAddrMaxDigits && a.digits[n]) ++n;
    const size_t nocts = (n + 1) / 2;
    if (2 + nocts > cap) return 0;
    out[0] = (uint8_t)n;       // digit count [SPEC 23.040 section 9.1.2.5]
    out[1] = a.toa;
    for (size_t i = 0; i < nocts; ++i) {
        const uint8_t lo = digit_to_semi(a.digits[2 * i]);
        const uint8_t hi = (2 * i + 1 < n) ? digit_to_semi(a.digits[2 * i + 1]) : 0x0F;
        out[2 + i] = (uint8_t)(lo | (hi << 4));
    }
    return 2 + nocts;
}

// ---------------------------------------------------------------------------
// 3. SCTS
// ---------------------------------------------------------------------------

static inline uint8_t swap_semi(uint8_t v) { return (uint8_t)(((v & 0x0F) << 4) | (v >> 4)); }

void scts_encode(const Scts& t, uint8_t out[7]) {
    out[0] = swap_semi((uint8_t)(t.year % 100));
    out[1] = swap_semi(t.month);
    out[2] = swap_semi(t.day);
    out[3] = swap_semi(t.hour);
    out[4] = swap_semi(t.minute);
    out[5] = swap_semi(t.second);
    const uint8_t mag = (uint8_t)(t.tz_quarters < 0 ? -t.tz_quarters : t.tz_quarters) & 0x7F;
    const uint8_t sign = (t.tz_quarters < 0) ? 0x08 : 0x00;  // '1' = negative (east)
    out[6] = swap_semi((uint8_t)(sign | mag));
}

bool scts_decode(const uint8_t in[7], Scts* out) {
    if (!in || !out) return false;
    out->year    = (uint16_t)(swap_semi(in[0]) % 100);
    out->month   = (uint8_t)(swap_semi(in[1]) % 99);
    out->day     = (uint8_t)(swap_semi(in[2]) % 99);
    out->hour    = (uint8_t)(swap_semi(in[3]) % 99);
    out->minute  = (uint8_t)(swap_semi(in[4]) % 99);
    out->second  = (uint8_t)(swap_semi(in[5]) % 99);
    const uint8_t raw = swap_semi(in[6]);
    const int mag = raw & 0x7F;
    out->tz_quarters = (int8_t)((raw & 0x08) ? -mag : mag);
    return true;
}

// ---------------------------------------------------------------------------
// 4. DCS
// ---------------------------------------------------------------------------

uint8_t dcs_build(Alphabet alpha, SmsClass cls) {
    // Stock 0x80a63030 core expression (verified): (alphabet&3)<<2 | class.
    // The flavor branches (|= 0xF0 for group 1111 / |= 0x10 for group 0010) are
    // only taken when the stock callers set compressed/flavor flags - we never do.
    const uint8_t c = (cls == SmsClass::None) ? 0x00 : (uint8_t)cls;
    return (uint8_t)((((uint8_t)alpha & 3) << 2) | c);
}

bool dcs_parse(uint8_t raw, Alphabet* alpha, SmsClass* cls) {
    // Stock DL whitelist [V 0x80a99e20 +0x184]: {0x00, 0x20-0x29, 0x31, 0x32,
    // 0x40-0x47, 0x7F}; everything else is zeroed (squashed -> Gsm7/None).
    if (!alpha || !cls) return false;
    *alpha = Alphabet::Gsm7;
    *cls = SmsClass::None;
    if (raw == 0x00) { *cls = SmsClass::Class0; return true; }
    if (raw >= 0x20 && raw <= 0x29) { *alpha = (Alphabet)((raw >> 2) & 3); *cls = (SmsClass)(raw & 3); return true; }
    if (raw == 0x31 || raw == 0x32) { *cls = (SmsClass)(raw & 3); return true; }             // group 0011
    if (raw >= 0x40 && raw <= 0x47) { *alpha = (Alphabet)((raw >> 2) & 3); *cls = (SmsClass)(raw & 3); return true; }  // MWI group; no MWI-type decode [V]
    if (raw == 0x7F) { *alpha = Alphabet::Data8; *cls = SmsClass::Class3; return true; }     // data download
    return false;
}

// ---------------------------------------------------------------------------
// 5. TPDU codec
// ---------------------------------------------------------------------------

TpduType tpdu_type(uint8_t first_octet) { return (TpduType)(first_octet & 0x03); }

static size_t vpd_len(uint8_t vpf) {
    switch (vpf) {
        case 1: return 1;   // relative  [V packer fmt2 -> 1 octet]
        case 2: return 7;   // absolute  [V packer fmt1 -> 7 octets]
        case 3: return 7;   // enhanced  [V packer fmt3 -> 7/14 octets; air = 7]
        default: return 0;  // none      [V packer fmt0 emits nothing]
    }
}

size_t submit_encode(const SmsSubmit& s, uint8_t* out, size_t cap) {
    if (!out || cap < 2) return 0;
    size_t o = 0;
    uint8_t da[kAddrMaxDigits / 2 + 3] = {0};
    const size_t da_len = addr_encode(s.da, da, sizeof(da));
    if (da_len == 0) return 0;
    const size_t need = 2 + da_len + 2 + vpd_len(s.vpf) + 1 + s.ud_len;
    if (need > cap) return 0;

    out[o++] = (uint8_t)((s.rp ? 0x80 : 0) | (s.udhi ? 0x40 : 0) | (s.srr ? 0x20 : 0) |
                         ((s.vpf & 3) << 3) | (s.rd ? 0x04 : 0) | 0x01);   // TP-MTI = 01 [SPEC]
    out[o++] = s.mr;
    memcpy(out + o, da, da_len); o += da_len;
    out[o++] = s.pid;
    out[o++] = s.dcs;
    for (size_t i = 0; i < vpd_len(s.vpf); ++i) out[o++] = s.vp.oct[i];
    out[o++] = (uint8_t)s.udl;
    memcpy(out + o, s.ud, s.ud_len); o += s.ud_len;
    return o;
}

bool deliver_decode(const uint8_t* in, size_t len, SmsDeliver* d) {
    if (!in || len < 1 || !d) return false;
    *d = SmsDeliver{};
    size_t o = 0;
    const uint8_t fo = in[o++];
    if ((fo & 0x03) != 0x00) return false;                    // TP-MTI must be DELIVER
    d->mms  = (fo >> 2) & 1;                                  // stock never reads it [V]
    d->sri  = (fo >> 5) & 1;
    d->udhi = (fo >> 6) & 1;
    d->rp   = (fo >> 7) & 1;
    size_t used = 0;
    if (!addr_decode(in + o, len - o, &used, &d->oa)) return false;
    o += used;
    if (o + 9 > len) return false;
    d->pid = in[o++];
    d->dcs = in[o++];
    Alphabet alpha; SmsClass cls;
    if (!dcs_parse(d->dcs, &alpha, &cls)) d->dcs = 0;         // stock squash [V]
    scts_decode(in + o, &d->scts);
    o += 7;
    if (o + 1 > len) return false;
    d->udl = in[o++];
    dcs_parse(d->dcs, &alpha, &cls);
    const bool gsm7 = (alpha == Alphabet::Gsm7);
    size_t ud_bytes = gsm7 ? ((d->udl * 7 + 7) / 8) : d->udl;
    if (ud_bytes > kUdMaxBytes) ud_bytes = kUdMaxBytes;       // stock 0x8C clamp [V]
    if (o + ud_bytes > len) ud_bytes = (len > o) ? len - o : 0;
    memcpy(d->ud, in + o, ud_bytes);
    d->ud_len = ud_bytes;
    if (gsm7 && ud_bytes * 8 / 7 < d->udl) d->udl = ud_bytes * 8 / 7;
    return true;
}

bool status_report_decode(const uint8_t* in, size_t len, SmsStatusReport* sr) {
    if (!in || len < 1 || !sr) return false;
    *sr = SmsStatusReport{};
    size_t o = 0;
    const uint8_t fo = in[o++];
    if ((fo & 0x03) != 0x02) return false;
    sr->udhi = (fo >> 6) & 1;
    sr->srq  = (fo >> 5) & 1;
    if (o + 1 > len) return false;
    sr->mr = in[o++];                                         // TP-MR
    SmsAddress ra;
    size_t used = 0;
    if (!addr_decode(in + o, len - o, &used, &ra)) return false;  // TP-RA
    o += used;
    sr->ra = ra;
    if (o + 7 > len) return false;
    scts_decode(in + o, &sr->scts);                           // TP-SCTS
    o += 7;
    if (o + 7 > len) return false;
    scts_decode(in + o, &sr->dt);                             // TP-DT
    o += 7;
    if (o + 2 > len) return false;
    sr->fcs = in[o++];                                        // TP-ST
    sr->pi  = in[o++];                                        // TP-PI
    if ((sr->pi & 0x01) && o + 1 <= len) o += 1;   // TP-PID present
    if ((sr->pi & 0x02) && o + 1 <= len) o += 1;   // TP-DCS present
    if ((sr->pi & 0x04) && o + 1 <= len) {
        sr->udl = in[o++];
        size_t ub = sr->udl > kUdMaxBytes ? kUdMaxBytes : sr->udl;
        if (o + ub > len) ub = (len > o) ? len - o : 0;
        memcpy(sr->ud, in + o, ub);
        sr->ud_len = ub;
    }
    return true;
}

size_t udh_concat_build(uint16_t ref, uint8_t max, uint8_t seq, bool ref16,
                        uint8_t* out, size_t cap, uint8_t* udhl) {
    if (!out || !udhl || cap < 6) return 0;
    if (ref16) {
        out[0] = 0x08; out[1] = 0x04;
        out[2] = (uint8_t)(ref >> 8); out[3] = (uint8_t)ref;   // BE [V 0x80ccd300 key order]
        out[4] = max;  out[5] = seq;
        *udhl = 6;
        return 6;
    }
    out[0] = 0x00; out[1] = 0x03;
    out[2] = (uint8_t)ref; out[3] = max; out[4] = seq;
    *udhl = 5;
    return 5;
}

bool udh_concat_parse(const uint8_t* ud, size_t ud_len, uint8_t udhl,
                      uint16_t* ref, uint8_t* max, uint8_t* seq, bool* ref16) {
    if (!ud || !ref || !max || !seq || !ref16) return false;
    if (udhl < 5 || ud_len < (size_t)(1 + udhl)) return false;
    const uint8_t* u = ud + 1;  // skip UDHL octet
    if (u[0] == 0x00 && u[1] >= 3) {   // concat 8-bit [V]
        *ref = u[2]; *max = u[3]; *seq = u[4]; *ref16 = false;
        return true;
    }
    if (u[0] == 0x08 && u[1] >= 4) {   // concat 16-bit [V]
        *ref = (uint16_t)((u[2] << 8) | u[3]); *max = u[4]; *seq = u[5]; *ref16 = true;
        return true;
    }
    return false;
}

size_t segment_septet_budget(Alphabet alpha, bool concatenated, bool ref16) {
    // [SPEC TS 23.040/23.038]
    (void)ref16;
    switch (alpha) {
        case Alphabet::Gsm7: return concatenated ? (ref16 ? 152 : 153) : 160;
        case Alphabet::Ucs2: return concatenated ? 67 : 70;
        case Alphabet::Data8: return concatenated ? 134 : 140;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// 6. RP/CP (TS 24.011) - [SPEC-CAVEAT] layouts; bounds [V]
// ---------------------------------------------------------------------------

size_t cp_data_build(uint8_t ti, uint8_t reference, const uint8_t* rpdu, size_t rpdu_len,
                     uint8_t* out, size_t cap) {
    if (!out || !rpdu || 3 + rpdu_len > cap) return 0;
    out[0] = (uint8_t)((ti << 4) | kCpSmsPd);
    out[1] = kCpData;
    out[2] = reference;
    memcpy(out + 3, rpdu, rpdu_len);
    return 3 + rpdu_len;
}

size_t cp_ack_build(uint8_t ti, uint8_t* out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = (uint8_t)((ti << 4) | kCpSmsPd);
    out[1] = kCpAck;
    return 2;
}

size_t cp_error_build(uint8_t ti, uint8_t cause, uint8_t* out, size_t cap) {
    if (!out || cap < 3) return 0;
    out[0] = (uint8_t)((ti << 4) | kCpSmsPd);
    out[1] = kCpError;
    out[2] = cause;
    return 3;
}

bool cp_parse(const uint8_t* in, size_t len, CpHeader* hdr,
              const uint8_t** body, size_t* body_len) {
    if (!in || len < 2 || !hdr) return false;
    if ((in[0] & 0x0F) != kCpSmsPd) return false;
    hdr->ti = (uint8_t)(in[0] >> 4);
    hdr->type = in[1];
    hdr->reference = 0;
    hdr->cause = 0;
    if (body) *body = nullptr;
    if (body_len) *body_len = 0;
    switch (hdr->type) {
        case kCpData:
            if (len < 3) return false;
            hdr->reference = in[2];
            if (body) *body = in + 3;
            if (body_len) *body_len = len - 3;
            return true;
        case kCpAck:
            if (len >= 3) hdr->reference = in[2];
            return true;
        case kCpError:
            if (len < 3) return false;
            hdr->cause = in[2];
            return true;
        default:
            return false;
    }
}

// RP addresses: MO RP-O = {0x00, 0x00}; RP-D = {contents-len, toa, bcd} where
// contents-len counts the TOA octet + BCD octets (TS 24.011 section 8.2.5.3, SPEC-CAVEAT).
static size_t rp_addr_encode(const SmsAddress& a, uint8_t* out, size_t cap) {
    uint8_t tmp[kAddrMaxDigits / 2 + 3] = {0};
    const size_t n = addr_encode(a, tmp, sizeof(tmp));  // {n_digits, toa, bcd...}
    if (n < 2 || n > cap) return 0;
    out[0] = (uint8_t)(n - 1);  // contents = TOA + BCD octets
    memcpy(out + 1, tmp + 1, n - 1);
    return n;                   // total bytes = 1 + contents
}

static bool rp_addr_decode(const uint8_t* in, size_t cap, size_t* used, SmsAddress* out) {
    if (cap < 1) return false;
    const uint8_t lc = in[0];
    if (lc == 0) {  // empty address + spare octet
        if (cap < 2) return false;
        *used = 2;
        out->len = 0;
        out->digits[0] = 0;
        return true;
    }
    if (cap < (size_t)lc + 1) return false;
    if ((size_t)(lc - 1) > (size_t)(kAddrMaxDigits / 2 + 2)) return false;
    // rebuild as {n_digits, toa, bcd} for the TP-DA decoder
    uint8_t tmp[kAddrMaxDigits / 2 + 4] = {0};
    tmp[0] = (uint8_t)((lc - 1) * 2);
    memcpy(tmp + 1, in + 1, lc);
    size_t inner = 0;
    if (!addr_decode(tmp, (size_t)lc + 1, &inner, out)) return false;
    *used = 1 + lc;
    return true;
}

size_t rp_data_mo_build(uint8_t mr, const SmsAddress& sca,
                        const uint8_t* tpdu, size_t tpdu_len,
                        uint8_t* out, size_t cap) {
    uint8_t da[kAddrMaxDigits / 2 + 4] = {0};
    const size_t da_len = rp_addr_encode(sca, da, sizeof(da));
    if (da_len == 0 || 4 + da_len + 1 + tpdu_len > cap) return 0;
    size_t o = 0;
    out[o++] = kRpDataMsToN;
    out[o++] = mr;
    out[o++] = 0x00;  // RP-O length 0
    out[o++] = 0x00;  // spare
    memcpy(out + o, da, da_len); o += da_len;
    out[o++] = (uint8_t)tpdu_len;
    memcpy(out + o, tpdu, tpdu_len); o += tpdu_len;
    return o;
}

bool rp_data_mt_parse(const uint8_t* in, size_t len, uint8_t* mr,
                      SmsAddress* sca, const uint8_t** tpdu, size_t* tpdu_len) {
    if (!in || len < 2 || !mr || !sca || !tpdu || !tpdu_len) return false;
    if (in[0] != kRpDataNToMs) return false;
    size_t o = 1;
    *mr = in[o++];  // RP-MR (the network's reference - echo it in our RP-ACK)
    size_t used = 0;
    if (!rp_addr_decode(in + o, len - o, &used, sca)) return false;   // RP-O = SMSC
    o += used;
    SmsAddress dest;
    if (!rp_addr_decode(in + o, len - o, &used, &dest)) return false; // RP-D = empty
    o += used;
    if (o + 1 > len) return false;
    const uint8_t ul = in[o++];
    if (o + ul > len) return false;
    *tpdu = in + o;
    *tpdu_len = ul;
    return true;
}

size_t rp_ack_mo_build(uint8_t mr, uint8_t* out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = kRpAckMsToN;
    out[1] = mr;
    return 2;
}

size_t rp_ack_mt_build(uint8_t mr, uint8_t* out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = kRpAckNToMs;
    out[1] = mr;
    return 2;
}

bool rp_error_parse(const uint8_t* in, size_t len, uint8_t* mr, uint8_t* cause) {
    if (!in || len < 2 || !mr || !cause) return false;
    const uint8_t mt = in[0];
    if (mt != kRpErrNToMs && mt != kRpErrMsToN) return false;
    *mr = in[1];
    *cause = (len >= 3) ? in[2] : 0;
    return true;
}

// ---------------------------------------------------------------------------
// 7. NAS transport envelope
// ---------------------------------------------------------------------------

size_t nas_transport_ul_build(const uint8_t* container, size_t len,
                              uint8_t* out, size_t cap) {
    if (!container || !out || len < kContainerMin || len > kContainerMax || cap < len + 3)
        return 0;  // bounds [V 0x80bd5474 asserts 2..0xFB]
    out[0] = 0x70;                 // plain EMM: PD=7, security header type 0
    out[1] = kEmmUlNasTransport;   // 0x65 [V]
    out[2] = (uint8_t)len;         // LV container, 1 length octet [V struct+0x107]
    memcpy(out + 3, container, len);
    return len + 3;
}

bool nas_transport_dl_parse(const uint8_t* in, size_t len,
                            const uint8_t** container, size_t* container_len) {
    // Plain DL NAS TRANSPORT; security-protected variants are handled (deciphered)
    // by the NAS port before this call. Stock DL decoder unlocated;
    // this mirrors the verified UL encoder + TS 24.301 section 9.9.3.22. [SPEC]
    if (!in || len < 4 || !container || !container_len) return false;
    if (in[0] != 0x70 || in[1] != kEmmDlNasTransport) return false;
    const size_t cl = in[2];
    if (cl < kContainerMin || cl > kContainerMax || 3 + cl > len) return false;
    *container = in + 3;
    *container_len = cl;
    return true;
}

size_t sms_ul_frame_build(uint8_t cp_ref, uint8_t rp_mr, const SmsAddress& sca,
                          const uint8_t* tpdu, size_t tpdu_len,
                          uint8_t* out, size_t cap) {
    uint8_t rp[300], cp[320];
    const size_t rl = rp_data_mo_build(rp_mr, sca, tpdu, tpdu_len, rp, sizeof(rp));
    if (rl == 0) return 0;
    const size_t cl = cp_data_build(cp_ref, cp_ref, rp, rl, cp, sizeof(cp));
    if (cl == 0) return 0;
    return nas_transport_ul_build(cp, cl, out, cap);
}

// ---------------------------------------------------------------------------
// 8. L2 ring binding
// ---------------------------------------------------------------------------

namespace l2ring {

struct RingEntry { uint16_t id; uint16_t len; uint32_t off; };

bool put_cmd(void* win, uint32_t cmd, uint16_t card, const void* payload, size_t len) {
    if (!win || !payload) return false;
    if (len != 0x1A4 && len != 0x20) return false;  // the two stock L2 payloads only
    volatile uint8_t* base = static_cast<volatile uint8_t*>(win);
    volatile RingCtl* ctl = reinterpret_cast<volatile RingCtl*>(base + kCtlOff);
    if (!can_put(ctl)) return false;  // stock asserts; we report and let the caller retry
    const uint16_t in = ctl->in;
    volatile uint8_t* body = base + kBodyOff + (uint32_t)in * kBodySize;
    *reinterpret_cast<volatile uint16_t*>(body + 0) = card;      // body[0] = card [V]
    // body[2..3] left stale - stock never writes them [V]
    *reinterpret_cast<volatile uint32_t*>(body + 4) = cmd;       // body[4] = cmdId [V]
    volatile uint8_t* d = body + 8;
    const uint8_t* s = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < len; ++i) d[i] = s[i];
    volatile RingEntry* e = reinterpret_cast<volatile RingEntry*>(
        base + kCtlOff + 4 + (uint32_t)in * 8);
    e->id = kEnvId;                                              // {0x000C, 0x01B8, off} [V]
    e->len = (uint16_t)kBodySize;
    e->off = (uint32_t)in * kBodySize;
    ctl->in = (uint16_t)((in + 1) % kSlots);
    return true;
}

bool parse_dl_data_msg(const uint8_t* msg, size_t msg_len, DlDataInd* out) {
    // Pinned layout, [V]. msg+4 id is matched against the DL
    // routing table before this parse; the carrier id itself is still open (needs a live capture).
    if (!msg || !out || msg_len < 0x50) return false;
    out->card     = (uint16_t)(msg[0x00] | (msg[0x01] << 8));
    out->id       = (uint32_t)msg[0x04] | ((uint32_t)msg[0x05] << 8) |
                    ((uint32_t)msg[0x06] << 16) | ((uint32_t)msg[0x07] << 24);
    out->matcher  = (uint32_t)msg[0x0C] | ((uint32_t)msg[0x0D] << 8) |
                    ((uint32_t)msg[0x0E] << 16) | ((uint32_t)msg[0x0F] << 24);
    out->list_id  = (uint16_t)(msg[0x10] | (msg[0x11] << 8));
    out->selector = (uint32_t)msg[0x14] | ((uint32_t)msg[0x15] << 8) |
                    ((uint32_t)msg[0x16] << 16) | ((uint32_t)msg[0x17] << 24);
    for (int i = 0; i < 2; ++i) {
        const uint8_t* p = msg + 0x2C + (size_t)i * 0x10;
        out->len[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    out->count   = (uint16_t)(msg[0x48] | (msg[0x49] << 8));
    if (out->count > 2) out->count = 2;  // stock clamps + error log [V]
    for (int i = 0; i < 2; ++i)
        out->idx[i] = (uint16_t)(msg[0x4C + (size_t)i * 4] | (msg[0x4D + (size_t)i * 4] << 8));
    return true;
}

} // namespace l2ring

// ---------------------------------------------------------------------------
// 9. Storage records
// ---------------------------------------------------------------------------

bool sms_record_encode(const SimSmsRecord& r, uint8_t out[kSimRecordSize]) {
    if (!out) return false;
    memset(out, 0xFF, kSimRecordSize);  // erased-card convention
    memset(out, 0, 13);                 // header region
    out[0] = r.status;
    uint8_t a[kAddrMaxDigits / 2 + 3] = {0};
    const size_t al = addr_encode(r.addr, a, sizeof(a));
    if (al == 0 || al > 12) return false;  // max 12 B address field in the record
    memcpy(out + 1, a, al);
    out[13] = r.pid;
    out[14] = r.dcs;
    scts_encode(r.scts, out + 15);
    out[22] = (uint8_t)r.udl;
    if (r.ud_len > sizeof(r.ud)) return false;
    memcpy(out + 23, r.ud, r.ud_len);
    return true;
}

bool sms_record_decode(const uint8_t in[kSimRecordSize], SimSmsRecord* r) {
    if (!in || !r) return false;
    *r = SimSmsRecord{};
    r->status = in[0];
    if (r->status == kRecFree) return true;
    size_t used = 0;
    SmsAddress a;
    if (!addr_decode(in + 1, 12, &used, &a)) {
        a.len = 0;
        a.digits[0] = 0;
        a.toa = in[2];
    }
    r->addr = a;
    r->pid = in[13];
    r->dcs = in[14];
    scts_decode(in + 15, &r->scts);
    r->udl = in[22];
    Alphabet alpha; SmsClass cls;
    const bool gsm7 = dcs_parse(r->dcs, &alpha, &cls) ? (alpha == Alphabet::Gsm7) : true;
    size_t ub = gsm7 ? (((size_t)r->udl * 7 + 7) / 8) : (size_t)r->udl;
    if (ub > sizeof(r->ud)) ub = sizeof(r->ud);
    memcpy(r->ud, in + 23, ub);
    r->ud_len = ub;
    return true;
}

bool smsp_parse(const uint8_t* rec, size_t len, SmsAddress* sca, uint8_t* dcs, uint8_t* vp) {
    // EF_SMSP, 28 B: {SMSC 12 B, DCS, VP, alpha 14 B} [SPEC TS 51.011 section 10.5.6]
    if (!rec || len < 14 || !sca) return false;
    if (rec[0] == 0) {
        sca->len = 0;
        sca->digits[0] = 0;
        sca->toa = 0x81;
    } else {
        const size_t cap = (rec[0] > 20) ? 22 : (size_t)rec[0] + 2;
        size_t used = 0;
        if (!addr_decode(rec, cap, &used, sca)) return false;
    }
    if (dcs) *dcs = rec[12];
    if (vp) *vp = rec[13];
    return true;
}

bool smsp_build(const SmsAddress& sca, uint8_t dcs, uint8_t vp, uint8_t* out, size_t cap) {
    if (!out || cap < 28) return false;
    memset(out, 0, cap);
    const size_t n = addr_encode(sca, out, 12);
    if (n == 0 || n > 12) return false;
    out[12] = dcs;
    out[13] = vp;
    return true;
}

bool smss_parse(const uint8_t* rec, size_t len, SmssState* out) {
    if (!rec || len < 2 || !out) return false;
    out->last_mr = rec[0];
    out->spare = rec[1];
    return true;
}

bool smss_build(const SmssState& s, uint8_t* out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = s.last_mr;
    out[1] = s.spare;
    return true;
}

// ---------------------------------------------------------------------------
// 10. MNSMS-lite
// ---------------------------------------------------------------------------

SmsStack::SmsStack(ISmsTransport& tx, ISmsStorage* st) : tx_(tx), st_(st) {}

void SmsStack::fire(const StackEvent& ev) {
    if (fn_) fn_(user_, ev);
}

void SmsStack::set_profile(Alphabet pref, SmsClass cls, uint8_t vp_rel, bool srr) {
    pref_alpha_ = pref;
    cls_ = cls;
    vp_rel_ = vp_rel;
    srr_ = srr;
}

// ---- MO --------------------------------------------------------------------

bool SmsStack::submit_build(const char* da_digits, uint8_t da_toa,
                            const uint16_t* ucs2, size_t nchars) {
    if (!da_digits || !ucs2 || nchars == 0 || mo_state_ != MoState::Idle) return false;

    SmsAddress da;
    da.toa = da_toa;
    size_t dl = 0;
    for (; dl < kAddrMaxDigits && da_digits[dl]; ++dl) da.digits[dl] = da_digits[dl];
    da.digits[dl] = 0;
    da.len = (uint8_t)dl;
    if (dl == 0) return false;

    // Alphabet choice: GSM7 if fully representable and it fits, else UCS2 [SPEC;
    // matches the stock DCS whitelist alphabets].
    uint8_t septets[200];
    size_t nsept = ucs2_to_gsm7_septets(ucs2, nchars, septets, sizeof(septets));
    Alphabet alpha = Alphabet::Gsm7;
    size_t units = nsept;
    if (nsept == SIZE_MAX || nsept > 160) {
        alpha = Alphabet::Ucs2;
        units = nchars;
    }
    const size_t budget = segment_septet_budget(alpha, true, false);
    const size_t parts = (units + budget - 1) / budget;
    if (parts == 0 || parts > 255) return false;

    for (size_t p = 0; p < parts; ++p) {
        SmsSubmit s{};
        s.vpf = 1;                          // relative VP
        s.vp.oct[0] = vp_rel_;
        s.srr = srr_;
        s.da = da;
        s.pid = 0;
        s.dcs = dcs_build(alpha, cls_);
        s.mr = ++mr_;
        uint8_t udh[6];
        uint8_t udhl = 0;
        if (parts > 1) {
            ++concat_ref_;
            udh_concat_build(concat_ref_, (uint8_t)parts, (uint8_t)(p + 1), false,
                             udh, sizeof(udh), &udhl);
        }
        if (alpha == Alphabet::Gsm7) {
            const size_t beg = p * budget;
            size_t cnt = units - beg;
            if (cnt > budget) cnt = budget;
            // UDH octets first, then the packed septet payload (packed in place).
            uint8_t seg[176];
            memcpy(seg, udh, udhl);
            memcpy(seg + udhl, septets + beg, cnt);
            uint16_t pc = (uint16_t)cnt;
            gsm7_pack(seg + udhl, &pc, 0);
            memcpy(s.ud, seg, (size_t)udhl + pc);
            s.ud_len = (size_t)udhl + pc;
            s.udl = cnt + (udhl ? ((size_t)udhl * 8 + 6) / 7 : 0);
        } else {
            const size_t beg = p * budget;
            size_t cnt = units - beg;
            if (cnt > budget) cnt = budget;
            const size_t fill = udhl ? (1 + udhl) : 0;
            s.ud[0] = udhl;
            memcpy(s.ud + 1, udh, udhl);
            memcpy(s.ud + fill, ucs2 + beg, cnt * 2);
            s.ud_len = fill + cnt * 2;
            s.udl = cnt;
        }

        uint8_t tpdu[200];
        const size_t tl = submit_encode(s, tpdu, sizeof(tpdu));
        if (tl == 0) return false;
        // Hand the EMM port the bare CP PDU; the NAS-transport envelope
        // ({0x70, 0x65, LV}) is the transport's job (see sms_ul_frame_build for
        // the standalone form).
        uint8_t rp[300], cp[320];
        const size_t rl = rp_data_mo_build(s.mr, sca_, tpdu, tl, rp, sizeof(rp));
        if (rl == 0) return false;
        ++cp_ref_;
        // TI stays 0 on the NAS-transport path (no real TI; correlation is
        // 1:1); only CP-Reference increments.
        const size_t cl = cp_data_build(0, cp_ref_, rp, rl, cp, sizeof(cp));
        if (cl == 0 || cl < kContainerMin || cl > kContainerMax) return false;
        if (!send_cp(cp, cl)) return false;
        if (p + 1 < parts) {
            // multipart: stock keeps ONE outstanding MO (TR1M) - later parts are
            // re-driven from SendCnf by the facade; we stop after part 1 here.
            mo_state_ = MoState::WaitRpAck;
            start_mo_timer();
            return true;
        }
    }
    mo_state_ = MoState::WaitRpAck;
    start_mo_timer();
    return true;
}

bool SmsStack::submit_text(const uint16_t* ucs2, size_t nchars, const char* da_digits,
                           uint8_t da_toa) {
    return submit_build(da_digits, da_toa, ucs2, nchars);
}

bool SmsStack::submit_pdu(const uint8_t* tpdu, size_t len) {
    // PDU mode (+CMGS <len>): the TE supplies the air TPDU - the RECOMMENDED path
    // (stock has no SUBMIT builder;). sca_ comes from +CSCA/EF_SMSP.
    if (!tpdu || len < 2 || len > 180 || mo_state_ != MoState::Idle) return false;
    if (tpdu_type(tpdu[0]) != TpduType::Submit) return false;
    uint8_t rp[300], cp[320];
    const size_t rl = rp_data_mo_build(tpdu[1], sca_, tpdu, len, rp, sizeof(rp));
    if (rl == 0) return false;
    ++cp_ref_;
    const size_t cl = cp_data_build(0, cp_ref_, rp, rl, cp, sizeof(cp));  // TI = 0 (NAS)
    if (cl == 0 || cl < kContainerMin || cl > kContainerMax) return false;
    if (!send_cp(cp, cl)) return false;
    mo_state_ = MoState::WaitRpAck;
    start_mo_timer();
    return true;
}

// ---- MT / DL ---------------------------------------------------------------

void SmsStack::handle_cp_data(const uint8_t* body, size_t len) {
    if (len < 1) return;
    const uint8_t rp_mti = body[0];
    if (rp_mti == kRpDataNToMs) {
        uint8_t mr = 0;
        SmsAddress oa;
        const uint8_t* tpdu = nullptr;
        size_t tl = 0;
        if (!rp_data_mt_parse(body, len, &mr, &oa, &tpdu, &tl)) return;

        SmsDeliver d{};
        if (!deliver_decode(tpdu, tl, &d)) return;  // silently drop undecodable

        // Concat reassembly (stock keys [V 0x80ccd300])
        uint16_t cref; uint8_t cmax, cseq; bool c16;
        ConcatSlot* slot = nullptr;
        if (d.udhi && udh_concat_parse(d.ud, d.ud_len, d.ud[0], &cref, &cmax, &cseq, &c16)) {
            for (auto& s : concat_)
                if (s.active && s.ref == cref) { slot = &s; break; }
            if (!slot) {
                for (auto& s : concat_)
                    if (!s.active) {
                        slot = &s;
                        s.active = true;
                        s.ref = cref;
                        s.total = cmax > 8 ? 8 : cmax;
                        s.have = 0;
                        s.seen = 0;
                        s.ud_bytes = 0;
                        s.alpha = Alphabet::Gsm7;
                        break;
                    }
            }
            if (slot && cseq >= 1 && cseq <= slot->total &&
                !(slot->seen & (1u << (cseq - 1)))) {
                const uint8_t udhl = d.ud[0];
                const size_t hdr = (size_t)(1 + udhl);
                Alphabet alpha; SmsClass cls;
                dcs_parse(d.dcs, &alpha, &cls);
                const size_t payload = (d.ud_len > hdr) ? (d.ud_len - hdr) : 0;
                if (slot->ud_bytes + payload <= sizeof(slot->buf)) {
                    memcpy(slot->buf + slot->ud_bytes, d.ud + hdr, payload);
                    slot->ud_bytes += payload;
                    slot->seen |= (uint8_t)(1u << (cseq - 1));
                    ++slot->have;
                }
                if (slot->have == slot->total) {
                    StackEvent ev{};
                    ev.id = EventId::SmsInd;
                    ev.mr = mr;
                    ev.concatenated = true;
                    ev.deliver = d;
                    ev.deliver.udhi = false;
                    memcpy(ev.deliver.ud, slot->buf, slot->ud_bytes);
                    ev.deliver.ud_len = slot->ud_bytes;
                    ev.deliver.udl = (alpha == Alphabet::Gsm7)
                                         ? slot->ud_bytes * 8 / 7
                                         : slot->ud_bytes;
                    slot->active = false;
                    if (st_) {
                        const int idx = st_->find_free(Storage::Sim);
                        const Storage where = (idx >= 0) ? Storage::Sim : Storage::Me;
                        const int ridx = st_->find_free(where);
                        if (ridx >= 0) {
                            SimSmsRecord rec{};
                            rec.status = kRecMtUnread;
                            rec.addr = d.oa;
                            rec.pid = d.pid;
                            rec.dcs = d.dcs;
                            rec.scts = d.scts;
                            memcpy(rec.ud, ev.deliver.ud, ev.deliver.ud_len);
                            rec.ud_len = ev.deliver.ud_len;
                            rec.udl = ev.deliver.udl;
                            if (st_->write(where, ridx, rec)) {
                                ev.storage = (int)where;
                                ev.record = ridx;
                            }
                        } else {
                            ev.id = EventId::MemFullInd;  // stock MEM_FULL_IND [V]
                        }
                    }
                    fire(ev);
                }
            }
        } else {
            // single-part message
            StackEvent ev{};
            ev.id = EventId::SmsInd;
            ev.mr = mr;
            ev.deliver = d;
            if (st_) {
                Storage where = Storage::Sim;
                int idx = st_->find_free(where);
                if (idx < 0) { where = Storage::Me; idx = st_->find_free(where); }
                if (idx >= 0) {
                    SimSmsRecord rec{};
                    rec.status = kRecMtUnread;
                    rec.addr = d.oa;
                    rec.pid = d.pid;
                    rec.dcs = d.dcs;
                    rec.scts = d.scts;
                    memcpy(rec.ud, d.ud, d.ud_len);
                    rec.ud_len = d.ud_len;
                    rec.udl = d.udl;
                    if (st_->write(where, idx, rec)) {
                        ev.storage = (int)where;
                        ev.record = idx;
                    }
                } else {
                    ev.id = EventId::MemFullInd;  // stock EV_MN_APP_SMS_MEM_FULL_IND_F [V]
                }
            }
            fire(ev);
        }

        // CP-ACK + RP-ACK(MS->N) echo the network's RP-MR [SPEC 24.011]
        uint8_t ack[4];
        const size_t al = cp_ack_build(0, ack, sizeof(ack));
        send_cp(ack, al);
        uint8_t rp[4];
        const size_t rl2 = rp_ack_mo_build(mr, rp, sizeof(rp));
        uint8_t cpd[16];
        ++cp_ref_;
        const size_t cl = cp_data_build(0, cp_ref_, rp, rl2, cpd, sizeof(cpd));  // TI = 0
        send_cp(cpd, cl);
        return;
    }

    if (rp_mti == kRpAckNToMs) {
        // RP-ACK for our MO -> EV_MN_APP_SEND_SMS_CNF_F [V event name]
        StackEvent ev{};
        ev.id = EventId::SendCnf;
        ev.mr = (len >= 2) ? body[1] : 0;
        fire(ev);
        mo_state_ = MoState::Idle;
        timer_armed_ = false;
        return;
    }

    if (rp_mti == kRpErrNToMs) {
        uint8_t mr = 0, cause = 0;
        rp_error_parse(body, len, &mr, &cause);
        StackEvent ev{};
        ev.id = EventId::SendErr;
        ev.mr = mr;
        ev.rp_cause = cause;
        fire(ev);
        mo_state_ = MoState::Idle;
        timer_armed_ = false;
        return;
    }
}

void SmsStack::handle_cp_ack() {
    // CP-ACK from the network closes our MT RP-ACK transaction. MO side treats a
    // stray CP-ACK as neutral.
}

void SmsStack::handle_cp_error(uint8_t cause) {
    StackEvent ev{};
    ev.id = EventId::SendErr;
    ev.rp_cause = cause;
    fire(ev);
    mo_state_ = MoState::Idle;
    timer_armed_ = false;
}

void SmsStack::on_nas_container(const uint8_t* cpdu, size_t len) {
    CpHeader hdr{};
    const uint8_t* body = nullptr;
    size_t body_len = 0;
    if (!cp_parse(cpdu, len, &hdr, &body, &body_len)) return;
    switch (hdr.type) {
        case kCpData:  handle_cp_data(body, body_len); break;
        case kCpAck:   handle_cp_ack(); break;
        case kCpError: handle_cp_error(hdr.cause); break;
        default: break;
    }
}

void SmsStack::tick(uint32_t now_ms) {
    now_ms_ = now_ms;
    if (timer_armed_ && now_ms >= timer_deadline_) {
        // TR1M expiry -> +CMS ERROR (SPEC value; stock timer id 0x2d1, value not
        // extracted [V presence, SPEC duration])
        timer_armed_ = false;
        if (mo_state_ == MoState::WaitRpAck) {
            StackEvent ev{};
            ev.id = EventId::SendErr;
            fire(ev);
            mo_state_ = MoState::Idle;
        }
    }
}

} // namespace sms
} // namespace f286
