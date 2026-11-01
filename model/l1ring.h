// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// l1ring.h - AP<->DSP Layer-1 command/response ring reconstruction (UMS9117).
//
// Ground truth (VERIFIED; VAs are kernel.bin
// space = file_off + 0x80a05e00; older reports that used the capstone tool are
// 0x38 LOWER for the same code):
//   0x81181d3c  alloc body (type 0)      VA 0x81181d3c
//   0x81181d8c  alloc dispatch 0/1/2     VA 0x81181d8c
//   0x81181edc  put core                 VA 0x81181edc   (thunks 0x81182076 kick-ok,
//                                                          0x81182082 kick-silent)
//   0x811816f0  slot addr -> handle      VA 0x811816f0
//   0x80e03c9a  message builder/sender   VA 0x80e03c9a   (pal_sci_adx.c)
//   0x80e03f6c  traced send w/ delay ring VA 0x80e03f6c
//   0x80e0412e  log-filtered send entry  VA 0x80e0412e
//   0x80c109a0  resp pump / dispatcher   VA 0x80c109a0   (layer1_adx_main.c)
//   0x80e2ded2  4xu16 cmd sender (0xd)   VA 0x80e2ded2
//
// Transport verdict (VERIFIED): POLL-BASED.  The put path's only "notify" is an
// OS event give (0x811befde(0,3)); no MMIO store to the DSP exists anywhere in
// the enqueue path.  The DSP polls the in/out indices in shared RAM.

#pragma once

#include <cstdint>
#include <cstddef>

#include "boot_hal.h"

