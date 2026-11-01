#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
"""Cut the F286 DSP firmware files out of your own stock firmware dump.

The f286-radio driver loads these from /lib/firmware/ums9117/:

  f286-gsm-dsp.bin      fw_modem partition (id 0x10000006), whole 0x300000 bytes
  f286-lte-dsp.bin      fw_dsp partition (id 0x10000005), first 0x2e4000 bytes
                        (what stock copies to 0x80448000; entry 0x80448400)
  f286-xdsp{A,B}-{boot,main}.bin
                        the LTE XDSP core images, which exist only inside the
                        stock kernel partition (load base 0x80a05e00)

Nothing here ships any firmware: you need a NAND backup of your own phone with
the partitions already carved out, one file per partition.

  python3 extract_dsp_fw.py --gsm p_10000006.bin --lte p_10000005.bin \\
      --kernel kernel.bin -o out/

ECC caveat: this cuts the partitions out verbatim. If the backup is a raw NAND
read taken without applying the per-page ECC, the DSP images carry the bit errors
the controller would normally correct (I measured ~1056 one-way flips over the
first 0x150000 bytes of fw_dsp against stock DRAM), and the DSP will not run. Use
an ECC-corrected dump, or reconstruct the image from a stock DRAM capture. The
XDSP images come from the kernel partition, which is not affected.

The XDSP images are checked against sha256 prefixes from the one F286 I have;
pass --no-check if your firmware build differs (and expect the word counts in
the driver to be wrong too).
"""

import argparse
import hashlib
import sys
from pathlib import Path

GSM_SIZE = 0x300000
LTE_SIZE = 0x2E4000
KERNEL_BASE = 0x80A05E00
# (file name, stock VA, length in 32-bit words, sha256 prefix). Word counts are
# the stock kernel's RW data at 0x81713f38..0x81713f4c.
XDSP_IMAGES = (
    ("f286-xdspA-boot.bin", 0x815019B4, 0x696, "83e15e8e"),
    ("f286-xdspA-main.bin", 0x814F7F00, 0x26AD, "db7a444e"),
    ("f286-xdspB-boot.bin", 0x81517684, 0x556, "e27de6f3"),
    ("f286-xdspB-main.bin", 0x8150DDE8, 0x2627, "4e911f63"),
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gsm", type=Path, help="fw_modem partition (0x10000006)")
    ap.add_argument("--lte", type=Path, help="fw_dsp partition (0x10000005)")
    ap.add_argument("--kernel", type=Path, help="stock kernel partition (kernel.bin)")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output directory")
    ap.add_argument("--no-check", action="store_true", help="skip the XDSP hash check")
    args = ap.parse_args()
    if not (args.gsm or args.lte or args.kernel):
        ap.error("give at least one of --gsm, --lte, --kernel")

    images = []
    if args.gsm:
        gsm = args.gsm.read_bytes()
        if len(gsm) < GSM_SIZE:
            sys.exit(f"{args.gsm}: {len(gsm):#x} bytes, expected at least {GSM_SIZE:#x}")
        images.append(("f286-gsm-dsp.bin", gsm[:GSM_SIZE]))
    if args.lte:
        lte = args.lte.read_bytes()
        if len(lte) < LTE_SIZE:
            sys.exit(f"{args.lte}: {len(lte):#x} bytes, expected at least {LTE_SIZE:#x}")
        images.append(("f286-lte-dsp.bin", lte[:LTE_SIZE]))
    if args.kernel:
        kernel = args.kernel.read_bytes()
        for name, va, words, prefix in XDSP_IMAGES:
            off = va - KERNEL_BASE
            data = kernel[off:off + words * 4]
            digest = hashlib.sha256(data).hexdigest()
            if len(data) != words * 4:
                sys.exit(f"{name}: kernel image too short")
            if not args.no_check and not digest.startswith(prefix):
                sys.exit(f"{name}: sha256 {digest[:8]} != {prefix} (different firmware build?)")
            images.append((name, data))

    args.out.mkdir(parents=True, exist_ok=True)
    for name, data in images:
        (args.out / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes sha256={hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
