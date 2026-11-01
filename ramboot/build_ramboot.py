#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
"""Build RAM-boot images of the stock Ruio F286 kernel from your own kernel.bin.

  kernel_stock_ram.bin      kernel.bin DHTB header + payload, unmodified
  kernel_shim_ram.bin       the same + shim.S appended at the payload end,
                            reset literal -> shim
  kernel_wdg_ram.bin        shim also arms the PMIC watchdog (45 s, reset mode)
                            and the kernel's own watchdog driver is stubbed out
  kernel_wdgspin_ram.bin    arms the watchdog and spins instead of entering the
                            kernel (tests that the watchdog reset keeps DRAM)

The outputs contain the stock kernel, so they stay on your machine (the
.gitignore keeps *.bin out of the repo). Nothing is ever written to flash:
ramboot.cmd loads an image into DRAM through FDL1.

  python build_ramboot.py path/to/kernel.bin [-o outdir]

Needs zig through pip (python -m pip install ziglang) for the ARM assembler.
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KBASE = 0x80A05E00
RESET_LIT = 0x208  # file offset of the literal loaded by `ldr pc,[pc]` at 0x80a06000
RESET_ENTRY = 0x80A061F0
WDG_FNS = (0x8136F3F0, 0x8136F4C4)  # stock PMIC watchdog config(cfg{run,mode,load}) and its sibling
KNOWN_KERNEL = "c1c808de"  # sha256 prefix of the kernel.bin these offsets were taken from


def zig(*args):
    subprocess.run([sys.executable, "-m", "ziglang", *args], check=True)


def main():
    ap = argparse.ArgumentParser(description="Build RAM-boot images of the stock F286 kernel.")
    ap.add_argument("kernel", help="stock kernel partition image (kernel.bin)")
    ap.add_argument("-o", "--out", default=HERE, help="output directory (default: next to this script)")
    ap.add_argument("--no-check", action="store_true",
                    help="accept a kernel.bin other than the one the offsets were taken from")
    args = ap.parse_args()

    k = open(args.kernel, "rb").read()
    digest = hashlib.sha256(k).hexdigest()
    if not args.no_check and not digest.startswith(KNOWN_KERNEL):
        sys.exit(f"unexpected kernel.bin (sha256 {digest[:8]}, want {KNOWN_KERNEL}); "
                 "the reset literal and watchdog offsets would be wrong")
    n = 0x200 + struct.unpack_from("<I", k, 0x30)[0]
    stock = bytearray(k[:n])
    assert struct.unpack_from("<I", stock, RESET_LIT)[0] == RESET_ENTRY, "reset literal not where expected"
    os.makedirs(args.out, exist_ok=True)
    out = lambda name: os.path.join(args.out, name)  # noqa: E731
    open(out("kernel_stock_ram.bin"), "wb").write(stock)

    shim_off = (n + 15) & ~15
    shim_va = KBASE + shim_off
    wdg = ["-DARM_WDG", f"-DWDG_LOAD={45 * 32768:#x}"]
    # kernel_wdgspin_nousb_ram.bin: the spin test without the USB shutdown, to see whether the
    # shutdown is what stops the boot ROM's download mode answering after the watchdog reset.
    variants = (("kernel_shim_ram.bin", []), ("kernel_wdg_ram.bin", wdg),
                ("kernel_wdgspin_ram.bin", wdg + ["-DSPIN"]),
                ("kernel_wdgspin_nousb_ram.bin", wdg + ["-DSPIN", "-DNO_USB_OFF"]))
    for name, defs in variants:
        with tempfile.TemporaryDirectory() as td:
            # linked (not just assembled) at its run address, so bl/ldr= resolve
            elf, binf, ld = (os.path.join(td, f) for f in ("shim.elf", "shim.bin", "shim.ld"))
            open(ld, "w").write(f"ENTRY(_start)\nSECTIONS {{ . = {shim_va:#x}; .text : {{ *(.text*) }} }}\n")
            zig("cc", "-target", "arm-freestanding-eabi", "-mcpu=cortex_a7", *defs, "-nostdlib",
                "-T", ld, os.path.join(HERE, "shim.S"), "-o", elf)
            zig("objcopy", "-O", "binary", "-j", ".text", elf, binf)
            shim = open(binf, "rb").read()
        assert shim_va + len(shim) < 0x81605E00, "shim must stay inside the 12 MB kernel area"
        img = stock + bytes(shim_off - n) + shim
        struct.pack_into("<I", img, RESET_LIT, shim_va)
        if "-DARM_WDG" in defs:
            # Otherwise the kernel re-programs/feeds the PMIC watchdog while it hangs: its two
            # driver entries return 0 at once (movs r0,#0; bx lr), so the shim's 45 s arming holds.
            for fn in WDG_FNS:
                off = fn - KBASE
                assert img[off:off + 2] == bytes.fromhex("2de9"), f"{fn:#x}: unexpected prologue"
                img[off:off + 4] = bytes.fromhex("00207047")
        open(out(name), "wb").write(img)
        print(f"{name}: shim {len(shim)} bytes at file {shim_off:#x} (VA {shim_va:#x}); reset literal -> shim")
    for name in ("kernel_stock_ram.bin",) + tuple(v[0] for v in variants):
        d = open(out(name), "rb").read()
        print(f"{name}: {len(d):#x} bytes sha256 {hashlib.sha256(d).hexdigest()}")


if __name__ == "__main__":
    main()