namespace f286 {
namespace dsp {

// ---------------------------------------------------------------------------
// Window layout (offsets inside the DSP window: LTE 0x80400000, GSM 0x800e0000).
// All VERIFIED from the code operating on ctl = win+0xe400.
// ---------------------------------------------------------------------------
namespace ring {

constexpr uint32_t CMD_CTL_OFF     = 0xe400;  // {u16 in; u16 out;}  (AP write idx, DSP read idx)
constexpr uint32_t CMD_ENTRY_OFF   = 0xe404;  // entry[i] at +8*i, 8 slots, i = (n)&7
constexpr uint32_t CMD_BODY_OFF    = 0xe4e0;  // chained message body area
constexpr uint32_t CMD_BODY_SIZE   = 0xc80;   // 3200 bytes
constexpr uint32_t RESP_CTL_OFF    = 0xe478;  // {u16 in; u16 out;}  (DSP write idx, AP read idx)
constexpr uint32_t RESP_ENTRY_OFF  = 0xe47c;  // entry[i] at +8*i {u16 type; u16 len; u32 addr}
constexpr uint32_t RESP_SLOTS      = 12;      // VERIFIED live ; stock N from RAM
constexpr uint32_t SPLIT_BODY_OFF  = 0x33b000; // 4 x 0x2800 split-msg body slots (alloc type 2)
constexpr uint32_t SPLIT_SLOT_SIZE = 0x2800;
constexpr uint32_t SPLIT_SLOTS     = 4;
constexpr uint32_t ULDATA_BODY_OFF = 0x345000; // 10 x 0x2800 UL data slots (alloc type 1)
constexpr uint32_t ULDATA_SLOTS    = 10;
constexpr uint32_t DSP_AWAKE_OFF   = 0x47c40;  // u32 awake word; LTE abs 0x80447C40 (VERIFIED load in put)
constexpr uint32_t NV9_CELL_OFF    = 0x47e00;  // u32 nv9|tick<<16; LTE abs 0x80447E00 (VERIFIED store)
constexpr uint32_t SLEEP_SYNC_OFF  = 0x47e14;  // u32 == 0x22222222 on resume; abs 0x80447E14 (VERIFIED)
constexpr uint32_t DUMP_CTL_OFF    = 0x10bf8;  // u32 dump control; 0x11 = wait mode, 0x33 = dump now

// Message header written at the start of every body (VERIFIED 0x80e03c9a).
constexpr uint16_t MARKER_INLINE = 0x5555;  // payload follows in the chained body
constexpr uint16_t MARKER_SPLIT  = 0xaaaa;  // payload in a 0x2800 slot; body+8 = u16 handle
constexpr uint16_t INLINE_MAX    = 0x190;   // payload <= 0x190 inline; >= 0x191 split

// Entry field: end-of-body offset relative to CMD_BODY_OFF, biased +4, wrapped.
// Stock: ((body + len) - 0xe4e0) & ~3, then + 4; if result > 0xc7f -> 0.
// The stale end_off in slot[in] doubles as the DSP-consumed watermark: because a
// producer may never leave more than 7 entries pending, slot[in]'s leftover
// end_off is exactly where the DSP finished consuming (VERIFIED logic in
// 0x81181d3c + 0x81181edc).

// Resp entry `type` values (VERIFIED dispatcher 0x80c109a0).
enum class RespType : uint16_t {
  DlMessage   = 0x0,   // DL message -> 0x80e04244 dispatcher (/)
  Calibration = 0x2,   // cal payload -> 0x80b77a4a (ids 0xa135->0xa136, 0xa158)
  Version     = 0x5,   // handshake: {u16 branch; u16 codeSvn; u16 aonChipId; u16 aonChipVer; char ver[]}
  DspAssert   = 0x7,   // {u32 id; u32 0x8100; u32 a; u32 b} -> assert handler
  CalibFwd    = 0xa,   // 8-byte cal forward -> 0x80b77b84
  DspStatus   = 0xb,   // status report (payload+0xfbec, RF-gated)
  Pop         = 0xf,   // no payload; entry consumed silently
};

#pragma pack(push, 1)
struct MsgHeader {          // 8 bytes, at body start
  uint16_t msgId;
  uint16_t marker;         // MARKER_INLINE / MARKER_SPLIT
  uint32_t word;           // desc->word (observed 0 for helper sends)
};
struct CmdEntry {          // 8 bytes, ring entry
  uint16_t cmdId;          // 0 for the pal_sci path (real id in MsgHeader)
  uint16_t len;            // WHOLE message: payload + 8
  uint32_t endOff;         // end-of-body watermark, see above
};
struct RespEntry {         // 8 bytes at RESP_ENTRY_OFF + 8*i
  uint16_t type;
  uint16_t len;
  uint32_t addr;           // payload offset in the window (bounded by ~0x10c00)
};
struct VersionResp {       // payload of RespType::Version (len <= 0x80)
  uint16_t branch;
  uint16_t codeSvn;
  uint16_t aonChipId;
  uint16_t aonChipVersion;
  // char versionString[] follows (NUL-terminated if len > 8)
};
struct AssertPayload {     // payload of RespType::DspAssert
  uint32_t id;
  uint32_t magic;          // 0x8100
  uint32_t arg0;
  uint32_t arg1;
};
#pragma pack(pop)

static_assert(sizeof(MsgHeader) == 8, "stock layout");
static_assert(sizeof(CmdEntry) == 8, "stock layout");
static_assert(sizeof(RespEntry) == 8, "stock layout");

// DSP assert name-table index (VERIFIED 0x80e2da68).  Name strings live in the
// kernel init-data around VA 0x81717528 (table of pointers, +idx*4):
// "LTEDSP: unknown LTE DSP", "MIPI ADDR ERROR", "NV ERROR:DRV Multimode
// common/reg_30_cfg/init_reg/reg_count", ...  (string bodies at 0x815886xx).
inline uint32_t assert_name_index(uint32_t id, uint32_t magic) {
  if (magic == 0x8100) {
    if (id >= 0x8101 && id <= 0x8108) return 4 + (id - 0x8101);
    return 0xc;
  }
  return (id < 4) ? (id & 0xff) : 3;
}

}  // namespace ring

// ---------------------------------------------------------------------------
// L1CmdRing - producer side + response pump for ONE DSP window.
// Matches the stock behaviour including the invariants:
//   * at most 7 cmd entries in flight (full when ((in+1)&7)==out),
//   * end_off chaining/wrap as documented above,
//   * kick (event give) when the ring drains or the DSP looks asleep,
//   * >0x190 payloads split into the 4 x 0x2800 slot ring, handle in body+8.
// ---------------------------------------------------------------------------
class L1CmdRing {
 public:
  L1CmdRing(DspHw& hw, uint32_t windowVa) : hw_(hw), win_(windowVa) {}

  // ---- state ---------------------------------------------------------------

