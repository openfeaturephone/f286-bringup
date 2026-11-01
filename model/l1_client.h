// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// l1_client.h - f286::l1 client: drives the LTE-DSP cmd ring and consumes the
// resp ring, mirroring the stock pal_sci_adx.c + layer1_adx_main.c mechanics.
//
// Stock behavior being reproduced (all VERIFIED unless noted):
//  - Send: 0x80e040f6(cmdId, payload, card, 0) -> 0x80e03c9a desc build ->
//    0x81181edc ring put. NO MMIO doorbell: the DSP polls the ring. The put's
//    event(0,3) give is an ADDITIONAL wake AFTER the entry is queued (occupancy
//    gate (in-out+8)%8 >= 2 at 0x81181f76-0x81181f8c) - never a refusal.
//    Send gate: refuse ONLY a full ring, i.e.
//    ((in+1)&7)==out (0x80e040da-0x80e040ea) - up to 7 entries in flight.
//  - put end_off model [V 0x81181eee-0x81181f6c]: slot[in].end_off = RAW body
//    start; slot[(in+1)&7].end_off = ((body+len) & ~3) + 4 (round-DOWN+4,
//    0 when >= 0xc80) pre-scribed as the DSP-consumed watermark, then in=(in+1)&7.
//  - Raw direct-put path (0x06/0x0d/0x0e): stock's dedicated boot senders call
//    the put wrappers directly with the RAW body (8/4/8 bytes TOTAL, no
//    {id,marker,word} header) and the cmd id in the ring slot u16, bypassing
//    ul_payload_length (which returns -1 for those ids). See send_raw.
//  - Receive: ADX task loop 0x80c10952 waits a signal, reads ONE resp entry
//    {u16 type, u16 len, u32 addr} @win+0xE47C+8*i, dispatches on type
//    (0=DL msg, 2=cal, 5=VERSION, 7=ASSERT, 0xa=cal sig, 0xb=status, 0xf=pop),
//    then out = (out+1) % 12. Payload = win + 0xFBEC + addr; bound
//    addr+len < 0x100C. Every reception re-arms the 30 s alive timer.
//  - Resp type settlement (FRESH raw disasm 0x80c109b6-0x80c109d6):
//      cmp #7 -> assert handler; cmp 0/2/5 below; 0xa/0xb/0xf above.
//      type 5 = version handshake, type 7 = DSP assert. ('s
//      "type 7 = version" was wrong; correction CONFIRMED.)
//
// HW abstraction: IL1Window maps window-relative offsets (see codec.h
// constants) onto real memory. Backends: /dev/mem mmap, the f286-radio sysfs
// queue binattr ([0x1c00,0x3800) = LTE window +0xe400 region), or a test
// double. The client never touches hardware directly.
#pragma once

#include <cstdint>
#include <cstddef>

#include "codec.h"
#include "messages.h"

namespace f286 {
namespace l1 {

// Memory-window interface. `off` is LTE-DSP-window relative (0 = AP VA
// 0x80400000). Implementations must handle 1/2/4-byte and bulk access.
class IL1Window {
public:
    virtual ~IL1Window() = default;
    virtual bool read(uint32_t off, void* dst, size_t len) = 0;
    virtual bool write(uint32_t off, const void* src, size_t len) = 0;
    // Millisecond sleep for polling loops (driver backend: usleep/msleep).
    virtual void sleep_ms(uint32_t ms) = 0;
    virtual uint32_t now_ms() = 0;
};

enum class RespKind : uint8_t {
    Version,     // type 5 - boot handshake answer (4x u16)
    Assert,      // type 7 - DSP assert
    DlMessage,   // type 0 - routed DL message (CNF/IND)
    Cal,         // type 2
    CalSig,      // type 0xa
    Status,      // type 0xb
    Pop,         // type 0xf
};

struct Response {
    RespKind kind;
    uint16_t resp_type;
    uint16_t len;
    uint32_t addr;                  // offset within resp body area (kRespBodyBase)
    VersionResp version;            // valid when kind == Version
    DspAssertResp assert_;          // valid when kind == Assert
};

struct DlMessage {
    uint16_t card;
    uint16_t msg_id;
    const uint8_t* body;            // points into the window view (see copy_dl)
    uint32_t body_len;
};

class L1Client {
public:
    // Does not own the window.
    void open(IL1Window* win);

