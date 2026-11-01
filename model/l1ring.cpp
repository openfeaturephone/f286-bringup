// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// l1ring.cpp - ring geometry coherence checks + resp-type naming.
// The ring algorithms live header-inline in l1ring.h (they mirror short stock
// loops); this file pins the decoded geometry with static_asserts and provides
// debug-naming helpers.

#include "l1ring.h"

namespace f286 {
namespace dsp {

static_assert(ring::CMD_CTL_OFF == 0xe400 && ring::CMD_ENTRY_OFF == 0xe404,
              "cmd ctl {u16 in,u16 out} then entries at +0xe404 (0x81181d3c/81181edc)");
static_assert(ring::CMD_BODY_OFF == 0xe4e0 && ring::CMD_BODY_SIZE == 0xc80,
              "chained body area 0xe4e0..0xf160, 3200 B (assert text in 0x81181d8c)");
static_assert(ring::RESP_CTL_OFF == 0xe478 && ring::RESP_ENTRY_OFF == 0xe47c,
              "resp pump 0x80c109a0: out@0xe47a, entries {type,len,addr}@0xe47c+8i");
static_assert(ring::SPLIT_BODY_OFF == 0x33b000 && ring::ULDATA_BODY_OFF == 0x345000,
              "alloc type 2/1 bases (0x81181d8c: 0x8073b000 / 0x80745000 abs)");
static_assert(ring::INLINE_MAX == 0x190, "split threshold: payload >= 0x191 (0x80e03c9a)");

const char* resp_type_name(uint16_t type) {
  switch (static_cast<ring::RespType>(type)) {
    case ring::RespType::DlMessage:   return "DL_MESSAGE (0)";
    case ring::RespType::Calibration: return "CALIBRATION (2)";
    case ring::RespType::Version:     return "VERSION/HANDSHAKE (5)";
    case ring::RespType::DspAssert:   return "DSP_ASSERT (7)";
    case ring::RespType::CalibFwd:    return "CALIB_FWD (0xa)";
    case ring::RespType::DspStatus:   return "DSP_STATUS (0xb)";
    case ring::RespType::Pop:         return "POP (0xf)";
    default:                          return "WRONG_RESP_ID";
  }
}

}  // namespace dsp
}  // namespace f286
