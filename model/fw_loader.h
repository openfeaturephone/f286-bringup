// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// fw_loader.h - stock firmware-partition loader reconstruction (UMS9117/F286).
//
// Ground truth (all VERIFIED unless noted):
//   0x80c2eee6  - top-level "read DSP/CM4 code partitions into DRAM"
//   0x80df0c5a/5e/64/68/6e/72 - load-list getters (dest VA / size per image)
//   0x8138e890  - BML partition open by name    (returns handle; err codes)
//   0x8138f0ba  - BML partition geometry        (0xd8-byte info struct copy)
//   0x8138e99e  - BML block read                (validates range, -> driver fptr)
//   0x80a09414  - fatal on any failure        (assert/reboot path)
//   partition-name/id registry   - table at VA 0x814cc3ac, stride 0x14,
//                                  {u32 id; const char* name; u32 a,b,c}
//
// Stock flow (0x80c2eee6):
//   1. read chip-id 0x402e00fc; if 0x98180000 open "BML CM4 Code  Download
//      Partition", if 0x98180001 open "BML CM4 Code  AB Download Partition";
//      read dest 0x80020000 size 0xc0000.  (Any other chip-id: skip CM4.)
//   2. if GSM getters non-zero: open "BML GSM DSP Code Partition" and read
//      0x300000 bytes to 0x80100000.
//   3. if LTE getters non-zero: open "BML LTE DSP Code Partition" and read
//      0x2e4000 bytes to 0x80448000.
//   4. any failure -> fatal (stock reboots; it never proceeds half-loaded).
// Reads are BLOCK-granular: count = (size-1)/blockBytes + 1 starting at block 0
// ("chunking" = by BML block; the remainder of the last block is over-read).

#pragma once

#include <cstdint>
#include <cstddef>

#include "boot_hal.h"

namespace f286 {
namespace dsp {

// ---------------------------------------------------------------------------
// Partition registry (table dump at VA 0x814cc3ac, VERIFIED byte-for-byte).
// ---------------------------------------------------------------------------
struct PartitionIdName {
  uint32_t    id;
  const char* name;
};
constexpr PartitionIdName kPartitionRegistry[] = {
    {0x10000001, "BML Downloaded NV Partition"},
    {0x1000000f, "BML PROTECT NV Partition"},
    {0x10000004, "BML MMI Resource Partition"},
    {0x1000001a, "K"},                              // single-letter entries (device letters)
    {0x10000003, "A"},
    {0x10000005, "BML LTE DSP Code Partition"},     // = fw_dsp_enc.bin (PARTITIONS.json)
    {0x00000008, "D"},
    {0x10000006, "BML GSM DSP Code Partition"},     // = fw_modem_enc.bin
    {0x10000007, "C"},
    {0x10000013, "BML CM4 Code  Download Partition"},
    {0x10000018, "BML CM4 Code  AB Download Partition"},
    {0x10000019, "BML delta NV Partition"},
};

// ---------------------------------------------------------------------------
// The load list (stock getters 0x80df0c5a..72, VERIFIED literal returns).
// Entry points: GSM base+0x20080 (boot flow 0x810b33fe), LTE base+0x400
// (boot flow 0x80e2d9e6 writes 0x80448400>>1).  CM4 entry is the WCN driver's
// concern (INFERRED - not traced here).
// ---------------------------------------------------------------------------
struct FwImage {
  const char* partitionName;
  uint32_t    dramDest;    // AP-side DRAM address the image is copied to
  uint32_t    sizeBytes;   // exact payload size; BML reads whole blocks
  uint32_t    entryOffset; // DSP entry point offset within the image
  DspCore     core;        // Gsm / Lte (CM4 marked by dest, handled separately)
};

constexpr FwImage kFwGsm = {"BML GSM DSP Code Partition", 0x80100000, 0x300000, 0x20080,
                            DspCore::Gsm};
constexpr FwImage kFwLte = {"BML LTE DSP Code Partition", 0x80448000, 0x2e4000, 0x400,
                            DspCore::Lte};
constexpr FwImage kFwCm4 = {nullptr /*selected by chip id*/, 0x80020000, 0xc0000, 0,
                            DspCore::Gsm /*placeholder - CM4 boots via WCN driver*/};

inline const char* cm4_partition_name(uint32_t chipId) {
  return (chipId == CHIP_ID_CM4_DOWNLOAD) ? "BML CM4 Code  Download Partition"
         : (chipId == CHIP_ID_CM4_AB)     ? "BML CM4 Code  AB Download Partition"
                                          : nullptr;
}

// ---------------------------------------------------------------------------
// Flash backend.  The stock code talks to the BML (block device layer) with
// named partitions; a Linux driver maps this onto MTD partitions by the same
// names or by the PARTITIONS.json ids (LTE DSP = 0x10000005, GSM DSP =
// 0x10000006, CM4 A/B = 0x10000013/0x10000018).
//
// Stock BML status codes seen in 0x8138e890/0x8138f0ba/0x8138e99e
// (semantics INFERRED from context): 0x80010000 not-initialised, 0x80020000
// out-of-range, 0x80020001 bad handle, 0x80050000/0x80070000/0x800a0000/
// 0x800c0000 open-state errors.
// ---------------------------------------------------------------------------
struct PartGeometry {
  uint32_t blockCount;   // info struct field (0xd8-byte BML info, offsets INFERRED)
  uint32_t blockBytes;   // info struct field
};

class PartitionStore {
 public:
  virtual ~PartitionStore() = default;
  // Open by stock partition NAME; returns false if not found.  `handle` is
  // opaque (stock keeps a small-int handle with refcounting).
  virtual bool     open(const char* name, int* handle) = 0;
  virtual bool     geometry(int handle, PartGeometry* out) = 0;
  // Block-granular read straight into DRAM at `destVa` (stock passes a physical
  // destination straight down to the NAND/DMA driver).
  virtual bool     read_blocks(int handle, uint32_t firstBlock, uint32_t count,
                               uint32_t destVa) = 0;
};

// ---------------------------------------------------------------------------
// Loader.  Mirrors 0x80c2eee6 exactly (order: CM4, GSM, LTE; fatal on error;
// zero as success).
// ---------------------------------------------------------------------------
class FwLoader {
 public:
  FwLoader(DspHw& hw, PartitionStore& store) : hw_(hw), store_(store) {}

