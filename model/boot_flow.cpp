// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// boot_flow.cpp - NV/calibration item tables + stock timing constants.
// The boot sequences are header-inline in boot_flow.h (each mirrors one short
// stock function); this file carries the tabulated data read from the
// stock code.

#include "boot_flow.h"

namespace f286 {
namespace dsp {

// LTE calibration NV items (0x80c10ee0, LTE_Calibration_NV.c) - VERIFIED
// prefix of the table; the function continues with further band tables beyond
// item 0x18 (tail truncated in this pass).
struct NvItemSpec {
  uint32_t id;
  uint32_t len;
  const char* name;
};
constexpr NvItemSpec kLteCalNvItems[] = {
    {0x4c, 0x90,   "LTE_NV_EUTRA_CUSTOMER_SETTINGS"},
    {0x22, 0x3c,   "LTE_CALI_COMMON"},
    {0x15, 0x2db0, "LTE_CALIBRATIONPARAM_T1_BAND"},
    {0x1a, 0x2d64, "LTE_CALIBRATIONPARAM_T2_BAND"},
    {0x1b, 0x2d24, "LTE_CALIBRATIONPARAM_T3_BAND"},
    {0x1c, 0x25b8, "LTE_CALIBRATIONPARAM_T4_BAND"},
    {0x1d, 0x3b0c, "LTE_CALIBRATIONPARAM_T5_BAND"},
    {0x1e, 0x6a98, "LTE_CALIBRATIONPARAM_T6_BAND"},
    {0x1f, 0x27c4, "LTE_CALIBRATIONPARAM_T7_BAND"},
    {0x20, 0x1f78, "LTE_CALIBRATIONPARAM_T8_BAND"},
    {0x21, 0x18ac, "LTE_CALIBRATIONPARAM_T9_BAND"},
    {0x0b, 0x1b6c, "LTE_CALIBRATIONPARAM_T10_BAND"},
    {0x0c, 0x14c0, "LTE_CALIBRATIONPARAM_T11_BAND"},
    {0x0d, 0x188c, "LTE_CALIBRATIONPARAM_T12_BAND"},
    {0x0e, 0x188c, "LTE_CALIBRATIONPARAM_T13_BAND"},
    {0x0f, 0x188c, "LTE_CALIBRATIONPARAM_T14_BAND"},
    {0x10, 0x2d68, "LTE_CALIBRATIONPARAM_T15_BAND"},
    {0x11, 0x24bc, "LTE_CALIBRATIONPARAM_T16_BAND"},
    {0x12, 0x24a0, "LTE_CALIBRATIONPARAM_T17_BAND"},
    {0x13, 0x2b64, "LTE_CALIBRATIONPARAM_T18_BAND"},
    {0x14, 0x6ab4, "LTE_CALIBRATIONPARAM_T19_BAND"},
    {0x16, 0x6ab4, "LTE_CALIBRATIONPARAM_T20_BAND"},
    {0x17, 0x2d68, "LTE_CALIBRATIONPARAM_T21_BAND"},
    {0x18, 0x3b0c, "LTE_CALIBRATIONPARAM_T22_BAND"},
};

const NvItemSpec* lte_cal_nv_items() { return kLteCalNvItems; }
size_t lte_cal_nv_item_count() { return sizeof(kLteCalNvItems) / sizeof(kLteCalNvItems[0]); }

// Stock timing constants (VERIFIED at the call sites).
constexpr uint32_t kAliveCheckPeriodMs   = 30000;  // 0x80bbf9d0 "LTEDSP Alive Check Timer"
constexpr uint32_t kSleepSyncPollMs      = 10;     // 0x80e2d848 delay between sync polls
constexpr uint32_t kSleepSyncMaxTries    = 100;    // 0x80e2d848 retry budget
constexpr uint32_t kSleepSyncMagic       = 0x22222222;
constexpr uint32_t kVersionRespMaxLen    = 0x80;   // 0x80c109a0 version length assert

static_assert(kSleepSyncMaxTries == 100 && kSleepSyncMagic == 0x22222222,
              "sleep-resume protocol (0x80e2d848)");

}  // namespace dsp
}  // namespace f286
