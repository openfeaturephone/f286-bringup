// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// boot_flow.h - full stock DSP boot orchestration (UMS9117 / Ruio F286).
//
// Ground truth: stock code, kernel.bin VA space (= file_off + 0x80a05e00).
//   LTE "1st poweron dsp" boot   0x80e2d9e6   (older reports: 0x80e2d9ae, capstone space)
//   LTE pre-boot / sleep-resume  0x80e2d848   (reports: 0x80e2d810)
//   LTE hold-early               0x80e2d832   (reports: 0x80e2d7fa)
//   LTE NV/cal table load        0x80c10ee0   (LTE_Calibration_NV.c)
//   NV9|tick staging             0x80c10e9c   (writes 0x80447E00)
//   GSM boot                     0x810b33fe   (reports: 0x810b33c6)
//   GSM param download           0x810e910c   (reports: 0x810e90d4)
//   GSM patch scan               0x810b3314   (layer1_dsp_download.c)
//   dsplog cmd-6 handler         0x80e2d8d6   ("dspcmd" log control)
//   deep-sleep cmd-0xd sender    0x80e2ded2   (reports: 0x80e2de9a)
//   L1 ADX init + 30s timer      0x80bbf9d0   (reports: 0x80bbf998)
//   alive-check dump write       0x80e2db28   (win+0x10bf8 = 0x33)
//
// Corrections to earlier assumptions:
//   * Boot-addr: HAL stores the caller's value VERBATIM; LTE caller passes
//     0x40224200 literally (= (0x80448000+0x400)>>1).  VERIFIED instruction.
//   * cmd 6 is NOT a fixed boot init message: 0x80e2d9e6's tail calls the
//     "dspcmd log control" handler with (0xff, 0), which sends cmd 6 ONLY when
//     the log-control feature was armed beforehand; otherwise it writes a
//     trace event (0xabcdef51) and returns.  Default payload = 8 zero bytes
//     (the "template" at RAM 0x81717514 = {u8 branch; u8 level; u8 pad; u8 pad;
//     u32 mask} in its reset state).
//   * The 4xu16 helper is hardcoded to cmd 0xd (deep sleep), gated on the
//     DSP-active flag - it is not the cmd-6 boot sender.
//   * The sleep-resume sync word is polled at ABSOLUTE 0x80447E14 and the NV9
//     stage cell is ABSOLUTE 0x80447E00 (window 0x80400000 + 0x47e14/0x47e00).

#pragma once

#include <cstdint>
#include <cstddef>

#include "boot_hal.h"
#include "l1ring.h"

namespace f286 {
namespace dsp {

// ---------------------------------------------------------------------------
// NV items consumed pre-release (VERIFIED item ids/sizes from the loaders).
// A driver sources them from its own NV store (nvcal-backed in stock).
// ---------------------------------------------------------------------------
struct NvSource {
  virtual ~NvSource() = default;
  // Read `len` bytes of NV item `id`; false if absent (stock asserts on some).
  virtual bool read(uint32_t id, uint32_t len, void* out) = 0;
  virtual uint32_t item_len(uint32_t id) = 0;
};

// ---------------------------------------------------------------------------
// LTE DSP bring-up (0x80e2d9e6 and helpers).
// ---------------------------------------------------------------------------
class LteDspBoot {
 public:
  LteDspBoot(DspHw& hw, NvSource& nv)
      : hw_(hw), nv_(nv), hal_(hw, DspCore::Lte), ring_(hw, kLteHal.windowVa) {}

  // ---- pre-boot (0x80e2d848 + 0x80e2d832) -------------------------------

  // First boot: hold start-ctrl low + hold in reset, then release the PMU power
  // domains (0x80d0fab4(1)).  Sleep resume: kick the trigger event, poll the
  // sync word at 0x80447E14 for 0x22222222 (100 x 10ms, fatal on timeout).
  void pre_boot() {
    if (!ever_booted_) {
      // 0x80e2d832
      hal_.start_ctrl(false);
      hal_.reset_hold(true);
      booted_ = false;
      // 0x80d0fab4(1) - the PMU clears the FPLinux driver currently misses.
      hal_.release_power(true);
    } else {
      uint32_t retries = 0;
      hw_.event_give(3);
      do {
        if (retries > 99) break;
        hw_.delay_ms(10);
        ++retries;
      } while (ring_.sleep_sync_word() != 0x22222222u);
      if (retries > 99)
        hw_.fatal("LTE_ADX: can not receive dsp resp ,dsp is still in sleep state!");  // layer1_adx_dsp.c 0x115
      hw_.delay_ms(10);
    }
    ever_booted_ = true;
  }

