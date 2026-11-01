# Radio bring-up status

State on 2026-11-01, one Ruio F286, FPLinux booted from RAM over USB (kernel
7.2.9-fplinux). Everything below happened on that one phone. "Stock" means the
phone's own firmware (Mocor), read from its kernel image or observed running.

## What works

**Whale-die power-on.** `radio_power` with `stock_early=1` brings the die up:
the WTLCP power status c4[4:0] reads 0 and the driver ends in `state=power-ok`.
Without `stock_early` the WTLCP wait times out with c4=7, d4=6. The difference is
the rest of the writes stock makes in its first second (emulated boot, 0.89 to
0.9 s): set-b25/clear-b24 pairs on the PMU power-domain registers 0x28, 0x2c,
0x34, 0x3c, 0x4c, 0x50, then 0x402b0210 = 0 -> 1, 0x402e0244 = 0x00010100 and
0x404d0008 |= 0x80. Two of those had earlier been removed from the driver as dead
code; the emulated stock boot executes them.

**XDSP cores A and B.** With `xdsp_cores=1`, `load_lte` loads both LTE XDSP
cores the way stock does at about 0.97 s: per core, a control pulse, the boot
loader into the core window, ready word 1, a second pulse, the main image, ready
word 2. Both cores reach ready 1 then 2 on hardware.

| core | window | control | ready word | boot loader | main image |
|---|---|---|---|---|---|
| A | 0xe0400000 | 0xe047f000 | 0xe0419908 | 0x815019b4 (0x696 words) | 0x814f7f00 (0x26ad words) |
| B | 0xe0480000 | 0xe04ff000 | 0xe0499540 | 0x81517684 (0x556 words) | 0x8150dde8 (0x2627 words) |

The four images exist only inside the stock kernel partition (addresses are
stock VAs); `tools/extract_dsp_fw.py` cuts them out of your own dump.

`load_gsm` (GSM DSP) and the Bluetooth driver are not covered by this page.

## What does not work: the LTE DSP release hangs the AP

`load_lte` copies the LTE DSP image (0x2e4000 bytes) to 0x80448000 and releases
the DSP (0x402e0130 bit16 clear, boot address 0x402e0248 = 0x40224200, which is
the entry 0x80448400 >> 1). About 1.1 s later the whole AP hangs hard:

- no panic, no reboot, no IRQ storm (the mailbox IRQ counts stay 0);
- PMU c0/c4/d4 are static up to the end;
- the USB gadget stays enumerated on the PC after death, so it is a real AP
  hang and not a reset;
- the driver is sitting in its 2 s settle `msleep` at that moment, so it is not
  the driver's own access that hangs;
- same result with `xdsp_cores=0` and with `xdsp_cores=1`;
- the death time is fixed (1.0 to 1.2 s after release) on every run.

The DSP also never seems to execute. `tools/phone/memsample` streamed every
changing word from 13 ms after the release until death: the DSP writes nothing,
not to the command/response queues, not to the handshake cells at 0x80447xxx,
not to the split/UL slots. On stock, the LTE DSP answers with its VERSION
message 57 ms after release and powers LTE_P1 (PMU c0[29:25]) 1 ms after
release. Ours never does either.

## What has been ruled out

- CP_SOFT_RST. 0x402b00b0 is 0x507 at boot; after the power sequence it is
  0x502 (WTLCP and DSP subsystem released). Only the BT bits 8/10 differ from
  stock's 0x2.
- Stock AON clock enables. Setting 0x402e0004 |= 0x16be0000 (includes
  LDSP/TGDSP_INTC_EB and MBOX_EB) and 0x402e0010 |= 0x30400 changes nothing:
  still silent, still dead at about 1.12 s.
- Cores held in power-request. Earlier builds wrote 0x402b2044 = 0x30000 and
  left the cores held off (c0[9:5]/[14:10] = 7). The stock pre-boot clear of
  0x402b2034 b25:24 fixed that; the LDSP field now reads 0 before release.
- Start control low at release. Stock sets 0x402e024c bit0 between boot
  address and release; the driver does too.
- Sending a first command. The stock cold boot sends the DSP nothing; the
  handshake is DSP-initiated (unprompted response type 5). `send_cmd6` is off.
- TWPLL off. No stage of the stock boot chain writes the TWPLL controls, and
  FPLinux's clock driver only reads them, so TWPLL should be running in both.
  I have not read the lock bit on hardware (0x403E0008 bit6, and 0x402B00A0 &
  0x1F1 should be 0x101).
