// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// fw_loader.cpp - partition registry lookups + stock load-list provenance.
// Companion to fw_loader.h (the loader class itself is header-inline to mirror
// the short stock control flow of 0x80c2eee6).

#include "fw_loader.h"

#include <cstring>

namespace f286 {
namespace dsp {

namespace {

// Registry dump, VA 0x814cc3ac (kernel.bin space), stride 0x14:
//   {u32 id; const char* name; u32 0; u32 0; u32 flags}
// The single-letter rows ("K","A","D","C") carry flags 0x20/0x7 in the stock
// table (device-letter entries, purpose INFERRED); the DSP-relevant rows are
// the named partitions used by 0x80c2eee6.
constexpr PartitionIdName kRegistry[] = {
    {0x10000001, "BML Downloaded NV Partition"},
    {0x1000000f, "BML PROTECT NV Partition"},
    {0x10000004, "BML MMI Resource Partition"},
    {0x1000001a, "K"},
    {0x10000003, "A"},
    {0x10000005, "BML LTE DSP Code Partition"},
    {0x00000008, "D"},
    {0x10000006, "BML GSM DSP Code Partition"},
    {0x10000007, "C"},
    {0x10000013, "BML CM4 Code  Download Partition"},
    {0x10000018, "BML CM4 Code  AB Download Partition"},
    {0x10000019, "BML delta NV Partition"},
};

}  // namespace

// Stock flash-partition ids from PARTITIONS.json (cross-checked against the
// registry): fw_dsp_enc = 0x10000005 (LTE DSP), fw_modem_enc = 0x10000006
// (GSM DSP), CM4 A/B = 0x10000013/0x10000018.
const PartitionIdName* find_partition_by_id(uint32_t id) {
  for (const auto& e : kRegistry)
    if (e.id == id) return &e;
  return nullptr;
}

const PartitionIdName* find_partition_by_name(const char* name) {
  if (name == nullptr) return nullptr;
  for (const auto& e : kRegistry)
    if (std::strcmp(e.name, name) == 0) return &e;
  return nullptr;
}

size_t partition_registry_size() { return sizeof(kRegistry) / sizeof(kRegistry[0]); }

// Compile-time coherence checks against the stock getters.
static_assert(kFwLte.dramDest == 0x80448000 && kFwLte.sizeBytes == 0x2e4000,
              "0x80df0c6e/72 must match");
static_assert(kFwGsm.dramDest == 0x80100000 && kFwGsm.sizeBytes == 0x300000,
              "0x80df0c64/68 must match");
static_assert(kFwCm4.dramDest == 0x80020000 && kFwCm4.sizeBytes == 0xc0000,
              "0x80df0c5a/5e must match");
static_assert(kFwLte.entryOffset == 0x400 && kFwGsm.entryOffset == 0x20080,
              "entry offsets per boot flows 0x80e2d9e6 / 0x810b33fe");

}  // namespace dsp
}  // namespace f286
