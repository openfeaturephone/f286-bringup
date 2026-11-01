// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// clocks.cpp -- AON gates / TWPLL DSP branches / RF-gate helpers, stock-transcribed.
// Comment VAs are stock instruction addresses; file_off = VA - 0x80a05e00.
#include "clocks.h"

#include "pmu.h"    // STATUS1 for the RF busy-delay idiom
#include "whale.h"  // whale_ensure_on

namespace f286 {
namespace power {

// stock: 0x8136F558
Confidence aon2_gate_pair(PmuBus& bus) {
    // 0x8136F558: [0x402E1004] = 0x100 ; [0x402E1010] = 0x200
    bus.write32(AON2_BANK + 0x004u, 0x100u);
    bus.write32(AON2_BANK + 0x010u, 0x200u);
    // (The keyed part of the stock helper -- 0xE551 -> [0x40310020], [0x40310008] |= 4,
    //  close 0xFFFF1AAE -- drives the AP deep-sleep TIMER engine, not a gate; see
    //. Intentionally not reproduced here.)
    return Confidence::Verified;
}

// stock: 0x8136F57A (caller-less in this build)
Confidence aon3_gate_pair(PmuBus& bus) {
    bus.write32(AON3_BANK + 0x004u, 0x100u);
    bus.write32(AON3_BANK + 0x010u, 0x200u);
    return Confidence::Verified;
}

// stock: 0x80A072E4 (values from step 2)
Confidence aon_bus_core_enable(PmuBus& bus) {
    bus.write32(AON3_BANK + 0x020u, 0xC'0000u);  // AON3 sel 1
    bus.write32(AON2_BANK + 0x020u, 0x4'0000u);  // AON2 sel 3
    return Confidence::Inferred;  // not re-disassembled 
}

// stock: 0x81252240 tail / 0x8125226E tail
Confidence rf_gate_0x134(PmuBus& bus) {
    // 0x8125224C: movw r0, #0x1000 ; ldr r1, =0x402E1000 ; str r0, [r1, #0x134]
    bus.write32(AON2_BANK + 0x134u, 0x1000u);
    // 0x81252292..98: same write to the AON3 bank (dump path)
    bus.write32(AON3_BANK + 0x134u, 0x1000u);
    return Confidence::Verified;
}

// stock: 0x811D0D7C
Confidence aon_dsp_clock_enable(PmuBus& bus, bool enable) {
    // 0x811D0D7C ldr r1, =0x402E0000 ; 0x811D0D80 ldr r0, [r1, #0x6c]
    u32 v = bus.read32(AON_BASE + 0x6Cu);
    if (enable) {
        // 0x811D0D84 movw r2, #0x9620 ; 0x811D0D88 orrs r0, r2
        v |= 0x9620u;
    } else {
        // 0x811D0D8C bfc r0, #0, #16  (clear bits 15:0)
        v &= 0xFFFF'0000u;
    }
    // 0x811D0D90 str r0, [r1, #0x6c]
    bus.write32(AON_BASE + 0x6Cu, v);
    return Confidence::Verified;
}

// stock: 0x811D0DFA..0x0E
Confidence dsp_int_enable_tail(PmuBus& bus) {
    // 0x811D0DFA ldr r1, =0x402E1000 ; 0x811D0DFE str 0x1A, [r1, #0x144]
    bus.write32(AON2_BANK + 0x144u, 0x1Au);
    // 0x811D0E02 ldr r1, =0x60831000 ; 0x811D0E06 str 0x40, [r1, #0x44]
    bus.write32(WHALE_APB + 0x1044u, 0x40u);
    // 0x811D0E08..0E: 0x50-iteration settle
    bus.spin(0x50);
    return Confidence::Verified;
}

// stock: 0x80C55282
Confidence xdsp_clk_mux_select(PmuBus& bus, u32 sel) {
    // 0x80C55288 ldr r0, [r4(=0x402D0000), #0x2b4]
    // 0x80C5528C bic r0, #3 ; 0x80C55290 str -- clear the mux
    u32 v = bus.read32(CLK_MGR_BASE + 0x2B4u) & ~3u;
    // then the matched name's sel bits are ORed in (0x80C552BE..D8)
    v |= (sel & 3u);
    bus.write32(CLK_MGR_BASE + 0x2B4u, v);
    return Confidence::Verified;
}

// stock: 0x80C553A0 / 0x80C553B0
Confidence twpll_dsp_branch_ops(PmuBus& bus, bool main_bank) {
    // 0x80C553A0 mov.w r1, #0x40000 ; cbz/bank pick ; str r1, [r0, #0x34]
    bus.write32((main_bank ? WHALE_APB + 0x1000u : WHALE_APB + 0x2000u) + 0x34u, 0x4'0000u);
    // 0x80C553B0 movw r1, #0x2000 ; ... str r1, [r0, #0x60]
    bus.write32((main_bank ? WHALE_APB + 0x1000u : WHALE_APB + 0x2000u) + 0x60u, 0x2000u);
    return Confidence::Verified;  // traffic Verified; bank polarity Inferred
}

// stock: 0x80D0F0B4 / 0x80D0F0EE / 0x80D0F146 / 0x811D0E02
Confidence whale_module_enable(PmuBus& bus, u32 value) {
    // str r0, [r1(=0x60831000), #0x44]
    bus.write32(WHALE_APB + 0x1044u, value);
    return Confidence::Verified;
}

// stock: 0x80D0CA36
bool rf_ensure_gate_pending(PmuBus& bus, u32 flag_struct_addr) {
    // 0x80D0CA36 ldr r0, =0x81713778 ; 0x80D0CA38 ldr r0, [r0, #0x10] ; bx lr
    // (0x81713778 is a RAM pointer cell; the caller supplies the resolved struct.)
    return bus.read32(flag_struct_addr + 0x10u) != 0u;
}

// stock: 0x81252304
Confidence rf_spi_write(PmuBus& bus, u32 id, u32 addr, u32 val) {
    // 0x81252318 mvn r0, #0x80000000 ; 0x8125231C and r1, r0, r6<<16
    // 0x81252322 orr r2, r1, r5
    const u32 word = (~0x8000'0000u & (addr << 16)) | val;
    // 0x81252320 ldr r0, =0xD15C2000
    // 0x8125232A ite eq ; 0x8125232C streq r2, [r0, #0x50] ; 0x8125232E strne [r0, #0x150]
    bus.write32(RF_SPI_BASE + (id == 0u ? 0x050u : 0x150u), word);
    // 0x81252332..50: 1000-iteration busy delay reading [0x402B00C4] each loop
    for (u32 i = 0; i < 1000u; ++i) (void)bus.read32(PMU_APB_BASE + STATUS1);
    return Confidence::Verified;
}

// stock: 0x81252354
bool rf_spi_read(PmuBus& bus, u32 id, u32 sel, u32& out) {
    // 0x81252362 ittee eq: cmd reg = 0xD15C2054 (id 0) / 0xD15C2154 ; data reg = cmd+4
    const u32 cmd = RF_SPI_BASE + (id == 0u ? 0x054u : 0x154u);
    const u32 data = cmd + 4u;
    // 0x81252380 mov r0, #0x80000000 ; orr r0, sb<<16 ; 0x81252388 str r0, [r8]
    bus.write32(cmd, 0x8000'0000u | (sel << 16));
    // 0x8125238E..9A: 1000-iteration busy delay
    for (u32 i = 0; i < 1000u; ++i) (void)bus.read32(PMU_APB_BASE + STATUS1);
    // 0x8125239C..C2: up to 10 retries while [data] bit31 set, 1000-delay between
    bool timeout = false;
    for (u32 retry = 0; retry <= 10u; ++retry) {
        if ((bus.read32(data) & 0x8000'0000u) == 0u) break;
        if (retry == 10u) {
            timeout = true;
            break;
        }
        for (u32 i = 0; i < 1000u; ++i) (void)bus.read32(PMU_APB_BASE + STATUS1);
    }
    // 0x812523C4/C6: *out = [data] ; return r6 (1 = gave up)
    out = bus.read32(data);
    return !timeout;
}

// stock: 0x8125229E
Confidence rf_ap_port_write(PmuBus& bus, u32 addr_hi, u32 val) {
    // 0x8125229E mvn r2, #0x80000000 ; and r0, r2, r0<<16 ; orrs r1
    // 0x812522A8 ldr r0, =0x40070000 ; 0x812522AA str r1, [r0, #0x240]
    bus.write32(RF_PORT_BASE + 0x240u, (~0x8000'0000u & (addr_hi << 16)) | val);
    // 0x812522AE/B0: [0x40070244] = 1
    bus.write32(RF_PORT_BASE + 0x244u, 1u);
    // 0x812522B4..C2: 1000-iteration busy delay
    for (u32 i = 0; i < 1000u; ++i) (void)bus.read32(PMU_APB_BASE + STATUS1);
    return Confidence::Verified;
}

// stock: 0x812522C6
u32 rf_ap_port_read(PmuBus& bus, u32 addr_hi) {
    // 0x812522CC orr r0, r1(0x80000000), r0<<16 ; 0x812522D0 str -> [0x40070240]
    bus.write32(RF_PORT_BASE + 0x240u, 0x8000'0000u | (addr_hi << 16));
    bus.write32(RF_PORT_BASE + 0x244u, 1u);
    for (u32 i = 0; i < 1000u; ++i) (void)bus.read32(PMU_APB_BASE + STATUS1);
    // 0x812522EA ldr r0, [r2, #0x248]
    return bus.read32(RF_PORT_BASE + 0x248u);
}

}  // namespace power
}  // namespace f286
