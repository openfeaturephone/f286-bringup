// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// pmu.cpp -- PMU_APB domain model, transcribed from the stock kernel.
// Every commented VA is a stock instruction address (Thumb), file_off = VA - 0x80a05e00.
#include "pmu.h"

namespace f286 {
namespace power {

// 32 kHz "tick" waits inside the stock polls are realized as raw iteration loops
// (e.g. 0x80D0EB78 counts to 0xF4240 = 1e6 with no sleep), so bus.spin is only
// used for the fixed 0x50-settle loops, never here.

// stock: 0x80D0FA7C (r0 = 1 on the cold-boot path, call site 0x80E2D890/0x80E2D892)
Confidence lte_preboot_power_release(PmuBus& bus, bool release) {
    // 0x80D0FA7C  mov.w r2, #0x02000000          ; bit25 mask
    // 0x80D0FA80  ldr   r1, =0x402B2000          ; pool literal: CLEAR bank base
    // 0x80D0FA82  asrs  r3, r2, #1               ; 0x01000000 = bit24 mask
    constexpr u32 BIT25 = 0x0200'0000u;
    constexpr u32 BIT24 = 0x0100'0000u;
    if (release) {
        // 0x80D0FA86  str r2, [r1, #0x44]   -> 0x402b2044 = 0x02000000
        //   (CLEAR alias of 0x402b0044 bit25)
        bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), BIT25);
        // 0x80D0FA88  str r3, [r1, #0x44]   -> 0x402b2044 = 0x01000000
        //   (CLEAR alias of 0x402b0044 bit24; the two writes clear bits 25:24)
        bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), BIT24);
        // 0x80D0FA8A  asrs r0, r2, #8       -> 0x00020000 (bit17)
        // 0x80D0FA8C  str  r0, [r1, #0xcc]  -> 0x402b20cc = 0x00020000
        //   (CLEAR alias of 0x402b00cc bit17 = shared DSP deep-sleep force)
        bus.write32(clr_bank(PMU_APB_BASE + DSP_SLP_FORCE), BIT25 >> 8);
        // 0x80D0FA90  mov.w r0, #0x03000000
        // 0x80D0FA94  str   r0, [r1, #0x34] -> 0x402b2034 = 0x03000000
        //   (CLEAR alias of 0x402b0034 bits 25:24 = XDSP "A"/LTE core power release.
        //    THE root-cause fix; live-proven 2026-10-23)
        bus.write32(clr_bank(PMU_APB_BASE + REQ_XDSP_AB), BIT25 | BIT24);
        return Confidence::Verified;
    }
    // Sleep/hold arm (r0 == 0): 0x80D0FA98 ldr r0, =0x402B1000
    //   0x80D0FA9A  str r2, [r0, #0x34] -> 0x402b1034 = 0x02000000 (SET bit25)
    //   0x80D0FA9C  str r3, [r1, #0x34] -> 0x402b2034 = 0x01000000 (CLEAR bit24)
    // Net effect of the mixed SET/CLEAR pair: open question .
    bus.write32(set_bank(PMU_APB_BASE + REQ_XDSP_AB), BIT25);
    bus.write32(clr_bank(PMU_APB_BASE + REQ_XDSP_AB), BIT24);
    return Confidence::Verified;
}

// stock: 0x80D0FA54. No BL/BLX/pool callers found  (blscan over the
// whole payload) -- likely the GSM twin of 0x80D0FA7C, superseded by the HAL.
Confidence gsm_preboot_power_release(PmuBus& bus, bool release) {
    // 0x80D0FA54  ldr r1, =0x402B2000 ; 0x80D0FA56 mov.w r2, #0x02000000
    constexpr u32 BIT25 = 0x0200'0000u;
    constexpr u32 BIT24 = 0x0100'0000u;
    if (release) {
        // 0x80D0FA5C  str r2, [r1, #0x44] -> 0x402b2044 = 0x02000000 (clear +0x44 bit25)
        bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), BIT25);
        // 0x80D0FA5E  movs r0, #5 ; 0x80D0FA60 str r0, [r1, #0xb0]
        //   -> 0x402b20b0 = 5 (CLEAR bank of 0x402b00b0: bits 0 and 2)
        bus.write32(clr_bank(PMU_APB_BASE + REG_00B0), 5u);
        // 0x80D0FA64/66 -> 0x402b20cc = 0x00020000 (clear DSP deep-sleep force bit17)
        bus.write32(clr_bank(PMU_APB_BASE + DSP_SLP_FORCE), BIT25 >> 8);
        // 0x80D0FA6A  mov.w r0, #0x03000000 ; falls into the common tail:
        // 0x80D0FA78  str r0, [r1, #0x38] -> 0x402b2038 = 0x03000000
        //   (CLEAR alias of 0x402b0038 bits 25:24 = XDSP "B"/GSM core release)
        bus.write32(clr_bank(PMU_APB_BASE + REQ_0038), BIT25 | BIT24);
        return Confidence::Verified;
    }
    // r0 == 0 arm: 0x80D0FA70 ldr r0, =0x402B1000
    //   0x80D0FA72 str r2, [r0, #0x38] -> 0x402b1038 = 0x02000000 (SET bit25)
    //   0x80D0FA74 mov.w r0, #0x01000000; 0x80D0FA78 str r0, [r1, #0x38]
    //     -> 0x402b2038 = 0x01000000 (CLEAR bit24 ONLY -
    //     0x03000000 is the release-path tail value 0x80D0FA6A).
    bus.write32(set_bank(PMU_APB_BASE + REQ_0038), BIT25);
    bus.write32(clr_bank(PMU_APB_BASE + REQ_0038), BIT24);
    return Confidence::Verified;
}

