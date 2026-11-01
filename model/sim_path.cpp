// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sim_path.cpp -- implementation, see sim_path.h for grounding notes.
// Confidence tags: [V] = transcribed from disassembled stock
// instructions (VA cited), [I] = offsets/flow real, semantics inferred or
// spec-derived (3GPP TS 51.011 / ISO 7816-3) where stock does the same thing
// one layer up in sim_uicc/mnc_sim (no byte-level pin needed for the port).

#include "sim_path.h"

#include <cstring>

namespace f286 {
namespace sim {

namespace {

constexpr u32 base_of(int card) { return card == 0 ? USIM0_BASE : USIM1_BASE; }

// ATR wire-complement classification, 0x80c6081e case 2 [V]:
//   0x3B (';'), 0x3F ('?')          -> direct/inverse standard forms
//   0xC4 (~0x3B), 0xC0 (~0x3F)      -> complement forms, class state 1
//   0xFC, 0xDC                      -> class state 2
//   0x03, 0x23 ('#')                -> class state 3
// Any other first byte: stock keeps the 300-tick ATR timer running and waits.
bool ts_classify(u8 ts, bool* direct) {
    switch (ts) {
        case 0x3B: *direct = true; return true;
        case 0x3F: *direct = false; return true;
        case 0xC4: *direct = true; return true;   // complement forms: same
        case 0xC0: *direct = false; return true;  // decode, inversion already
        case 0xFC: *direct = true; return true;   // undone by the receiver
        case 0xDC: *direct = false; return true;
        case 0x03: *direct = true; return true;
        case 0x23: *direct = true; return true;
        default: return false;
    }
}

u8 kDiTable[16] = {0, 1, 2, 4, 8, 16, 32, 64, 12, 20, 0, 16, 12, 8, 12, 0};  // [I] ISO Di

}  // namespace

// ---------------------------------------------------------------------------
// ATR parse -- clean-room ISO 7816-3 decode constrained to what stock checks:
// TS classify [V], T0 = Y1<<4|K [V], per-nibble TA/TB/TC/TD chain, historical
// K bytes, TCK xor-fold [V]. The stock driver tracks the same fields in the
// 0x2c-stride per-ctrl struct @0x8194b7b8 {+0x1c TS, +0x1d T0, +0x1e/+0x1f
// iface bytes, +0x20 extra, +0x26 idx, +0x27 hist cnt, +0x28 expected,
// +0x2a done-flag, +0x18 flags(bit1 TA1, bit2 TCK-expected)} (byte loop
// 0x80c61d14..0x80c620e0, idx==1 helper 0x80c61b90, idx==2 helper
// 0x80c63fe0, validator 0x80c61776).
// ---------------------------------------------------------------------------
bool parse_atr(const u8* buf, u32 len, Atr* out) {
    if (buf == nullptr || out == nullptr || len < 2) return false;
    Atr& a = *out;
    a = Atr{};
    a.ts = buf[0];
    if (!ts_classify(a.ts, &a.direct)) return false;

    u32 pos = 1;
    a.raw[0] = buf[0];
    for (u32 i = 1; i < len && i < sizeof(a.raw); ++i) a.raw[i] = buf[i];
    a.len = len;

    const u8 t0 = buf[pos++];
    a.y1 = static_cast<u8>(t0 >> 4);
    a.k = static_cast<u8>(t0 & 0x0f);

    bool have_td = true;  // Y nibble acts as "TD chain" level 1
    u8 y = a.y1;
    for (int lvl = 0; lvl < 4 && have_td; ++lvl) {
        if (y & 0x01) {  // TA(lvl)
            if (pos >= len) return false;
            a.ta[lvl] = buf[pos]; a.ta_present[lvl] = true; ++pos;
        }
        if (y & 0x02) {  // TB(lvl)
            if (pos >= len) return false;
            a.tb[lvl] = buf[pos]; a.tb_present[lvl] = true; ++pos;
        }
        if (y & 0x04) {  // TC(lvl)
            if (pos >= len) return false;
            a.tc[lvl] = buf[pos]; a.tc_present[lvl] = true; ++pos;
        }
        if (y & 0x08) {  // TD(lvl)
            if (pos >= len) return false;
            a.td[lvl] = buf[pos]; a.td_present[lvl] = true; ++pos;
            if (lvl == 0) a.t_protocol = static_cast<u8>(a.td[0] & 0x0f);
            y = static_cast<u8>(a.td[lvl] >> 4);  // next nibble group
            have_td = true;
        } else {
            have_td = false;
        }
    }

    // TA1 -> Fi/Di (stock stores selectors to 0x81710360/0x81710370 from TA1
    // in 0x80c6081e case 3, then ETU = kFdTable[sel]*0x174).
    if (a.ta_present[0]) {
        const u8 ta1 = a.ta[0];
        const u16 fi_idx = static_cast<u16>(ta1 >> 4);  // 0..15
        // kFdTable[8+i] = Fi(i) for i=0..13 (372,372,558,744,1116,1488,1860,
        // RFU,RFU,512,768,1024,1536,2048); RFU(0) -> classic 372.
        const u16 fi = (fi_idx <= 13) ? kFdTable[8 + fi_idx] : 0;
        a.fi = (fi != 0) ? fi : 372;
        a.di = kDiTable[ta1 & 0x0f];
    }

    // Historical bytes.
    for (u32 i = 0; i < a.k && pos < len; ++i) {
        if (a.historical_len < sizeof(a.historical)) {
            a.historical[a.historical_len++] = buf[pos];
        }
        ++pos;
    }

    // TCK: present iff any TD(i) declares T>0 (ISO 7816-3; stock expects it
    // when its T>0 flag is set -- 0x80c63fe0 sets atr_ctx+0x18 bit2).
    if (a.t_protocol != 0) {
        if (pos >= len) return false;
        a.tck = buf[pos]; a.tck_present = true; ++pos;
    }
    a.tck_ok = true;
    if (a.tck_present) {
        u8 x = 0;
        for (u32 i = 0; i < pos - 1; ++i) x = static_cast<u8>(x ^ buf[i]);
        a.tck_ok = (x == a.tck);
    }
    (void)a.raw;  // raw kept for logging parity with stock debug dumps
    return pos <= len && a.historical_len == a.k;
}

// ---------------------------------------------------------------------------
// Activation [V unless noted] -- 0x80c61358 (init), 0x80c5ded4+0x80c5faa6
// (power-down clear: CTRL bits 0x200/0x40, 50*AON-value or 0x2134 spin, AON
// helper off, CLK &= ~0x800), 0x80c63dd6 (pads), 0x80c5df70 (AON helper),
// 0x80c5fd88 (reset).
// ---------------------------------------------------------------------------
void power_down_clear(IUsimHw& hw, int card) {
    const u32 b = base_of(card);
    u32 v = hw.read32(b + reg::CTRL);
    v &= ~0x240u;                    // clear bit9|bit10 (stock 0x200/0x40 pair)
    hw.write32(b + reg::CTRL, v);
    for (u32 i = 0; i < 0x50; ++i) hw.spin(0x50);  // stock 50-iteration idiom
    u32 c = hw.read32(b + reg::CLK);
    c &= ~0x800u;
    hw.write32(b + reg::CLK, c);
}

void pads_enable(IUsimHw& hw, int card) {
    // FUN @0x80c63dd6: card0 -> [0x71301000]=0x800000 else [0x71302000]=0x800000
    hw.write32(PAD_CFG[card == 0 ? 0 : 1], 0x800000u);  // [[inferred]] USIM pad mux/pull
}

void aon_enable(IUsimHw& hw, int card) {
    // Helper 0x80c5df70: logical ctrl -> 0x402e1540-family SET alias; the
    // on-path writes 0x20 (dispatch 0x80c5df88..0x80c5e008; literals
    // 0x402e1540 @0x80c5e40c, 0x402e2540 @0x80c5e420 = the CLR twin, used on
    // the off path). Card1
    // must write 0x402e1544 (PUBCP_SIM2_TOP_CTRL SET) -- the old single
    // array wrote 0x402e2540 for card1, i.e. it CLEARED SIM1's bit instead
    // of enabling card1. 0x20 = b5 cp_simX_clk_pl (SIM clock polarity), the "control signal" SIM_CTL1 defers to when CLK_PL_SEL=0.
    hw.write32(AON_SIM_CTRL_SET[card == 0 ? 0 : 1], 0x20u);
}

void vddsim_enable(IUsimHw& hw) {
    // Stock NEVER does this (zero-xref ldo print @0x80c5db54). FPLinux must.
    u16 v = hw.adi_read(ADI_VDDSIM0_EN);
    hw.adi_write(ADI_VDDSIM0_EN, static_cast<u16>(v | 1u));
    // SEL: idx0=1.8V idx2=3.0V per class-B/C card. Leave
    // at OTP default here; a port may force 3.0V for class C cards: sel=2. [I]
}

void controller_init(IUsimHw& hw, int card) {
    const u32 b = base_of(card);
    hw.write32(b + reg::INT_EN, 0);
    hw.write32(b + reg::CTRL, 0x8000u);
    hw.write32(b + reg::CLK, 0x200u);        // 26 MHz (0x80c63e1e returns 26000000)
    hw.write32(b + reg::IRQ_EN, 1u);
    hw.write32(b + reg::ETU_TX, 0);
    hw.write32(b + reg::ETU_RX, 0);
    hw.write32(b + reg::RST_DLY, 0);
    hw.write32(b + reg::INIT1, 0);
    hw.write32(b + reg::INIT0, 0);
}

void card_reset(IUsimHw& hw, int card, bool warm) {
    const u32 b = base_of(card);
    hw.write32(b + reg::EVT_CLR, 0xffffu);
    hw.write32(b + reg::RST_DLY, warm ? 0xccu : 0u);
    // Activation CTRL write: stock 0x80c632dc writes 0x2003 (bits 0,1,13)
    // directly to +0x18 (0x80C633E0-E8: movw r2,#0x2003; str [r3,#0x18]).
    hw.write32(b + reg::CTRL, 0x2003u);
    // ETU reload from the F/D selector stock keeps per card (0x8194b7a8[]):
    // default classic T=0. divisor*Fi = 2*372 = 744
    // (spec default etu_dvdr); the old kFdTable[9]*372 overflowed the
    // [15:0] divider to 0x1C90. Warm path: stock also re-arms +0x24=0xcc above.
    const u32 rl = etu_reload(kDefaultDivisorIndex, 1);  // divisor 2 x Fi 372 = 744
    hw.write32(b + reg::ETU_TX, rl);
    hw.write32(b + reg::ETU_RX, rl);
    // IRQ_EN: stock writes 1 (0x80C63364) then |= 0x101 (0x80C63426-2E:
    // movw r2,#0x101; orrs r1,r2; str r1,[r0,#0x3c]).
    hw.write32(b + reg::IRQ_EN, 1u);
    u32 e = hw.read32(b + reg::IRQ_EN);
    e |= 0x101u;
    hw.write32(b + reg::IRQ_EN, e);
    hw.irq_enable(card);                     // IRQ 10 (card0) / 13 (card1)
}

int activate(IUsimHw& hw, int card, Atr* atr) {
    vddsim_enable(hw);       // FPLinux-only in practice (stock: OTP default)
    power_down_clear(hw, card);
    controller_init(hw, card);
    pads_enable(hw, card);
    aon_enable(hw, card);
    // cold reset -> ATR
    card_reset(hw, card, false);
    const u32 b = base_of(card);
    u8 buf[33];
    u32 n = 0;
    for (;;) {
        u32 cnt = hw.read32(b + reg::FIFO_RX_CNT) & 0x1ffu;
        while (cnt-- > 0) {
            if (n < sizeof(buf)) buf[n++] = static_cast<u8>(hw.read32(b + reg::RX));
            else (void)hw.read32(b + reg::RX);
        }
        if (n >= 2) break;  // TS+T0 seen; parser drives the rest
        if (!hw.wait_event(TicksAtr)) return -1;  // stock ATR timer = 300 ticks
    }
    // drain the remainder (T0 already tells the parser the total length)
    for (u32 guard = 0; guard < 32; ++guard) {
        u32 cnt = hw.read32(b + reg::FIFO_RX_CNT) & 0x1ffu;
        if (cnt == 0) break;
        while (cnt-- > 0) {
            if (n < sizeof(buf)) buf[n++] = static_cast<u8>(hw.read32(b + reg::RX));
            else (void)hw.read32(b + reg::RX);
        }
        if (!hw.wait_event(4)) break;
    }
    if (!parse_atr(buf, n, atr)) return -2;
    // apply negotiated F/D if TA1 present (stock 0x80c6081e case 3)
    if (atr->ta_present[0]) {
        u16 fi_idx = static_cast<u16>(atr->ta[0] >> 4);
        if (fi_idx > 13) fi_idx = 0;  // RFU nibble -> classic
        // reload = divisor*Fi. Stock's TA1-derived
        // divisor selection (0x80c5f7a4) is not fully pinned -- use the
        // default divisor row (2, the spec-default 26 MHz case) x the
        // negotiated Fi; max reload 2*2048 = 4096 fits [15:0].
        u32 rl = etu_reload(kDefaultDivisorIndex, fi_idx);
        if (kFdTable[8 + fi_idx] == 0) rl = etu_reload(kDefaultDivisorIndex, 0);  // RFU -> 744
        hw.write32(b + reg::ETU_TX, rl);
        hw.write32(b + reg::ETU_RX, rl);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// T=0 transport. Char behavior [V]: echo/retransmit/NULL-byte compare
// 0x80c620e4 (received==last-sent, ==~last-sent -> 0x80c64070 rewrite,
// ==0x60 NULL with >0x3c count guard), TX FIFO-full assert
// "SendInstrCode_T1, TX fifo full" @0x80c5e48c. ACK/SW/GET-RESPONSE semantics
// [I] TS 51.011 as built by the sim_uicc templates.
// ---------------------------------------------------------------------------
bool T0Transport::tx_bytes(const u8* p, u32 n) {
    const u32 b = base_of(card_);
    for (u32 i = 0; i < n; ++i) {
        for (u32 guard = 0; guard < 0x10000; ++guard) {
            if (((hw_.read32(b + reg::FIFO_TX_CNT) >> 0) & 0x1ffu) < 0x100u) break;
            hw_.spin(1);
            if (guard == 0xffff) return false;  // stock asserts TX fifo full
        }
        hw_.write32(b + reg::TX, p[i]);
    }
    return true;
}

int T0Transport::rx_byte(u8* out) {
    const u32 b = base_of(card_);
    for (u32 guard = 0; guard < TicksWaitResp; ++guard) {
        if ((hw_.read32(b + reg::FIFO_RX_CNT) & 0x1ffu) != 0) {
            *out = static_cast<u8>(hw_.read32(b + reg::RX));
            return 0;
        }
        if (!hw_.wait_event(1)) return -1;
    }
    return -1;
}

bool T0Transport::xfer(const T0Apdu& a, u8* out, u32 out_cap, u32* out_len, u16* sw) {
    u8 hdr[5] = {a.cla, a.ins, a.p1, a.p2, a.p3};
    if (!tx_bytes(hdr, 5)) return false;
    u32 sent = 0;
    // Phase 1: command-data direction, driven by procedure bytes [V for the
    // NULL/retransmit rules; ACK triage per TS 51.011 as built by sim_uicc].
    while (sent < a.tx_len) {
        u8 pb = 0;
        if (rx_byte(&pb) != 0) return false;
        if (pb == 0x60) continue;                       // NULL byte: keep waiting
        if (pb == 0x6C) {                               // wrong P3: retry w/ SW2
            u8 sw2 = 0;
            if (rx_byte(&sw2) != 0) return false;
            hdr[4] = sw2;
            if (!tx_bytes(hdr, 5)) return false;
            continue;
        }
        if (pb == a.ins) {                              // ACK all remaining
            if (!tx_bytes(a.data + sent, a.tx_len - sent)) return false;
            sent = a.tx_len;
            break;
        }
        if (pb == static_cast<u8>(~a.ins)) break;       // ACK none: go receive
        if (pb == static_cast<u8>(a.ins + 1) || pb == static_cast<u8>(a.ins - 1)) {
            hw_.write32(base_of(card_) + reg::TX, a.data[sent++]);  // ACK one
            continue;
        }
        return false;  // unexpected procedure byte during command data
    }
    // Phase 2: response. SIM T=0 convention (TS 51.011): a command that sent
    // data (SELECT) answers with SW only (often 0x9Fxx/0x61xx = GET RESPONSE
    // size); a command with empty data field (READ BINARY / GET RESPONSE)
    // answers with P3 data bytes followed by SW 0x9000.
    u32 recvd = 0;
    const u32 expect_data = (sent == 0 && a.tx_len == 0) ? a.p3 : 0;
    for (;;) {
        u8 byte = 0;
        if (rx_byte(&byte) != 0) return false;
        if (recvd < out_cap) out[recvd++] = byte;
        if (recvd >= expect_data + 2) {  // data bytes + SW1 + SW2 collected
            if (recvd - 2 >= out_cap) return false;
            *sw = static_cast<u16>((out[recvd - 2] << 8) | out[recvd - 1]);
            *out_len = recvd - 2;
            return true;
        }
    }
}

bool T0Transport::get_response(u8 want, u8* out, u32 out_cap, u32* out_len, u16* sw) {
    T0Apdu gr{0xA0, 0xC0, 0, 0, want, nullptr, 0, want};
    return xfer(gr, out, out_cap, out_len, sw);
}

bool T0Transport::select(u16 file_id, u8* fci, u32 fci_cap, u32* fci_len) {
    u8 data[2] = {static_cast<u8>(file_id >> 8), static_cast<u8>(file_id & 0xff)};
    T0Apdu a{0xA0, 0xA4, 0x00, 0x00, 2, data, 2, 0};  // P3=2: send FID, recv SW
    u16 sw = 0;
    u32 n = 0;
    if (!xfer(a, fci, fci_cap, &n, &sw)) return false;
    // 9Fxx/61xx: xx bytes of FCI available via GET RESPONSE
    if ((sw >> 8) == 0x9Fu || (sw >> 8) == 0x61u) {
        const u8 want = static_cast<u8>(sw & 0xff);
        u16 sw2 = 0;
        u32 got = 0;
        if (want != 0 && fci != nullptr && fci_cap > 0) {
            if (!get_response(want, fci, fci_cap, &got, &sw2)) return false;
        }
        if (fci_len) *fci_len = got;
        return sw2 == 0x9000;
    }
    if (fci_len) *fci_len = n;
    return sw == 0x9000;
}

bool T0Transport::read_binary(u32 offset, u8* out, u32 len) {
    u16 sw = 0;
    u32 n = 0;
    T0Apdu a{0xA0, 0xB0, static_cast<u8>((offset >> 8) & 0x7f),
             static_cast<u8>(offset & 0xff), static_cast<u8>(len), nullptr, 0, len};
    if (!xfer(a, out, len, &n, &sw)) return false;
    if ((sw >> 8) == 0x9Fu || (sw >> 8) == 0x61u) {
        u16 sw2 = 0;
        u32 got = 0;
        if (!get_response(static_cast<u8>(sw & 0xff), out, len, &got, &sw2)) return false;
        n = got;
        sw = sw2;
    }
    return sw == 0x9000 && n == len;
}

// ---------------------------------------------------------------------------
// CPIN chain [I -- byte-level APDUs are TS 51.011; stock builds the same in
// sim_uicc from the EF table @0x8154b560 (0x34-byte rows: ICCID 0x2FE2 path
// {3F00} type5 10B; IMSI 0x6F07 path {3F00,7F20})]. No PIN, no USIML3, no
// AUTH -- CPIN?/CIMI/CCID need only transparent reads.
// ---------------------------------------------------------------------------
void decode_bcd_digits(const u8* raw, u32 n, char* out, u32 out_cap) {
    u32 o = 0;
    for (u32 i = 0; i < n && o + 1 < out_cap; ++i) {
        const u8 lo = raw[i] & 0x0f, hi = (raw[i] >> 4) & 0x0f;
        if (lo <= 9) out[o++] = static_cast<char>('0' + lo);
        if (hi <= 9) out[o++] = static_cast<char>('0' + hi);
    }
    out[o] = '\0';
}

// EF_IMSI semi-octet decode, TS 51.011 section 10.3.2 (fix):
// body[0] = digit count; body[1] LOW nibble = parity digit (skipped, not part
// of the IMSI); IMSI digit 1 = body[1] HIGH nibble; then low/high nibbles of
// body[2..]. Max 15 digits. pySim vector 08 29 82 60 82 00 00 20 80 ->
// 228062800000208 (unit-tested in sms_selftest.cpp #11).
void decode_imsi_digits(const u8* body, u32 len, char* out, u32 out_cap) {
    u32 o = 0;
    if (len >= 2) {
        const u8 d1 = static_cast<u8>((body[1] >> 4) & 0x0f);
        if (d1 <= 9 && o + 1 < out_cap) out[o++] = static_cast<char>('0' + d1);
    }
    for (u32 i = 2; i < len && o + 1 < out_cap; ++i) {
        const u8 lo = body[i] & 0x0f, hi = (body[i] >> 4) & 0x0f;
        if (lo <= 9 && o + 1 < out_cap) out[o++] = static_cast<char>('0' + lo);
        if (hi <= 9 && o + 1 < out_cap) out[o++] = static_cast<char>('0' + hi);
    }
    out[o] = '\0';
}

int read_identity(IUsimHw& hw, SimIdentity* id) {
    if (id == nullptr) return -3;
    *id = SimIdentity{};
    Atr atr;
    const int rc = activate(hw, 0, &atr);
    if (rc != 0) return rc;
    id->atr_ok = true;
    id->present = true;  // a card that ATRs is present (stock has no CD line;
                         //  detect = ATR-timeout/card-status events, INFERRED)

    T0Transport t0(hw, 0);
    u8 buf[64];
    u32 n = 0;

    // SELECT MF 3F00 -> SELECT DF_TELECOM 7F20 -> SELECT EF_IMSI 6F07
    if (!t0.select(0x3F00u, buf, sizeof(buf), &n)) return -4;
    if (!t0.select(0x7F20u, buf, sizeof(buf), &n)) return -5;
    if (!t0.select(0x6F07u, buf, sizeof(buf), &n)) return -6;
    // READ BINARY 9 bytes: [0]=digit count, [1] hi nibble = digit1 (lo nibble =
    // parity digit, skipped), [2..] two BCD digits each (TS 51.011 section 10.3.2).
    if (!t0.read_binary(0, buf, 9)) return -7;
    decode_imsi_digits(buf, 9, id->imsi, sizeof(id->imsi));

    // SELECT MF 3F00 -> SELECT EF_ICCID 2FE2 -> READ BINARY 10
    if (!t0.select(0x3F00u, buf, sizeof(buf), &n)) return -8;
    if (!t0.select(0x2FE2u, buf, sizeof(buf), &n)) return -9;
    if (!t0.read_binary(0, buf, 10)) return -10;
    decode_bcd_digits(buf, 10, id->iccid, sizeof(id->iccid));
    return 0;
}

}  // namespace sim
}  // namespace f286
