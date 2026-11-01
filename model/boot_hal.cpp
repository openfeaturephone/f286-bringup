// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// boot_hal.cpp - stock HAL provenance tables + coherence checks.
// The register sequences themselves are header-inline in boot_hal.h (each is a
// 1-3 instruction setter in stock); this file carries the wrapper-thunk map and
// static verification of the decoded constants.

#include "boot_hal.h"

namespace f286 {
namespace dsp {

// Stock thunk wrappers (all plain B.W thunks, VERIFIED by disassembly).
// kernel.bin VA space; older table entries are 0x38 lower.
struct ThunkMap {
  uint32_t thunkVa;
  uint32_t targetVa;
  const char* role;
};
constexpr ThunkMap kThunks[] = {
    {0x811a76c6, 0x811d0c98, "get GSM window (0x800e0000)"},
    {0x811a76ca, 0x811d0ca0, "get LTE window (0x80400000)"},
    {0x811a76ce, 0x811d0ca8, "get scratch A (0x10c00)"},
    {0x811a76da, 0x811d0ce0, "GSM reset hold/release (0x402e0130 bit17)"},
    {0x811a7746, 0x811d0d3e, "GSM boot-addr (0x402e0250, verbatim)"},
    {0x811a774a, 0x811d0d4e, "GSM start-ctrl (0x402e0254 bit0)"},
    {0x811a7766, 0x811d0d4e, "GSM reboot helper (start high -> addr -> start low -> hold -> 0x100 delay)"},
    {0x811a7794, 0x00000000, "BE->LE word loader (param images)"},
    {0x811a77c6, 0x811d0ca4, "stub getter (0)"},
    {0x811a77ca, 0x811d0c9c, "get LTE window (2nd)"},
    {0x811a77ce, 0x811d0cfa, "LTE reset hold/release (0x402e0130 bit16)"},
    {0x811a77d6, 0x811d0d46, "LTE boot-addr (0x402e0248, verbatim)"},
    {0x811a77da, 0x811d0d68, "LTE start-ctrl (0x402e024c bit0)"},
    {0x811a780a, 0x811d0cae, "get scratch B (0x11c00)"},
    {0x811a780e, 0x811d0cb4, "get scratch 0x1000"},
    {0x811a7812, 0x811d0dce, "DSP int enable A (tail 0x402e1144=0x1a)"},
    {0x811a7816, 0x811d0e4a, "DSP int route B (0x60832044=0x40 restore)"},
};

const ThunkMap* thunk_map() { return kThunks; }
size_t thunk_map_size() { return sizeof(kThunks) / sizeof(kThunks[0]); }

// Static verification of the decoded HAL semantics.
static_assert(kGsmHal.resetMask == 0x20000, "0x811d0ce0 orr/bic #0x20000 (bit17)");
static_assert(kLteHal.resetMask == 0x10000, "0x811d0cfa orr/bic #0x10000 (bit16)");
static_assert(kGsmHal.bootAddrReg == 0x402e0250, "0x811d0d3e str r0,[r1,#0x250]");
static_assert(kLteHal.bootAddrReg == 0x402e0248, "0x811d0d46 str r0,[r1,#0x248]");
static_assert(kGsmHal.startCtrlReg == 0x402e0254, "0x811d0d4e bit0");
static_assert(kLteHal.startCtrlReg == 0x402e024c, "0x811d0d68 bit0");
static_assert((0x80448000u + kLteHal.entryOffset) >> 1 == 0x40224200,
              "stock LTE boot-addr literal in 0x80e2d9e6");
static_assert((0x80100000u + kGsmHal.entryOffset) >> 1 == 0x40090040,
              "stock GSM boot-addr computed in 0x810b33fe");

}  // namespace dsp
}  // namespace f286
