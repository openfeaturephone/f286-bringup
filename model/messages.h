// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// messages.h - f286::l1 typed message catalog for the Unisoc UMS9117 (Ruio F286)
// AP->LTE-DSP (XDSP) command interface ("L1"/CPHY layer).
//
// Provenance: every struct is transcribed from the stock modem firmware
// (kernel.bin, VA = file_off + 0x80a05e00), function VAs
// cited per field group. Tags: [V] = VERIFIED against raw disassembly
// of the cited stock VA; [I] = INFERRED (offset pinned, semantics
// deduced). Lengths are the stock pal_sci_adx.c length table 0x80e038a0
// (0x80e038a0) - the send core copies exactly length_table[id] payload bytes
// from the caller's buffer, so the table IS the wire size.
//
// The DSP ring cmdIds below are named from the stock watchdog/wait strings
// ("Wait for CPHY_xxx_CNF timeout", "RMH_xxxTimerExpired, las send CPHY_xxx").
//
// Target: little-endian wire, host-endian-agnostic pack/unpack (codec.h).
// Compiles with: g++ -std=c++17 -c
#pragma once

#include <cstdint>
#include <cstddef>

namespace f286 {
namespace l1 {

// ---------------------------------------------------------------------------
// 1. cmdId constants (UL = AP->DSP senders; DL = DSP->AP dispatch table)
// ---------------------------------------------------------------------------

// --- UL ids with a stock sender (VA of the send-site function) -------------
enum CmdId : uint16_t {
    CMD_DSP_INIT                 = 0x06,  // [V] boot tail 0x80e2d962; 8 B zeros; resp = type-5 version
    CMD_DSP_SLEEP                = 0x0d,  // [V] 0x80e2de9a; 4x u16 args
    CMD_CPHY_DEACTIVATE_REQ      = 0x0e,  // [V] 0x80e2df1c; sync-flag handshake, 1 s ShowDown watchdog
    CMD_GAP_WRECK_CONFIRM_ASYNC  = 0x11,  // [I] 0x80c0d25e region; 6 B
    CMD_CONNECTED_CFG_A          = 0x12,  // [I] 0x1ac B connected-mode family
    CMD_CONNECTED_CFG_B          = 0x13,  // [I] 0x050 B connected-mode family
    CMD_CPHY_IDLE_CONFIG_REQ     = 0x15,  // [V] PAL_CphyIdleConfigCnf wait (0x80c0d3aa); 0xb8 B
    CMD_CPHY_IDLE_CONFIG2_REQ    = 0x16,  // [V] sender 0x80c0d5e0 (tail 0x80c0d604); 0xf0 B, async
    CMD_CONNECTED_CFG_C          = 0x17,  // [I] 0x2c0 B
    CMD_CPHY_CONNECTED_MODE_REQ  = 0x18,  // [V] sender 0x80c0db10 (send 0x80c0db2e), waits CNF 2000 ticks
    CMD_MEAS_CFG_4B              = 0x19,  // [I] 4 B
    CMD_MEAS_CFG_14B             = 0x1b,  // [I] 0x14 B
    CMD_BIG_CFG_61C              = 0x1c,  // [I] 0x61c B, pointer-invoked wrapper 0x80e04420
    CMD_CPHY_MEAS_INTRA_INTER_CONFIG = 0x1d, // [V] CPHY_MEAS_INTRA/INTER_CONFIG_CNF anchors; 0x23a0 B, SPLIT
    CMD_MEAS_CFG_28B             = 0x20,  // [I] 0x28 B
    CMD_MEAS_CFG_4B_B            = 0x23,  // [I] 4 B
    CMD_MEAS_CFG_8B              = 0x25,  // [I] 8 B
    CMD_TCM_SLAVE_MEAS_A         = 0x2c,  // [I] lrrc_tcm_slave_meas.c 0x8128826a; 0x11dc B
    CMD_TCM_SLAVE_MEAS_B         = 0x2d,  // [I] 0x8128841c; 0x11dc B
    CMD_TCM_SLAVE_MEAS_C         = 0x2e,  // [I] 0x81287cf4; 0x11d8 B
    CMD_SERVING_CFG_48B          = 0x30,  // [I] 0x48 B (same sender fn as 0x6e)
    CMD_SERVING_CFG_1B_A         = 0x32,  // [I] 1 B
    CMD_SERVING_CFG_1B_B         = 0x34,  // [I] 1 B
    CMD_SERVING_CFG_70B          = 0x36,  // [I] 0x70 B
    CMD_SERVING_CFG_30B          = 0x38,  // [I] 0x30 B
    CMD_SERVING_CFG_18B          = 0x3b,  // [I] 0x18 B
    CMD_MEAS_CFG_48B             = 0x3e,  // [I] 0x48 B
    CMD_TCM_MEAS_20B_A           = 0x3f,  // [I] 0x20 B
    CMD_TCM_MEAS_20B_B           = 0x40,  // [I] 0x20 B
    CMD_MEAS_CONFIG_V2           = 0x43,  // [I] builder 0x811dd234 <- prim 0x324; 0x1b0 B
    CMD_MEAS_CFG_58B             = 0x46,  // [I] 0x58 B
    CMD_SHORT_OP_4B              = 0x48,  // [I] wait only 0x14 ticks (fn 0x80c0dfee)
    CMD_TCM_MEAS_D0              = 0x4a,  // [I] 0xd0 B
    CMD_PAGING_CFG_REQ           = 0x4c,  // [I] paging/RRC cfg, waits 2000; 4 B (same fn sends 0x70)
    CMD_UNSIZED_A                = 0x4d,  // length-table assert "add the message" (dead/unsized)
    CMD_UNSIZED_B                = 0x4e,  // ditto
    CMD_CPHY_BAND_SWEEP_REQ      = 0x54,  // [V] sender 0x80c0e04c; RMH "Measure Cell Timer" 160 s
    CMD_MBMS_CFG_16B             = 0x56,  // [I] 0x16 B
    CMD_MBMS_CFG_56B             = 0x59,  // [I] waits 2000; 0x56 B
    CMD_MBMS_CFG_4B              = 0x5b,  // [I] 4 B
    CMD_CPHY_PHY_SUSPEND_REQ     = 0x5c,  // [V] waits CPHY_PHY_SUSPEND_CNF; 6 B (prim 0x326 sends {u32 u8<<16})
    CMD_PHY_SUSPEND_26B          = 0x5e,  // [I] 0x26 B
    CMD_PHY_RESUME_2B            = 0x5f,  // [I] 2 B
    CMD_SYNC_CELL_STOP_FAMILY    = 0x60,  // [I] 8 B; second SyncCellStop-region waiter 0x80c0ef30
    CMD_UE_CATEGORY_CFG          = 0x62,  // [I] ueCategory/sharememory; 0x18 B
    CMD_MEAS_CFG_4B_C            = 0x65,  // [I] 4 B
    CMD_SHAREMEMORY_DUMP         = 0x66,  // [I] 0x1004 B, SPLIT
    CMD_MEAS_CFG_6B              = 0x67,  // [I] 6 B
    CMD_MEAS_CFG_4B_D            = 0x68,  // [I] 4 B
    CMD_MEAS_CFG_1B_A            = 0x6b,  // [I] 1 B
    CMD_MEAS_CFG_1B_B            = 0x6c,  // [I] 1 B; RMH direct-send 0x80ffe75e {u8 1, 3 B pad}
    CMD_MEAS_CFG_1B0             = 0x6e,  // [I] waits 2000; 0x1b0 B
    CMD_EARFCN_BAND_LIST_REQ     = 0x70,  // [V] sender 0x80c0ec72; 0xb0 B; see FreqScanReq
    CMD_FREQ_SCAN_REQ            = 0x71,  // [V] sender 0x80c0ec9c; 0x48 B wire; RMH "FreqScan Cell Timer" 10 s
    CMD_SYNC_CELL_SMALL_REQ      = 0x73,  // [V] sender 0x80c0ecf0, caller 0x80ee65b8; 8 B
    CMD_CPHY_SYNC_SEARCH_BATCH_REQ = 0x74,// [V] sender 0x80c0ecc6, builders 0x80ee66f8/0x80ee6702; 0x148 B
    CMD_CPHY_SYNC_CELL_STOP_REQ  = 0x75,  // [V] send 0x80c0ed60, waits PAL_CphySyncCellStopCnf 2000; 4 B
    CMD_MBMS_CTRL_BIG            = 0x77,  // [I] 0x1ac B
    CMD_CPHY_PAGING_RECEIVE_REQ  = 0x7a,  // [V] sender 0x80c0f1ec (caller 0x80ed6eee), waits CNF 2000; 0x8a B
    CMD_CPHY_MBSFN_AREA_INFO_CONFIG_REQ = 0x7c, // [V-name] waits ..._CNF; 4 B
    CMD_CPHY_MCCH_CONFIG_REQ     = 0x7e,  // [V-name] waits CPHY_MCCH_CONFIG_CNF 2000; 0x3e8 B, SPLIT
    CMD_CPHY_MBMS_CONTROL_REQ    = 0x81,  // [V-name] waits CPHY_MBMS_CONTROL_CNF 2000; 4 B
    CMD_MBSFN_PMCH_2A            = 0x83,  // [I] 0x2a B
    CMD_MBMS_1B0                 = 0x84,  // [I] 0x1b0 B
    CMD_MBMS_6C                  = 0x87,  // [I] 0x6c B
    CMD_MBMS_8B                  = 0x89,  // [I] 8 B
    CMD_MBMS_C                   = 0x8a,  // [I] 0xc B
    CMD_MBMS_E                   = 0x8b,  // [I] 0xe B
    CMD_MBSFN_PMCH_CFG_8B        = 0x8d,  // [I] waits CPHY_MBSFN_PMCH_CONFIG_CNF (0x80c0f898); 8 B
    CMD_PMCH_CFG_4B              = 0x8f,  // [V-len] length table = 4 B ('s "0x98" row was DL 0x91)
    CMD_PMCH_CFG_98B             = 0x91,  // [V-len] 0x98 B
    CMD_CPHY_MBSFN_PMCH_CONFIG_REQ = 0x92,// [V-name] 0x9c B
    CMD_MBMS_10B                 = 0x93,  // [I] 0x10 B
    CMD_PMCH_20B                 = 0x94,  // [I] 0x20 B
    CMD_MBMS_8B_B                = 0x96,  // [I] 8 B
    CMD_MEAS_REPORT_CFG          = 0x98,  // [I] builder 0x811de938 <- prim 0x352; 0x1b0 B
    CMD_MBMS_A                   = 0x9b,  // [I] 0xa B
    CMD_CPHY_SIB1_CFG_STOP_REQ   = 0x9c,  // [V] waits PAL_CphySIB1CfgStopCnf 2000 (0x80c0f9b4); 4 B
    CMD_CPHY_CARD_INFO_REQ       = 0x9f,  // [V] send 0x80c0fa3e (b.w 0x80e040f6); waits CPHY_CARD_INFO_CNF; 4 B
    CMD_MBMS_11CC                = 0xa0,  // [I] 0x11cc B (no send site; DSP->AP or disabled)
    CMD_MBMS_6B                  = 0xa2,  // [I] 6 B
    CMD_MBMS_D0                  = 0xa4,  // [I] 0xd0 B (no send site)
    CMD_MBMS_16B                 = 0xa6,  // [I] 0x16 B (no send site)
    CMD_MBMS_1B0_B               = 0xaa,  // [I] 0x1b0 B (no send site)
    CMD_MBMS_AD                  = 0xad,  // [I] 6 B
};

// --- DL-only ids (length table returns -1 => no UL payload) [V] ------------
// 0x00..0x10, 0x24, 0x29..0x2b, 0x31, 0x33, 0x35, 0x37, 0x39, 0x3a, 0x3d,
// 0x47, 0x49, 0x69, 0x6a, 0x6d, 0x72, 0x76, 0x7b, 0x7d, 0x7f, 0x80, 0x82,
// 0x88, 0x8e, 0x90, 0x99, 0x9a, 0x9d, 0xa1, 0xa3, 0xa5, 0xa7, 0xa8, 0xa9,
// 0xab, 0xac  (0x80e038a0 switch "break" group == 0x80e04244 DL set).
enum DlId : uint16_t {
    DL_CANDIDATE_LIST      = 0x00,  // [V] <=0x29 x {i32 earfcn, i32 metric} @+4; count u16 @+0x14c
    DL_MEAS_CELL_LIST      = 0x01,  // [V] 5 x 0x14 B entries @+8; meas scaled (x*100)>>4
    DL_MIB_BUFDESC         = 0x02,  // [V] buffer-pool MIB descriptor 
    DL_SYNC_CNF            = 0x03,  // [V] i16 timing @+0xa (>>4 -> card+0x8b2); clears armed flags
    DL_EARFCN_LIST_A       = 0x05,  // [V] flag u8 @+2, count u16 @+4, <=4 x u16 earfcn @+8
    DL_EARFCN_LIST_B       = 0x06,  // [V] same family as 0x05 (+ i16 timing @+0xa)
    DL_BAND_LIST           = 0x47,  // [V] <=0x29 u8 band verdicts; count @+~0x5c
    DL_EARFCN_LIST_C       = 0x45,  // [V] 0x05/0x06 family
    DL_EARFCN_LIST_D       = 0x86,  // [V] 0x05/0x06 family
    DL_CAMP_CNF            = 0x0c,  // [V] i16 @+2 (>>4); card state := 2 (camped)
    DL_BAND_SWEEP_CNF      = 0x53,  // [V] BAND_SWEEP (0x54) status CNF (handler 0x80ee41ca)
    DL_CARD_INFO_COPY      = 0x22,  // [V] 0x1b0 capability struct copy -> card frame +0x2804
    DL_CARD_INFO_CNF       = 0x9e,  // [V] CARD_INFO completion (posts RRC prim, 0xFC desc)
    DL_SHAREMEM_1B0        = 0x43,  // (UL id 0x43 shares numeric space; DL routing has no 0x43)
};

// Canonical UL payload length per cmdId [V] - transcribed from the
// 0x80e038a0 (0x80e038a0) switch. Returns 0 for "id not a UL message".
uint16_t ul_payload_length(uint16_t cmd_id);

// Short stock-derived name for logs (never null; "UNKNOWN_0xXX" fallback).
const char* cmd_name(uint16_t id);

// ---------------------------------------------------------------------------
// 2. Request payload structs (wire = little-endian, packed, exact sizes)
// ---------------------------------------------------------------------------

// 0x06 DSP init [V] template init-data VA 0x81717514 = 8 zero bytes;
// send 0x80e2d962: alloc 8 -> memcpy template -> put(6, buf, 8, kick=0).
struct DspInitReq {
    uint16_t p0 = 0;   // +0x00 [V] stock template all-zero
    uint16_t p1 = 0;   // +0x02
    uint16_t p2 = 0;   // +0x04
    uint16_t p3 = 0;   // +0x06
};
static_assert(sizeof(DspInitReq) == 8, "wire size");

// 0x0d DSP deep-sleep request [V] 0x80e2de9a(r0..r3) -> strh at +0/+2/+4/+6.
// Stock caller args unpinned ; resume = poll win+0x47e14
// (AP 0x80447E14) == 0x22222222 .
struct DspSleepReq {
    uint16_t p0 = 0;   // +0x00 [V] arg r0
    uint16_t p1 = 0;   // +0x02 [V] arg r1
    uint16_t p2 = 0;   // +0x04 [V] arg r2
    uint16_t p3 = 0;   // +0x06 [V] arg r3
};
static_assert(sizeof(DspSleepReq) == 8, "wire size");

// 0x0e CPHY_DEACTIVATE_REQ [V] 0x80e2df1c: writes sync_flag 0x11111111 first,
// sends {0, marker}, polls flag == 0xAAAAAAAA (1 s watchdog). Variant 2 = 0x6666.
struct CphyDeactivateReq {
    uint16_t reserved = 0;  // +0x00 [V] always 0
    uint16_t marker   = 0x5555; // +0x02 [V] 0x5555 or 0x6666
};
static_assert(sizeof(CphyDeactivateReq) == 4, "wire size");
constexpr uint16_t kDeactivateMarker1 = 0x5555;  // [V]
constexpr uint16_t kDeactivateMarker2 = 0x6666;  // [V]

// 0x9f CPHY_CARD_INFO_REQ [V] 4 B, stock forwards caller's u32 (0 in the
// minimal path). Answer: DL 0x22 (0x1b0 copy) + DL 0x9e (completion).
struct CardInfoReq {
    uint32_t opaque = 0;  // +0x00 [I] stock sends 0 (caller 0x80eee106)
};
static_assert(sizeof(CardInfoReq) == 4, "wire size");

// DL 0x22 / 0x9e CARD_INFO capability block, 0x1b0 B [V mechanics, I semantics]
// (whole block memcpy'd to card frame +0x2804).
struct CardInfoCnf {
    static constexpr uint16_t kWireSize = 0x1b0;

