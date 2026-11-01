// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// f286_bus.h -- hardware abstraction for the F286 (Unisoc UMS9117) radio power layer.
//
// Everything in f286::power talks to hardware ONLY through PmuBus so the layer is
// unit-testable on the host (record/replay bus) and portable into the Linux driver
// (ioremap'd __iomem pointers behind one implementation).
//
// Access-width rule (VERIFIED from stock code):
//   * PMU_APB (0x402b0000), AON_APB (0x402e0000), whale APB (0x60830000),
//     whale AON (0xd15e0000), misc-die (0xe0100000): 32-bit only, in stock too.
//   * ADI analog-die window (0x40600000 / 0x40608xxx): 16-bit registers behind the
//     ADI protocol -- out of scope of this layer, but the
//     bus interface exposes 16-bit ops so the ADI shim can be added later.
//
// SET/CLEAR bank rule (VERIFIED for PMU_APB at instruction level):
//   base+0x1000 = write-1-to-SET alias, base+0x2000 = write-1-to-CLEAR alias.
//   CAUTION (seen on hardware): on real silicon the
//   BASE register readback does not reliably reflect alias writes. Never gate a
//   sequence on `read32(base) & bit`; poll the dedicated STATUS registers instead.
#pragma once

#include <cstdint>

namespace f286 {
namespace power {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// How well a reconstructed function is grounded in the binary.
//   Verified: every step transcribed from disassembled stock instructions this
//             session (the comment cites the instruction VAs).
//   Inferred: semantics/name assigned by us; the register traffic is real but the
//             meaning is deduced (or taken from earlier notes without
//             re-checking). Marked // [[inferred]] per function.
enum class Confidence {
    Verified,
    Inferred,
};

// Physical bus + time source. One implementation per environment.
class PmuBus {
public:
    virtual ~PmuBus() = default;

    // -- MMIO ---------------------------------------------------------------
    virtual u32 read32(u32 pa) = 0;
    virtual void write32(u32 pa, u32 value) = 0;
    virtual u16 read16(u32 pa) = 0;  // only used by the ADI window (not this layer)
    virtual void write16(u32 pa, u16 value) = 0;

    // -- time ---------------------------------------------------------------
    // Stock uses two delay idioms; both are kept so a port can reproduce timing.
    // spin(0x50): the stock 0x50-iteration busy loop, e.g. 0x80D0E90C+0xC.
    virtual void spin(u32 iterations) = 0;
    // ~n ticks of the 32.768 kHz always-on counter (stock helper 0x80A0B552 +
    // compare loop, e.g. ensure-on 0x80D0F2FA+0x1E..0x32 waits >=5 ticks).
    virtual void delay_ticks32k(u32 ticks) = 0;
};

// Read-modify-write helpers over the bus (stock RMWs the same registers inline).
inline u32 rmw_set(PmuBus& bus, u32 pa, u32 bits) {
    const u32 v = bus.read32(pa) | bits;
    bus.write32(pa, v);
    return v;
}
inline u32 rmw_clr(PmuBus& bus, u32 pa, u32 bits) {
    const u32 v = bus.read32(pa) & ~bits;
    bus.write32(pa, v);
    return v;
}

// SET/CLEAR alias addresses for a W1P-style block (PMU_APB banking, VERIFIED by
// every stock writer, e.g. 0x80D0FA7C uses 0x402B2000 = PMU+0x2000 as the base).
constexpr u32 set_bank(u32 base_reg) { return base_reg + 0x1000u; }
constexpr u32 clr_bank(u32 base_reg) { return base_reg + 0x2000u; }

}  // namespace power
}  // namespace f286
