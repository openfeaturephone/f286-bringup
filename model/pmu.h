// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// pmu.h -- PMU_APB (0x402b0000) power-domain model of the UMS9117 (Ruio F286).
//
// Reconstructed instruction-by-instruction from the stock kernel
// (kernel.bin, payload VA = file_off + 0x80a05e00).
// Provenance tags below refer to stock VAs; "file_off = VA - 0x80a05e00".
//
// The block model:
//   * One "whale" baseband power die carries the DSP/CORE domains. Its islands
//     are switched by the PMU_APB request registers (+0x34/+0x38/+0x3c/+0x44)
//     and their readiness is read back from the two status registers
//     (0x402b00c0 / 0x402b00c4 / 0x402b00d4).
//   * Requests are BIT PAIRS: bit25 and bit24 of each request register.
//     Clear-both = let the domain power up (VERIFIED live: 0x402b2034=0x03000000
//     powered the LTE XDSP core, c0[9:5] 7 -> 0). The exact meaning of driving
//     only ONE of the two bits (stock's SET-bit25/CLEAR-bit24 sweeps) is an
//     open question --.
//   * SET (+0x1000) / CLEAR (+0x2000) alias banks, write-1-to-affect.
#pragma once

#include "f286_bus.h"

namespace f286 {
namespace power {

// ---------------------------------------------------------------------------
// Register map (all VERIFIED as instruction operands unless noted).
// ---------------------------------------------------------------------------
constexpr u32 PMU_APB_BASE = 0x402b0000u;  // physical, AP-writable directly (no key)

constexpr u32 PMU_CLR_BANK = PMU_APB_BASE + 0x2000u;  // e.g. 0x402b2000 (pool 0x80D0FA80)
constexpr u32 PMU_SET_BANK = PMU_APB_BASE + 0x1000u;  // e.g. 0x402b1000 (pool 0x80D0FA98)

// Domain request registers (base offsets). Stock drives them through the banks;
// offsets below are relative to PMU_APB_BASE.
constexpr u32 REQ_0028 = 0x28u;    // request pair, full sweep 0x80D0FAA0+0x1C
constexpr u32 REQ_002C = 0x2Cu;    // request pair, full sweep 0x80D0FAA0+0x22
constexpr u32 REQ_XDSP_AB = 0x34u; // XDSP core pair A -- LTE DSP core (see StatusField)
constexpr u32 REQ_0038 = 0x38u;    // XDSP core pair B -- GSM DSP core (HAL table id 2)
constexpr u32 REQ_WHU3G = 0x3Cu;   // WHu3g island: bit25 1=force-down, 0=up (0x80D0F4C2+0x2E0..2E6)
constexpr u32 REQ_WTLCP = 0x44u;   // WTLCP island: bit25/bit24 pair; bit24=1 force-down (0x80D0E89C+0x18)
constexpr u32 REG_00B0 = 0xB0u;    // GSM pre-boot writes 5 to the CLEAR bank (0x80D0FA5E): bits 0,2
constexpr u32 REG_0210 = 0x210u;   // bit0 toggled around the isolation-release step (0x80D0F4C2+0x6E, 0x80D0F186+0x5A)
constexpr u32 DSP_SLP_FORCE = 0xCCu;  // bit17 = DSP shared deep-sleep force, bit21 = CM4 force
constexpr u32 AP_SLP_FORCE = 0x230u;  // [[inferred]] AP-sleep entry SETs 0x1f0000 (bank1) = 5 domains down

// Status registers.
constexpr u32 STATUS0 = 0xC0u;  // island status fields, 0=ON / 7=OFF, 5 bits each
constexpr u32 STATUS1 = 0xC4u;  // [4:0] WTLCP island status (0=ON / 7=OFF)
constexpr u32 WTLCP_STATE = 0xD4u;  // [7:4] WTLCP power FSM state; ready == 6

// Status fields of STATUS0 (0x402b00c0). Field -> domain/request register pairing
// is VERIFIED two ways: the stock DSP HAL table (init data 0x81705E94) pairs id0 with reg +0x34 + poll mask 0x03e0, id2 with reg +0x38 + mask
// 0x7c00; and a hardware run showed clearing +0x34 drives [9:5] to 0 for the LTE DSP.
// NOTE: an earlier guess had A=[9:5]=GSM; that was wrong.
struct Status0 {
    static constexpr u32 WHU3G = 0x0000'001Fu;  // [4:0]  WHu3g island (poll 0x80D0ED6A)
    static constexpr u32 XDSP_LTE = 0x0000'03E0u;  // [9:5]  XDSP "A": LTE DSP core  (reg +0x34, HAL id 0)
    static constexpr u32 XDSP_GSM = 0x0000'7C00u;  // [14:10] XDSP "B": GSM DSP core (reg +0x38, HAL id 2)
    static constexpr u32 CM4 = 0x000F'8000u;  // [19:15] CM4/WCN island
};
// Status field encodes (all fields share this convention, VERIFIED polls):
constexpr u32 FIELD_ON = 0u;   // 0b00000 == powered
constexpr u32 FIELD_OFF = 7u;  // 0b00111 == powered down (upper bits read noise)

inline u32 field(u32 regval, u32 mask) {
    // position of the LSB of an already-contiguous mask
    u32 shift = 0;
    while (((mask >> shift) & 1u) == 0u) ++shift;
    return (regval & mask) >> shift;
}

// ---------------------------------------------------------------------------
// Operations (pmu.cpp). Every function takes the bus; nothing touches hardware
// directly. Order of writes inside each function is stock-exact.
// ---------------------------------------------------------------------------

// Stock LTE pre-boot power release -- stock VA 0x80D0FA7C, called with r0=1 from
// the LTE DSP boot fn at 0x80E2D892 on EVERY cold boot. THE fix for the silent
// DSP : without the last write the XDSP cores
// stay held (STATUS0 [9:5] and [14:10] read 7).
//   arg release==true  -> the four bank2 CLEAR writes (boot arm).
//   arg release==false -> the sleep arm: SET bit25 / CLEAR bit24 of +0x34
//                         (stock 0x80D0FA98..0x80D0FA9E).
Confidence lte_preboot_power_release(PmuBus& bus, bool release);

// GSM/XDSP-B sibling -- stock VA 0x80D0FA54, release arm r0=1. NO stock callers
// found (blscan ); the GSM path normally releases +0x38 through the
// DSP HAL instead (xdsp_power_release_wait). Kept for completeness; [[inferred]]
// as "the GSM pre-boot release".
Confidence gsm_preboot_power_release(PmuBus& bus, bool release);

// Full domain-request sweep -- stock VA 0x80D0FAA0, sole caller 0x80C2F422 inside
// the OS power task (0x80c2f132 family). Runs before the modem power messages.
Confidence full_radio_release_sweep(PmuBus& bus);

// DSP power-release + interrupt-enable HAL -- stock VA 0x811D0D96 (through the
// Thumb veneer 0x811A77DA). id: 0 = LTE/XDSP-A (reg +0x34), 2 = GSM/XDSP-B
// (reg +0x38); other ids are rejected by stock (`(1<<id) & ~5` != 0).
// Sequence: CLEAR-bank write ~mask (=0x03000000) of the id's request register,
// CLEAR 0x402b0044 bits 25:24, poll STATUS0 mask 0x3e0 / 0x7c00 == 0 up to 0x1000
// tries, then [0x402e1144]=0x1a, [0x60831044]=0x40 (whale int enable), settle.
Confidence xdsp_power_release_wait(PmuBus& bus, u32 id);

// Deep-sleep force bit17 of 0x402b00cc -- stock VA 0x811D0D54(1=SET bank,
// 0=CLEAR bank). Shared by BOTH XDSPs; clearing it alone does NOT power a
// domain (live-proven 2026-10-20). bit21 = CM4 force (cleared by CM4 boot).
Confidence dsp_deep_sleep_force(PmuBus& bus, bool force);

// CM4 deep-sleep-force clear -- stock CM4 boot 0x80c2ee9c writes
// 0x402b20cc = 0x00200000 (CLEAR bank, bit21).
Confidence cm4_force_clear(PmuBus& bus);

// -- polling (stock readiness criteria) --------------------------------------

// WHu3g power-UP poll -- stock VA 0x80D0ED6A(0) / 0x80D0ED18: STATUS0[4:0] must
// read 0 on three consecutive samples, max 1'000'000 iterations. Returns true on
// success. Timeout logs "DRV : Read HU3G Power Status Timeout, state1/2/3, cnt".
Confidence wait_whu3g_on(PmuBus& bus, u32 max_iters);

// WHu3g power-DOWN poll -- stock VA 0x80D0EE0E: STATUS0[4:0] stable == 7.
Confidence wait_whu3g_off(PmuBus& bus, u32 max_iters);

// WTLCP ready poll -- stock VA 0x80D0EB78 (with force-retry, see below).
// Ready == (STATUS1[4:0] == 0) AND (WTLCP_STATE[7:4] == 6), each stable across
// three consecutive reads; max 1'000'000 iterations. On timeout with the field
// still 7 stock FORCE-retries once (0x80D0EC50): clears 0x402b0044 bits 25:24
// via the CLEAR bank, re-asserts the whale power request and re-polls (0x80D0EAD4).
// whale_apb_base is 0x60830000 (see whale.h) -- needed for the force path.
Confidence wait_wtlcp_ready(PmuBus& bus, u32 whale_apb_base, u32 max_iters);

// Sleep-driver wake re-power -- stock VA 0x81602040, instruction-exact
// (-2 fix): 0x402b20cc=0x20000, 0x402b2044=0x02000000 then
// 0x01000000 (bit25/bit24 WTLCP pair), silent 0x10000 delay, [0x60830044]|=0x40.
Confidence sleep_wake_dsp_repower(PmuBus& bus, u32 whale_apb_base);

// AP-sleep domain force -- stock VA 0x81602064, instruction-exact
// (-3 fix): AP-AHB syscons base 0x20E01000 --
// [0x20E01010]=0x1803, [0x20E01088 + 0x8C..0xA0]=0x20000 (7 words),
// [0x20E0100C]=1, [0x402E1140]=0x10000, [0x402E1524]=3,
// [0x402b1230 (SET bank)]=0x1f0000. ([0xE4DC] bic #1 omitted: base unpinned.)
Confidence ap_sleep_force_domains(PmuBus& bus);

}  // namespace power
}  // namespace f286
