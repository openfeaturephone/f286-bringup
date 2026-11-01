// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// boot_hal.h - Unisoc UMS9117 (Ruio F286) DSP boot HAL, reference reconstruction.
//
// Ground truth: stock kernel.bin, VA = file_off + 0x80a05e00.  Every function
// below carries the stock VA of the code it mirrors.  NOTE: some older notes
// quote disassembler-space VAs that are 0x38 LOWER than the kernel.bin VA for
// the same instruction; where it matters both are given.
//
// Reconstructed region:
//   - raw HAL setter/getter block  VA 0x811d0c98 .. 0x811d0e4f
//   - thunk wrapper block          VA 0x811a76c6 .. 0x811a7817
//   - LTE pre-boot power release   VA 0x80d0fab4   (0x80d0fab4)
//   - boot-class descriptor table  VA 0x811d0b34   (3 x 0x34-byte table at RAM
//                                                   0x818bebc0, vptr 0x811d0c2c)
//
// VERIFIED = read directly from the disassembly.
// INFERRED = reasoned, not instruction-pinned; flagged inline.

#pragma once

#include <cstdint>
#include <cstddef>

namespace f286 {
namespace dsp {

// ---------------------------------------------------------------------------
// Physical register map (all VERIFIED from the HAL literal pool at VA
// 0x811d0e78: 800e0000 80400000 402e0000 402b1000 402b2000 81705e94
//              402b0000 402e1000 60831000 60832000).
// ---------------------------------------------------------------------------
constexpr uint64_t AON_APB_BASE     = 0x402e0000;  // DSP boot registers
constexpr uint64_t AON_APB2_BASE    = 0x402e1000;  // DSP interrupt-line enable
constexpr uint64_t PMU_BASE         = 0x402b0000;  // PMU status bank (polled)
constexpr uint64_t PMU_A_BASE       = 0x402b1000;  // PMU bank A write mirrors
constexpr uint64_t PMU_B_BASE       = 0x402b2000;  // PMU bank B write mirrors
constexpr uint64_t INT_ROUTER_A     = 0x60831000;  // int-router block A (store target)
constexpr uint64_t INT_ROUTER_B     = 0x60832000;  // int-router block B (store target)

namespace aon {
constexpr uint64_t DSP_CLK_EN     = AON_APB_BASE + 0x6c;   // |=0x9620 on, &=0xffff0000 off [0x811d0db4]
constexpr uint64_t DSP_RESET      = AON_APB_BASE + 0x130;  // bit17=GSM bit16=LTE, 1=hold   [0x811d0ce0/0xcfa]
constexpr uint64_t LTE_BOOT_ADDR  = AON_APB_BASE + 0x248;  // HAL writes VERBATIM           [0x811d0d46]
constexpr uint64_t LTE_START_CTRL = AON_APB_BASE + 0x24c;  // bit0 toggled 0 -> 1           [0x811d0d68]
constexpr uint64_t GSM_BOOT_ADDR  = AON_APB_BASE + 0x250;  // HAL writes VERBATIM           [0x811d0d3e]
constexpr uint64_t GSM_START_CTRL = AON_APB_BASE + 0x254;  // bit0 toggled 0 -> 1           [0x811d0d4e]
constexpr uint64_t CHIP_ID        = AON_APB_BASE + 0xfc;   // 0x98180000/0x98180001         [loader 0x80c2eee6]
}  // namespace aon

namespace pmu {
constexpr uint64_t CORE_PWR_STATUS = PMU_BASE + 0xc0;      // poll: 0x3e0=XDSP-A bits[9:5], 0x7c00=XDSP-B bits[14:10]
constexpr uint64_t SLP_OVR_A       = PMU_A_BASE + 0xcc;    // =0x20000, sleep override A   [0x811d0d8c arg!=0]
constexpr uint64_t SLP_OVR_B       = PMU_B_BASE + 0xcc;    // =0x20000, sleep override B   [0x811d0d8c arg==0]
// Bank CLEAR mirrors: writing a set bit CLEARS that request bit in 0x402b0xxx.  [0x80d0fab4]
constexpr uint64_t B1_CLR_0x34     = PMU_A_BASE + 0x34;    // =0x02000000 clears 0x402b0034 b25
constexpr uint64_t B2_CLR_0x34     = PMU_B_BASE + 0x34;    // =0x03000000 clears 0x402b0034 b25:24 (XDSP core power reqs)
constexpr uint64_t B2_CLR_0x44     = PMU_B_BASE + 0x44;    // =0x02000000 then 0x01000000, clears 0x402b0044 b25,b24
constexpr uint64_t B2_CLR_0xCC     = PMU_B_BASE + 0xcc;    // =0x00020000 clears 0x402b00cc b17
constexpr uint64_t B2_SET_0x44     = PMU_B_BASE + 0x44;    // =0x3000000 written BEFORE the poll (int_enable 0x811d0dce; CLR-bank alias clearing 0x402b0044 b25:24 - name kept, "SET" is a mislabel)
}  // namespace pmu

namespace intr {
constexpr uint64_t DSP_INT_LINE  = AON_APB2_BASE + 0x144;  // =0x1a, DSP interrupt-line enable [0x811d0dce tail]
constexpr uint64_t ROUTER_A_0x44 = INT_ROUTER_A + 0x44;    // =0x40                            [0x811d0dce tail]
constexpr uint64_t ROUTER_B_0x44 = INT_ROUTER_B + 0x44;    // =0x40                            [0x811d0e4a]
}  // namespace intr

// HAL constant getters [0x811d0ca8 / 0x811d0cae / 0x811d0cb4 / 0x811d0cba].
constexpr uint32_t SCRATCH_OFF_A    = 0x10c00;
constexpr uint32_t SCRATCH_OFF_B    = 0x11c00;
constexpr uint32_t SCRATCH_OFF_1000 = 0x1000;
constexpr uint32_t CMD_QUEUE_OFF    = 0xe400;   // L1 cmd-ring control offset inside the DSP window

// Chip-id values at 0x402e00fc, selecting the CM4 firmware partition [0x80c2eee6].
constexpr uint32_t CHIP_ID_CM4_DOWNLOAD = 0x98180000;  // -> "BML CM4 Code  Download Partition" (id 0x10000013)
constexpr uint32_t CHIP_ID_CM4_AB       = 0x98180001;  // -> "BML CM4 Code  AB Download Partition" (id 0x10000018)

// ---------------------------------------------------------------------------
// Hardware abstraction.  A Linux driver implements this with ioremap +
// ioread/iowrite; tests use a RAM mock.  Stock code does plain non-atomic RMW
// on the AON registers (single-writer context) - preserve that.
// ---------------------------------------------------------------------------
class DspHw {
 public:
  virtual ~DspHw() = default;