  uint16_t cmd_in()  const { return hw_.load16(win_ + ring::CMD_CTL_OFF); }
  uint16_t cmd_out() const { return hw_.load16(win_ + ring::CMD_CTL_OFF + 2); }
  bool     cmd_full() const { return ((cmd_in() + 1) & 7) == cmd_out(); }
  uint16_t cmd_pending() const { return (cmd_in() - cmd_out() + 8) & 7; }

  uint32_t awake_word() const { return hw_.load32(win_ + ring::DSP_AWAKE_OFF); }
  uint32_t sleep_sync_word() const { return hw_.load32(win_ + ring::SLEEP_SYNC_OFF); }
  uint32_t dump_ctl() const { return hw_.load32(win_ + ring::DUMP_CTL_OFF); }
  void     request_dump() { hw_.store32(win_ + ring::DUMP_CTL_OFF, 0x33); }  // 0x80e2db28

  // ---- body allocation (0x81181d3c, VERIFIED) ----------------------------

  // Returns the window offset for a new body of `size` bytes, or 0 on overflow
  // (stock asserts "LTE_ADX: alloc too big space from cmd_mem").  Empty ring ->
  // body starts at 0xe4e0; otherwise continue at slot[in]'s end_off + 0xe4e0.
  uint32_t alloc_body(uint32_t size) const {
    if (size >= ring::CMD_BODY_SIZE) return 0;
    if (cmd_in() == cmd_out()) return ring::CMD_BODY_OFF;
    const uint32_t endOff = hw_.load32(win_ + ring::CMD_ENTRY_OFF + 4 + cmd_in() * 8);
    return endOff + ring::CMD_BODY_OFF;
  }

  // ---- put (0x81181edc, VERIFIED) ----------------------------------------

  // `bodyOff`/`len` as produced by alloc_body; `len` = whole message
  // (payload+8), or 8 for the raw fixed-8 commands.  `cmdId` lands in the ring
  // entry (0 for pal_sci messages; 6/0xd for the fixed-8 commands).
  // `allowTrigger` mirrors the stock send path's "extra" argument (0x81182076
  // 4th arg): stock give-predicate (VERIFIED, incl. thunk 0x81182082 which
  // suppresses the first disjunct):
  //   (!allowTrigger || awake==0) || pending > 1   ->  event give (wake trigger)
  // Returns false if the ring is full (stock defers to its delay ring instead).
  bool put(uint16_t cmdId, uint32_t bodyOff, uint16_t len, bool allowTrigger = true) {
    if (cmd_full()) return false;

    const uint32_t entryVa = win_ + ring::CMD_ENTRY_OFF + cmd_in() * 8;
    hw_.store16(entryVa, cmdId);
    hw_.store16(entryVa + 2, len);

    // Stock put end_off model (-2, 0x81181eee-0x81181f6c):
    // TWO stores - slot[in].end_off = RAW body start (arg1 - 0xe4e0); the
    // round-DOWN+4 watermark ((body+len) & ~3, +4, 0 past 0xc7f) goes into
    // slot[(in+1)&7].end_off as the DSP-consumed mark; then in = (in+1)&7.
    hw_.store32(entryVa + 4, bodyOff - ring::CMD_BODY_OFF);
    uint32_t end = (bodyOff + len - ring::CMD_BODY_OFF) & ~3u;
    end += 4;
    if (end >= ring::CMD_BODY_SIZE) end = 0;
    hw_.store32(win_ + ring::CMD_ENTRY_OFF + ((cmd_in() + 1) & 7) * 8 + 4, end);

    hw_.store16(win_ + ring::CMD_CTL_OFF, static_cast<uint16_t>((cmd_in() + 1) & 7));

    if (!allowTrigger || awake_word() == 0 || cmd_pending() > 1) hw_.event_give(3);
    return true;
  }

  // ---- split-slot ring (0x81181d8c type 2 + 0x811816f0, VERIFIED) ------

