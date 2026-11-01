// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sim_path.h -- f286::sim: USIM activation, ATR parse, T=0 APDU transport.
//
// Clean-room C++ transcription of the stock Unisoc UMS9117 SIM path in
// kernel.bin (VA = file_off + 0x80a05e00; all VAs below are kernel.bin VAs).
//
// HEADLINE (answers "who drives the SIM"): the AP CPU drives the SIM chip
// DIRECTLY through the USIM controller at 0x70F00000 (card0) / 0x71000000
// (card1). Literal-pool sweep of the whole usim_drv region
// (0x80c5d400-0x80c64800) finds ONLY these MMIO bases: 0x402e1000 (AON APB
// APB_EB0 SET-bank base)/0x402e1540/0x402e2540 (PUBCP_SIM1_TOP_CTRL SET/CLR
// aliases; +4/+8 = SIM2/SIM3,-M3), 0x71300000/0x71301000/
// 0x71302000 (pad
// mux), 0x70f00000 (controller) -- ZERO DSP-window (0x804xxxxx/0x800exxxx)
// and ZERO L1-ring accesses. The DSP/L1 is not in the SIM path at all; the
// "card" L1 command 0x9f is DSP capability info, not the SIM.
//
// Layering in stock (VAs of the layers, assert-string anchored):
//   usim_drv.c        0x80c5d5c2..0x80c64800  HW + ISO-7816 char layer (this file's scope)
//   sim_uicc_*.c      0x80a93874/0x80a9d6ec   APDU framing + card mgmt msgs
//   mnc_sim_*.c       0x80a2aeb8/0x80ab8c7c   card manager (PIN state, app list)
//   usim_al_*.c       0x80b60284              EF API + AUTHENTICATE
//   mnsim_*.c         0x80b4c0d0/0x80bada28   signals to ATC (CPIN owner)
//
// Everything hardware touches IUsimHw only, so the parser/transport run on
// the host (record/replay double) or in the Linux driver (ioremap + ADI).
#pragma once

#include <cstdint>

