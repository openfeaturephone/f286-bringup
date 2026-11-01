/* RAM-boot handoff shim for the stock UMS9117 kernel (Ruio F286).
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 willisdylan
 *
 * FDL1 jumps to the kernel image + 0x200 (0x80a06000: ldr pc,[pc]); the reset literal there is
 * pointed at this shim, appended past the kernel payload (VA 0x816033d0, inside the area the kernel
 * later uses for its page tables). It does what boot1 does and FDL1 does not:
 *  - FDL1 leaves its USB (MUSB 0x20200000) enumerated with a receive DMA into usb_recv_buf
 *    0x80-0x3280, which overlaps the kernel's vector/handler copy at 0x0-0x1b14. Soft-disconnect,
 *    pulse the OTG reset (AP AHB RST 0x20e00004 bits 4-6, via the +0x1000 set / +0x2000 clear
 *    aliases, as in FPLinux ums9117-musb.c), then clear the OTG enable (AP AHB EB 0x20e00000 bit 4,
 *    boot1's first action), and wait so the host sees the detach.
 *  - clear word 0 (the kernel treats 0xAAAAAAAA / 0x54464e53 there as a dump-mode marker).
 * Then restore the reset literal and enter the kernel exactly like boot1: ARM, SVC, IRQ/FIQ masked,
 * r0 = 0x80a06000. */
    .syntax unified
    .arm
    .global _start
_start:
    cpsid   if
#ifndef NO_USB_OFF
    ldr     r1, =0x20200001          @ MUSB POWER: clear SOFTCONN (bit 6)
    ldrb    r2, [r1]
    bic     r2, r2, #0x40
    strb    r2, [r1]
    ldr     r1, =0x20e01004          @ AP AHB RST set alias: assert OTG reset
    mov     r2, #0x70
    str     r2, [r1]
    ldr     r3, =0x200000
1:  subs    r3, r3, #1
    bne     1b
    ldr     r1, =0x20e02004          @ AP AHB RST clear alias: release OTG reset
    str     r2, [r1]
    ldr     r1, =0x20e02000          @ AP AHB EB clear alias: OTG enable off
    mov     r2, #0x10
    str     r2, [r1]
    ldr     r3, =0x4000000           @ ~0.1-1 s, uncached: let the host see the detach
2:  subs    r3, r3, #1
    bne     2b
#endif
#ifdef ARM_WDG
    /* Arm the PMIC (SC2720) watchdog in reset mode, so a hung kernel is reset after
     * WDG_LOAD/32768 s with DRAM kept (then: download key held -> FDL1 -> read DRAM). Same writes
     * as the stock driver 0x8136f3c4 (clocks) + 0x8136f3f0(cfg{run 1, mode 0, load}). */
    ldr     r0, =0x40608c08          @ WDG clock enables |= 4 (c08, c10)
    bl      adi_rd
    orr     r1, r0, #4
    ldr     r0, =0x40608c08
    bl      adi_wr
    ldr     r0, =0x40608c10
    bl      adi_rd
    orr     r1, r0, #4
    ldr     r0, =0x40608c10
    bl      adi_wr
    ldr     r0, =0x40608060          @ unlock
    ldr     r1, =0xe551
    bl      adi_wr
    ldr     r0, =0x40608048          @ CTRL |= INT | RST
    bl      adi_rd
    orr     r1, r0, #9
    ldr     r0, =0x40608048
    bl      adi_wr
    ldr     r0, =0x40608048          @ CTRL |= NEW
    bl      adi_rd
    orr     r1, r0, #4
    ldr     r0, =0x40608048
    bl      adi_wr
    ldr     r0, =0x40608070
    mov     r1, #1
    bl      adi_wr
    ldr     r0, =0x4060806c
    ldr     r1, =0xffff
    bl      adi_wr
    ldr     r0, =0x40608044          @ LOAD_HI
    ldr     r1, =(WDG_LOAD >> 16)
    bl      adi_wr
    ldr     r0, =0x40608040          @ LOAD_LO
    ldr     r1, =(WDG_LOAD & 0xffff)
    bl      adi_wr
    ldr     r0, =0x40608048          @ CTRL |= RUN
    bl      adi_rd
    orr     r1, r0, #2
    ldr     r0, =0x40608048
    bl      adi_wr
    ldr     r0, =0x40608060          @ lock
    ldr     r1, =0x1aae
    bl      adi_wr
#ifdef SPIN
    /* test variant: never enter the kernel, just wait for the watchdog (does the arming work, and
     * does DRAM - the image loaded here - survive the reset?) */
3:  b       3b
#endif
#endif
    mov     r1, #0
    str     r1, [r1]                 @ word 0 = 0
    ldr     r1, =0x80a06008          @ restore the kernel's reset literal
    ldr     r2, =0x80a061f0
    str     r2, [r1]
    mov     r1, #0
    mcr     p15, 0, r1, c7, c5, 0    @ invalidate I-cache + branch predictor
    mcr     p15, 0, r1, c7, c5, 6
    dsb
    isb
    ldr     r0, =0x80a06000
    bx      r0
    .ltorg

#ifdef ARM_WDG
/* ADI (PMIC bus, controller 0x40600000) in full-physical-address mode, as left by the boot ROM/FDL1
 * (FPLinux uboot ums9117-mmc.c). adi_rd: r0 = register address -> r0 = 16-bit value (bounded poll).
 * adi_wr: r0 = register address, r1 = value. Clobber r2, r3. */
adi_rd:
    ldr     r2, =0x40600000
    str     r0, [r2, #0x28]          @ RD_CMD
    ldr     r3, =0x100000
1:  ldr     r0, [r2, #0x2c]          @ RD_DATA: bit 31 = busy
    cmp     r0, #0
    bge     2f
    subs    r3, r3, #1
    bne     1b
2:  ldr     r2, =0xffff
    and     r0, r0, r2
    bx      lr
adi_wr:
    ldr     r2, =0x40600000
    ldr     r3, =0x100000
1:  push    {r0}
    ldr     r0, [r2, #0x30]          @ FIFO_STS bit 11 = full
    tst     r0, #0x800
    pop     {r0}
    beq     2f
    subs    r3, r3, #1
    bne     1b
2:  str     r1, [r0]
    ldr     r3, =0x100000
3:  push    {r0}
    ldr     r0, [r2, #0x30]          @ bit 10 = empty
    tst     r0, #0x400
    pop     {r0}
    bne     4f
    subs    r3, r3, #1
    bne     3b
4:  bx      lr
    .ltorg
#endif
