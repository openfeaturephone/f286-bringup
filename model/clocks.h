// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// clocks.h -- AON clock gates, TWPLL DSP-branch selects and RF-gate helpers.
//
// Gate-bank convention (VERIFIED by usage):
//   banks 0x402E0000 (AON) / 0x402E1000 (AON2) / 0x402E2000 (AON3),
//   +0x1000 = write-1-to-SET alias, +0x2000 = write-1-to-CLEAR alias.
//   Hardware caveat: base-register readback does NOT
//   reflect alias writes on this silicon -> never gate on it.
#pragma once

#include "f286_bus.h"

namespace f286 {
namespace power {

constexpr u32 AON2_BANK = 0x402E'1000u;
constexpr u32 AON3_BANK = 0x402E'2000u;
constexpr u32 AON_BASE = 0x402E'0000u;
constexpr u32 CLK_MGR_BASE = 0x402D'0000u;   // AP clock manager 
constexpr u32 WHALE_APB = 0x6083'0000u;

// RF-die window / ports (stock VAs use these as identity VAs == PAs).
constexpr u32 RF_SPI_BASE = 0xD15C'2000u;    // whale-side RF-chip SPI window
constexpr u32 RF_PORT_BASE = 0x4007'0000u;   // AP-side RF-SPI/AGCP port block

// -- AON gate sets -----------------------------------------------------------

// AON2 gate pair -- stock helper 0x8136F558 (Thumb twin family 0x8136F4DA):
//   [0x402E1004] = 0x100 ; [0x402E1010] = 0x200 ; (then the keyed 0x40310008 |= 4,
//   which belongs to the deep-sleep TIMER engine and is NOT needed for plain MMIO).
// Live-verified: this pair opens the 0x40310000 page.
Confidence aon2_gate_pair(PmuBus& bus);

// AON3 companion pair -- stock helper 0x8136F57A: [0x402E2004] = 0x100 ;
// [0x402E2010] = 0x200. NO stock caller (helper itself is caller-less).
Confidence aon3_gate_pair(PmuBus& bus);

// Bus-core enables -- stock early clk init 0x80A072E4:
//   [0x402E2020] = 0xC0000 (AON3, sel 1) ; [0x402E1020] = 0x40000 (AON2, sel 3).
Confidence aon_bus_core_enable(PmuBus& bus);

// RF-block gate -- stock RF init 0x81252240 tail: [0x402E1134] = 0x1000 ;
// dump counterpart 0x8125226E tail: [0x402E2134] = 0x1000 (AON3).
Confidence rf_gate_0x134(PmuBus& bus);

// AON DSP clock enable -- stock HAL 0x811D0D7C(arg):
//   arg!=0: [0x402E006C] |= 0x9620   (bits 5,6,11,12,13,15)
//   arg==0: [0x402E006C] &= ~0xFFFF  (clears the whole low half)
Confidence aon_dsp_clock_enable(PmuBus& bus, bool enable);

// DSP interrupt-path reset clear + whale int enable -- stock HAL 0x811D0D96 tail:
//   [0x402E1144] = 0x1A ; [0x60831044] = 0x40 ; 0x50 settle.
// (Called by pmu.cpp xdsp_power_release_wait; exposed for standalone use.)
Confidence dsp_int_enable_tail(PmuBus& bus);

// -- TWPLL DSP branches -------------------------------------------------------

// XDSP clock mux select -- stock 0x80C55282: name lookup drives
// [0x402D0000 + 0x2B4] bits 1:0:
//   0 = "...M_XTL"  (26 MHz crystal family; name string @0x80C545E2)
//   1 = "TWPLL_38M4" (string @0x80C55384)
//   2 = "TWPLL_51M2" (string @0x80C54AC4)  <- the 51.2 MHz DSP clock family
Confidence xdsp_clk_mux_select(PmuBus& bus, u32 sel);

// TWPLL DSP-branch enable ops -- stock 0x80C553A0/0x80C553B0:
//   main  (arg!=0): [0x60831034] = 0x40000 ; [0x60831060] = 0x2000
//   twin  (arg==0): [0x60832034] = 0x40000 ; [0x60832060] = 0x2000
// CAUTION : if 0x60832xxx is the CLEAR bank of
// 0x60831xxx the twin writes CANCEL the main ones. Stock only ever writes ONE
// bank per op (the clk framework calls the op twice with opposite args).
// Default: main only. [[inferred]] bit semantics.
Confidence twpll_dsp_branch_ops(PmuBus& bus, bool main_bank);

// Whale module enables -- stock writes [0x60831044] = 0x80 around every die-local
// access (0x80D0F0B4/0x80D0F0EE/0x80D0F146) and = 0x40 in the DSP int path
// (0x811D0E02). Both AP-REACHABLE.
Confidence whale_module_enable(PmuBus& bus, u32 value);

// -- RF-gate helpers (0x812523xx family) -------------------

// Ensure-gate -- stock 0x80D0CA36: returns [[0x81713778]+0x10] (a driver flag);
// the RF accessors call whale_ensure_on when it is non-zero (0x812522F0).
// Here: return true when stock would re-run the ensure-on.
bool rf_ensure_gate_pending(PmuBus& bus, u32 flag_struct_addr);

// RF transceiver SPI write -- stock 0x81252304(id, addr, val):
//   ensure-gate -> whale_ensure_on if pending;
//   word = (~0x80000000 & (addr << 16)) | val ;
//   [0xD15C2050] (id==0) or [0xD15C2150] (id!=0) = word ;
//   1000-iteration delay reading 0x402b00c4 each iteration.
Confidence rf_spi_write(PmuBus& bus, u32 id, u32 addr, u32 val);

// RF transceiver SPI read -- stock 0x81252354(id, sel, *out):
//   [0xD15C2054 / 0xD15C2154] = 0x80000000 | (sel << 16) ;
//   1000-iteration delay ; poll [base+4] bit31 clear (<= 10 retries) ; *out = [base+4].
// Returns true on success, false after 10 retries (stock returns the flag in r6).
bool rf_spi_read(PmuBus& bus, u32 id, u32 sel, u32& out);

// AP-side RF port write/read -- stock 0x8125229E / 0x812522C6:
//   [0x40070240] = word ; [0x40070244] = 1 ; 1000-delay ; read -> [0x40070248].
Confidence rf_ap_port_write(PmuBus& bus, u32 addr_hi, u32 val);
u32 rf_ap_port_read(PmuBus& bus, u32 addr_hi);

}  // namespace power
}  // namespace f286