  // MMIO (physical addresses).
  virtual uint32_t read32(uint64_t pa) = 0;
  virtual void     write32(uint64_t pa, uint32_t value) = 0;

  // DSP-window / shared DRAM (AP-side addresses, e.g. 0x80440000..).
  virtual uint32_t load32(uint32_t va) = 0;
  virtual uint16_t load16(uint32_t va) = 0;
  virtual uint8_t  load8(uint32_t va) = 0;
  virtual void     store32(uint32_t va, uint32_t value) = 0;
  virtual void     store16(uint32_t va, uint16_t value) = 0;
  virtual void     copy_to_dram(uint32_t destVa, const void* src, size_t bytes) = 0;
  virtual void     memset_dram(uint32_t destVa, uint8_t value, size_t bytes) = 0;

  // Cache maintenance before the DSP fetches its image.  INFERRED: the stock
  // BML/DMA path hides this; on Linux, clean D-cache to PoC for the range.
  virtual void     cache_clean_range(uint32_t destVa, size_t bytes) = 0;

  virtual void     delay_ms(uint32_t ms) = 0;
  virtual uint32_t tick_ms() = 0;        // monotonic tick (stock 0x80a0815e)
  virtual void     event_give(int /*id*/) {}  // stock OS event 0x811befde(0,3); no-op OK (DSP polls rings)
  [[noreturn]] virtual void fatal(const char* why) = 0;  // stock 0x80a09414 assert/reboot
};

// ---------------------------------------------------------------------------
// Per-core profile.  The stock HAL is a 3-entry class table (RAM 0x818bebc0,
// stride 0x34, entries {vptr=0x811d0c2c, ...}; init 0x811d0b34).  GSM and LTE
// are the two instances the boot flows drive (VERIFIED via wrapper thunks
// 0x811a76da -> 0x811d0ce0 and 0x811a77ce -> 0x811d0cfa).  The third
// instance is the CM4/WCN companion (booted by the WCN driver, out of scope).
// ---------------------------------------------------------------------------
enum class DspCore { Gsm, Lte };

struct CoreHalProfile {
  DspCore  core;
  uint32_t windowVa;      // AP window onto DSP data/code RAM
  uint32_t codeSize;      // firmware code size per HAL getter
  uint32_t resetMask;     // bit in AON_APB+0x130, set = hold in reset
  uint64_t bootAddrReg;   // written VERBATIM by the HAL
  uint64_t startCtrlReg;  // bit0 toggled 0 -> 1
  uint32_t entryOffset;   // firmware entry point offset inside the load image
};

constexpr CoreHalProfile kGsmHal = {
    DspCore::Gsm,
    0x800e0000,  // window   [pool 0x811d0e78; getter 0x811d0c98, thunk 0x811a76c6]
    0x320000,    // codeSize [getter 0x811d0cc6]
    0x20000,     // bit17    [0x811d0ce0: orr/bic #0x20000 @0x402e0130]
    aon::GSM_BOOT_ADDR,  // [0x811d0d3e: str r0,[r1,#0x250]]
    aon::GSM_START_CTRL, // [0x811d0d4e: bit0]
    0x20080,     // entry = load base + 0x20080 (boot flow 0x810b33fe adds it)
};

constexpr CoreHalProfile kLteHal = {
    DspCore::Lte,
    0x80400000,  // window   [pool 0x811d0e7c; getter 0x811d0c9c, thunk 0x811a77ca]
    0x32c000,    // codeSize [getter 0x811d0ccc]
    0x10000,     // bit16    [0x811d0cfa: orr/bic #0x10000 @0x402e0130]
    aon::LTE_BOOT_ADDR,  // [0x811d0d46: str r0,[r1,#0x248]]
    aon::LTE_START_CTRL, // [0x811d0d68: bit0]
    0x400,       // entry = load base + 0x400 (boot flow 0x80e2d9e6 passes 0x80448400>>1)
};

constexpr const CoreHalProfile& profile_of(DspCore c) {
  return (c == DspCore::Gsm) ? kGsmHal : kLteHal;
}

// ---------------------------------------------------------------------------
// DspBootHal - drives one DSP core through the raw HAL register sequence.
// ---------------------------------------------------------------------------
class DspBootHal {
 public:
  explicit DspBootHal(DspHw& hw, DspCore core) : hw_(hw), prof_(profile_of(core)) {}