    // --- send side -------------------------------------------------------
    // Build + enqueue one command. Payload copied inline when
    // payload_len <= kSplitThreshold; larger payloads go to a split slot
    // (payload written to win[kSplitSlotBase + slot*kSplitSlotSize]) and the
    // message carries the u16 handle. `card` is the DSP card id (0 on F286).
    bool send(uint16_t cmd_id, const void* payload, uint32_t payload_len,
              uint32_t card = 0, uint32_t word = 0);

    // Typed convenience wrappers (payload structs from messages.h).
    bool send_dsp_init();                        // 0x06, 8 zero bytes
    bool send_dsp_sleep(const DspSleepReq&);     // 0x0d
    bool send_deactivate(const CphyDeactivateReq&);  // 0x0e (+ flag handshake in sequences)
    bool send_card_info(const CardInfoReq&);     // 0x9f
    bool send_band_sweep(const BandSweepReq&);   // 0x54
    bool send_sync_search_batch(const SyncSearchBatchReq&);  // 0x74
    bool send_sync_cell_small(const SyncCellSmallReq&);      // 0x73
    bool send_sync_cell_stop(const SyncCellStopReq&);        // 0x75
    bool send_freq_scan(const FreqScanReq&);     // 0x71
    bool send_earfcn_band_list(const EarfcnBandListReq&);    // 0x70
    // Raw direct-put path [V 0x80e2d9bc/0x80e2df1e/0x80e2dfbe]: `raw_body` is
    // the WHOLE message (no 8-byte header), `cmd_id` goes into the ring slot
    // u16, len is hardcoded by the caller (8/4/8 for 0x06/0x0d/0x0e), kick=0.
    // Bypasses ul_payload_length - required for 0x06/0x0d/0x0e (absent from the
    // table, DL-only default -1); do not use for table-sized framed messages.
    bool send_raw(uint16_t cmd_id, const void* raw_body, uint32_t len);

    // --- receive side ----------------------------------------------------
    // Poll the resp ring once; when an entry is pending, dispatch by type,
    // advance `out` (% 12) and return true. `payload` (optional) receives the
    // raw resp body (win + 0xFBEC + addr, `len` bytes).
    bool poll_response(Response* out, uint8_t* payload = nullptr,
                       uint32_t payload_cap = 0);

    // Wait for a specific response kind; timeout in ms (0 = single poll).
    bool wait_kind(RespKind kind, Response* out, uint32_t timeout_ms);

    // Wait for the next DL message with the given msg id; copies up to
    // body_cap bytes into body_out. `dl_card` filters (pass 0xffff for any).
    bool wait_dl(uint16_t msg_id, uint32_t timeout_ms, DlMessage* out,
                 uint8_t* body_out = nullptr, uint32_t body_cap = 0,
                 uint16_t dl_card = 0xffff);

    // Boot handshake primitive: wait version (resp type 5) after cmd 0x06.
    bool wait_version(VersionResp* out, uint32_t timeout_ms = kAliveCheckMs);

    // Non-blocking drain: dispatch Pop/Status/Cal entries, stop at first
    // DL/Version/Assert (returned via `out` if non-null).
    bool pump(Response* out = nullptr, uint8_t* payload = nullptr,
              uint32_t payload_cap = 0);

    // --- state -----------------------------------------------------------
    IL1Window* window() const { return win_; }
    uint16_t cmd_in() const { return cmd_in_; }
    uint16_t cmd_out() const { return cmd_out_; }
    uint16_t resp_out() const { return resp_out_; }
    int in_flight() const { return (cmd_in_ - cmd_out_ + kCmdEntryCount) & 7; }
    uint32_t last_alive_ms() const { return last_alive_ms_; }

private:
    bool read_cmd_ctl();
    bool write_cmd_in();
    bool read_resp_ctl();
    bool write_resp_out();
    // Ring entry + advance, stock-exact put tail [V 0x81181eee-0x81181f6e]:
    // slot[in] = {id, len, RAW start}; round-DOWN+4 watermark into slot[(in+1)&7];
    // body_off_ = watermark; in = (in+1)&7; publish in.
    bool put_entry(uint16_t id, uint16_t len, uint32_t start_off);

    IL1Window* win_ = nullptr;
    uint16_t cmd_in_ = 0;
    uint16_t cmd_out_ = 0;
    uint16_t resp_in_ = 0;
    uint16_t resp_out_ = 0;
    uint16_t next_split_slot_ = 0;   // 4 x 0x2800 body slots, type-2 alloc [V]
    uint16_t next_handle_ = 1;       // u16 handle embedded in split messages
    uint32_t body_off_ = 0;          // current body-area cursor (rel kBodyBase)
    uint32_t last_alive_ms_ = 0;
};

}  // namespace l1
}  // namespace f286