// stock: 0x80D0FAA0. Sole caller BL @ 0x80C2F422 (OS power task).
Confidence full_radio_release_sweep(PmuBus& bus) {
    // 0x80D0FAA0 ldr r0, =0x402B2000 ; 0x80D0FAA2 mov.w r1, #0x02000000
    constexpr u32 BIT25 = 0x0200'0000u;
    constexpr u32 BIT24 = 0x0100'0000u;
    // 0x80D0FAA6  str r1, [r0, #0x44]  -> clear +0x44 bit25
    bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), BIT25);
    // 0x80D0FAA8  movs r2, #5 ; 0x80D0FAAA str r2, [r0, #0xb0] -> clear +0xb0 bits 0,2
    bus.write32(clr_bank(PMU_APB_BASE + REG_00B0), 5u);
    // 0x80D0FAAE/B0 -> clear +0xcc bit17 (DSP deep-sleep force)
    bus.write32(clr_bank(PMU_APB_BASE + DSP_SLP_FORCE), BIT25 >> 8);
    // 0x80D0FAB4/B8 -> +0x38 = 0x03000000 (both XDSP-B bits cleared)
    bus.write32(clr_bank(PMU_APB_BASE + REQ_0038), BIT25 | BIT24);
    // The rest of the sweep drives ONE register pair each as SET-bit25 then
    // CLEAR-bit24 (clear is last and wins). Registers in stock order:
    //   +0x34 (0x80D0FABA..C0), +0x28 (0x80D0FAC2..C4), +0x2c (0x80D0FAC6..C8),
    //   +0x4c (0x80D0FACA..CC), +0x50 (0x80D0FACE..D0), +0x3c (0x80D0FAD2..D4).
    // NOTE: this corrects ("sweeps ... the same
    // way"): +0x38 is the only pure double-clear; the others are mixed SET/CLEAR.
    const u32 regs[] = {REQ_XDSP_AB, REQ_0028, REQ_002C, 0x4Cu, 0x50u, REQ_WHU3G};
    for (const u32 off : regs) {
        bus.write32(set_bank(PMU_APB_BASE + off), BIT25);
        bus.write32(clr_bank(PMU_APB_BASE + off), BIT24);
    }
    return Confidence::Verified;
}

// Per-DSP release table baked into stock init data (decompressed init data
// image @ VA 0x81705E94):
//   id 0: reg 0x402B0034, mask 0xFCFFFFFF   (HAL writes ~mask = 0x03000000)
//   id 2: reg 0x402B0038, mask 0xFCFFFFFF
// (id 1 entry is zeroed; stock rejects `(1<<id) & ~5 != 0` at 0x811D0D9E.)
struct XdspEntry {
    u32 req_reg;
    u32 status_mask;  // STATUS0 field to poll
};
constexpr XdspEntry kXdsp[3] = {
    {PMU_APB_BASE + REQ_XDSP_AB, Status0::XDSP_LTE},  // id 0
    {0, 0},                                           // id 1 unused
    {PMU_APB_BASE + REQ_0038, Status0::XDSP_GSM},     // id 2
};

