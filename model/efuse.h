// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// efuse.h -- minimal chip-identity reads (AON block) used by the boot path.
//
// Stock consumer: the FW loader 0x80c2eee6 reads the AON chip-id word to pick
// the CM4 companion image (+ section at line 182;
// base/compare literals re-confirmed : pool 0x402E0000 @0x80C2F2B4,
// compare 0x98180000 @0x80C2F2B8).
#pragma once

#include "f286_bus.h"

namespace f286 {
namespace power {

constexpr u32 AON_CHIP_ID = 0x402E'00FCu;  // [31:0] chip id (POR value, read-only)

// Known silicon variants (observed in the stock loader's compare chain).
enum class ChipVariant : u32 {
    Cm4ImageA = 0x9818'0000u,  // CM4 partition "Download"
    Cm4ImageB = 0x9818'0001u,  // CM4 partition "AB" -- this unit (Ruio F286)
};

// Read the raw chip id. (Semantics Verified via; the exact stock
// load instruction is base+0xFC off the 0x402E0000 page --.)
inline u32 read_chip_id(PmuBus& bus) { return bus.read32(AON_CHIP_ID); }

// CM4 image select -- which companion firmware the loader stages for this chip.
// [[inferred]] helper: folds the stock if/else-if chain into a typed result.
inline ChipVariant cm4_image_select(PmuBus& bus) {
    const u32 id = read_chip_id(bus);
    if (id == static_cast<u32>(ChipVariant::Cm4ImageA)) return ChipVariant::Cm4ImageA;
    if (id == static_cast<u32>(ChipVariant::Cm4ImageB)) return ChipVariant::Cm4ImageB;
    // stock: neither -> fatal (bl 0x80a093dc = kernel panic path, 0x80C2EF12)
    return ChipVariant::Cm4ImageB;  // driver: log + assume the running unit's variant
}

// Stock-exact transcribed gate (efuse.cpp): returns the matched variant, or
// Cm4ImageB with the caller responsible for the "neither -> fatal" case.
Confidence cm4_image_select_stock(PmuBus& bus, ChipVariant& out);

}  // namespace power
}  // namespace f286
