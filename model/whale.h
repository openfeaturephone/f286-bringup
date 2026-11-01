// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// whale.h -- whale-die power-on (the baseband power die), stock entry 0x80D0F4C2.
//
// The "whale" die carries the WTLCP island (LTE DSP), WHu3g island (GSM DSP),
// the CM4/WCN core and the RF die. Stock powers it from the AP through:
//   * AON2 whale bridge enable      [0x402E10B0] = 1
//   * whale APB request register    0x60830044 (isolation bit13, power bit6, en bit7)
//   * whale AON page                0xD15E0000 (+0x4C/+0x58/+0x68, clock banks +0x1000/+0x2000)
//   * RF-side gate                  0xD15D0020 (0 -> 2)
//   * misc-die config page          0xE0100000 (+0x100C/+0x1014/+0x102C, +0x20/+0x24)
// All addresses are PHYSICAL (the stock MMU map is IDENTITY;
// the old "0x515E0000 PA" and "0x60100000 PA" readings are refuted).
//
// AP-REACHABILITY WARNING (seen on hardware, 2026-10-21): the die-local clock-mux
// writes to 0xD15E1028/0xD15E2000 hung the bus even at the identity PA once the
// die showed power signs. They are marked AP-UNREACHABLE below; the open
// question is the true access path.
#pragma once

#include "f286_bus.h"