  // Allocates a split-payload slot, copies `payload`, returns its u16 handle.
  // Stock keeps the round-robin index in AP RAM ([0x8175dda2], mod 4).
  // NOTE: the stock handle formula (0x811816f0) is anchored to the ABSOLUTE
  // LTE base 0x8073b000 = LTE window + 0x33b000; the split path is therefore
  // LTE-only (asserts "ulAddr >= g_lte_start + 0x32C000 + 0xF000").
  uint16_t split_alloc_put(const void* payload, uint16_t len, uint8_t* slotIndex) {
    uint8_t next = slotIndex ? *slotIndex : 0;
    const uint8_t idx = next++ & 3;
    if (slotIndex) *slotIndex = next;
    const uint32_t slotVa = win_ + ring::SPLIT_BODY_OFF + idx * ring::SPLIT_SLOT_SIZE;
    hw_.copy_to_dram(slotVa, payload, len);
    hw_.cache_clean_range(slotVa, len);
    // 0x811816f0: handle = (addr + 0x7f8c5000) / 0x2800 & 0xff
    //              = (addr - 0x8073b000) / 0x2800  (mod 2^32)  ->  idx for LTE.
    const uint32_t handle =
        ((slotVa + 0x7f8c5000u) / ring::SPLIT_SLOT_SIZE) & 0xffu;
    return static_cast<uint16_t>(handle);
  }

  // ---- high-level send (0x80e03c9a / 0x80e03f6c, VERIFIED) --------------

  // Builds {msgId, marker, word, payload} and enqueues on the cmd ring.
  // Returns false when the ring is full (stock queues to the AP-RAM delay ring
  // 0x818e916c and flushes later from the LGRANT task - a Linux driver can
  // simply retry; the DSP polls).
    bool send_message(uint16_t msgId, const void* payload, uint16_t payloadLen,
                      uint32_t word = 0, uint8_t* splitIndex = nullptr) {
    // the ring-entry len is the WHOLE message length the
    // stock send-core builds: (split ? 4 : payloadLen) + 8 = 12 for split
    // (0x80e03c9a: len = (split?4:payload)+8; l1_client.cpp
    // already did this). The old payloadLen+8 over-reserved the 0xC80 body
    // area via the end_off watermark and handed the DSP a wrong entry.len.
    const uint16_t wholeLen = static_cast<uint16_t>(
        (payloadLen >= ring::INLINE_MAX + 1 ? 4 : payloadLen) +
        sizeof(ring::MsgHeader));
    const uint32_t bodyOff = alloc_body(wholeLen);
    if (bodyOff == 0) return false;

    const uint32_t bodyVa = win_ + bodyOff;
    ring::MsgHeader hdr{msgId, 0, word};
    const void* copySrc = payload;
    uint16_t copyLen = payloadLen;

    if (payloadLen >= ring::INLINE_MAX + 1) {  // >= 0x191 -> split
      hdr.marker = ring::MARKER_SPLIT;
      uint8_t idx = splitIndex ? *splitIndex : 0;
      const uint16_t handle = split_alloc_put(payload, payloadLen, &idx);
      if (splitIndex) *splitIndex = idx;
      splitHandle_[0] = handle;
      splitHandle_[1] = 0;
      copySrc = splitHandle_;
      copyLen = 4;  // u16 handle + pad
    } else {
      hdr.marker = ring::MARKER_INLINE;
    }

    hw_.copy_to_dram(bodyVa, &hdr, sizeof(hdr));
    if (copyLen) hw_.copy_to_dram(bodyVa + sizeof(ring::MsgHeader), copySrc, copyLen);
    hw_.cache_clean_range(bodyVa, wholeLen);

    // cmdId = 0 in the entry (VERIFIED: the builder always enqueues with 0;
    // the DSP reads the real id from MsgHeader.msgId).
    return put(0, bodyOff, wholeLen, true);
  }

  // Fixed 8-byte raw command - the boot cmd 6 (dsplog) and cmd 0xd (deep-sleep)
  // shape.  VERIFIED 0x80e2d8d6 / 0x80e2ded2: lock -> alloc(0, 8) ->
  // memcpy 8 bytes -> send(cmdId, body, len=8, 0) -> unlock.
  // NOTE: unlike the pal_sci path there is NO MsgHeader here - the body IS the
  // 8 raw payload bytes and the ring entry carries the real cmdId with len=8.
  bool send_fixed8(uint16_t cmdId, const uint8_t payload[8]) {
    const uint32_t bodyOff = alloc_body(8);
    if (bodyOff == 0) return false;
    hw_.copy_to_dram(win_ + bodyOff, payload, 8);
    hw_.cache_clean_range(win_ + bodyOff, 8);
    return put(cmdId, bodyOff, 8, true);
  }

