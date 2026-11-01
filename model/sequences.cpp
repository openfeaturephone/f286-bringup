// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sequences.cpp - implementation. Each function documents its stock provenance
// and the expected DL chain. Constants from messages.h (stock watchdogs).
#include "sequences.h"

#include <cstring>

namespace f286 {
namespace l1 {

SequenceResult boot_handshake(L1Client& c, VersionResp* version, uint32_t timeout_ms) {
    SequenceResult r;
    // [V boot tail 0x80e2d962]: alloc 8, memcpy template (init-data
    // 0x81717514 = 8 zero bytes), put(6, buf, 8, kick=0) - FIRST command,
    // sent RAW via the direct-put path (no header framing).
    if (!c.send_dsp_init()) { r.fail_step = 1; return r; }
    // [V pump 0x80c10a50-0x80c10aee]: answer is resp TYPE 5 (<= 0x80 B):
    // {u16 branch, u16 svn, u16 aon_chip_id, u16 aon_chip_version}.
    if (!c.wait_version(version, timeout_ms)) { r.fail_step = 2; return r; }
    r.ok = true;
    return r;
}

SequenceResult card_info(L1Client& c, CardInfoCnf* caps, uint32_t timeout_ms) {
    SequenceResult r;
    // [V sender 0x80c0fa3e, wait "Wait for CPHY_CARD_INFO_CNF timeout"
    // @0x80c0fa58]: sync send 0x9f (4 B, stock forwards 0).
    if (!c.send_card_info(CardInfoReq{})) { r.fail_step = 1; return r; }
    // Expected chain [V]:
    //   DL 0x22 = the 0x1b0 capability copy (memcpy -> card frame +0x2804),
    //   DL 0x9e = completion (posts the RRC prim, 0xFC descriptor).
    uint8_t buf[0x200];
    DlMessage m;
    r.fail_dl_id = DL_CARD_INFO_COPY;
    if (!c.wait_dl(DL_CARD_INFO_COPY, timeout_ms, &m, buf, sizeof(buf))) {
        r.fail_step = 2; return r;
    }
    if (caps && m.body_len >= CardInfoCnf::kWireSize)
        decode_card_info(m.body, caps);
    r.fail_dl_id = DL_CARD_INFO_CNF;
    if (!c.wait_dl(DL_CARD_INFO_CNF, timeout_ms, &m)) { r.fail_step = 3; return r; }
    r.ok = true;
    return r;
}

SequenceResult band_sweep(L1Client& c, uint32_t band_id, uint32_t earfcn_lo,
                          uint32_t earfcn_hi, CandidateListCnf* candidates,
                          BandVerdictCnf* verdicts, uint32_t timeout_ms) {
    SequenceResult r;
    // [V builder router prim 0x35a @0x80a2435e]: memset 0x2c; +4 <- body+4,
    // +8 = 1, +0xc/+0x10/+0x14/+0x18 <- body; tail zeroed; send ASYNC
    // (no semaphore in stock; RMH "Measure Cell Timer" 160 s guards it).
    BandSweepReq req;
    req.word_04 = earfcn_lo;   // [I] plan word semantics
    req.word_0c = earfcn_hi;   // [I]
    req.word_10 = band_id;     // [I]
    if (!c.send_band_sweep(req)) { r.fail_step = 1; return r; }
    // Expected chain [V DL table]: 0x53 status CNF, 0x00 candidates, 0x47 bands.
    uint8_t buf[0x200];
    DlMessage m;
    r.fail_dl_id = DL_BAND_SWEEP_CNF;
    if (!c.wait_dl(DL_BAND_SWEEP_CNF, timeout_ms, &m)) { r.fail_step = 2; return r; }
    r.fail_dl_id = DL_CANDIDATE_LIST;
    if (!c.wait_dl(DL_CANDIDATE_LIST, timeout_ms, &m, buf, sizeof(buf))) {
        r.fail_step = 3; return r;
    }
    if (candidates)
        std::memcpy(candidates->raw, buf,
                    (m.body_len < sizeof(candidates->raw)) ? m.body_len
                                                           : sizeof(candidates->raw));
    r.fail_dl_id = DL_BAND_LIST;
    if (!c.wait_dl(DL_BAND_LIST, timeout_ms, &m, buf, sizeof(buf))) {
        r.fail_step = 4; return r;
    }
    if (verdicts) {
        // [V handler 0x80ee2ecc] <=0x29 u8 verdicts from ~+0xa, count @~+0x5c.
        verdicts->count = 0;
        for (int i = 0; i < BandVerdictCnf::kMaxBands &&
             (0xa + 2 * i + 1) < static_cast<int>(m.body_len); ++i)
            verdicts->verdicts[i] = buf[0xa + 2 * i];
        verdicts->count = (0x5c + 1) < m.body_len ? buf[0x5c] : 0;
    }
    r.ok = true;
    return r;
}

SequenceResult cell_search(L1Client& c, const SyncCell* cells, int n_cells,
                           uint16_t mode, uint32_t batch_timeout_ms) {
    SequenceResult r;
    if (!cells || n_cells <= 0) { r.fail_step = 1; return r; }
    // [V builder 0x80ee66f8]: batches of 20 (ceil(n/20) messages).
    int sent = 0;
    while (sent < n_cells) {
        SyncSearchBatchReq req;
        req.mode = mode;
        int n = n_cells - sent;
        if (n > SyncSearchBatchReq::kMaxCells) n = SyncSearchBatchReq::kMaxCells;
        for (int i = 0; i < n; ++i) {
            req.cells[i].earfcn = cells[sent + i].earfcn;
            req.cells[i].key = cells[sent + i].key;
            req.cells[i].flag8 = cells[sent + i].flag8;
        }
        req.count = static_cast<uint32_t>(n);
        if (!c.send_sync_search_batch(req)) { r.fail_step = 2; return r; }
        sent += n;
        // Expected chain per batch [V DL table]:
        //   DL 0x03 sync CNF (timing i16 @+0xa, >>4),
        //   DL 0x02 MIB buffer-pool descriptor,
        //   DL 0x0c camp CNF (card state := 2),
        //   DL 0x01 5-cell meas list.
        // Stock re-sends and re-arms the 4 s watchdog per retry.
        uint8_t buf[0x100];
        DlMessage m;
        r.fail_dl_id = DL_SYNC_CNF;
        if (!c.wait_dl(DL_SYNC_CNF, batch_timeout_ms, &m, buf, sizeof(buf))) {
            r.fail_step = 3; return r;
        }
        if (r.fail_dl_id == DL_SYNC_CNF) r.fail_dl_id = 0;
        // The remaining DLs arrive as the DSP finishes; wait best-effort so a
        // caller's ring stays clean, but do not fail the sequence on them.
        c.wait_dl(DL_MIB_BUFDESC, batch_timeout_ms, &m, buf, sizeof(buf));
        if (m.msg_id == DL_MIB_BUFDESC) { /* caller can re-wait for details */ }
        c.wait_dl(DL_CAMP_CNF, batch_timeout_ms, &m);
        c.wait_dl(DL_MEAS_CELL_LIST, batch_timeout_ms, &m, buf, sizeof(buf));
    }
    r.ok = true;
    return r;
}

SequenceResult read_mib_sib1(L1Client& c, uint32_t earfcn, MibBufDesc* mib_desc,
                             SyncCnf* sync_cnf, uint32_t timeout_ms) {
    SequenceResult r;
    // Small sync on one earfcn first (mode-2-style plain sync path):
    // 0x73 8 B {u16 0, u16 0, u32 earfcn} [V builder 0x80ee6592]; alternative:
    // 0x74 with one cell, mode 2 (handover/plain sync) [I].
    SyncCellSmallReq sreq;
    sreq.earfcn = earfcn;
    if (!c.send_sync_cell_small(sreq)) { r.fail_step = 1; return r; }
    uint8_t buf[0x100];
    DlMessage m;
    r.fail_dl_id = DL_SYNC_CNF;
    // [V handler 0x80ee3e76] i16 timing @+0xa (>>4), u16 @+0xc checked.
    if (!c.wait_dl(DL_SYNC_CNF, timeout_ms, &m, buf, sizeof(buf))) {
        r.fail_step = 2; return r;
    }
    if (sync_cnf && m.body_len >= 0x0e) {
        sync_cnf->timing_x16 = get_i16(buf + 0x0a);
        sync_cnf->status_0c = get_u16(buf + 0x0c);
    }
    // MIB via DL 0x02 buffer-pool descriptor [V]:
    // {u16 @+0, u32 @+0xc, u32 @+0x10, u8 @+0x14, u16 @+0x16}.
    r.fail_dl_id = DL_MIB_BUFDESC;
    if (!c.wait_dl(DL_MIB_BUFDESC, timeout_ms, &m, buf, sizeof(buf))) {
        r.fail_step = 3; return r;
    }
    if (mib_desc && m.body_len >= 0x18) {
        mib_desc->w00 = get_u16(buf + 0);
        mib_desc->pool_0c = get_u32(buf + 0x0c);
        mib_desc->w10 = get_u32(buf + 0x10);
        mib_desc->b14 = buf[0x14];
        mib_desc->w16 = get_u16(buf + 0x16);
    }
    r.ok = true;
    return r;
}

SequenceResult dsp_deactivate(L1Client& c, uint32_t timeout_ms) {
    SequenceResult r;
    IL1Window* w = c.window();
    if (!w) { r.fail_step = 1; return r; }
    // [V 0x80e2df1c-0x80e2dfd4] the exact 3-step handshake:
    //   1. sync_flag (AP VA 0x80447E14) = 0x11111111
    //   2. send cmd 0x0e RAW 4-byte body {u16 0, u16 0x5555} (direct put
    //      0x80e2dfbe, no header framing)
    //   3. poll flag == 0xAAAAAAAA ("ShowDown" watchdog = 1 s)
    uint8_t one[4];
    put_u32(one, kDeactSyncArmed);
    if (!w->write(kDeactSyncFlag, one, 4)) { r.fail_step = 1; return r; }
    CphyDeactivateReq req;
    if (!c.send_deactivate(req)) { r.fail_step = 2; return r; }
    uint32_t deadline = w->now_ms() + timeout_ms;
    for (;;) {
        uint8_t f[4];
        if (w->read(kDeactSyncFlag, f, 4) && get_u32(f) == kDeactSyncDone) break;
        if (static_cast<int32_t>(deadline - w->now_ms()) <= 0) {
            r.fail_step = 3; return r;   // "dsp not return flag!timerout!"
        }
        w->sleep_ms(1);
    }
    r.ok = true;
    return r;
}

SequenceResult dsp_sleep(L1Client& c, uint16_t p0, uint16_t p1, uint16_t p2,
                         uint16_t p3, uint32_t wake_timeout_ms) {
    SequenceResult r;
    DspSleepReq req;
    req.p0 = p0; req.p1 = p1; req.p2 = p2; req.p3 = p3;
    if (!c.send_dsp_sleep(req)) { r.fail_step = 1; return r; }
    // [V] wake = poll win+0x47e14 (AP 0x80447E14) == 0x22222222
    // (stock: <= 100 x 10 ms); afterwards re-run boot_handshake (a new
    // cmd 0x06 -> resp type 5). An earlier version polled 0x0e14 (wrong cell).
    IL1Window* w = c.window();
    if (!w || wake_timeout_ms == 0) { r.ok = true; return r; }
    uint32_t deadline = w->now_ms() + wake_timeout_ms;
    for (;;) {
        uint8_t f[4];
        if (w->read(kSleepSyncWord, f, 4) && get_u32(f) == 0x22222222u) break;
        if (static_cast<int32_t>(deadline - w->now_ms()) <= 0) {
            r.fail_step = 2; return r;
        }
        w->sleep_ms(10);
    }
    r.ok = true;
    return r;
}

SequenceResult paging_receive(L1Client& c, const PagingReceiveReq& req,
                              uint16_t cnf_expect_id, uint32_t timeout_ms) {
    SequenceResult r;
    // [V sender 0x80c0f1ec]: armed-gate check, then sync send 0x7a +
    // semaphore wait 2000 ticks ("CPHY_PAGING_RECEIVE_CNF" family).
    // 0x7a IS in the length table (0x8a) -> normal framed send; the raw
    // direct-put path is only for 0x06/0x0d/0x0e.
    // CNF pairing req-1 => DL 0x79 [I, heuristic].
    if (!c.send(CMD_CPHY_PAGING_RECEIVE_REQ, req.raw, sizeof(req.raw))) {
        r.fail_step = 1; return r;
    }
    DlMessage m;
    r.fail_dl_id = cnf_expect_id;
    if (!c.wait_dl(cnf_expect_id, timeout_ms, &m)) { r.fail_step = 2; return r; }
    r.ok = true;
    return r;
}

}  // namespace l1
}  // namespace f286
