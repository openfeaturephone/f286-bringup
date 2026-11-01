#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# radio_power with stock_early=1 (the remaining stock first-second writes: PMU domain pairs,
# 0x402b0210, 0x402e0244, 0x404d0008). Without it the WTLCP wait times out with c4=7 d4=6.
# Question it answers: does WTLCP (c4[4:0]) reach 0? (It did on my phone, 2026-11-01.)
# Needs FPLinux running on the phone with SSH up. Read-mostly: the only writes are the module
# parameter and radio_power.
#   PHONE=root@<addr> SSH_OPTS="-i key" bash rig/stock_early_test.sh
. "$(dirname "$0")/phone.sh"
OUT="$LOGDIR/stock_early_$(date +%Y%m%d_%H%M%S).log"
{
echo "== phone $PHONE"
phone "uname -a; ls $PARAMS"
echo "== waiting for the radio attributes (the deferred work creates them ~60 s after boot)"
phone "for i in \$(seq 1 120); do [ -e $RADIO/radio_power ] && break; sleep 1; done; ls $RADIO"
echo "== params before"
phone "for f in stock_early xdsp_cores gsm_iram dieclk e010_light unlock; do [ -e $PARAMS/\$f ] && echo \$f=\$(cat $PARAMS/\$f); done"
echo "== baseline status"
phone "cat $RADIO/status"
echo "== stock_early=1, radio_power"
phone "echo 1 > $PARAMS/stock_early && echo stock_early=\$(cat $PARAMS/stock_early) && echo 1 > $RADIO/radio_power && echo radio_power accepted"
echo "== waiting 40 s for the power thread (the WTLCP wait caps at 30 s)"
sleep 40
echo "== status after"
phone "cat $RADIO/status"
echo "== dmesg (radio power lines)"
phone "dmesg | grep -E 'f286-radio|stock_early|power:|RMW|ADI|whu3g|WTLCP' | tail -80"
} 2>&1 | tee "$OUT"
echo "saved $OUT"