// stock: 0x811D0D96 (veneer 0x811A77DA; callers 0x8124C42A = DSP log pump, and the
// LTE/GSM boot tails). "ready to boot" literally means the status field == 0.
Confidence xdsp_power_release_wait(PmuBus& bus, u32 id) {
    if (id >= 3 || id == 1) return Confidence::Verified;  // stock silently ignores
    const XdspEntry& e = kXdsp[id];
    // 0x811D0DAE  ldr r5, [r2]          ; read current value of the request register
    // 0x811D0DB8  str r5, [tbl + id*12 + 8]   ; stock saves it for the sleep path
    // 0x811D0DBC  ldr r3, [tbl + id*12 + 4]   ; mask = 0xFCFFFFFF
    // 0x811D0DBE  mvns r3, r3                 ; 0x03000000
    // 0x811D0DC0  ldr r2, =0x402B2000 ... str r3, [r2 + (reg & 0xFFF)]
    //   -> CLEAR-bank write of ~mask to the id's request register.
    const u32 base = e.req_reg & ~0xFFFu;
    bus.write32(clr_bank(e.req_reg), ~0xFCFF'FFFFu);
    // 0x811D0DC2/C6 -> 0x402b2044 = 0x03000000 (clear WTLCP +0x44 bits 25:24 too)
    bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0300'0000u);
    // 0x811D0DCE..F8: poll up to 0x1000 tries:
    //   id 0 -> (STATUS0 & 0x03e0) == 0 ; id 2 -> (STATUS0 & 0x7c00) == 0
    for (u32 i = 0; i < 0x1000u; ++i) {
        if (field(bus.read32(PMU_APB_BASE + STATUS0), e.status_mask) == FIELD_ON) break;
    }
    // 0x811D0DFA/FE -> [0x402E1144] = 0x1A   (AON2 +0x144 reset-clear bits 1,3,4)
    bus.write32(0x402E'1144u, 0x1Au);
    // 0x811D0E02/06 -> [0x60831044] = 0x40   (whale DSP interrupt enable)
    bus.write32(0x6083'1044u, 0x40u);
    // 0x811D0E08..0E: 0x50-iteration settle loop
    bus.spin(0x50);
    (void)base;
    return Confidence::Verified;
}

// stock: 0x811D0D54 (SET when arg!=0 via bank1, CLEAR when arg==0 via bank2).
// A byte-identical twin exists at 0x811D0D66.
Confidence dsp_deep_sleep_force(PmuBus& bus, bool force) {
    // 0x811D0D54 mov.w r1, #0x20000
    // 0x811D0D5A ldr r0, =0x402B1000 (force) / 0x811D0D5E =0x402B2000 (clear)
    // 0x811D0D60 str r1, [r0, #0xcc]
    bus.write32((force ? set_bank : clr_bank)(PMU_APB_BASE + DSP_SLP_FORCE), 0x0002'0000u);
    return Confidence::Verified;
}

// stock: 0x80c2ee9c (CM4 boot), write verified by.
Confidence cm4_force_clear(PmuBus& bus) {
    bus.write32(clr_bank(PMU_APB_BASE + DSP_SLP_FORCE), 0x0020'0000u);  // bit21
    return Confidence::Inferred;  // not re-disassembled 
}

// -- polling ------------------------------------------------------------------

namespace {
// Triple-read stability idiom used by every stock poll (e.g. 0x80D0EDCA..F0):
// read the field three times, require all equal AND equal to `want`.
template <typename F>
bool stable3(F&& read_field, u32 want, u32 max_iters, u32& counter) {
    for (;;) {
        const u32 a = read_field();
        const u32 b = read_field();
        const u32 c = read_field();
        ++counter;
        if (a == b && a == c && a == want) return true;
        if (counter > max_iters) return false;
    }
}
}  // namespace

// stock: 0x80D0ED6A(0) [poll arm] and 0x80D0ED18 [no-trace variant used by 0x80D0F186].
Confidence wait_whu3g_on(PmuBus& bus, u32 max_iters) {
    u32 n = 0;
    stable3([&] { return field(bus.read32(PMU_APB_BASE + STATUS0), Status0::WHU3G); },
            FIELD_ON, max_iters, n);
    return Confidence::Verified;  // poll loop stock-exact; success/failure is caller-logged
}

// stock: 0x80D0EE0E (power-DOWN poll; success == field stable 7).
Confidence wait_whu3g_off(PmuBus& bus, u32 max_iters) {
    u32 n = 0;
    stable3([&] { return field(bus.read32(PMU_APB_BASE + STATUS0), Status0::WHU3G); },
            FIELD_OFF, max_iters, n);
    return Confidence::Verified;
}

// stock: 0x80D0EB78 (poll) + 0x80D0EC50 (force-retry) + 0x80D0EAD4 (re-poll).
Confidence wait_wtlcp_ready(PmuBus& bus, u32 whale_apb_base, u32 max_iters) {
    u32 n = 0;
    // 0x80D0EBA0..EC12: loop with triple-read of both fields, counter at [fp+0xC].
    u32 c4_last = 0;
    bool ok = false;
    for (;;) {
        ok = stable3(
            [&] { return field(bus.read32(PMU_APB_BASE + STATUS1), 0x0000'001Fu); },
            FIELD_ON, max_iters, n);
        // (The d4==6 check rides inside stock's combined condition; modeled here.)
        const u32 d4 = field(bus.read32(PMU_APB_BASE + WTLCP_STATE), 0x0000'00F0u);
        c4_last = field(bus.read32(PMU_APB_BASE + STATUS1), 0x0000'001Fu);
        if (ok && d4 == 6u) break;
        if (n > max_iters) break;
    }
    if (ok) {
        // 0x80D0EB42..: success side effect: [0x60830044] |= 0x80 (via 0x80D0E90C),
        // 0x50 spin, a read of [0x5150D000+0x18] (NEW), 0x50 spin.
        rmw_set(bus, whale_apb_base + 0x44u, 0x80u);
        bus.spin(0x50);
        bus.read32(0x5150'D000u + 0x18u);
        bus.spin(0x50);
        return Confidence::Verified;
    }
    // 0x80D0EC22: if the last c4 sample was 7 -> one FORCE retry:
    if (c4_last == FIELD_OFF) {
        // 0x80D0EC50 ldr r0, =0x402B2000 ; 0x80D0EC56 str 0x02000000 -> clear +0x44 bit25
        bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0200'0000u);
        // 0x80D0EC58/5A str 0x01000000 -> clear +0x44 bit24
        bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0100'0000u);
        // 0x80D0EC5C..64: [0x60830044] |= 0x40 (re-assert whale WTLCP power request)
        rmw_set(bus, whale_apb_base + 0x44u, 0x40u);
        // 0x80D0EC66 bl 0x80D0EAD4  (one more bounded re-poll; result only logged)
        u32 n2 = 0;
        stable3([&] { return field(bus.read32(PMU_APB_BASE + STATUS1), 0x0000'001Fu); },
                FIELD_ON, 1'000'000u, n2);
    }
    return Confidence::Verified;  // stock returns regardless; caller logs
}

// stock: 0x81602040 -- decoded instruction-exact (pointer-invoked from the
// OS power task).
Confidence sleep_wake_dsp_repower(PmuBus& bus, u32 whale_apb_base) {
    // 0x81602048 str.w r2, [r1, #0xcc] -> 0x402b20cc = 0x00020000 (bit17 clear)
    bus.write32(clr_bank(PMU_APB_BASE + DSP_SLP_FORCE), 0x0002'0000u);
    // 0x8160204c lsls r2, r0, #9 (0x10000 << 9 = 0x02000000)
    // 0x8160204e str   r2, [r1, #0x44] -> 0x402b2044 = 0x02000000 (WTLCP bit25)
    bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0200'0000u);
    // 0x81602050 lsls r2, r0, #8 (0x01000000); 0x81602052 str -> WTLCP bit24.
    // The standard bit25/bit24 WTLCP release pair, once each.
    bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0100'0000u);
    // 0x81602054/56 subs r0,#1 / bhs: pure 0x10000-iteration countdown -
    // a silent delay, NO bus traffic.
    for (volatile u32 i = 0; i < 0x10000u; ++i) {}
    // 0x8160205a orr r1, r1, #0x40 -> [0x60830044] |= 0x40
    rmw_set(bus, whale_apb_base + 0x44u, 0x40u);                        // whale power request
    return Confidence::Verified;
}

// stock: 0x81602064 -- decoded instruction-exact. 0x20E01000 = AP-AHB syscons (pool 0x816023F0).
Confidence ap_sleep_force_domains(PmuBus& bus) {
    // 0x8160206a str 0x1803, [r0, #0x10] -> [0x20E01010]
    bus.write32(0x20E0'1010u, 0x1803u);
    // 0x81602070-88 str 0x20000 -> [0x20E01088] AND [0x20E0108C..0x20E010A0]
    // (SEVEN words)
    for (u32 a = 0x20E0'1088u; a <= 0x20E0'10A0u; a += 4u) bus.write32(a, 0x0002'0000u);
    // 0x8160208e str 1, [r0, #0xc] -> [0x20E0100C]
    bus.write32(0x20E0'100Cu, 1u);
    // 0x81602094 str 0x10000 -> [0x402E1000+0x140] = [0x402E1140]
    bus.write32(0x402E'1140u, 0x0001'0000u);
    // 0x8160209c str 3 -> [0x402E1524] (was missing)
    bus.write32(0x402E'1524u, 3u);
    // 0x816020a4 str 0x1F0000 -> [0x402B1000+0x230] = [0x402b1230] (SET bank)
    bus.write32(set_bank(PMU_APB_BASE + 0x230u), 0x001F'0000u);
    // 0x816020ac bic [0xE4DC], #1 -- data-cell word; movw/movt base not pinned
    // by the decode, omitted.
    return Confidence::Verified;
}

}  // namespace power
}  // namespace f286
