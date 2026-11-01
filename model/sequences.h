// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sequences.h - drive-ready L1 sequences ("see a network" minimal plan,
//) as typed functions with the expected response chains.
//
// Precondition (/, outside this layer):
//   PMU whale on -> WTLCP on -> NV9 staging word [win+0x47e00] = NV9|tick<<16
//   (AP 0x80447E00;) -> LTE DSP release. After release the
//   DSP polls the cmd ring; the FIRST
//   command must be CMD_DSP_INIT (0x06) and the FIRST response is resp type 5.
#pragma once

#include "l1_client.h"

namespace f286 {
namespace l1 {

struct SequenceResult {
    bool ok = false;
    uint16_t fail_step = 0;      // where it stopped (per-function step ids)
    uint32_t fail_dl_id = 0;     // DL id awaited when a wait timed out
};

// Step 0. cmd 0x06 (8 zero bytes) -> resp type 5 VERSION.
// Expected chain: [V] resp 5 {branch, svn, aon_chip_id, aon_chip_ver};
// stock then clears IsDspLoadingCode and gives the TX semaphore. DSP is awake.
SequenceResult boot_handshake(L1Client& c, VersionResp* version = nullptr,
                              uint32_t timeout_ms = kAliveCheckMs);

// Step 1. CPHY_CARD_INFO_REQ (0x9f, 4 B zeros) -> DL 0x22 (0x1b0 capability
// copy) then DL 0x9e (completion). Stock waits the CNF 2000 ticks; the
// 30 s alive timer is re-armed by any response.
SequenceResult card_info(L1Client& c, CardInfoCnf* caps = nullptr,
                         uint32_t timeout_ms = kWaitTicksDefault);

// Step 2. CPHY_BAND_SWEEP_REQ (0x54) on one band.
// Expected chain: DL 0x53 (status CNF, async), DL 0x00 (<=0x29 candidate
// {earfcn, metric} pairs), DL 0x47 (band verdicts). Stock "Measure Cell
// Timer" = 160 s; results only accepted when the request was armed first.
SequenceResult band_sweep(L1Client& c, uint32_t band_id, uint32_t earfcn_lo,
                          uint32_t earfcn_hi,
                          CandidateListCnf* candidates = nullptr,
                          BandVerdictCnf* verdicts = nullptr,
                          uint32_t timeout_ms = kWatchdogMeasureMs);

// Step 3. CPHY_SYNC_SEARCH_BATCH (0x74) over an earfcn list, <=20 per message,
// auto-batched like the stock builder (ceil(n/20) messages).
// Expected chain per batch: DL 0x03 (sync CNF, timing i16 >>4), DL 0x02 (MIB
// buffer-pool descriptor), DL 0x0c (camp CNF, state := 2), DL 0x01 (5-cell
// meas list). "Search or Sync Cell Timer" = 4 s per batch.
struct SyncCell { uint32_t earfcn; int32_t key; uint8_t flag8; };
SequenceResult cell_search(L1Client& c, const SyncCell* cells, int n_cells,
                           uint16_t mode = SyncSearchBatchReq::kModeList,
                           uint32_t batch_timeout_ms = kWatchdogSyncMs);

// Step 4. SIB1/MIB read-through: after a successful 0x74 sync on the target
// earfcn, the MIB arrives as DL 0x02 (buffer-pool descriptor) and camp
// confirm as DL 0x0c; SIB1 bytes flow through the DL 0x45/0x05/0x06
// earfcn-list family (flag u8 @+2 selects the SIB1-configured path) [I].
// SIB1 reception CONFIG is armed via the meas-config family (0x1d/0x43);
// 0x9c is only the SIB1-CFG-STOP (PAL_CphySIB1CfgStopCnf) [V].
SequenceResult read_mib_sib1(L1Client& c, uint32_t earfcn,
                             MibBufDesc* mib_desc = nullptr,
                             SyncCnf* sync_cnf = nullptr,
                             uint32_t timeout_ms = kWatchdogSyncMs);

// Step 5 (optional). Clean DSP shutdown - the verified 0x0e handshake:
// [0x80447E14] = 0x11111111; send {u16 0, u16 0x5555}; poll until the flag
// reads 0xAAAAAAAA (1 s ShowDown watchdog).
SequenceResult dsp_deactivate(L1Client& c, uint32_t timeout_ms = kWatchdogDeactMs);

// Step 6 (optional). Deep sleep (0x0d, 4x u16) + wake poll of win+0xe14
// == 0x22222222; on resume re-run boot_handshake.
SequenceResult dsp_sleep(L1Client& c, uint16_t p0 = 0, uint16_t p1 = 0,
                         uint16_t p2 = 0, uint16_t p3 = 0,
                         uint32_t wake_timeout_ms = 1000);

// Paging receive (0x7a) - requires the paging config armed via the 0x4c
// family (stock armed-gate card+0x2000+0xf8f == 1). Payload is the RRC-built
// 0x8a B config (modeled opaque). Waits CPHY_PAGING_RECEIVE_CNF, 2000 ticks.
SequenceResult paging_receive(L1Client& c, const PagingReceiveReq& req,
                              uint16_t cnf_expect_id = 0x79,
                              uint32_t timeout_ms = kWaitTicksDefault);

}  // namespace l1
}  // namespace f286
