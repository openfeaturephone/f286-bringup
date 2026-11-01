// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// l1_client.cpp - ring driver. Mechanics transcribed from the stock firmware:
//   put core   0x81181ea4 (via 0x8118203e)   - entry write, in = (in+1)&7
//   send core  0x80e03c62 / API 0x80e040f6 - marker + split decision
//   resp pump  0x80c10952 (body 0x80c109a2)  - type switch + out = (out+1)%12
// No doorbell exists : the DSP polls the rings.
#include "l1_client.h"

#include <cstring>

namespace f286 {
namespace l1 {

namespace {
constexpr uint16_t kRingMask7 = 7;
constexpr uint32_t kPollIntervalMs = 5;
}  // namespace

void L1Client::open(IL1Window* win) {
    win_ = win;
    cmd_in_ = cmd_out_ = resp_in_ = resp_out_ = 0;
    body_off_ = 0;
    next_split_slot_ = 0;
    next_handle_ = 1;
    last_alive_ms_ = win_ ? win_->now_ms() : 0;
    if (win_) {
        // Adopt the rings' current indices if the DSP/boot already set them.
        uint8_t ctl[4];
        if (win_->read(kCmdRingCtl, ctl, 4)) {
            cmd_in_ = get_u16(ctl + 0);
            cmd_out_ = get_u16(ctl + 2);
        }
        uint8_t rctl[4];
        if (win_->read(kRespRingCtl, rctl, 4)) {
            resp_in_ = get_u16(rctl + 0);
            resp_out_ = get_u16(rctl + 2);
        }
    }
}

// ---------------------------------------------------------------------------
// send side
// ---------------------------------------------------------------------------

bool L1Client::read_cmd_ctl() {
    uint8_t ctl[4];
    if (!win_->read(kCmdRingCtl, ctl, 4)) return false;
    cmd_in_ = get_u16(ctl + 0);
    cmd_out_ = get_u16(ctl + 2);
    return true;
}

bool L1Client::write_cmd_in() {
    uint8_t ctl[2];
    put_u16(ctl, cmd_in_);
    return win_->write(kCmdRingCtl, ctl, 2);
}

bool L1Client::read_resp_ctl() {
    uint8_t ctl[4];
    if (!win_->read(kRespRingCtl, ctl, 4)) return false;
    resp_in_ = get_u16(ctl + 0);
    resp_out_ = get_u16(ctl + 2);
    return true;
}

bool L1Client::write_resp_out() {
    uint8_t ctl[2];
    put_u16(ctl, resp_out_);
    return win_->write(kRespRingCtl + 2, ctl, 2);
}

bool L1Client::put_entry(uint16_t id, uint16_t len, uint32_t start_off) {
    // Stock put tail [V 0x81181eee-0x81181f6e]: TWO stores.
    // 1. slot[in].end_off = RAW body start (arg1 - 0xe4e0) [0x81181efe-04].
    // 2. slot[(in+1)&7].end_off = ((body+len) & ~3) + 4, 0 when >= 0xc80
    //    (round-DOWN+4 watermark, DSP-consumed mark) [0x81181f46-6c];
    //    then in = (in+1)&7, published at 0x81181f6e.
    const uint32_t watermark = body_end_off(start_off, len);
    uint8_t eb[8];
    put_u16(eb + 0, id);
    put_u16(eb + 2, len);
    put_u32(eb + 4, start_off);
    if (!win_->write(kCmdEntryBase + 8u * (cmd_in_ & kRingMask7), eb, 8))
        return false;
    uint8_t wb[4];
    put_u32(wb, watermark);
    if (!win_->write(kCmdEntryBase + 8u * ((cmd_in_ + 1) & kRingMask7) + 4, wb, 4))
        return false;
    body_off_ = watermark;
    cmd_in_ = static_cast<uint16_t>((cmd_in_ + 1) & kRingMask7);
    return write_cmd_in();
}

bool L1Client::send(uint16_t cmd_id, const void* payload, uint32_t payload_len,
                    uint32_t card, uint32_t word) {
    (void)card;  // send-core arg 2 is a kick-mode flag, card rides as stack arg
                 // 5 into put's event path [-10]; F286 = 0.
    if (!win_) return false;
    if (!read_cmd_ctl()) return false;

    const uint16_t wire_len = ul_payload_length(cmd_id);
    if (wire_len == 0) return false;  // not a UL id (DL-only / unknown);
                                      // raw ids 0x06/0x0d/0x0e go via send_raw
    // The DSP reads exactly wire_len bytes; caller must supply that much.
    if (payload_len < wire_len) return false;

    // Send gate [V 0x80e040da-0x80e040ea]: refuse ONLY a full ring
    // ((in+1)&7 == out) - stock queues up to 7 in flight.
    if (((cmd_in_ + 1) & kRingMask7) == cmd_out_) return false;

    const bool split = wire_len > kSplitThreshold;
    const PackedCmd cmd = pack_cmd(cmd_id, wire_len, word, next_handle_);
    // Split msg_len = 8 + 4 (handle tail), matching stock r5=4 ->
    // add.w r1, r5, #8 = 12-byte message [V 0x80e03c9c].

    // 1. write the 8-byte header at the cursor, payload right after it -
    //    NO intermediate staging buffer (stock memcpy 0x81096392 fills
    //    the body area directly: inline 0x80e03e06-0e, slot 0x80e03d74-7e).
    if (body_off_ + cmd.msg_len > kBodySize) body_off_ = 0;  // [V 0-wrap]
    if (!win_->write(kBodyBase + body_off_, cmd.msg, kMsgHeaderSize)) return false;
    if (split) {
        // 2a. inline tail = the 4-byte handle field (stock memcpy(desc+8, sp+8, 4)
        //     [V 0x80e03dae-0x80e03e0a]), payload into its 0x2800 slot.
        if (!win_->write(kBodyBase + body_off_ + kMsgHeaderSize,
                         cmd.msg + kMsgHeaderSize, 4))
            return false;
        const uint32_t slot_off = kSplitSlotBase + next_split_slot_ * kSplitSlotSize;
        if (!win_->write(slot_off, payload, wire_len)) return false;
        next_split_slot_ = static_cast<uint16_t>((next_split_slot_ + 1) % kSplitSlotCount);
        next_handle_ = static_cast<uint16_t>(next_handle_ + 1);
    } else if (wire_len) {
        if (!win_->write(kBodyBase + body_off_ + kMsgHeaderSize, payload, wire_len))
            return false;
    }

    // 3. ring entry + advance [V 0x81181edc]
    return put_entry(cmd_id, cmd.msg_len, body_off_);
    // No doorbell store: DSP polls [V].
}

bool L1Client::send_raw(uint16_t cmd_id, const void* raw_body, uint32_t len) {
    // Stock direct-put path for the dedicated boot senders:
    //   0x06 0x80e2d9bc-dbe put(6, raw8, 8, kick=0); 0x0d 0x80e2df1e-f20
    //   put(0xd, raw8, 8); 0x0e 0x80e2dfbe-c0 put(0xe, raw4, 4).
    // The body IS the raw message (no {id,marker,word} header); the cmd id
    // travels in the ring slot u16; ul_payload_length is NOT consulted
    // (it returns -1 for these ids - stock table default group).
    if (!win_ || !raw_body || len == 0 || len > 0xFFFFu) return false;
    if (!read_cmd_ctl()) return false;
    if (((cmd_in_ + 1) & kRingMask7) == cmd_out_) return false;  // full

    if (body_off_ + len > kBodySize) body_off_ = 0;  // [V 0-wrap]
    if (!win_->write(kBodyBase + body_off_, raw_body, len)) return false;
    return put_entry(cmd_id, static_cast<uint16_t>(len), body_off_);
}

bool L1Client::send_dsp_init() {
    // [V boot tail 0x80e2d9bc] RAW 8-byte body = the zero template (init-data
    // 0x81717514), direct put kick=0 - not a framed message.
    DspInitReq r{};
    uint8_t b[8];
    pack_dsp_init(r, b);
    return send_raw(CMD_DSP_INIT, b, sizeof(b));
}

bool L1Client::send_dsp_sleep(const DspSleepReq& r) {
    uint8_t b[8];
    pack_dsp_sleep(r, b);
    return send_raw(CMD_DSP_SLEEP, b, sizeof(b));   // [V 0x80e2df1e]
}

bool L1Client::send_deactivate(const CphyDeactivateReq& r) {
    uint8_t b[4];
    pack_deactivate(r, b);
    return send_raw(CMD_CPHY_DEACTIVATE_REQ, b, sizeof(b));  // [V 0x80e2dfbe]
}

bool L1Client::send_card_info(const CardInfoReq& r) {
    uint8_t b[4];
    pack_card_info(r, b);
    return send(CMD_CPHY_CARD_INFO_REQ, b, sizeof(b));
}

bool L1Client::send_band_sweep(const BandSweepReq& r) {
    uint8_t b[sizeof(BandSweepReq)];
    pack_band_sweep(r, b);
    return send(CMD_CPHY_BAND_SWEEP_REQ, b, sizeof(b));
}

bool L1Client::send_sync_search_batch(const SyncSearchBatchReq& r) {
    uint8_t b[SyncSearchBatchReq::kWireSize];
    pack_sync_search_batch(r, b);
    return send(CMD_CPHY_SYNC_SEARCH_BATCH_REQ, b, sizeof(b));
}

bool L1Client::send_sync_cell_small(const SyncCellSmallReq& r) {
    uint8_t b[8];
    pack_sync_cell_small(r, b);
    return send(CMD_SYNC_CELL_SMALL_REQ, b, sizeof(b));
}

bool L1Client::send_sync_cell_stop(const SyncCellStopReq& r) {
    uint8_t b[4];
    pack_sync_cell_stop(r, b);
    return send(CMD_CPHY_SYNC_CELL_STOP_REQ, b, sizeof(b));
}

bool L1Client::send_freq_scan(const FreqScanReq& r) {
    uint8_t b[FreqScanReq::kWireSize];
    pack_freq_scan(r, b);
    return send(CMD_FREQ_SCAN_REQ, b, sizeof(b));
}

bool L1Client::send_earfcn_band_list(const EarfcnBandListReq& r) {
    uint8_t b[EarfcnBandListReq::kWireSize];
    pack_earfcn_band_list(r, b);
    return send(CMD_EARFCN_BAND_LIST_REQ, b, sizeof(b));
}

// ---------------------------------------------------------------------------
// receive side
// ---------------------------------------------------------------------------

bool L1Client::poll_response(Response* out, uint8_t* payload, uint32_t payload_cap) {
    if (!win_) return false;
    if (!read_resp_ctl()) return false;
    if (resp_in_ == resp_out_) return false;  // empty

    const uint16_t i = static_cast<uint16_t>(resp_out_ % kRespEntryCount);
    uint8_t eb[8];
    if (!win_->read(kRespEntryBase + 8u * i, eb, 8)) return false;
    RespRingEntry e;
    e.type = get_u16(eb + 0);
    e.len = get_u16(eb + 2);
    e.addr = get_u32(eb + 4);

    // Sanity bound [V pump 0x80c10a06: addr + len < 0x100C].
    if (static_cast<uint64_t>(e.addr) + e.len >= kRespBodySize) {
        // Stock asserts and still pops; do the same minus the assert.
        e.len = 0;
    }

    uint8_t body[kRespBodySize];
    if (e.len && e.len <= kRespBodySize) {
        if (!win_->read(kRespBodyBase + e.addr, body, e.len)) return false;
    }

    if (out) {
        out->kind = RespKind::Pop;
        out->resp_type = e.type;
        out->len = e.len;
        out->addr = e.addr;
        switch (e.type) {
        case RESP_DL_MSG:  out->kind = RespKind::DlMessage; break;
        case RESP_CAL:     out->kind = RespKind::Cal; break;
        case RESP_VERSION: out->kind = RespKind::Version; break;
        case RESP_ASSERT:  out->kind = RespKind::Assert; break;
        case RESP_CAL_SIG: out->kind = RespKind::CalSig; break;
        case RESP_STATUS:  out->kind = RespKind::Status; break;
        default:           out->kind = RespKind::Pop; break;  // incl. WRONG RESP_ID path
        }
        if (out->kind == RespKind::Version)
            decode_version(body, e.len, &out->version);
        if (out->kind == RespKind::Assert)
            decode_assert(body, e.len, &out->assert_);
    }
    if (payload && payload_cap && e.len) {
        uint32_t n = (e.len < payload_cap) ? e.len : payload_cap;
        std::memcpy(payload, body, n);
    }

    // Pop: out = (out + 1) % 12 [V udiv/mls vs r10=0xC].
    resp_out_ = static_cast<uint16_t>((resp_out_ + 1) % kRespEntryCount);
    write_resp_out();
    last_alive_ms_ = win_->now_ms();  // every reception re-arms the alive timer
    return true;
}

bool L1Client::pump(Response* out, uint8_t* payload, uint32_t payload_cap) {
    for (;;) {
        Response r;
        if (!poll_response(&r, payload, payload_cap)) return false;
        if (r.kind == RespKind::Pop || r.kind == RespKind::Status ||
            r.kind == RespKind::CalSig || r.kind == RespKind::Cal)
            continue;  // housekeeping types: keep walking
        if (out) *out = r;
        return true;
    }
}

bool L1Client::wait_kind(RespKind kind, Response* out, uint32_t timeout_ms) {
    uint32_t deadline = win_->now_ms() + timeout_ms;
    for (;;) {
        Response r;
        if (poll_response(&r, nullptr, 0)) {
            if (r.kind == kind) {
                if (out) *out = r;
                return true;
            }
            continue;  // other types: keep the ring moving
        }
        if (timeout_ms && static_cast<int32_t>(deadline - win_->now_ms()) <= 0)
            return false;
        win_->sleep_ms(kPollIntervalMs);
    }
}

bool L1Client::wait_dl(uint16_t msg_id, uint32_t timeout_ms, DlMessage* out,
                       uint8_t* body_out, uint32_t body_cap, uint16_t dl_card) {
    uint32_t deadline = win_->now_ms() + timeout_ms;
    for (;;) {
        Response r;
        uint8_t body[kRespBodySize];
        if (poll_response(&r, body, sizeof(body))) {
            if (r.kind == RespKind::DlMessage && r.len >= 8) {
                DlFrame f;
                if (decode_dl(body, r.len, &f) && f.msg_id == msg_id &&
                    (dl_card == 0xffff || f.card == dl_card)) {
                    if (out) {
                        out->card = f.card;
                        out->msg_id = f.msg_id;
                        out->body_len = f.body_len;
                        out->body = nullptr;
                    }
                    if (body_out && body_cap) {
                        uint32_t n = (f.body_len < body_cap) ? f.body_len : body_cap;
                        std::memcpy(body_out, f.body, n);
                        if (out) out->body = body_out;
                    }
                    return true;
                }
            }
            continue;
        }
        if (timeout_ms && static_cast<int32_t>(deadline - win_->now_ms()) <= 0)
            return false;
        win_->sleep_ms(kPollIntervalMs);
    }
}

bool L1Client::wait_version(VersionResp* out, uint32_t timeout_ms) {
    Response r;
    if (!wait_kind(RespKind::Version, &r, timeout_ms)) return false;
    if (out) *out = r.version;
    return true;
}

}  // namespace l1
}  // namespace f286
