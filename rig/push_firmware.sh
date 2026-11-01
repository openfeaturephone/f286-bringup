#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# Copy the DSP firmware into /lib/firmware/ums9117/ on the running phone. FPLinux's root is a
# writable overlay in RAM, so this has to be repeated after every boot.
#   PHONE=root@<addr> SSH_OPTS="-i key" bash rig/push_firmware.sh DIR
# DIR holds the files tools/extract_dsp_fw.py wrote (f286-lte-dsp.bin, f286-gsm-dsp.bin,
# f286-xdsp{A,B}-{boot,main}.bin). Files are streamed over ssh, so no scp/sftp is needed.
. "$(dirname "$0")/phone.sh"
DIR=${1:?usage: push_firmware.sh DIR}
phone "mkdir -p /lib/firmware/ums9117"
for f in "$DIR"/f286-*.bin; do
	[ -e "$f" ] || { echo "no f286-*.bin in $DIR"; exit 1; }
	n=$(basename "$f")
	phone "cat > /lib/firmware/ums9117/$n" < "$f"
	echo "pushed $n ($(wc -c < "$f") bytes)"
done
phone "ls -l /lib/firmware/ums9117/; sha256sum /lib/firmware/ums9117/f286-*.bin"