    uint16_t cfg_0c = 0;   // +0x0c [V off] forwarded to RRC (duplex-mode class [I])
    uint16_t cfg_10 = 0;   // +0x10 [V off]
    uint8_t  cfg_14 = 0;   // +0x14 [V off]
    uint8_t  cfg_16 = 0;   // +0x16 [V off]
    uint16_t cfg_18 = 0;   // +0x18 [V off]
    uint32_t cfg_28 = 0;   // +0x28 [V off]
    uint16_t cfg_2c = 0;   // +0x2c [V off]
    uint16_t cfg_34 = 0;   // +0x34 [V off]
    uint8_t  cfg_3c = 0;   // +0x3c [V off]
    uint8_t  cfg_40 = 0;   // +0x40 [V off]
    uint16_t cfg_44 = 0;   // +0x44 [V off]
    uint8_t  cfg_4c = 0;   // +0x4c [V off]
    // +0x50..0x1af unmapped raw [I]
    uint8_t  raw[0x1b0] = {};

    // Band-capability subtable - stock caches it OUTSIDE the 0x1b0 copy
    // (header pair @0x8170E632+card*0x40, entries @0x8170E820+card*8).
    struct BandCap {          // [V] consumer conversions
        uint16_t band;        // band number
        uint16_t band2;       // companion/secondary band id [I]
        int16_t  freq_raw;    // MHz = (raw + 14000) / 100  [V]
        int16_t  rssi_raw;    // dBm = (raw*2 + 4000) / 100  [V]
    };
    static constexpr int kMaxBands = 8;  // [V] loop breaks at 8