namespace f286 {
namespace power {

// Physical bases (identity-mapped VAs == PAs).
constexpr u32 WHALE_APB_BASE = 0x6083'0000u;  // power-request regs; +0x44 live-proven
constexpr u32 WHALE_AON_BASE = 0xD15E'0000u;  // whale AON/PMU page (AP-UNREACHABLE live)
constexpr u32 RF_GATE_BASE = 0xD15D'0000u;    // RF/SPI-side gate; +0x20: 0 -> 2
constexpr u32 MISC_DIE_BASE = 0xE010'0000u;   // misc die cfg page (light path)
constexpr u32 AON_APB_BASE = 0x402E'0000u;    // AP always-on block (bridges/gates)

// 0x60830044 request bits (VERIFIED inline RMWs throughout 0x80D0E5xx-0x80D0FC90).
struct WhaleReq44 {
    static constexpr u32 ISOLATION = 0x2000u;  // bit13: 1 = hold die in isolation
    static constexpr u32 WTLCP_POWER = 0x40u;  // bit6:  WTLCP power-up request
    static constexpr u32 ENABLE = 0x80u;       // bit7:  follow-up module enable
};

// ---------------------------------------------------------------------------
// Operations (whale.cpp), stock-exact ordering.
// ---------------------------------------------------------------------------

// Bridge enable -- stock VA 0x80D0EFE4: [0x402E10B0] = 1, 0x50 spin, trace.
// MUST be the first whale-related write of any power-on.
Confidence whale_bridge_enable(PmuBus& bus);

// Ensure-on -- stock VA 0x80D0F2FA (the LIVE power path; 0x80D0F4C2 calls it when
// first-boot flag or forced). Order:
//   1  bridge enable (0x80D0EFE4)
//   2  [0x60830044] |= 0x2000                      isolation request
//   3  >=5 x 32k ticks                             (0x80A0B552 compare loop)
//   4  [0x60830044] |= 0x40                        WTLCP power request
//   5  wait_wtlcp_ready (0x80D0EB78, includes the force retry)
//   6  [0x60830044] |= 0x40; [0xE010102C] |= 0x2000000; [0xE010102C] &= ~0x1000000
//                                                  (0x80D0E95C; SET first, THEN clear)
//   7  [0x60830044] &= ~0x2000                     isolation release
//   8  0x50 spin; [0x60830044] |= 0x80 + 0x50 spin (0x80D0E90C)
//   9  [0xD15E0058] |= 1                           first whale-AON touch  [[UNREACHABLE live]]
//  10  0x50 spin; light path (below); [0xD15E0068] = 0xDD00DD
//  11  die clock block (die_clk_config, 0x80D0F146)              [[UNREACHABLE live]]
Confidence whale_ensure_on(PmuBus& bus);

// Light-path trio -- stock VA 0x80D0F2FA+0x70..0x82 / 0x80D0F186+0xC2..0xD6:
//   [0xE010100C] &= ~0x1000000  (clear bit24)
//   [0xE0101014]  = 0x000000CC  (bits 2,3,6,7)
//   [0xD15E0068]  = 0x00DD00DD  [[UNREACHABLE live]]
// CORRECTION vs prior reports: stock VAs are the 0xE0101000 page (identity ->
// PA 0xE0101000). The old "0x601010xx" recipe wrote a DIFFERENT, unknown slave
// that happened to accept writes (live finding).
Confidence light_path(PmuBus& bus);

// Die-local clock mux -- stock VA 0x80D0F0EE + 0x80D0F146 (called from ensure-on):
//   [0x60831044] = 0x80; 0x50 spin          whale module enable (AP-REACHABLE)
//   rd [0xD15E0028]; [0xD15E1028] = 0x380   SET bits 9:7 of reg +0x28  [[UNREACHABLE]]
//   rd [0xD15E0028]; rd [0xD15E0000]
//   [0xD15E2000] = 0x380                    CLEAR bits 9:7 of reg +0x00  [[UNREACHABLE]]
//   [0x60831044] = 0x80; 0x50 spin
//   rd [0xD15E0000]; [0xD15E2000] = 0x300000 CLEAR bit21 of reg +0x00  [[UNREACHABLE]]
//   rd [0xD15E0000]
// Banking VERIFIED (same +0x1000/+0x2000 idiom as PMU_APB); bit roles INFERRED.
Confidence die_clk_config(PmuBus& bus);

// The stock power-on entry -- stock VA 0x80D0F4C2, r0==0 arm (wslp chain:
// 0x81139be6 -> 0x80d0f944 -> here). Calls whale_ensure_on once (guarded by
// the stock state byte [0x81707258+3], modeled by `first`), then
// [0x60831034] = 0x410, [0xD15E0058] |= 1, RF gate [0xD15D0020] 0->2, and the
// WHu3g island up: [0x402b003c] &= ~0x2000000 + wait_whu3g_on, then the
// once-only BB-int/DMA1/GPTM wake (stock 0x8119972A/0x80D2ED26 -- stubbed here).
Confidence whale_power_on(PmuBus& bus, bool first);

// Power-off arms of 0x80D0F4C2 (r0!=0):
//   gsm_too==false : WHu3g only: [0x402b003c] |= 0x2000000 + wait_whu3g_off.
//   gsm_too==true  : + the full die-down tail (0x80D0F4C2+0x3A..0x23E):
//       [0x402b0210] |= 1; [0x60830044] |= 0x80; [0xD15E004C] |= 2, |= 4
//       (twice, with 0x50 spins); [0x60830044] &= ~0x80; then if the modem-state
//       checks pass: [0x60830044] |= 0x40, [0xE010102C] &= ~0x2000000,
//       [0x60830044] &= ~0x40, [0x402b1044] = 0x01000000 (SET bank bit24 =
//       WTLCP force-down), [0x60832034] = 0x410 (twin bank), tail-call 0x80D0F040
//       ([0x402E20B0] = 0xF, AON3 whale bridge enable).
Confidence whale_power_off(PmuBus& bus, bool gsm_too);

// WTLCP power-DOWN request -- stock VA 0x80D0E89C (0x80d0e89c):
//   [0x60830044] &= ~0x40 ; [0x402b1044] = 0x01000000 (SET bank: bit24 = force down).
Confidence wtlcp_power_down(PmuBus& bus);

// Full whale bring-up (the "dead init" variant) -- stock VA 0x80D0F186, sole
// caller is the unreferenced L1-init 0x80CE83EA .
// UNIQUE steps not in ensure-on (escalation set for the die-local page):
//   [0x402b00cc] &= ~0x20000 ; [0x402b20cc] = 0x20000
//   [0x402E2144] = 0x10 then 1 ; [0x402E1144] = 2
//   bridge enable ; [0x402b2044] = 0x02000000, = 0x01000000 ; [0x60830044] |= 0x40
//   [0x402E0244] = 0x00010100 ; wait_wtlcp_ready
//   [0x402b0210] &= ~1 ; 0x50 ; light-path 0xE010102C step ; [0x402b0210] |= 1
//   [0x60830044] |= 0x80 ; light path ; die_clk_config
//   [0x402b003c] &= ~0x2000000 ; wait_whu3g_on ; [0x404D0008] |= 0x80
Confidence whale_full_init(PmuBus& bus);

// RTC-wake re-power -- stock VA 0x80D0F272 (+ tail 0x80D0F2AA): like ensure-on but
// WITHOUT the isolation request; [0xD15E0068] = 0xDD00DD + die_clk_config at the end.
Confidence whale_rtc_wake_repower(PmuBus& bus);

// 30 s housekeeping hook (stock: "LTEDSP Alive Check Timer").
// Stock semantics: 30000 ms timer re-armed on EVERY DSP response; on expiry it does
// NOT reset anything -- it writes 0x33 to the DSP dump-trigger word (win+0x10bf8,
// AP VA 0x8044_0BF8 region; literal pool 0x80E2DDD8) and dumps DSP memory to the
// diag port. A driver may therefore skip it, or implement diagnostics-only.
// This function encodes the faithful trigger write. [[inferred]] (win base differs
// between fw variants; verify against the fw in use).
Confidence dsp_alive_check_expired(PmuBus& bus, u32 dsp_win_dump_word);

// Sleep-path extras discovered  (new, not in any earlier report):
//  * 0x80D0FAD8: [0xD15E0164] = v (from helper 0x8124DF74); [0xE0101020] |= 0xFFF;
//    [0xE0101024] |= 0xFFF; [0xD15E0154] |= 0x1FEFF3FF; [0xD15E0158] |= 0x1FEFF3FF.
//  * 0x80D0FB10: [0x60831034] = 0x10 (sleep-side value of the whale sub-block reg).
// Encoded as one function because stock keeps them in the sleep-entry family.
// [[inferred]] role: modem-sleep entry (irq masks + clock down).
Confidence whale_sleep_entry_extras(PmuBus& bus, u32 d15e0164_value);

}  // namespace power
}  // namespace f286