- Firmware content. A spot-check (sample words and the head) matched after the
  copy, so this was ruled out at first. A full comparison since has reopened it;
  see "The DSP code image is corrupted" below.

## The DSP code image is corrupted

Comparing the whole loaded image against stock DRAM, not just a sample, changed
this. `f286-lte-dsp.bin` differs from the image stock runs in 1056 single bits
over the first 0x150000 bytes checked; every difference is a 1 in the file and a
0 in DRAM, and the two stock boots I captured agree on all 1056 positions.
`f286-gsm-dsp.bin` is the same kind of thing. That one-directional, boot-stable
pattern is the signature of a raw NAND read with the ECC left unapplied:
`extract_dsp_fw.py` carves the partition verbatim from the NAND backup, so the
bit flips the NAND controller would have corrected on a real read are still in
the file. So FPLinux is releasing a DSP whose code image has about a thousand
flipped bits, which is a strong candidate for why it never executes (any flip in
the reset vector or the early setup would do it). This is separate from the empty
calibration tables below: that is NV data the driver skips, this is the DSP's own
code. The next test is to rebuild the image from a stock DRAM capture (the bytes
that actually ran, already ECC-corrected) and repeat `load_lte`.

## Differences from stock that remain at release time

From a register diff of FPLinux after `radio_power` against the running stock
phone:

- HU3G is on in FPLinux, off in stock.
- BT_DOMAIN and CM4 are off in FPLinux, on in stock. Stock starts the CM4 long
  before the LTE release.
- Stock releases the GSM DSP (0.964 s) before the XDSP cores and the LTE DSP.
  FPLinux does not.
- Not yet applied from the stock first second: 0x402e0134 = 0x200; CM4 start
  bits 0x402e2114 = 0x10, 0x1; 0x402e1004 = 0x3a0100 as a 32-bit store (the
  driver does a 16-bit 0x100); 0x402e3044 = 0x400; 0x402e1000 = 0x80000 and
  later 0x2000; 0x402b00cc = 0xc2 and 0x402b003c = 0x10303 (direct); the
  mailbox init at 0.939 s; the post-XDSP gates at 0.998 s. The 0x402b20cc
  clears are also in a different order (stock clears bit21 before bit17).
- No LTE RF calibration. Before the release, stock (0x80C10EA8) copies about
  30 NV items back to back into DSP memory starting at 0x80678000 (items 0x15,
  0x1a-0x22, 0x0b-0x14, 0x16-0x19, 0x28, 0x2c, 0x31, 0x33; each with an
  asserted size). In stock DRAM that range holds data that is identical across
  boots; in the firmware file FPLinux loads it is 99.96 % zero. The driver logs
  "NV param setup skipped". So FPLinux releases a DSP whose calibration tables
  are empty.

## Open questions

1. Is the ~1.1 s death a watchdog? The fixed timing suggests one. The PMU
   watchdog reset flag register 0x402b3080 bit4 is the LTE watchdog; reading it
   after a hang needs the DRAM-preserving reset path (`ramboot/`), and I have not
   done that yet.
2. Does the DSP execute at all? Zero writes from 13 ms on, and no LTE_P1 power
   request, say no. Candidates: the corrupted code image (see "The DSP code image
   is corrupted" — the current leading suspect); an exec clock still gated (the
   whale die-local clock block at 0xD15E1028/0xD15E2000, which hangs the bus when
   touched from Linux; `dieclk=1` tries it in a separate kthread); the boot
   address coding; or a missing CM4/GSM prerequisite.
3. Does the calibration block matter for starting at all? The next test is to put
   this phone's own calibration block (from a stock DRAM image) into the firmware
   file at offset 0x230000 (= 0x80678000 - 0x80448000) and repeat `load_lte`.
4. Where does an early DSP assert go? Stock's dump-control word is at on-chip
   0x00010BF8, and the ring the HAL returns is offset 0xE400 from a separately
   returned window. The DRAM window at 0x8040E400 held untouched 0xAA fill in
   both stock images I read, so watching on-chip share RAM 0xD000-0x12000 during
   `load_lte` is worth doing.
5. The GIC mask the driver applies (ICENABLER2 0xe0000030) probably includes the
   WCN mailbox interrupts (hwirq 68/69). That has to be settled before Bluetooth
   and radio can run together.