  // ---- NV/cal staging (0x80c10ee0 + 0x80c10e9c) --------------------------

  // AP-side calibration/NV tables (0x80c10ee0): item 0x4c "EUTRA customer
  // settings" (0x90 B), item 0x22 "cali common" (0x3c B) at RAM 0x80678000,
  // then ~25 per-band calibration tables (items 0x15,0x1a,0x1b,0x1c,0x1d,0x1e,
  // 0x1f,0x20,0x21,0xb,0xc,0xd,0xe,0xf,0x10,0x11,0x12,0x13,0x14,0x16,0x17,0x18,
  // ...) chained in one buffer, pointer table built at RAM 0x817073b4+.
  // These stay AP-side; the DSP pulls them via the cal channel (resp type 2).
  // A driver can load them lazily; only item 0x4c/0x22 are read unconditionally.
  void load_cal_nv_tables(void* scratchBuf, size_t scratchLen) {
    (void)scratchLen;
    uint8_t* p = static_cast<uint8_t*>(scratchBuf);
    if (nv_.item_len(0x4c) == 0x90) nv_.read(0x4c, 0x90, p);  // EUTRA_CUSTOMER_SETTINGS
    p += 0x90;
    if (nv_.item_len(0x22) == 0x3c) nv_.read(0x22, 0x3c, p);  // CALI_COMMON
    // Per-band tables: NV ids/sizes tabulated in; a minimal
    // boot (camping smoke test) does not need them (INFERRED).
  }

  // NV9|tick staging (0x80c10e9c, VERIFIED): [0x80447E00] = nv9(u32);
  // [0x80447E00] |= tick << 16.  Missing NV -> 0.
  void stage_nv9_cell() {
    const uint32_t cellVa = kLteHal.windowVa + ring::NV9_CELL_OFF;  // 0x80447E00
    uint32_t nv9 = 0;
    nv_.read(9, 4, &nv9);
    const uint32_t tick = hw_.tick_ms();
    hw_.store32(cellVa, nv9);
    hw_.store32(cellVa, hw_.load32(cellVa) | (tick << 16));
  }

  // ---- the boot sequence itself (0x80e2d9e6, VERIFIED step-for-step) -------

  void boot() {
    pre_boot();                      // 0x80e2d848  (incl. 0x80d0fab4(1) on first boot)
    load_cal_nv_tables(calBuf_, sizeof(calBuf_));  // 0x80c10ee0
    stage_nv9_cell();                // 0x80c10e9c
    hal_.clock_enable(true);         // 0x811d0db4(1)   0x402e006c |= 0x9620
    hal_.start_ctrl(false);          // 0x402e024c bit0 = 0
    hal_.reset_hold(true);           // 0x402e0130 |= 0x10000
    // VERIFIED literal from 0x80e2d9e6: thunk_811d0d46(0x40224200).
    // = (load base 0x80448000 + entry 0x400) >> 1.  HAL stores it VERBATIM.
    hal_.set_boot_addr(0x40224200u);
    hal_.start_ctrl(true);           // 0x402e024c bit0 = 1
    hal_.reset_hold(false);          // RELEASE - DSP runs
    booted_ = true;
    dspActive_ = false;              // until the version handshake answers
    // 0x80e2d9e6 tail: 0x80e2d8d6(0xff, 0) - sends cmd 6 ONLY when the
    // dspcmd log-control was armed (see send_dsplog_cmd6).  No default command
    // is sent on a stock cold boot.
    send_dsplog_cmd6_if_armed();
  }

  // ---- dspcmd log control (0x80e2d8d6) --------------------------------------