    static int32_t freq_raw_to_khz(int16_t raw) { return (static_cast<int32_t>(raw) + 14000); }   // (=MHz*100)
    static int32_t freq_raw_to_mhz(int16_t raw) { return freq_raw_to_khz(raw) / 100; }
    static int32_t rssi_raw_to_dbm_x100(int16_t raw) { return raw * 2 + 4000; }                    // dBm*100
    static int32_t rssi_raw_to_dbm(int16_t raw) { return rssi_raw_to_dbm_x100(raw) / 100; }
};

// 0x54 CPHY_BAND_SWEEP_REQ, 0x2c B [V] router case prim 0x35a builder
// (0x80a24130 @0x80a2435e-0x80a2438c): memset 0x2c, then:
//   +0x00 = 0 (unset), +0x04 <- body+4, +0x08 = 1, +0x0c..+0x18 <- body,
//   +0x1c..+0x2b = 0. Semantics of the body words = band/earfcn plan [I].
// Answer: DL 0x53 (status CNF) + DL 0x00 candidates + DL 0x47 band verdicts.
struct BandSweepReq {
    uint32_t word_00 = 0;    // +0x00 [V] stock leaves 0
    uint32_t word_04 = 0;    // +0x04 [V] <- RRC body+4   (earfcn_lo [I])
    uint32_t one_08  = 1;    // +0x08 [V] constant 1
    uint32_t word_0c = 0;    // +0x0c [V] <- body+0x0c   (earfcn_hi [I])
    uint32_t word_10 = 0;    // +0x10 [V] <- body+0x10   (band_id [I])
    uint32_t word_14 = 0;    // +0x14 [V] <- body+0x14
    uint32_t word_18 = 0;    // +0x18 [V] <- body+0x18
    uint32_t zero_1c = 0;    // +0x1c [V] stock zeroes tail
    uint32_t zero_20 = 0;    // +0x20 [V]
    uint32_t zero_24 = 0;    // +0x24 [V]
    uint32_t zero_28 = 0;    // +0x28 [V]
};
static_assert(sizeof(BandSweepReq) == 0x2c, "wire size");

// 0x74 CPHY_SYNC_SEARCH_BATCH_REQ, 0x148 B - <=20 cells per message.
// [V] builder 0x80ee66f8 (0x80ee66f8-0x80ee6810), decode:
//   memset(payload, 0, 0x148); mode u16 @+0x02 (from req[0x18]: 1->1 else 0;
//   handover path 0x80ee6702 writes 2); per cell i (batch of <=20):
//     u32 earfcn  @ 16*i + 0x04   <- src entry +0x0c (-1 asserted)
//     i32 val     @ 16*i + 0x08   <- src entry +0x00 (-1 -> stays -1)
//     u32 spare   @ 16*i + 0x0c   <- src entry +0x04
//     u16 flag    @ 16*i + 0x10   <- u8 src entry +0x08 (zero-extended)
//   (the flag store lands at 16*(i+1), i.e. cell i's flag sits at the head of
//    slot i+1; cell 0's head doubles as {reserved, mode}.)
//   u32 count @+0x144; caller loops ceil(n/20) batches (r7 batch counter).
// Watchdog "Search or Sync Cell Timer" 4 s; the three RMH names
// CPHY_SYNC_REQ / CPHY_HANDOVER_SYNC_REQ / CPHY_FREQ_SEARCH_CELL_REQ are
// modes of this one message [I].
struct SyncSearchBatchReq {
    static constexpr uint16_t kWireSize = 0x148;
    static constexpr int kMaxCells = 20;          // [V] udiv by 0x14 in builder
    static constexpr uint16_t kModeList = 0;      // [V] cell-list search
    static constexpr uint16_t kModeList1 = 1;     // [V] cell-list search (armed variant)
    static constexpr uint16_t kModeHandover = 2;  // [V] plain/handover sync (no list)

