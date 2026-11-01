#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# Prepare an Ubuntu WSL distribution for building FPLinux with the Ruio F286 port.
# Run as root inside WSL:
#   bash tools/setup_fplinux_wsl.sh [FPLINUX_URL [BRANCH]]
# FPLINUX_URL  defaults to https://github.com/openfeaturephone/fplinux (the fork with the port)
# BRANCH       defaults to ruio-f286
set -eux
url=${1:-https://github.com/openfeaturephone/fplinux}
branch=${2:-ruio-f286}

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
  git curl xz-utils build-essential bc bison flex libssl-dev libelf-dev \
  dwarves rsync cpio iproute2 openssh-client uidmap p7zip-full \
  python3.14-venv python3.14-dev squashfs-tools e2fsprogs mtools dosfstools \
  device-tree-compiler u-boot-tools qemu-user-binfmt \
  fuse-overlayfs slirp4netns

# Builds run rootless as this user; containers need subordinate ids.
if ! id fplinux >/dev/null 2>&1; then
  useradd -m -s /bin/bash fplinux
fi
usermod --add-subuids 100000-165535 --add-subgids 100000-165535 fplinux

if [ ! -d /home/fplinux/fplinux ]; then
  sudo -u fplinux -H git clone -b "$branch" "$url" /home/fplinux/fplinux
fi
sudo -u fplinux -H bash -lc 'cd ~/fplinux && ./fplinux doctor'