  // Payload block (RAM 0x81717514, 8 bytes): {u8 branch; u8 level; u8 pad;
  // u8 pad; u32 mask}.  branch: 1 = "1622", 0 = "321" (dspcmd arg parsing);
  // defaults are all-zero - which is exactly the "cmd 6 template" the older
  // reports saw.
  struct DspLogControl {
    uint8_t  branch;
    uint8_t  level;
    uint8_t  pad[2];
    uint32_t mask;
  };
  static_assert(sizeof(DspLogControl) == 8, "stock template size");

  void arm_dsplog_control(const DspLogControl& cfg) {
    logControl_ = cfg;
    logControlArmed_ = true;  // stock [0x81717508]
  }

  void send_dsplog_cmd6_if_armed() {
    if (!logControlArmed_) return;  // stock: trace 0xabcdef51, return - NO cmd 6
    uint8_t raw[8];
    for (int i = 0; i < 4; ++i) raw[i] = reinterpret_cast<const uint8_t*>(&logControl_)[i];
    for (int i = 0; i < 4; ++i) raw[4 + i] = reinterpret_cast<const uint8_t*>(&logControl_.mask)[i];
    ring_.send_fixed8(6, raw);
  }

  // ---- handshake / housekeeping ------------------------------------------------

  bool dsp_active() const { return dspActive_; }

  // Wait for the version handshake (resp type 5) - stock clears
  // IsDspLoadingCode and prints "LTEDSP_Ver[%d]: %s" here.  The 30s alive
  // watchdog below is the timeout budget.
  bool wait_handshake(uint32_t timeoutMs) {
    struct V : L1CmdRing::Sink {
      bool got = false;
      void on_version(const ring::VersionResp&, const char*, uint16_t) override { got = true; }
    } sink;
    const uint32_t deadline = hw_.tick_ms() + timeoutMs;
    while (!sink.got) {
      ring_.pump(sink);
      if (hw_.tick_ms() >= deadline) return false;
      hw_.delay_ms(10);
    }
    dspActive_ = true;  // stock [0x8175ddca] (set in the version branch)
    return true;
  }

  // Deep-sleep request (0x80e2ded2, VERIFIED): 4x u16 payload, raw 8 bytes,
  // cmd 0xd - refused unless the DSP is active (handshake done).
  bool request_deep_sleep(uint16_t p0, uint16_t p1, uint16_t p2, uint16_t p3) {
    if (!dspActive_) return false;
    const uint8_t raw[8] = {
        static_cast<uint8_t>(p0), static_cast<uint8_t>(p0 >> 8),
        static_cast<uint8_t>(p1), static_cast<uint8_t>(p1 >> 8),
        static_cast<uint8_t>(p2), static_cast<uint8_t>(p2 >> 8),
        static_cast<uint8_t>(p3), static_cast<uint8_t>(p3 >> 8)};
    return ring_.send_fixed8(0x0d, raw);
  }

  L1CmdRing& ring() { return ring_; }
  DspBootHal& hal() { return hal_; }

 private:
  DspHw&      hw_;
  NvSource&   nv_;
  DspBootHal  hal_;
  L1CmdRing   ring_;
  bool        ever_booted_ = false;   // [0x8175ddb6]
  bool        booted_ = false;        // [0x8175ddb0]
  bool        dspActive_ = false;     // [0x8175ddca]
  bool        logControlArmed_ = false;  // [0x81717508]
  DspLogControl logControl_{};
  uint8_t     calBuf_[0x90 + 0x3c];   // items 0x4c + 0x22 (band tables go beyond)
};

// ---------------------------------------------------------------------------
// GSM DSP bring-up (0x810b33fe + 0x810e910c).
// ---------------------------------------------------------------------------
class GsmDspBoot {
 public:
  GsmDspBoot(DspHw& hw, NvSource& nv)
      : hw_(hw), nv_(nv), hal_(hw, DspCore::Gsm), ring_(hw, kGsmHal.windowVa) {}

