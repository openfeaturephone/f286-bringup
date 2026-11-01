#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# Stream the phone's kernel log to the PC and keep it, so the lines just before a hard hang
# survive. BusyBox dmesg has no -w, so this reads /dev/kmsg directly. With MIRROR=1 every line
# is also written to /dev/pts/0, which is the fplinux-terminal on the phone's screen (the
# kernel console is not shown there).
#   PHONE=root@<addr> SSH_OPTS="-i key" bash rig/kmsg_stream.sh [tag]
. "$(dirname "$0")/phone.sh"
OUT="$LOGDIR/stream_$(date +%Y%m%d_%H%M%S)${1:+_$1}.log"
echo "streaming to $OUT (Ctrl+C to stop)"
if [ "${MIRROR:-0}" = 1 ]; then
	phone "cat /dev/kmsg | while IFS= read -r l; do echo \"\$l\"; echo \"\$l\" > /dev/pts/0; done" | tee "$OUT"
else
	phone "cat /dev/kmsg" | tee "$OUT"
fi
