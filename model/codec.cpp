// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// codec.cpp - wire pack/unpack, byte-exact vs the stock firmware.
#include "codec.h"

#include <cstring>

namespace f286 {
namespace l1 {

uint32_t body_end_off(uint32_t cur_body_off, uint32_t msg_len) {
    // [V put core 0x81181edc raw disasm 0x81181f46-0x81181f6c]: watermark =
    // ((body + len) - 0xe4e0) & ~3 (round-DOWN), then + 4; 0 when >= 0xc80.
    // Written into slot[(in+1)&7].end_off as the DSP-consumed mark; slot[in]
    // itself gets the RAW body start (l1ring.h model).
    uint32_t end = (cur_body_off + msg_len) & ~3u;
    end += 4u;
    if (end >= kBodySize) end = 0;
    return end;
}

PackedCmd pack_cmd(uint16_t cmd_id, uint32_t payload_len, uint32_t word,
                   uint16_t split_handle) {
    // [V send core 0x80e03c9a raw disasm]: framing {u16 id @+0, u16 marker @+2,
    // u32 word @+4, payload @+8}; whole len > 0x190 (cmp #0x190 / bls) ->
    // split: marker 0xaaaa, msg_len = 12, msg+8 = {u16 handle, 2 pad bytes};
    // else inline: marker 0x5555, msg_len = payload_len + 8.
    // the payload is never copied into this object -
    // L1Client::send writes it directly into the ring body area / split slot.
    PackedCmd out{};
    out.payload_len = static_cast<uint16_t>(payload_len);
    std::memset(out.msg, 0, sizeof(out.msg));
    put_u16(out.msg + 0, cmd_id);
    put_u32(out.msg + 4, word);
    if (payload_len > kSplitThreshold) {
        out.split = true;
        out.handle = split_handle;
        out.msg_len = static_cast<uint16_t>(kMsgHeaderSize + 4);
        put_u16(out.msg + 2, kMarkerSplit);
        put_u16(out.msg + 8, split_handle);
    } else {
        out.split = false;
        out.handle = 0;
        out.msg_len = static_cast<uint16_t>(kMsgHeaderSize + payload_len);
        put_u16(out.msg + 2, kMarkerInline);
    }
    return out;
}

uint32_t pack_dsp_init(const DspInitReq& r, uint8_t* out) {
    put_u16(out + 0, r.p0); put_u16(out + 2, r.p1);
    put_u16(out + 4, r.p2); put_u16(out + 6, r.p3);
    return 8;
}

uint32_t pack_dsp_sleep(const DspSleepReq& r, uint8_t* out) {
    put_u16(out + 0, r.p0); put_u16(out + 2, r.p1);
    put_u16(out + 4, r.p2); put_u16(out + 6, r.p3);
    return 8;
}

uint32_t pack_deactivate(const CphyDeactivateReq& r, uint8_t* out) {
    put_u16(out + 0, r.reserved);
    put_u16(out + 2, r.marker);
    return 4;
}

uint32_t pack_card_info(const CardInfoReq& r, uint8_t* out) {
    put_u32(out, r.opaque);
    return 4;
}

uint32_t pack_band_sweep(const BandSweepReq& r, uint8_t* out) {
    // Field order [V builder 0x80a2435e-0x80a2438c].
    put_u32(out + 0x00, r.word_00);
    put_u32(out + 0x04, r.word_04);
    put_u32(out + 0x08, r.one_08);
    put_u32(out + 0x0c, r.word_0c);
    put_u32(out + 0x10, r.word_10);
    put_u32(out + 0x14, r.word_14);
    put_u32(out + 0x18, r.word_18);
    put_u32(out + 0x1c, 0);
    put_u32(out + 0x20, 0);
    put_u32(out + 0x24, 0);
    put_u32(out + 0x28, 0);
    return sizeof(BandSweepReq);
}

uint32_t pack_sync_search_batch(const SyncSearchBatchReq& r, uint8_t* out) {
    // [V builder 0x80ee66f8 raw disasm] byte-exact reproduction:
    //   memset(payload, 0, 0x148); mode u16 @+2;
    //   per iteration i: [16i+4]=earfcn, [16i+8]=val(-1 kept), [16i+0xc]=spare,
    //                    [16i+0x10]=u8 flag (lands at slot i+1 base);
    //   count u32 @+0x144.
    std::memset(out, 0, SyncSearchBatchReq::kWireSize);
    put_u16(out + 0x02, r.mode);
    const int n = (r.count > SyncSearchBatchReq::kMaxCells)
                      ? SyncSearchBatchReq::kMaxCells : static_cast<int>(r.count);
    for (int i = 0; i < n; ++i) {
        const SyncSearchBatchReq::Cell& c = r.cells[i];
        put_u32(out + 16 * i + 0x04, c.earfcn);
        put_i32(out + 16 * i + 0x08, c.key);    // stock keeps -1 as-is
        put_u32(out + 16 * i + 0x0c, c.spare);
        out[16 * i + 0x10] = c.flag8;   // stock strh of a zero-extended u8
    }
    put_u32(out + 0x144, static_cast<uint32_t>(n));
    return SyncSearchBatchReq::kWireSize;
}

uint32_t pack_sync_cell_small(const SyncCellSmallReq& r, uint8_t* out) {
    put_u16(out + 0, r.reserved);
    put_u16(out + 2, r.flags);
    put_u32(out + 4, r.earfcn);   // [V builder 0x80ee6592]
    return 8;
}

uint32_t pack_sync_cell_stop(const SyncCellStopReq& r, uint8_t* out) {
    put_u32(out, r.opaque);
    return 4;
}

uint32_t pack_freq_scan(const FreqScanReq& r, uint8_t* out) {
    std::memset(out, 0, FreqScanReq::kWireSize);
    put_u16(out + 0x00, r.reserved);
    put_u16(out + 0x02, r.earfcn);   // [V]
    put_u16(out + 0x04, r.count);    // [V]
    for (int i = 0; i < FreqScanReq::kMaxPairs; ++i) {
        put_u16(out + 6 + 4 * i, r.pairs[i].a);
        put_u16(out + 6 + 4 * i + 2, r.pairs[i].b);
    }
    return FreqScanReq::kWireSize;
}

uint32_t pack_earfcn_band_list(const EarfcnBandListReq& r, uint8_t* out) {
    std::memset(out, 0, EarfcnBandListReq::kWireSize);
    put_u16(out + 0x00, r.reserved);
    put_u16(out + 0x02, r.earfcn);   // [V]
    put_u16(out + 0x04, r.count);    // [V]
    for (int i = 0; i < EarfcnBandListReq::kMaxPairs; ++i) {
        put_u16(out + 6 + 4 * i, r.pairs[2 * i]);
        put_u16(out + 6 + 4 * i + 2, r.pairs[2 * i + 1]);
    }
    out[0xaa] = r.flag_aa;           // [V]
    put_u32(out + 0xac, r.word_ac);  // [V]
    return EarfcnBandListReq::kWireSize;
}

bool decode_dl(const uint8_t* buf, uint32_t len, DlFrame* out) {
    if (!buf || !out || len < sizeof(DlFrameHeader)) return false;
    out->card = get_u16(buf + 0);
    out->msg_id = get_u16(buf + 2);
    out->body = buf + sizeof(DlFrameHeader);
    out->body_len = len - sizeof(DlFrameHeader);
    return true;
}

bool decode_version(const uint8_t* p, uint32_t len, VersionResp* out) {
    if (!p || !out || len < 8) return false;   // [V] pump asserts len <= 0x80
    out->dsp_branch = get_u16(p + 0);
    out->code_svn = get_u16(p + 2);
    out->aon_chip_id = get_u16(p + 4);
    out->aon_chip_version = get_u16(p + 6);
    return true;
}

bool decode_assert(const uint8_t* p, uint32_t len, DspAssertResp* out) {
    if (!p || !out || len < 16) return false;
    out->a0 = get_u32(p + 0);
    out->a1 = get_u32(p + 4);
    out->a2 = get_u32(p + 8);
    out->a3 = get_u32(p + 12);
    return true;
}

void decode_card_info(const uint8_t* p, CardInfoCnf* out) {
    if (!p || !out) return;
    std::memcpy(out->raw, p, CardInfoCnf::kWireSize);
    out->cfg_0c = get_u16(p + 0x0c);   // [V offsets]
    out->cfg_10 = get_u16(p + 0x10);
    out->cfg_14 = p[0x14];
    out->cfg_16 = p[0x16];
    out->cfg_18 = get_u16(p + 0x18);
    out->cfg_28 = get_u32(p + 0x28);
    out->cfg_2c = get_u16(p + 0x2c);
    out->cfg_34 = get_u16(p + 0x34);
    out->cfg_3c = p[0x3c];
    out->cfg_40 = p[0x40];
    out->cfg_44 = get_u16(p + 0x44);
    out->cfg_4c = p[0x4c];
}

}  // namespace l1
}  // namespace f286
