// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// whale.cpp -- whale-die power sequencing, transcribed from the stock kernel.
// Comment VAs are stock instruction addresses; file_off = VA - 0x80a05e00.
#include "whale.h"

#include "pmu.h"

namespace f286 {
namespace power {

// stock: 0x80D0EFE4
Confidence whale_bridge_enable(PmuBus& bus) {
    // 0x80D0F002 ldr r1, =0x402E1000 ; 0x80D0F004 movs r0, #1
    // 0x80D0F006 str r0, [r1, #0xb0]   -> [0x402E10B0] = 1 (AON2 whale bridge)
    bus.write32(AON_APB_BASE + 0x1000u + 0xB0u, 1u);
    // 0x80D0F016..1E: 0x50-iteration spin
    bus.spin(0x50);
    return Confidence::Verified;
}

// stock: 0x80D0F0EE (first half) + 0x80D0F146 (adds the final release write).
Confidence die_clk_config(PmuBus& bus) {
    // --- 0x80D0F0EE ---
    // 0x80D0F0EE ldr r1, =0x60831000 ; 0x80D0F0F0 movs r0, #0x80
    // 0x80D0F0F4 str r0, [r1, #0x44]  -> [0x60831044] = 0x80 (whale module enable;
    //             this page is AP-REACHABLE, live-proven)
    bus.write32(WHALE_APB_BASE + 0x1044u, 0x80u);
    bus.spin(0x50);                              // 0x80D0F0F6..FE
    (void)bus.read32(WHALE_AON_BASE + 0x28u);    // 0x80D0F102 rd [0xD15E0028]  [[UNREACHABLE]]
    // 0x80D0F104 movw r6, #0x380 ; 0x80D0F108 ldr r0, =0xD15E1000
    // 0x80D0F10A str r6, [r0, #0x28]  -> [0xD15E1028] = 0x380: SET bank of reg +0x28
    //             (bits 9:7 = three clock enables). [[AP-UNREACHABLE: live bus hang]]
    bus.write32(WHALE_AON_BASE + 0x1000u + 0x28u, 0x380u);
    (void)bus.read32(WHALE_AON_BASE + 0x28u);    // 0x80D0F10C rd-back
    (void)bus.read32(WHALE_AON_BASE);            // 0x80D0F126 rd [0xD15E0000]
    // 0x80D0F128 ldr r0, =0xD15E2000 ; 0x80D0F12A str r6, [r0]
    //             -> [0xD15E2000] = 0x380: CLEAR bank of reg +0x00, bits 9:7.
    bus.write32(WHALE_AON_BASE + 0x2000u, 0x380u);
    (void)bus.read32(WHALE_AON_BASE);            // 0x80D0F12C rd-back
    // --- 0x80D0F146 ---
    bus.write32(WHALE_APB_BASE + 0x1044u, 0x80u);  // 0x80D0F14C..50 (again)
    bus.spin(0x50);                                // 0x80D0F152..5A
    (void)bus.read32(WHALE_AON_BASE);              // 0x80D0F15E
    // 0x80D0F160 mov.w r1, #0x300000 ; 0x80D0F166 str r1, [r2] (r2 = 0xD15E2000)
    //             -> CLEAR bit21 of reg +0x00: the last-cleared clock/release bit.
    bus.write32(WHALE_AON_BASE + 0x2000u, 0x0030'0000u);
    (void)bus.read32(WHALE_AON_BASE);              // 0x80D0F168
    return Confidence::Verified;  // transcription Verified; bit semantics Inferred
}

// stock: 0x80D0F2FA+0x70..0x82 (ensure-on), 0x80D0F186+0xC2..0xD6 (full init),
// 0x80D0F202..0x0A + 0x80D0F2AA..0xB0 (RTC-wake).
Confidence light_path(PmuBus& bus) {
    // 0x80D0F370 ldr r0, =0xE0101000
    // 0x80D0F372 ldr r2, [r0, #0xc] ; 0x80D0F374 bic r2, #0x1000000
    // 0x80D0F378 str r2, [r0, #0xc]  -> [0xE010100C] &= ~0x01000000
    rmw_clr(bus, MISC_DIE_BASE + 0x100Cu, 0x0100'0000u);
    // 0x80D0F37A movs r2, #0xCC ; 0x80D0F37C str r2, [r0, #0x14]
    //             -> [0xE0101014] = 0x000000CC
    bus.write32(MISC_DIE_BASE + 0x1014u, 0xCCu);
    // 0x80D0F37E mov.w r0, #0xDD00DD ; 0x80D0F382 str r0, [r1, #0x68] (r1 = 0xD15E0000)
    //             -> [0xD15E0068] = 0x00DD00DD   [[AP-UNREACHABLE live]]
    bus.write32(WHALE_AON_BASE + 0x68u, 0x00DD'00DDu);
    return Confidence::Verified;
}

// stock: 0x80D0F2FA
Confidence whale_ensure_on(PmuBus& bus) {
    // 1: 0x80D0F304 bl 0x80D0EFE4
    whale_bridge_enable(bus);
    // 2: 0x80D0F30C..14: [0x60830044] |= 0x2000 (isolation request)
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ISOLATION);
    // 3: 0x80D0F318..2A: >= 5 ticks of the 32k counter (helper 0x80A0B552)
    bus.delay_ticks32k(5);
    // 4: 0x80D0F32C..34: [0x60830044] |= 0x40 (WTLCP power request)
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    // 5: 0x80D0F338 bl 0x80D0EB78 (poll; force-retry inside)
    wait_wtlcp_ready(bus, WHALE_APB_BASE, 1'000'000u);
    // 6: 0x80D0F33C bl 0x80D0E95C:
    //    [0x60830044] |= 0x40 ; 0x50 spin ;
    //    [0xE010102C] |= 0x2000000 ; [0xE010102C] &= ~0x1000000 ; 0x50 spin
    //    (SET bit25 first, THEN clear bit24 -- order clarified )
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    bus.spin(0x50);
    rmw_set(bus, MISC_DIE_BASE + 0x102Cu, 0x0200'0000u);
    rmw_clr(bus, MISC_DIE_BASE + 0x102Cu, 0x0100'0000u);
    bus.spin(0x50);
    // 7: 0x80D0F340..48: [0x60830044] &= ~0x2000 (isolation release)
    rmw_clr(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ISOLATION);
    // 8: 0x80D0F34C..56: 0x50 spin ; 0x80D0F358 bl 0x80D0E90C ([0x60830044] |= 0x80 + spin)
    bus.spin(0x50);
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);
    // 9: 0x80D0F35C..64: [0xD15E0058] |= 1  (first whale-AON touch) [[UNREACHABLE]]
    rmw_set(bus, WHALE_AON_BASE + 0x58u, 1u);
    // 10: 0x80D0F366..6E spin ; light path ([0xE010100C]/[0xE0101014]/[0xD15E0068])
    bus.spin(0x50);
    light_path(bus);
    // 11: 0x80D0F39E bl 0x80D0F146 (die clock block)  [[UNREACHABLE writes inside]]
    die_clk_config(bus);
    return Confidence::Verified;
}

// stock: 0x80D0F4C2, r0 == 0 arm (0x80D0F702..0x80D0F7CE)
Confidence whale_power_on(PmuBus& bus, bool first) {
    // 0x80D0F702: if (forced || state[+3] == 0) -> run ensure-on
    if (first) {
        // 0x80D0F708 bl 0x80D0F2FA
        whale_ensure_on(bus);
        // 0x80D0F70C bl 0x80D0E90C ; 0x80D0F710..18 [0xD15E0058] |= 1 ; 0x50 spin
        rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
        rmw_set(bus, WHALE_AON_BASE + 0x58u, 1u);  // [[UNREACHABLE]]
        bus.spin(0x50);
    }
    // 0x80D0F73A..3C: [0x60831034] = 0x410 (whale sub-block clock/reset config;
    //                 AP-REACHABLE page, live-proven writable)
    bus.write32(WHALE_APB_BASE + 0x1034u, 0x410u);
    bus.spin(0x50);  // 0x80D0F754..5C
    // 0x80D0F778 bl 0x80D0E90C ([0x60830044] |= 0x80 + spin)
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);  // helper-internal spin (veneer -> 0x811750b2, loop 0x80d0e912)
    // 0x80D0F77C..86: if ([0xD15D0020] == 0) [0xD15D0020] = 2  (RF-side gate)
    if (bus.read32(RF_GATE_BASE + 0x20u) == 0u) bus.write32(RF_GATE_BASE + 0x20u, 2u);
    // 0x80D0F7A0..A6: [0x402b003c] &= ~0x02000000  (WHu3g island power-UP request)
    rmw_clr(bus, PMU_APB_BASE + REQ_WHU3G, 0x0200'0000u);
    // 0x80D0F7AA bl 0x80D0ED6A(0): poll STATUS0[4:0] == 0 (max 1e6)
    wait_whu3g_on(bus, 1'000'000u);
    // 0x80D0F7AE..CE: once-only tail (stock): BB int unmask 0x8119972A(1,0),
    // DMA1 wake 0x80D2ED26(1) -- driver/platform services, not bus traffic.
    return Confidence::Verified;
}

// stock: 0x80D0F4C2, r0 != 0 arms
Confidence whale_power_off(PmuBus& bus, bool gsm_too) {
    if (!gsm_too) {
        // WHu3g-only down (0x80D0F4C2 -> 0x80D0F5E2 -> 0x80D0F610..0x80D0F63A):
        // 0x80D0F62C..32: [0x402b003c] |= 0x02000000 (force down)
        rmw_set(bus, PMU_APB_BASE + REQ_WHU3G, 0x0200'0000u);
        // 0x80D0F636 bl 0x80D0EE0E: poll STATUS0[4:0] stable == 7
        wait_whu3g_off(bus, 1'000'000u);
        return Confidence::Verified;
    }
    // Full die-down tail (0x80D0F4FC..0x80D0F700):
    // 0x80D0F52E strb (state flag) ; 0x80D0F530..38: [0x402b0210] |= 1
    rmw_set(bus, PMU_APB_BASE + REG_0210, 1u);
    // 0x80D0F53C bl 0x80D0E90C ; 0x80D0F540..48 [0xD15E004C] |= 2 ; 0x50
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);  // helper-internal spin
    rmw_set(bus, WHALE_AON_BASE + 0x4Cu, 2u);
    bus.spin(0x50);
    // 0x80D0F556..5E [0xD15E004C] |= 4 ; 0x50 ; 0x80D0F56C bl 0x80D0E90C
    rmw_set(bus, WHALE_AON_BASE + 0x4Cu, 4u);
    bus.spin(0x50);
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);  // helper-internal spin
    // 0x80D0F570..58 [0xD15E0058] |= 1 ; 0x50 ; 0x80D0F586 bl 0x80D0E90C
    rmw_set(bus, WHALE_AON_BASE + 0x58u, 1u);
    bus.spin(0x50);
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);  // helper-internal spin
    // 0x80D0F58A..B4: [0xD15E004C] |= 2 ; 0x50 ; |= 4 ; 0x50
    rmw_set(bus, WHALE_AON_BASE + 0x4Cu, 2u);
    bus.spin(0x50);
    rmw_set(bus, WHALE_AON_BASE + 0x4Cu, 4u);
    bus.spin(0x50);
    // 0x80D0F5F4..FA: [0x60830044] &= ~0x80 ; 0x50
    rmw_clr(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);
    // 0x80D0F606/63C: stock queries 0x8124DFF2/0x8111C3C2 (modem state) and only
    // completes the die-down when both pass; modeled as unconditional here.
    // 0x80D0F66A..70: [0x60830044] |= 0x40 ; 0x50
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    bus.spin(0x50);
    // 0x80D0F67C..82: [0xE010102C] &= ~0x02000000 ; 0x50
    rmw_clr(bus, MISC_DIE_BASE + 0x102Cu, 0x0200'0000u);
    bus.spin(0x50);
    // 0x80D0F68E..98: rd [0x402b00c0] (trace); [0x60830044] &= ~0x40
    (void)bus.read32(PMU_APB_BASE + STATUS0);
    rmw_clr(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    // 0x80D0F69A..A0: [0x402b1044] = 0x01000000 (SET bank bit24 = WTLCP force down)
    bus.write32(set_bank(PMU_APB_BASE + REQ_WTLCP), 0x0100'0000u);
    // 0x80D0F6D6..D8: [0x60832034] = 0x410 (twin bank write --
    // the power-off arm also configures the +0x2000 twin of the whale sub-block)
    bus.write32(WHALE_APB_BASE + 0x2034u, 0x410u);
    bus.spin(0x50);
    // 0x80D0F6FA..700: tail-call 0x80D0F040: ~2-tick wait; [0x402E20B0] = 0xF
    // (AON3 whale-side bridge enable)
    bus.delay_ticks32k(2);
    bus.write32(AON_APB_BASE + 0x2000u + 0xB0u, 0xFu);
    return Confidence::Verified;
}

// stock: 0x80D0E89C
Confidence wtlcp_power_down(PmuBus& bus) {
    // 0x80D0E8A8..AC: [0x60830044] &= ~0x40 (drop the power request)
    rmw_clr(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    // 0x80D0E8AE..B4: [0x402b1044] = 0x01000000 (SET bank: bit24 force-down)
    bus.write32(set_bank(PMU_APB_BASE + REQ_WTLCP), 0x0100'0000u);
    // 0x80D0E8B6..D6: trace of [0x402b0044]
    (void)bus.read32(PMU_APB_BASE + REQ_WTLCP);
    return Confidence::Verified;
}

// stock: 0x80D0F186 (dead in this build; escalation set for the die-local page)
Confidence whale_full_init(PmuBus& bus) {
    // 0x80D0F18A..92: [0x402b00cc] &= ~0x00020000 (RMW clear)
    rmw_clr(bus, PMU_APB_BASE + DSP_SLP_FORCE, 0x0002'0000u);
    // 0x80D0F196..9C: [0x402b20cc] = 0x00020000 (bank2 clear, redundant)
    bus.write32(clr_bank(PMU_APB_BASE + DSP_SLP_FORCE), 0x0002'0000u);
    // 0x80D0F1A0..AA: [0x402E2144] = 0x10 then 1 (AON3 whale-side system reset clear)
    bus.write32(AON_APB_BASE + 0x2000u + 0x144u, 0x10u);
    bus.write32(AON_APB_BASE + 0x2000u + 0x144u, 1u);
    // 0x80D0F1AE..B2: [0x402E1144] = 2 (AON2 reset clear bit1)
    bus.write32(AON_APB_BASE + 0x1144u, 2u);
    // 0x80D0F1B6: bridge enable
    whale_bridge_enable(bus);
    // 0x80D0F1BA lsls r0, r5(=1), #0x19 -> 0x02000000
    // 0x80D0F1BC..C0: [0x402b2044] = 0x02000000 ; = 0x01000000 (clear +0x44 bits 25:24)
    bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0200'0000u);
    bus.write32(clr_bank(PMU_APB_BASE + REQ_WTLCP), 0x0100'0000u);
    // 0x80D0F1C2..CA: [0x60830044] |= 0x40
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    // 0x80D0F1CC..D0: [0x402E0244] = 0x00010100
    bus.write32(AON_APB_BASE + 0x244u, 0x0001'0100u);
    // 0x80D0F1D4: wait_wtlcp_ready (0x80D0EB78)
    wait_wtlcp_ready(bus, WHALE_APB_BASE, 1'000'000u);
    // 0x80D0F1D8..E0: [0x402b0210] &= ~1 ; 0x50
    rmw_clr(bus, PMU_APB_BASE + REG_0210, 1u);
    bus.spin(0x50);
    // 0x80D0F1EE: bl 0x80D0E95C ([0x60830044] |= 0x40 + E010102C set25/clr24 + spins)
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    bus.spin(0x50);
    rmw_set(bus, MISC_DIE_BASE + 0x102Cu, 0x0200'0000u);
    rmw_clr(bus, MISC_DIE_BASE + 0x102Cu, 0x0100'0000u);
    bus.spin(0x50);
    // 0x80D0F1F2..FE: [0x402b0210] |= 1 ; bl 0x80D0E90C
    rmw_set(bus, PMU_APB_BASE + REG_0210, 1u);
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);  // helper-internal spin
    // 0x80D0F202..16: light path
    light_path(bus);
    // 0x80D0F218: bl 0x80D0F146 (die clock block)  [[UNREACHABLE writes inside]]
    die_clk_config(bus);
    // 0x80D0F220..26: [0x402b003c] &= ~0x02000000 (WHu3g up)
    rmw_clr(bus, PMU_APB_BASE + REQ_WHU3G, 0x0200'0000u);
    // 0x80D0F228: bl 0x80D0ED18 (WHu3g poll, no-trace variant)
    wait_whu3g_on(bus, 1'000'000u);
    // 0x80D0F22C..36: [0x404D0008] |= 0x80 (0x404D block enable; end of bring-up)
    rmw_set(bus, 0x404D'0008u, 0x80u);
    return Confidence::Verified;
}

// stock: 0x80D0F272 (+ tail 0x80D0F2AA)
Confidence whale_rtc_wake_repower(PmuBus& bus) {
    // 0x80D0F274: bridge enable (0x80D0EFE4) -- no isolation dance on this path
    whale_bridge_enable(bus);
    // 0x80D0F286..8E: [0x60830044] |= 0x40
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    // 0x80D0F290: wait_wtlcp_ready ; 0x80D0F294: E95C step ; 0x80D0F298: E90C
    wait_wtlcp_ready(bus, WHALE_APB_BASE, 1'000'000u);
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::WTLCP_POWER);
    bus.spin(0x50);
    rmw_set(bus, MISC_DIE_BASE + 0x102Cu, 0x0200'0000u);
    rmw_clr(bus, MISC_DIE_BASE + 0x102Cu, 0x0100'0000u);
    bus.spin(0x50);
    rmw_set(bus, WHALE_APB_BASE + 0x44u, WhaleReq44::ENABLE);
    bus.spin(0x50);
    // 0x80D0F29C..2A8: light path then [0xD15E0068] = 0xDD00DD again, die_clk_config
    light_path(bus);
    die_clk_config(bus);
    return Confidence::Verified;
}

// stock: alive-check timer callback 0x80E2DAF8 (created by L1 ADX init, 30000 ms,
// re-armed by the resp pump 0x80C109A2 on every response). Timeout action is
// DIAGNOSTIC ONLY: dump-trigger word = 0x33, then a 0x4000-word HDLC dump.
// [[inferred]] win-relative offset.
Confidence dsp_alive_check_expired(PmuBus& bus, u32 dsp_win_dump_word) {
    // 0x80E2DAF8..: [dump_word] = 0x33 (literal pool 0x80E2DDD8)
    bus.write32(dsp_win_dump_word, 0x33u);
    return Confidence::Inferred;
}

// stock: 0x80D0FAD8 (0xD15E0164 + irq-mask words) and 0x80D0FB10 ([0x60831034]=0x10).
// NEW discoveries ; found while scanning past 0x80D0FA7C. [[inferred]]
Confidence whale_sleep_entry_extras(PmuBus& bus, u32 d15e0164_value) {
    // 0x80D0FADA bl 0x8124DF74 (value source) ; 0x80D0FADE..E0: [0xD15E0164] = v
    bus.write32(WHALE_AON_BASE + 0x164u, d15e0164_value);          // [[UNREACHABLE]]
    // 0x80D0FAE2..EC: [0xE0101020] |= 0xFFF ; [0xE0101024] |= 0xFFF
    rmw_set(bus, MISC_DIE_BASE + 0x1020u, 0x0000'0FFFu);
    rmw_set(bus, MISC_DIE_BASE + 0x1024u, 0x0000'0FFFu);
    // 0x80D0FAF4..FE: [0xD15E0154] |= 0x1FEFF3FF ; 0x80D0FB02..08: [0xD15E0158] |= same
    rmw_set(bus, WHALE_AON_BASE + 0x154u, 0x1FEF'F3FFu);           // [[UNREACHABLE]]
    rmw_set(bus, WHALE_AON_BASE + 0x158u, 0x1FEF'F3FFu);           // [[UNREACHABLE]]
    // 0x80D0FB2A..2E: [0x60831034] = 0x10 (sleep value; the wake value is 0x410)
    bus.write32(WHALE_APB_BASE + 0x1034u, 0x10u);
    return Confidence::Inferred;
}

}  // namespace power
}  // namespace f286