namespace f286 {
namespace sim {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

enum class Confidence { Verified, Inferred };

// ---------------------------------------------------------------------------
// Register map (all offsets VERIFIED in the register-access helpers:
// TX 0x80c64070, RX 0x80c6253a/0x80c62ea8, IRQ 0x80c62fae/0x80c62dbc,
// init 0x80c61358, reset 0x80c5fd88, clock 0x80c632dc, FIFO counts
// 0x80c640c0/0x80c5e324).
// ---------------------------------------------------------------------------
constexpr u32 USIM0_BASE = 0x70F00000u;   // base-array literal @0x80c60404 /
constexpr u32 USIM1_BASE = 0x71000000u;   // pair @0x814c8240 {0x70F00000,0x71000000}
constexpr u32 USIM_IRQ[2] = {10, 13};     // reset/activate fn, BL os-irq @0x80c63438
// The per-card "on" write must
// hit the SET alias. 0x402E0540 =
// PUBCP_SIM1_TOP_CTRL, 0x402E1540 = its SET, 0x402E2540 = its CLR alias;
// +4/+8 = PUBCP_SIM2/SIM3_TOP_CTRL (SET 0x1544/0x1548, CLR 0x2544/0x2548).
// The old single array made card1's ON write clear SIM1 instead.
constexpr u32 AON_SIM_CTRL_SET[2] = {0x402e1540u, 0x402e1544u};  // SET bank (card2 would be 0x402e1548)
constexpr u32 AON_SIM_CTRL_CLR[2] = {0x402e2540u, 0x402e2544u};  // CLR bank (card2 would be 0x402e2548)
constexpr u32 PAD_CFG[2] = {0x71301000u, 0x71302000u};       // write 0x800000
// misnomer fix -- 0x402e1000 is the AON APB_EB0 SET-bank
// BASE the stock helper offsets by 0x540+4*ctrl (no SIM bit exists in
// APB_EB0; SIM gates are APB_EB2 b20:16 @0x402E10B0).
constexpr u32 AON_APB_SET_BANK = 0x402e1000u;                // literal, helper @0x80c5df70

// ADI (analog die) window: vddsim LDO. Stock kernel.bin NEVER enables it
// (the ldo print @0x80c5db54 has zero xrefs; rail table 0x814c7124 is dead
// code) -- the PMU OTP default must already be ON in stock, or vddsim is
// wired to a strap. FPLinux must switch it on itself .
constexpr u32 ADI_VDDSIM0_EN  = 0x40608CC0u;  // bit0 = LDO enable
constexpr u32 ADI_VDDSIM0_SEL = 0x40608CC4u;  // 0=1.8V, 2=3.0V (voltage table)

namespace reg {
constexpr u32 TX      = 0x00;  // W: byte to card FIFO
constexpr u32 RX      = 0x04;  // R: byte from card FIFO
constexpr u32 INT_RAW = 0x08;  // R: raw int status (bits 3/4/5/6 tested)
constexpr u32 INIT0   = 0x0c;  // init block write
constexpr u32 INT_EN  = 0x10;  // RMW; bit0 = RX
constexpr u32 EVT_CLR = 0x14;  // W: 0xffff = clear all; per-event 4/8/0x10/0x20
constexpr u32 CTRL    = 0x18;  // activation writes 0x2003 = bits 0,1,13
                               //   (0x80C633E0-E8). bit10
                               //   (0x400) = TX_EN (stop = clear it), bit12
                               //   0x1000 = DO_DEACT, bit11 0x800 = DO_ACT,
                               //   bit9 RX_EN, bit7 POWER_EN, bit6 SIM_RST;
                               //   bit15 cleared after ATR timeout;
                               //   0x8000 at init; 0x40/0x80 reset handshake
constexpr u32 CLK     = 0x1c;  // SIM_CTL1 CLK_MODE[10:8] ARM-clk divider
                               //   (spec Table 5-126): 0x100=13M 0x200=26M
                               //   0x300=64/76.8M 0x400=128M; bit11 CLK_EN
                               //   (clock stop), bit4 CLK_PL_SEL
constexpr u32 ETU_TX  = 0x20;  // NOTE : +0x20 is the spec's
                               //   SIM_RX_CK_DVD and +0x34 the SIM_TX_CK_DVD --
                               //   the two names are swapped vs the spec; both
                               //   get the same reload value, so behavior is
                               //   unchanged (SIM_SEH.TX_ETU_SEL rst 0)
constexpr u32 RST_DLY = 0x24;  // SIM_SEH retransmit control
                               //   ([7:4] TX_RETRX_LMT, [3:0] RX_RETRX_LMT) --
                               //   warm-path 0xCC = 12/12 limits, DMA off
constexpr u32 INIT1   = 0x2c;
constexpr u32 ETU_RX  = 0x34;  // same reload as ETU_TX (see swap note above)
constexpr u32 IRQ_EN  = 0x3c;  // SIM_CTL2, sole field
                               //   RX_INT_MARK [8:0] receive watermark (rst
                               //   0x100) -- stock writes 1 then |= 0x101 are
                               //   watermark values, NOT enables (0x80C63426-2E:
                               //   movw r2,#0x101; orrs; str [r0,#0x3c])
constexpr u32 FIFO_RX_CNT = 0x80;  // & 0x1ff
constexpr u32 FIFO_TX_CNT = 0x84;  // & 0x1ff
}  // namespace reg

// ISO 7816-3 tables, stock literal @0x814e8854 (6 code refs; u16 array).
// [0..7] = clock divisors (2..256); [8..21] = the Fi column in ISO order
// (Fi(0)=372, Fi(1)=372, Fi(2)=558, Fi(3)=744, Fi(4)=1116, Fi(5)=1488,
// Fi(6)=1860, Fi(7)=Fi(8)=RFU, Fi(9)=512, Fi(10)=768, Fi(11)=1024,
// Fi(12)=1536, Fi(13)=2048). ETU reload = divisor row (kFdTable[0..7]) * Fi
// (spec default 744 = 2*372).
constexpr u16 kFdTable[22] = {
    2, 4, 8, 16, 32, 64, 128, 256,                  // divisors
    372, 372, 558, 744, 1116, 1488, 1860, 0,        // Fi[0..7]  (7 = RFU)
    0, 512, 768, 1024, 1536, 2048,                  // Fi[8..13] (8 = RFU)
};
constexpr u8 kDefaultFiIndex = 8;  // kFdTable[8] = 372 (stock defaults F=0x174,
                                   //  D=1: 0x80c6081e case 3 writes 0x174
                                   //  when the negotiated value is unset)
constexpr u8 kDefaultDivisorIndex = 0;  // kFdTable[0] = 2 -> default 744 ETU
constexpr u16 kEtuBase = 0x174;    // 372, stock literal multiplier

// Stock debug timers (OS-tick, 0x80c634e0): clk-stop 5, ATR 300, RST 300,
// Status-Word 150, BWT 1100, WAIT-RESPONSE 5000.
constexpr u32 TicksAtr = 300;
constexpr u32 TicksSw = 150;
constexpr u32 TicksBwt = 1100;
constexpr u32 TicksWaitResp = 5000;

// ---------------------------------------------------------------------------
// Hardware surface. One implementation per environment (Linux driver:
// ioremap for the 0x7xxx/0x402e pages + ADI shim for 0x4060 8xxx + GIC irq).
// ---------------------------------------------------------------------------
class IUsimHw {
public:
    virtual ~IUsimHw() = default;
    virtual u32 read32(u32 pa) = 0;
    virtual void write32(u32 pa, u32 value) = 0;
    // ADI analog-die 16-bit access (vddsim LDO).
    virtual u16 adi_read(u32 pa) = 0;
    virtual void adi_write(u32 pa, u16 value) = 0;
    // Controller IRQ (stock uses OS API 0x8113e6e6 with irq 10/13).
    virtual void irq_enable(int card) = 0;
    virtual void irq_disable(int card) = 0;
    virtual bool wait_event(u32 ticks) = 0;   // IRQ/timer wait, false on timeout
    virtual void spin(u32 iterations) = 0;    // stock busy-delay idiom
    virtual void delay_ticks(u32 ticks) = 0;  // stock OS-timer tick delay
};

// ---------------------------------------------------------------------------
// ATR -- ISO 7816-3 decode. Pure function over the received bytes; mirrors
// what stock assembles per byte:
//   * TS accepted = 0x3B/0x3F plus wire-complement forms 0xC0/0xC4/0xFC/0xDC/
//     0x03/0x23 -- 0x80c6081e case 2 ('?'=';'/complement classify into
//     states 0x81710293[u] = 1/2/3).
//   * T0 low nibble = K (0x80c61b90 stores byte&0xf to atr_ctx+0xc); high
//     nibble = Y1 presence bits (TD1/TC1/TB1/TA1 tracked in atr_ctx).
//   * completion flag = atr_ctx+0x2a once hist-count reaches K
//     (byte loop 0x80c61d14..0x80c620e0); TCK = XOR of TS..TCK-1
//     (0x80c61776 + 0x80c5e248 xor-fold over atr_ctx+0x1c buffer and the
//     0x8194ae50 historical ring), compared to buffer[len].
//   * TA1 -> Fi/Di selectors stored to 0x81710360/0x81710370, then ETU =
//     kFdTable[sel]*0x174 and optional PPS/warm-reset (0x80c6081e case 3).
//   * ATR handed up by 0x80a97450(ctrl, len, buf) (sim_uicc layer) and the
//     card-active signal 0x80c5f892(u, 0).
// ---------------------------------------------------------------------------
struct Atr {
    u8 raw[33];
    u32 len = 0;
    u8 ts = 0;
    bool direct = true;      // true: TS=0x3B direct convention
    u8 y1 = 0;               // T0 high nibble
    u8 k = 0;                // T0 low nibble: historical-bytes count
    u8 t_protocol = 0;       // first TD low nibble; 0 when no TD1
    u8 ta[4] = {0, 0, 0, 0}; bool ta_present[4] = {false, false, false, false};
    u8 tb[4] = {0, 0, 0, 0}; bool tb_present[4] = {false, false, false, false};
    u8 tc[4] = {0, 0, 0, 0}; bool tc_present[4] = {false, false, false, false};
    u8 td[4] = {0, 0, 0, 0}; bool td_present[4] = {false, false, false, false};
    u8 historical[15] = {0};
    u32 historical_len = 0;
    u8 tck = 0;              // 0 when absent (T=0, no T>0 declared)
    bool tck_present = false;
    bool tck_ok = false;     // XOR of TS..TCK-1 == TCK (stock xor-fold check)
    u16 fi = 372;            // from TA1 high nibble via kFdTable
    u8 di = 1;               // from TA1 low nibble (1,1,2,4,8,12,16,32,...)
};

// Decode up to `len` received bytes. Returns false if the ATR is malformed
// beyond recovery (bad TS, or truncated before historical bytes end).
bool parse_atr(const u8* buf, u32 len, Atr* out);

// ETU reload value the stock writes to +0x20/+0x34: divisor * Fi.
// (FUNCTIONAL): the old kFdTable[fi]*372 overflowed the
// [15:0] divider (372*372 = 0x21C90 -> effective 0x1C90 = 7312). Spec
// anchors: SIM_RX_CK_DVD reset 0x1740 = 16*372 () and functional default
// etu_dvdr 744 = 2*372 (), i.e. reload = clock-divisor row (kFdTable
// [0..7]) * Fi (kFdTable[8..21]). The divisor index stock derives from TA1
// low nibble (0x80c5f7a4) is not fully pinned -- divisor row 0 (=2, the
// 26 MHz case) is the spec/stock default; higher rows are [I].
constexpr u32 etu_reload(u16 divisor_index, u16 fi_index) {
    return static_cast<u32>(kFdTable[divisor_index & 0x7u]) *
           static_cast<u32>(kFdTable[8 + (fi_index <= 13u ? fi_index : 0u)]);
}

// ---------------------------------------------------------------------------
// Activation (transcribed from 0x80c61358 init + 0x80c5fd88 reset +
// 0x80c5ded4/0x80c5faa6 power-down + pad fn 0x80c63dd6 + AON helper
// 0x80c5df70). Per-step register writes VERIFIED (values+cites above); the
// overall sequence is a port flow [I]: stock card_reset 0x80c5fd88 is
// TYPE-dispatched with per-card retry counters, not this fixed sequence.
// ---------------------------------------------------------------------------
void power_down_clear(IUsimHw& hw, int card);       // stock pre-reset quiesce
void controller_init(IUsimHw& hw, int card);        // 0x80c61358 init block
void pads_enable(IUsimHw& hw, int card);            // 0x71301000/2000 = 0x800000
void aon_enable(IUsimHw& hw, int card);             // AON_SIM_CTRL_SET[card] = 0x20
                                                    //   (0x20 = b5 cp_simX_clk_pl,
                                                    //   SIM clock polarity;
                                                    //)
void vddsim_enable(IUsimHw& hw);                    // ADI 0x40608CC0 |= 1
void card_reset(IUsimHw& hw, int card, bool warm);  // 0x80c5fd88
// Full cold activation; returns 0 on ATR ok, -1 timeout, -2 bad ATR.
int activate(IUsimHw& hw, int card, Atr* atr);

// ---------------------------------------------------------------------------
// T=0 transport. Byte behavior matches the stock char handlers:
//   * RX compare (0x80c620e4): received == last-sent -> echo; == ~last-sent
//     -> retransmit (0x80c64070 re-write); == 0x60 -> NULL byte (stock
//     counts >0x3c then re-arms a timer); else data byte -> ring.
//   * ACK semantics: procedure byte == INS -> all remaining data; ~INS ->
//     none; INS+1/INS-1 -> one byte. (T=0 as used by sim_uicc templates.)
//   * SW: last two bytes of the response (stock Status-Word timer 150).
//   * 0x6C xx -> re-issue with P3=xx; 0x9F xx / 0x61 xx -> GET RESPONSE (0xC0).
// ---------------------------------------------------------------------------
struct T0Apdu {
    u8 cla, ins, p1, p2, p3;
    const u8* data;      // may be null when tx_len==0
    u32 tx_len;          // bytes to SEND (command data)
    u32 rx_len;          // expected response data (0 -> P3 field is tx_len? stock:
                         //  P3 = tx_len ? tx_len : rx_len on the wire)
};

class T0Transport {
public:
    T0Transport(IUsimHw& hw, int card) : hw_(hw), card_(card) {}
    // Raw T=0 transaction with auto GET RESPONSE. out: response data;
    // sw: trailing SW1 SW2. Returns false on timeout/parity exhaustion.
    bool xfer(const T0Apdu& a, u8* out, u32 out_cap, u32* out_len, u16* sw);
    // SELECT by file id (2 bytes) on the current DF.
    bool select(u16 file_id, u8* fci, u32 fci_cap, u32* fci_len);
    // READ BINARY on the currently selected transparent EF.
    bool read_binary(u32 offset, u8* out, u32 len);
    // GET RESPONSE continuation (INS 0xC0), sw1sw2 = e.g. 0x9Fxx/0x61xx.
    bool get_response(u8 want, u8* out, u32 out_cap, u32* out_len, u16* sw);

private:
    bool tx_bytes(const u8* p, u32 n);
    int rx_byte(u8* b);  // 0 ok, -1 timeout; applies the ~INS retransmit rule
    IUsimHw& hw_;
    int card_;
    u32 base_;
};

// ---------------------------------------------------------------------------
// CPIN chain (minimal, T=0, no PIN, no USIML3 -- the three EFs are
// PIN-transparent; stock EF table @0x8154b560: ICCID 0x2FE2 {3F00}
// transparent 10B, IMSI 0x6F07 {3F00,7F20} 9B).
// ---------------------------------------------------------------------------
struct SimIdentity {
    bool present = false;
    bool atr_ok = false;
    char imsi[16] = {0};   // decimal string, from EF_IMSI
    char iccid[21] = {0};  // decimal string, from EF_ICCID
};
// vddsim -> AON -> pads -> init -> cold reset -> ATR -> SELECT 3F00 ->
// SELECT 7F20 -> SELECT 6F07 -> READ BINARY -> SELECT 2FE2 -> READ BINARY.
// Returns 0 and fills `id` on success (AT+CPIN? -> "READY", AT+CIMI, CCID).
int read_identity(IUsimHw& hw, SimIdentity* id);
// BCD-swapped-digit decode used by both EFs (stock parses the same wire form).
void decode_bcd_digits(const u8* raw, u32 n, char* out, u32 out_cap);
// EF_IMSI semi-octet decode, TS 51.011 section 10.3.2 :
// body[0] = digit count, body[1] lo nibble = parity digit (skipped), digit 1 =
// body[1] HI nibble, then lo/hi of body[2..]. 15 digits max. pySim vector
// 08 29 82 60 82 00 00 20 80 -> "228062800000208" (sms_selftest.cpp #11).
void decode_imsi_digits(const u8* body, u32 len, char* out, u32 out_cap);

}  // namespace sim
}  // namespace f286