  // 0x810b33fe, VERIFIED step-for-step.  GSM has NO PMU release call here and
  // NO cmd-6 tail; its handshake/housekeeping is the GSM driver's business.
  void boot() {
    hal_.reset_hold(true);     // thunk_811d0ce0(1)  0x402e0130 |= 0x20000
    if (!paramsLoaded_) param_download();  // 0x810e910c
    hal_.start_ctrl(false);    // thunk_811d0d4e(0)  0x402e0254 bit0 = 0
    hal_.reset_hold(true);     // idempotent second hold (VERIFIED in stock)
    hal_.clock_enable(true);   // 0x811d0db4(1)
    // thunk_811d0d3e(([0x81712f1c] + 0x20080) >> 1), base = 0x80100000:
    hal_.set_boot_addr(static_cast<uint32_t>((0x80100000u + kGsmHal.entryOffset) >> 1));  // 0x40090040
    for (volatile uint32_t i = 0; i < 100; ++i) {
    }                        // stock: empty 100-iteration loop
    hal_.start_ctrl(true);   // 0x402e0254 bit0 = 1
    hal_.reset_hold(false);  // RELEASE
    // 0x811d0d18 stub + BLX 0x80fc467c post-boot hook (INFERRED no-op for us).
    paramsLoaded_ = true;    // [0x81712ab4] - set even when the download was skipped
  }

  // 0x810e910c, VERIFIED.  Writes parameter blocks into the GSM DSP DRAM
  // image WHILE THE CORE IS HELD IN RESET (caller must hold it, stock does).
  void param_download() {
    const uint32_t base = 0x80100000u;  // [0x81712f1c]

    // cfg block
    hw_.store32(base + 0x20044, 0);
    hw_.store32(base + 0xb8000, 0);
    hw_.store32(base + 0x20048, 0xbb);  // default; 0xaa when config flag == 1
    // (stock picks 0xaa only if [0x81706e44]==1 and [0x81712ecc]!=1)

    // param block @base+0x19670, 0x6f8 bytes, one-shot guard [0x81706e45]
    hw_.memset_dram(base + 0x19670, 0, 0x6f8);
    hw_.store32(base + 0x19670 + 0x638, hw_.tick_ms());  // clock/time value
    hw_.store32(base + 0x19670 + 0x63c, 0);
    uint32_t featureFlag = 1;  // stock: NV-backed; 0x80c2f4ae(0, base+0x19cb0) result
    nv_.read(0, 4, &featureFlag);  // INFERRED source mapping
    hw_.store32(base + 0x19670 + 0x6ec, featureFlag);
    hw_.store32(base + 0x19670 + 0x6f4, 0);
    hw_.store32(base + 0x19670 + 0x774, hw_.tick_ms());
    // +0x6e4: {u8 flag, u16 val} packed by 0x80a080f2(2,&x) - encode 0
    hw_.store32(base + 0x19670 + 0x6e4, 0);
    hw_.store32(base + 0x19670, 1);  // header word = 1 (last store in stock)
    // RF calibration fills: 0x80b1c610 + 0x80b18218 (nvcal-backed tables;
    // a driver without RF cal can skip - GSM DSP still boots, INFERRED).
    hw_.cache_clean_range(base, 0x300000);
  }

  L1CmdRing& ring() { return ring_; }
  DspBootHal& hal() { return hal_; }

 private:
  DspHw&      hw_;
  NvSource&   nv_;
  DspBootHal  hal_;
  L1CmdRing   ring_;
  bool        paramsLoaded_ = false;
};

// ---------------------------------------------------------------------------
// 30 s alive watchdog (0x80bbf9d0 timer + 0x80e2dafe cb chain).
// Stock: period 30000 ms ("LTEDSP Alive Check Timer"); on timeout it only
// DUMPS (win+0x10bf8 = 0x33 -> DSP dumps 0x4000 words HDLC-escaped) - there is
// no recovery path.  Re-armed by every resp/DL in stock semantics.
// ---------------------------------------------------------------------------
class AliveWatchdog {
 public:
  explicit AliveWatchdog(DspHw& hw, LteDspBoot& lte) : hw_(hw), lte_(lte) {}

  void feed() { lastFed_ = hw_.tick_ms(); }  // call from every resp/DL
  bool expired(uint32_t now) const {
    return lte_.dsp_active() && (now - lastFed_ >= 30000);
  }
  void on_timeout() { lte_.ring().request_dump(); }  // stock 0x80e2db28

 private:
  DspHw&      hw_;
  LteDspBoot& lte_;
  uint32_t    lastFed_ = 0;
};

}  // namespace dsp
}  // namespace f286
