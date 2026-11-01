# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# Shared settings for the rig scripts; sourced, not run.
#   PHONE     ssh destination of the phone running FPLinux, e.g. root@<address the runner printed>
#   SSH_OPTS  extra ssh options, e.g. "-i /path/to/session/id_ed25519 -o StrictHostKeyChecking=no"
#   SSH       ssh binary (default: ssh)
#   LOGDIR    where captures go (default: ./logs)
: "${PHONE:?set PHONE to the ssh destination of the phone, e.g. root@ADDRESS}"
SSH_BIN=${SSH:-ssh}
LOGDIR=${LOGDIR:-./logs}
mkdir -p "$LOGDIR"
# Git Bash on Windows rewrites lone /tmp/... style arguments into Windows paths.
export MSYS_NO_PATHCONV=1

phone() {
	# shellcheck disable=SC2086
	"$SSH_BIN" $SSH_OPTS -o ConnectTimeout=10 "$PHONE" "$@"
}

RADIO=/sys/bus/platform/devices/soc:radio@0
PARAMS=/sys/module/f286_radio/parameters