  const CoreHalProfile& profile() const { return prof_; }
  uint32_t window() const { return prof_.windowVa; }

  // 0x811d0db4 - VERIFIED both branches.
  void clock_enable(bool on) {
    if (on)
      hw_.write32(aon::DSP_CLK_EN, hw_.read32(aon::DSP_CLK_EN) | 0x9620u);
    else
      hw_.write32(aon::DSP_CLK_EN, hw_.read32(aon::DSP_CLK_EN) & 0xffff0000u);
  }

  // 0x811d0ce0 (GSM) / 0x811d0cfa (LTE).
  void reset_hold(bool hold) {
    if (hold)
      hw_.write32(aon::DSP_RESET, hw_.read32(aon::DSP_RESET) | prof_.resetMask);
    else
      hw_.write32(aon::DSP_RESET, hw_.read32(aon::DSP_RESET) & ~prof_.resetMask);
  }

  // 0x811d0d3e (GSM, 0x250) / 0x811d0d46 (LTE, 0x248).
  // VERIFIED: the HAL stores the value VERBATIM; the entry>>1 shift belongs to
  // the CALLERS (0x80e2d9e6 stores 0x40224200 = 0x80448400>>1; 0x810b33fe
  // stores (0x80100000+0x20080)>>1 = 0x40090040).  Pass the shifted value.
  void set_boot_addr(uint32_t entryShifted) { hw_.write32(prof_.bootAddrReg, entryShifted); }

  // 0x811d0d4e (GSM) / 0x811d0d68 (LTE).
  void start_ctrl(bool high) {
    if (high)
      hw_.write32(prof_.startCtrlReg, hw_.read32(prof_.startCtrlReg) | 1u);
    else
      hw_.write32(prof_.startCtrlReg, hw_.read32(prof_.startCtrlReg) & ~1u);
  }

  // 0x811d0d8c - DSP sleep override (writers also seen at 0x8124c11e/0x8124c430
  // in the RFCAL/diag paths, same registers).
  void pmu_sleep_override(bool bankA) {
    hw_.write32(bankA ? pmu::SLP_OVR_A : pmu::SLP_OVR_B, 0x20000u);
  }

  // 0x80d0fab4 - DSP power release, VERIFIED INSTRUCTION-BY-INSTRUCTION
  // (disassembly; the decompiler view folds the duplicate 0x402b2044 store).
  // Writes go to the PMU bank CLEAR mirrors (each set bit clears the matching
  // request bit in 0x402b0xxx).  "1st poweron" passes lte=true:
  //   lte=true : 0x402b2044 = 0x02000000 ; 0x402b2044 = 0x01000000 (clears
  //              0x402b0044 b25 then b24) ; 0x402b20cc = 0x00020000 (clears
  //              0x402b00cc b17) ; 0x402b2034 = 0x03000000 (clears 0x402b0034
  //              b25:24 - the XDSP-A/B CORE power requests the FPLinux driver
  //              misses).
  //   lte=false: 0x402b1034 = 0x02000000 (clears 0x402b0034 b25) ;
  //              0x402b2034 = 0x01000000 (clears 0x402b0034 b24).
  void release_power(bool lte) {
    if (lte) {
      hw_.write32(pmu::B2_CLR_0x44, 0x02000000u);
      hw_.write32(pmu::B2_CLR_0x44, 0x01000000u);
      hw_.write32(pmu::B2_CLR_0xCC, 0x00020000u);
      hw_.write32(pmu::B2_CLR_0x34, 0x03000000u);
    } else {
      hw_.write32(pmu::B1_CLR_0x34, 0x02000000u);
      hw_.write32(pmu::B2_CLR_0x34, 0x01000000u);
    }
  }

