#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# One-shot radio sequence once SSH is reachable: radio_power -> load_lte -> capture.
# The phone may hang during load_lte (see docs/bringup-status.md), so stream the kernel log
# with kmsg_stream.sh in a second terminal first; the dmesg snapshots below are a fallback.
#   PHONE=root@<addr> SSH_OPTS="-i key" bash rig/boot_sequence.sh
. "$(dirname "$0")/phone.sh"
echo "== $PHONE: starting phone-side dmesg snapshots"
phone "nohup sh -c 'dmesg > /tmp/d1.log; sleep 25; dmesg > /tmp/d2.log; sleep 60; dmesg > /tmp/d3.log' >/dev/null 2>&1 &"
echo "== radio_power"
phone "echo 1 > $RADIO/radio_power" && echo accepted
sleep 12
phone "dmesg | grep -E 'power:|whu3g' | tail -8"
echo "== load_lte"
phone "echo 1 > $RADIO/load_lte" && echo accepted
sleep 30
echo "== t+30s"
phone "dmesg | grep -vE 'xdsp poll' | tail -30"
sleep 45
echo "== t+75s (post-poll, post-release)"
phone "dmesg | grep -vE 'xdsp poll' | grep -iE 'RESP|VERSION|CHANGED|release|exec vector|staged|cores|XDSP|pump|poll [0-9]+0:' | tail -30"
phone "cat $RADIO/status"
echo "== pulling phone-side dmesg snapshots"
for f in d2 d3; do
	phone "cat /tmp/$f.log" > "$LOGDIR/$f.log" 2>/dev/null && echo "saved $LOGDIR/$f.log"
done
echo "== done. grep d2/d3 for: RESP .* 5 / VERSION / CHANGED"