    // One cell as the CALLER describes it (mirrors the stock RRC source
    // entry {i32 @0, u32 @4, u8 @8, i32 earfcn @0xc}); pack_sync_search_batch
    // scatters these into the wire slots stock-exactly.
    struct Cell {
        int32_t  key = -1;       // [V] src+0x00 (-1 -> wire slot val = -1)
        uint32_t spare = 0;      // [V] src+0x04
        uint8_t  flag8 = 0;      // [V] src+0x08
        uint32_t earfcn = 0;     // [V] src+0x0c (-1 asserted by stock)
    };

    // Wire SLOT view (16 B each, base 16*i) after packing:
    //   slot 0: {u16 flag0(0), u16 mode, u32 earfcn0, i32 val0, u32 spare0}
    //   slot i: {u16 flag_i, u16 pad, u32 earfcn_i, i32 val_i, u32 spare_i}
    // with flag_i (from Cell i-1's flag8) at slot i base; iteration n-1's
    // flag lands at +0x140 (gap before count).
    uint16_t mode = kModeList;       // +0x02 [V]
    Cell cells[kMaxCells];           // caller-filled
    uint32_t count = 0;              // +0x144 [V] cells actually filled
};
// NOTE: SyncSearchBatchReq is a LOGICAL carrier, not the wire image -
// always emit with pack_sync_search_batch (codec.h), which reproduces
// the stock builder's byte-exact slot scatter.

// 0x73 small single-cell sync, 8 B [V] builder @0x80ee6592: memset 8,
// u32 earfcn @+0x04 <- req[0x20]; +0x00/+0x02 stay 0 in the stock path
// (guessed {u16, u16 mode, u32 earfcn} - mode not written [I]).
struct SyncCellSmallReq {
    uint16_t reserved = 0;   // +0x00 [V] 0
    uint16_t flags = 0;      // +0x02 [V] 0 in stock path
    uint32_t earfcn = 0;     // +0x04 [V]
};
static_assert(sizeof(SyncCellSmallReq) == 8, "wire size");

// 0x75 CPHY_SYNC_CELL_STOP_REQ, 4 B [V] (send 0x80c0ed60, wait 2000 ticks).
struct SyncCellStopReq {
    uint32_t opaque = 0;     // [I] stock forwards caller value; 0 observed
};
static_assert(sizeof(SyncCellStopReq) == 4, "wire size");

// 0x70 earfcn + band list (freq-scan family), 0xb0 B built / 0xb0 sent [V].
// Builder 0x80ee5d08 (mar_pal_mimoon_rrc prim 0x337b) - FRESH correction:
// this builder's send call goes to the 0x70 sender (0x80c0ec72), not 0x71.
// Wire: +0x00 u16 0, +0x02 u16 earfcn <- req[0x70], +0x04 u16 pair count,
// +0x06.. pairs {u16,u16} (stock fills 1-2 live pairs from card+0x96c/0x970;
// the RRC request's band list is walked into a stack scratch for bookkeeping).
struct EarfcnBandListReq {
    static constexpr uint16_t kWireSize = 0xb0;
    static constexpr int kMaxPairs = 0x29;        // [V] cmp #0x29 in builder loops
    uint16_t reserved = 0;       // +0x00 [V]
    uint16_t earfcn = 0;         // +0x02 [V]
    uint16_t count = 0;          // +0x04 [V]
    uint16_t pairs[kMaxPairs * 2];  // +0x06..+0xa9 [V layout] {earfcn, band} [I semantics]
    uint8_t  flag_aa = 0;        // +0xaa [V] <- req[0x74] (u8)
    uint32_t word_ac = 0;        // +0xac [V] <- req[0x78]
    EarfcnBandListReq() : pairs{} {}
};
static_assert(sizeof(EarfcnBandListReq) == EarfcnBandListReq::kWireSize, "wire size");

// 0x71 freq scan (RMH "FreqScan Cell Timer"/rssiSync 10 s), 0x48 B wire [V].
// Built in a 0xb0 frame but length_table[0x71] = 0x48 => only the head
// reaches the DSP. Sender fn 0x80c0ec9c has no direct BL callers in the
// scanned PS/RMH regions (pointer-reached or alternative path) [I].
struct FreqScanReq {
    static constexpr uint16_t kWireSize = 0x48;
    static constexpr int kMaxPairs = 16;          // (0x48 - 6) / 4
    uint16_t reserved = 0;       // +0x00
    uint16_t earfcn = 0;         // +0x02 [V] <- req[0x70]
    uint16_t count = 0;          // +0x04 [V]
    struct Pair { uint16_t a; uint16_t b; };      // {earfcn, band} [I]
    Pair pairs[kMaxPairs];       // +0x06..+0x45 [V layout]
    uint8_t pad[2] = {};         // +0x46 tail of the 0x48 cut
    FreqScanReq() : pairs{} {}
};
static_assert(sizeof(FreqScanReq) == FreqScanReq::kWireSize, "wire size");

// 0x7a CPHY_PAGING_RECEIVE_REQ, 0x8a B [V sender]. The stock payload is a
// PERSISTENT per-card struct at CARD0 + card*0x4254 + 0x47c + 0x2b10
// (caller 0x80ed6eee passes base+0x2b10); RRC fills paging config into it
// (PCCH config, paging occasion [I]). Armed gate: card+0x2000+0xf8f == 1
// (set by 0x4c paging-cfg family) [V]. Driver models it as opaque bytes.
struct PagingReceiveReq {
    static constexpr uint16_t kWireSize = 0x8a;
    uint8_t raw[kWireSize] = {};  // [I] RRC-populated paging config
};

// 0x18 CPHY_CONNECTED_MODE_REQ, 0x6f8 B [V sender]. Sender zeroes +0xb0 u16;
// if payload+0x46c != 0 it first drains a pending CNF (0x80c0d852). Heavy
// RRC-built struct - only attempt after SIB1 decode. Modeled opaque.
struct ConnectedModeReq {
    static constexpr uint16_t kWireSize = 0x6f8;
    uint8_t raw[kWireSize] = {};  // [I] RRC-built; +0x46c = pending-CNF flag seen by sender
};

// 0x16 idle config (2nd phase / stop), 0xf0 B [V sender 0x80c0d5e0], async.
struct IdleConfigReq {
    static constexpr uint16_t kWireSize = 0xf0;
    uint8_t raw[kWireSize] = {};  // [I] builder 0x80ee074e
};

// ---------------------------------------------------------------------------
// 3. Response / CNF structs (resp-pump payloads + DL messages)
// ---------------------------------------------------------------------------

// Resp type 5 (version handshake), <=0x80 B [V] pump 0x80c10a50-0x80c10aee:
// four u16s read at payload+0/2/4/6 -> "Dsp branch:%04x,code svn info:%04x,
// AON chip id:%04x, AON chip version:%04x"; clears IsDspLoadingCode, gives
// g_LteL1AdxTxSema. THIS is the boot handshake answer for cmd 0x06.
struct VersionResp {
    uint16_t dsp_branch = 0;        // +0x00 [V]
    uint16_t code_svn = 0;          // +0x02 [V]
    uint16_t aon_chip_id = 0;       // +0x04 [V]
    uint16_t aon_chip_version = 0;  // +0x06 [V]
};

// Resp type 7 (DSP assert) [V] handler 0x80e2da30: reads {u32,u32} at payload;
// if second u32 == 0x8100 the first u32 indexes a jump table (ids 4..0xc),
// else first u32 < 4 is the id; log "dsp assert:0x%x,0x%x,0x%x,0x%x:%s" with
// name table at init-data 0x81717528+0x20.
struct DspAssertResp {
    uint32_t a0 = 0;   // +0x00 [V] assert id (or jump-table selector)
    uint32_t a1 = 0;   // +0x04 [V] == 0x8100 selects the table path
    uint32_t a2 = 0;   // +0x08 [V]
    uint32_t a3 = 0;   // +0x0c [V]
    static constexpr uint32_t kTableMagic = 0x8100;  // [V]
};

// DL 0x00 freq/cell candidate list [V] handler 0x80ee30a0.
struct CandidateListCnf {
    static constexpr uint16_t kWireSize = 0x150;
    static constexpr int kMaxEntries = 0x29;      // [V]
    struct Entry { int32_t earfcn; int32_t metric; };  // +4 + 8*i [V]
    uint8_t raw[kWireSize] = {};
    static uint16_t count(const uint8_t* p) {           // u16 @+0x14c [V]
        return static_cast<uint16_t>(p[0x14c] | (p[0x14d] << 8));
    }
    static Entry entry(const uint8_t* p, int i) {
        const uint8_t* q = p + 4 + 8 * i;
        Entry e;
        e.earfcn = static_cast<int32_t>(uint32_t(q[0]) | (uint32_t(q[1]) << 8) |
                    (uint32_t(q[2]) << 16) | (uint32_t(q[3]) << 24));
        e.metric = static_cast<int32_t>(uint32_t(q[4]) | (uint32_t(q[5]) << 8) |
                    (uint32_t(q[6]) << 16) | (uint32_t(q[7]) << 24));
        return e;
    }
};

// DL 0x01 5-cell measurement list [V] handler 0x80ee31de (5 x 0x14 B @+8;
// meas i16s scaled (x*100)>>4 by the consumer -> 1/16 units on the wire).
struct MeasCellListCnf {
    static constexpr int kMaxEntries = 5;
    struct Entry {              // 0x14 B
        int16_t earfcn;         // +0x00 -1 = none
        int16_t meas0;          // +0x02 (x*100)>>4 units after conversion
        int16_t meas1;          // +0x04
        uint16_t pad06;         // +0x06
        int16_t meas2;          // +0x08
        uint16_t pad0a;         // +0x0a
        int16_t meas3;          // +0x0c
        uint16_t pad0e;         // +0x0e
        uint32_t w10;           // +0x10 (shared with payload+0x10 u32 [V])
        uint8_t  b14;           // +0x14 (payload+0x14 u8 [V])
        uint8_t  pad15[3];
    };
    uint8_t raw[8 + 0x14 * kMaxEntries] = {};
    static int32_t meas_to_hundredths(int16_t x) { return (static_cast<int32_t>(x) * 100) >> 4; }  // [V]
};

// DL 0x02 MIB buffer-pool descriptor [V] : 0x10 B
// {u16 @+0, u32 @+0xc, u32 @+0x10, u8 @+0x14, u16 @+0x16} - resolves through
// the buffer pool to the MIB save record (SFN/cell-id material).
struct MibBufDesc {
    uint16_t w00;      // +0x00
    uint32_t pool_0c;  // +0x0c
    uint32_t w10;      // +0x10
    uint8_t  b14;      // +0x14
    uint16_t w16;      // +0x16
};

// DL 0x03 sync CNF [V]: i16 timing @+0xa (>>4 stored), u16 @+0xc checked.
struct SyncCnf {
    int16_t  timing_x16;   // +0x0a [V] quarter-units: stock does timing >> 4
    uint16_t status_0c;    // +0x0c [V] checked by handler
    static int32_t timing_div16(int16_t x) { return x >> 4; }
};

// DL 0x0c camp CNF [V]: i16 @+2 timing >>4; card state := 2 (camped).
struct CampCnf {
    int16_t timing_x16;    // +0x02 [V]
};

// DL 0x05/0x06/0x45/0x86 earfcn list result [V]: flag u8 @+2 (1 =>
// SIB1-configured path), count u16 @+4, <=4 x u16 earfcn @+8.
struct EarfcnListCnf {
    static constexpr int kMaxEarfcns = 4;   // [V]
    uint8_t  flag;      // +0x02
    uint16_t count;     // +0x04
    uint16_t earfcns[kMaxEarfcns];  // +0x08
};

// DL 0x47 band verdict list [V]: <=0x29 u8 verdicts (low byte of u16s)
// starting ~+0xa; count @~+0x5c. Guard: card+0x988 == 1.
struct BandVerdictCnf {
    static constexpr int kMaxBands = 0x29;
    uint8_t verdicts[kMaxBands] = {};
    uint8_t count = 0;
};

// ---------------------------------------------------------------------------
// 4. Stock wait/timeout constants (ticks; stock tick = 1 ms class)
// ---------------------------------------------------------------------------
constexpr uint32_t kWaitTicksDefault   = 2000;  // [V] movw r1,#0x7d0 at sync senders
constexpr uint32_t kWaitTicksShortOp   = 0x14;  // [V] cmd 0x48
constexpr uint32_t kWatchdogSyncMs     = 4000;  // [V] "Search or Sync Cell Timer" 4 s
constexpr uint32_t kWatchdogRssiMs     = 10000; // [V] "rssiSync Cell Timer" 10 s
constexpr uint32_t kWatchdogMeasureMs  = 160000;// [V] "Measure Cell Timer" (0x54) 160 s
constexpr uint32_t kWatchdogDeactMs    = 1000;  // [V] ShowDown / CPHY_DEACTIVATE 1 s
constexpr uint32_t kAliveCheckMs       = 30000; // [V] "LTEDSP Alive Check Timer" 30 s
constexpr uint32_t kVersionWaitMs      = 30000; // [V] dump-ctl==0x11 post-version wait

}  // namespace l1
}  // namespace f286