  // Poll criterion used by stock int_enable: the masked core-power bits at
  // 0x402b00c0 must read 0 (route 0 mask 0x3e0 = XDSP-A, route 2 mask 0x7c00 =
  // XDSP-B).  VERIFIED branch structure of 0x811d0dce.
  static uint32_t core_power_mask(int routeId) {
    return (routeId == 0) ? 0x3e0u : 0x7c00u;
  }

  // Runtime int-route table (stock: RAM 0x81705e94 + id*0xc, entries
  // {void* reg; uint32_t mask; uint32_t saved}).  VERIFIED table access pattern;
  // the entry VALUES are populated at runtime by the WCN/RFCAL drivers
  // (INFERRED contents) - fill from your own IRQ map.
  struct IntRoute {
    uint64_t regPa;      // table +0x00: peripheral register base P
    uint32_t mask;       // table +0x04: unmask bit mask
    uint32_t saved;      // table +0x08: stock caches *P here before modifying
  };
  void set_int_route(int id, IntRoute r) { routes_[id & 3] = r; }

  // 0x811d0dce - DSP interrupt enable, route `id`:
  //   1. if ((1 << id) & 0xfffffffa) != 0 -> return (the mask admits ids 0 and 2
  //      only; blocks id 1 and ids 3..31).  VERIFIED.
  //   2. routes_[id].saved = *(u32*)P                       (cache, VERIFIED)
  //   3. *(u32*)(P + 0x2000) = ~mask                        (unmask, VERIFIED:
  //      `P[0x800] = ~mask` in u32 indexing = byte offset 0x2000)
  //   4. 0x402b2044 = 0x3000000 BEFORE the poll (0x811d0dce: this is
  //      the CLR-bank alias clearing 0x402b0044 b25:24 = PD_WTLCP_SYS_CFG
  //      FORCE_SHUTDOWN/AUTO_SHUTDOWN_EN - the "B2_SET_0x44" name below is a
  //      mislabel kept for continuity; pmu.cpp xdsp_power_release_wait puts
  //      this write BEFORE the 0x1000-iteration poll).
  //   5. poll 0x402b00c0 & mask(route): route 0 -> 0x3e0, route 2 -> 0x7c00,
  //      until the masked bits read 0, max 0x1000 iterations (other ids skip).
  //   6. tail: 0x402e1144 = 0x1a ; 0x60831044 = 0x40.
  // The 0x402e1144=0x1a store is the only "doorbell-like" AON write here; it is
  // issued ONCE per int-enable, NOT per message (the ring transport itself is
  // poll-based - the put path's only notify is an OS event give).
  void int_enable(int id) {
    const uint32_t idb = static_cast<uint32_t>(id) & 0xffu;
    if ((1u << idb) & 0xfffffffau) return;

    IntRoute& r = routes_[idb & 3];
    if (r.regPa != 0) {
      r.saved = hw_.read32(r.regPa);
      hw_.write32(r.regPa + 0x2000, ~r.mask);
    }

    hw_.write32(pmu::B2_SET_0x44, 0x03000000u);  // BEFORE the poll (stock order)
    if (idb == 0 || idb == 2) {
      const uint32_t mask = core_power_mask(idb);
      uint32_t iters = 0;
      while ((hw_.read32(pmu::CORE_PWR_STATUS) & mask) != 0) {
        if (++iters >= 0x1000) break;
      }
    }
    hw_.write32(intr::DSP_INT_LINE, 0x1a);
    hw_.write32(intr::ROUTER_A_0x44, 0x40);
  }

  // 0x811d0e4a - the matching restore/disable for route B: delay ~0x50
  // iterations, 0x60832044 = 0x40, then *P = saved value (VERIFIED; note the
  // restore writes P itself, not P+0x2000).
  void int_restore_b(int id) {
    const uint32_t idb = static_cast<uint32_t>(id) & 0xffu;
    if ((1u << idb) & 0xfffffffau) return;
    for (volatile uint32_t i = 0; i < 0x50; ++i) {
    }
    hw_.write32(intr::ROUTER_B_0x44, 0x40);
    const IntRoute& r = routes_[idb & 3];
    if (r.regPa != 0) hw_.write32(r.regPa, r.saved);
  }

 private:
  DspHw&          hw_;
  CoreHalProfile  prof_;
  IntRoute        routes_[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
};

}  // namespace dsp
}  // namespace f286