  // Full stock sequence.  Returns false only where stock would have continued
  // (i.e. never - stock fatals on failure; we report and let the caller decide).
  bool load_all(uint32_t chipId) {
    bool ok = true;
    if (chipId == CHIP_ID_CM4_DOWNLOAD || chipId == CHIP_ID_CM4_AB)
      ok = load_one(cm4_partition_name(chipId), kFwCm4.dramDest, kFwCm4.sizeBytes);
    ok = ok && load_one(kFwGsm.partitionName, kFwGsm.dramDest, kFwGsm.sizeBytes);
    ok = ok && load_one(kFwLte.partitionName, kFwLte.dramDest, kFwLte.sizeBytes);
    return ok;
  }

  // One partition -> DRAM copy (stock inline block of 0x80c2eee6 + 0x8138e99e
  // range checks).  Reads ceil(size/blockBytes) whole blocks to block 0.
  bool load_one(const char* name, uint32_t destVa, uint32_t sizeBytes) {
    if (name == nullptr || sizeBytes == 0) return false;
    int handle = -1;
    if (!store_.open(name, &handle)) return false;

    PartGeometry geo{};
    if (!store_.geometry(handle, &geo) || geo.blockBytes == 0) return false;
    if (sizeBytes > geo.blockCount * geo.blockBytes) return false;  // stock size check

    const uint32_t blocks = (sizeBytes - 1) / geo.blockBytes + 1;   // stock rounding
    if (!store_.read_blocks(handle, 0, blocks, destVa)) return false;

    // INFERRED (not visible in the stock loader - the BML/NAND DMA path hides
    // cache maintenance): make the image visible to the DSP before release.
    hw_.cache_clean_range(destVa, sizeBytes);
    return true;
  }

  // Convenience for drivers that already have the images in memory (e.g. from
  // fw_dsp_enc.bin / fw_modem_enc.bin files) - NOT stock behaviour, but keeps
  // the same dest/size/entry constants and cache discipline.
  void ram_load(const FwImage& img, const void* data) {
    hw_.copy_to_dram(img.dramDest, data, img.sizeBytes);
    hw_.cache_clean_range(img.dramDest, img.sizeBytes);
  }

 private:
  DspHw&         hw_;
  PartitionStore& store_;
};

}  // namespace dsp
}  // namespace f286