  // ---- response pump (0x80c109a0, VERIFIED) -------------------------------

  struct Sink {
    virtual ~Sink() = default;
    virtual void on_dl_message(uint32_t /*addrVa*/, uint16_t /*len*/) {}
    virtual void on_calibration(uint32_t /*addrVa*/, uint16_t /*len*/) {}
    virtual void on_version(const ring::VersionResp& /*v*/, const char* /*str*/, uint16_t /*len*/) {}
    virtual void on_assert(const ring::AssertPayload& /*a*/, uint32_t /*nameIdx*/) {}
    virtual void on_calib_fwd(uint32_t /*addrVa*/) {}
    virtual void on_dsp_status(uint32_t /*addrVa*/) {}
  };

  // Walks the resp ring from `out`, dispatching each entry to `sink`, and
  // advances `out` (mod 12).  `active` entries are consumed in order; returns
  // the number of entries handled.  Stock advances out for EVERY recognised
  // type including 0xf (pop-only).
  int pump(Sink& sink) {
    int handled = 0;
    while (resp_out() != resp_in()) {
      const uint32_t entryVa = win_ + ring::RESP_ENTRY_OFF + resp_out() * 8;
      ring::RespEntry e{};
      e.type = hw_.load16(entryVa);
      e.len = hw_.load16(entryVa + 2);
      e.addr = hw_.load32(entryVa + 4);
      const uint32_t payloadVa = win_ + e.addr;

      switch (static_cast<ring::RespType>(e.type)) {
        case ring::RespType::DlMessage:
          sink.on_dl_message(payloadVa, e.len);
          break;
        case ring::RespType::Calibration:
          sink.on_calibration(payloadVa, e.len);
          break;
        case ring::RespType::Version: {
          ring::VersionResp v{};
          v.branch = hw_.load16(payloadVa);
          v.codeSvn = hw_.load16(payloadVa + 2);
          v.aonChipId = hw_.load16(payloadVa + 4);
          v.aonChipVersion = hw_.load16(payloadVa + 6);
          versionString_[0] = '\0';
          if (e.len > 8 && e.len <= 0x80) {
            int n = e.len - 8;
            if (n > kVerMax - 1) n = kVerMax - 1;
            for (int i = 0; i < n; ++i) versionString_[i] = static_cast<char>(hw_.load8(payloadVa + 8 + i));
            versionString_[n] = '\0';
          }
          sink.on_version(v, versionString_, e.len);
          break;
        }
        case ring::RespType::DspAssert: {
          ring::AssertPayload a{};
          a.id = hw_.load32(payloadVa);
          a.magic = hw_.load32(payloadVa + 4);
          a.arg0 = hw_.load32(payloadVa + 8);
          a.arg1 = hw_.load32(payloadVa + 12);
          sink.on_assert(a, ring::assert_name_index(a.id, a.magic));
          break;
        }
        case ring::RespType::CalibFwd:
          sink.on_calib_fwd(payloadVa);
          break;
        case ring::RespType::DspStatus:
          sink.on_dsp_status(payloadVa);
          break;
        case ring::RespType::Pop:
          break;
        default:
          // stock: "LTE_ADX: WRONG RESP_ID: %d" then assert
          hw_.store16(win_ + ring::RESP_CTL_OFF + 2,
                      static_cast<uint16_t>((resp_out() + 1) % ring::RESP_SLOTS));
          return handled;
      }
      hw_.store16(win_ + ring::RESP_CTL_OFF + 2,
                  static_cast<uint16_t>((resp_out() + 1) % ring::RESP_SLOTS));
      ++handled;
    }
    return handled;
  }

  uint16_t resp_in()  const { return hw_.load16(win_ + ring::RESP_CTL_OFF); }
  uint16_t resp_out() const { return hw_.load16(win_ + ring::RESP_CTL_OFF + 2); }

 private:
  static constexpr int kVerMax = 96;
  DspHw&   hw_;
  uint32_t win_;
  uint16_t splitHandle_[2] = {0, 0};
  char     versionString_[kVerMax] = {0};
};

}  // namespace dsp
}  // namespace f286
