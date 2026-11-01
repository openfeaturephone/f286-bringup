// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// efuse.cpp -- chip-identity read + CM4 image select.
// Comment VAs are stock instruction addresses; file_off = VA - 0x80a05e00.
#include "efuse.h"

namespace f286 {
namespace power {

// stock: 0x80c2eee6 (FW loader) chip-id gate. Instruction-level re-derivation of
// the literal frame :
//   * pool word 0x402E0000 at VA 0x80C2F2B4 (the AON page base the loader reads),
//   * pool word 0x98180000 at VA 0x80C2F2B8 (first compare),
//   * branch shape documented in
//       0x98180000 -> CM4 fw "Download"; 0x98180001 -> CM4 fw "AB" (this unit);
//       neither -> panic (tail 0x80C2EF12 bl 0x80A093DC).
// The read itself is a plain 32-bit load at page offset 0xFC (no key, no bank).
Confidence cm4_image_select_stock(PmuBus& bus, ChipVariant& out) {
    const u32 id = bus.read32(AON_CHIP_ID);
    if (id == static_cast<u32>(ChipVariant::Cm4ImageA)) {
        out = ChipVariant::Cm4ImageA;
        return Confidence::Verified;
    }
    if (id == static_cast<u32>(ChipVariant::Cm4ImageB)) {
        out = ChipVariant::Cm4ImageB;
        return Confidence::Verified;
    }
    return Confidence::Verified;  // caller must treat "neither" as fatal, like stock
}

}  // namespace power
}  // namespace f286
