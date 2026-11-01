// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// codec.h - f286::l1 wire codec: DSP-window ring framing, message packing,
// response/DL frame decoding. Everything byte-exact against the stock
// put/get path (kernel.bin, VA = file_off + 0x80a05e00):
//   put core   0x81181ea4 (entered 0x8118203e kick / 0x8118204a silent)
//   send core  0x80e03c62 (split decision), traced API 0x80e040f6
//   resp pump  0x80c10952 (body 0x80c109a2, layer1_adx_main.c)
// All offsets are LTE-DSP-window relative; AP VA = 0x80400000 + offset.
#pragma once

#include <cstdint>
#include <cstddef>

#include "messages.h"

namespace f286 {
namespace l1 {

// ---------------------------------------------------------------------------
// Window geometry (all VERIFIED -)
// ---------------------------------------------------------------------------
constexpr uint32_t kWindowBase       = 0x80400000;  // AP VA of DSP window 0

// L3 cmd ring ("active ring", ctl ptr held in AP RAM [0x8175dde0])
constexpr uint32_t kCmdRingCtl       = 0x0e400;  // {u16 in; u16 out;}
constexpr uint32_t kCmdEntryBase     = 0x0e404;  // entry[8], stride 8
constexpr uint32_t kCmdEntryCount    = 8;        // &7 wrap [V]

// L2 cmd queue (6 slots @0xE448..) and resp queue (12 slots) - resp is the
// one the driver consumes; the L2 queue is stock-internal.
constexpr uint32_t kRespRingCtl      = 0x0e478;  // {u16 in; u16 out;}
constexpr uint32_t kRespEntryBase    = 0x0e47c;  // entry[12], stride 8 [V]
constexpr uint32_t kRespEntryCount   = 12;       // % 12 (udiv/mls vs r10=0xC) [V]

// Shared message-body area for cmd/resp rings
constexpr uint32_t kBodyBase         = 0x0e4e0;  // [V]
constexpr uint32_t kBodySize         = 0x0c80;   // 3200 B [V]

// DSP resp body area: handlers compute payload = resp_addr + 0xFBEC;
// sanity bound resp_addr + len < 0x100C [V pump 0x80c10a06-0x80c10a12].
constexpr uint32_t kRespBodyBase     = 0x0fbec;
constexpr uint32_t kRespBodySize     = 0x100c;

// Split (big message) body slots: 4 x 0x2800 at win+0x33b000 [V]
constexpr uint32_t kSplitSlotBase    = 0x33b000;
constexpr uint32_t kSplitSlotSize    = 0x2800;
constexpr uint32_t kSplitSlotCount   = 4;

// Message framing
constexpr uint16_t kMarkerInline     = 0x5555;   // payload inline [V]
constexpr uint16_t kMarkerSplit      = 0xaaaa;   // payload in slot, msg+8 = u16 handle [V]
constexpr uint32_t kSplitThreshold   = 0x190;    // len > 0x190 -> split [V 0x80e03c8c]
constexpr uint32_t kMsgHeaderSize    = 8;        // {u16 id; u16 marker; u32 word;} [V]

// Sleep / boot eyes (windows into the DSP's state)
// the sleep-sync cell is LTE window +0x47E14
// (AP 0x80447E14 = HS window 0x80447000 + 0xE14); the old 0x0e14 here was a
// stale hs-base-relative shorthand - a different, never-written cell.
constexpr uint32_t kSleepSyncWord    = 0x47e14;  // == 0x22222222 on resume [V]
constexpr uint32_t kDeactSyncFlag    = 0x047e14; // AP VA 0x80447E14: 0x11111111 -> 0xAAAAAAAA [V]
constexpr uint32_t kDeactSyncDone    = 0xAAAAAAAAu;  // [V]
constexpr uint32_t kDeactSyncArmed   = 0x11111111u;  // [V]

// Pre-release NV staging word: NV9 | tick<<16. the stock
// literal is AP 0x80447E00 = LTE window +0x47E00 (the old 0x3e00 was not a
// window offset; 0x3E00 is the documented LOW POWER AUDIO bank).
constexpr uint32_t kNv9StageWord     = 0x47e00;  // [V 0x80c10e9c]

// ---------------------------------------------------------------------------
// Wire structs (byte-exact, little-endian on the wire)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct CmdRingEntry {        // 8 B, kCmdEntryBase + 8*i   [V]
    uint16_t cmd_id;         // +0
    uint16_t len;            // +2  WHOLE message length (payload + 8)
    uint32_t end_off;        // +4  body-area end offset (see below)
};
static_assert(sizeof(CmdRingEntry) == 8, "ring entry");

struct RespRingEntry {       // 8 B, kRespEntryBase + 8*i   [V]
    uint16_t type;           // +0  dispatch code
    uint16_t len;            // +2  payload bytes
    uint32_t addr;           // +4  offset within kRespBodyBase
};
static_assert(sizeof(RespRingEntry) == 8, "resp entry");

struct MsgHeader {           // 8 B message prefix in the body area  [V]
    uint16_t msg_id;         // +0
    uint16_t marker;         // +2  kMarkerInline | kMarkerSplit
    uint32_t word;           // +4  desc word (0 observed on helper sends)
};
static_assert(sizeof(MsgHeader) == 8, "msg header");

// DSP->AP DL message, as staged by the dispatcher (buffer 0x81aac1f0):
// {u16 card @+0, u16 msgId @+2, payload @+8}  [ header note]
struct DlFrameHeader {
    uint16_t card;           // +0
    uint16_t msg_id;         // +2
    uint16_t rsvd4;          // +4  [I] unpinned gap before payload
    uint16_t rsvd6;          // +6
};
static_assert(sizeof(DlFrameHeader) == 8, "dl header");
#pragma pack(pop)

// Resp type codes (resp pump dispatch, 0x80c109a2 raw disasm)  [V]
enum RespType : uint16_t {
    RESP_DL_MSG     = 0x0,   // -> 0x80e0420c -> 0x80e04244 DL dispatcher
    RESP_CAL        = 0x2,   // -> 0x80b77a12 / 0x80b77aee (cal enabled gate)
    RESP_VERSION    = 0x5,   // version handshake (boot 0x06 answer); <= 0x80 B
    RESP_ASSERT     = 0x7,   // DSP assert -> 0x80e2da30
    RESP_CAL_SIG    = 0xa,   // 8 B forwarded as signal 0x2f
    RESP_STATUS     = 0xb,   // DSP status (u32 table idx -> 0x81083298)
    RESP_POP        = 0xf,   // pop only: out = (out+1) % 12
};

// ---------------------------------------------------------------------------
// Little-endian byte helpers (host-endian agnostic)
// ---------------------------------------------------------------------------
inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline void put_i32(uint8_t* p, int32_t v) { put_u32(p, static_cast<uint32_t>(v)); }
inline uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t get_i32(const uint8_t* p) { return static_cast<int32_t>(get_u32(p)); }
inline int32_t get_i16(const uint8_t* p) { return static_cast<int16_t>(get_u16(p)); }

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

// Ring entry end_off WATERMARK, stock formula [V put core 0x81181edc raw
// disasm 0x81181f46-0x81181f6c]:
//   next_entry_end_off = ((body_off + msg_len) & ~3) + 4  (round-DOWN+4),
//   relative to kBodyBase; wraps to 0 when >= kBodySize. Stock pre-scribes it
//   into slot[(in+1)&7] BEFORE in advances; slot[in].end_off itself carries the
//   RAW body start (see L1Client send/put_entry - the stale slot[in] value
//   doubles as the DSP-consumed watermark).
uint32_t body_end_off(uint32_t cur_body_off, uint32_t msg_len);

// A packed command message HEADER (an earlier version of
// pack_cmd memcpy'd the variable payload into this fixed 12 B object - stack
// smash on every real send). The payload is NEVER staged here: msg is exactly
// the 8-byte {u16 id, u16 marker, u32 word} header plus the 4-byte split tail
// {u16 handle, 2 pad}. The caller writes the payload straight into the ring
// body area after the header (split: into its 0x2800 slot), matching stock's
// checked memcpy 0x81096392 (inline 0x80e03e06-0e / slot 0x80e03d74-7e).
struct PackedCmd {
    uint8_t msg[kMsgHeaderSize + 4];   // header (8 B); split: u16 handle @+8
    uint16_t msg_len;                  // whole message length = payload_len + 8
    uint16_t payload_len;              // as sent (full len; split iff > kSplitThreshold)
    bool split;
    uint16_t handle;                   // valid when split
};

// Pack the message header only (payload NOT copied - see PackedCmd).
// payload_len > kSplitThreshold selects the split framing (marker 0xaaaa,
// msg_len 12, u16 handle at msg+8); otherwise marker 0x5555, msg_len +8.
PackedCmd pack_cmd(uint16_t cmd_id, uint32_t payload_len, uint32_t word,
                   uint16_t split_handle);

// Serialize the typed requests (little-endian, explicit field order).
uint32_t pack_dsp_init(const DspInitReq&, uint8_t* out);
uint32_t pack_dsp_sleep(const DspSleepReq&, uint8_t* out);
uint32_t pack_deactivate(const CphyDeactivateReq&, uint8_t* out);
uint32_t pack_card_info(const CardInfoReq&, uint8_t* out);
uint32_t pack_band_sweep(const BandSweepReq&, uint8_t* out);
// Stock-exact 0x74 scatter: memset 0x148, mode @+2, then for cell i:
// earfcn @16i+4, val @16i+8, spare @16i+0xc, u8 flag @16i+0x10, count @+0x144.
uint32_t pack_sync_search_batch(const SyncSearchBatchReq&, uint8_t* out);
uint32_t pack_sync_cell_small(const SyncCellSmallReq&, uint8_t* out);
uint32_t pack_sync_cell_stop(const SyncCellStopReq&, uint8_t* out);
uint32_t pack_freq_scan(const FreqScanReq&, uint8_t* out);
uint32_t pack_earfcn_band_list(const EarfcnBandListReq&, uint8_t* out);

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Parse a DL message staged frame (as delivered by the dispatcher):
// returns msg id and views into `buf` (buf = payload at kRespBodyBase+addr).
struct DlFrame {
    uint16_t card;
    uint16_t msg_id;
    const uint8_t* body;   // payload bytes (after the 8-byte staged header)
    uint32_t body_len;     // len - 8
};
bool decode_dl(const uint8_t* buf, uint32_t len, DlFrame* out);

bool decode_version(const uint8_t* p, uint32_t len, VersionResp* out);   // resp type 5
bool decode_assert(const uint8_t* p, uint32_t len, DspAssertResp* out);  // resp type 7
void decode_card_info(const uint8_t* p0x1b0, CardInfoCnf* out);          // DL 0x22

}  // namespace l1
}  // namespace f286
