#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
# axidump.sh - dump PUB AXI debug/detect regs (base 0x300E0000) via /dev/mem
B=0x300E0000
rd() {  # rd <hex offset> <label>
  off=$(( $B + $1 ))
  skip=$(( off / 4 ))
  v=$(dd if=/dev/mem bs=4 skip=$skip count=1 2>/dev/null | od -An -tx4 | tr -d ' \n')
  echo "$2 (0x$(printf %x $1)) = $v"
}
echo "== AXI debug/detect dump =="
rd 0x500C AXI_ADDR_DETECT_CTRL
rd 0x5010 AXI_ADDR_WRAP_CTRL
rd 0x5200 BRIDGE_DEBUG_SIGNAL_R_AON
rd 0x5204 BRIDGE_DEBUG_SIGNAL_R_AP
rd 0x5208 BRIDGE_DEBUG_SIGNAL_R_CM4
rd 0x5210 BRIDGE_DEBUG_SIGNAL_R_PUBCP
rd 0x00c0 PMU_PWR_STATUS1_DBG_is_at_402B00C0_not_PUB
echo "== done =="
