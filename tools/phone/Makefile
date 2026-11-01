# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# Static armhf binaries for the phone (the FPLinux rootfs is BusyBox, so the
# tools carry their own libc). Default compiler is zig through pip
# (python -m pip install ziglang), targeting musl because zig cannot link
# glibc statically. musl's off_t is 64-bit, so /dev/mem offsets above
# 0x80000000 stay positive. Override with e.g.
#   make CC=arm-linux-gnueabihf-gcc CFLAGS="-O2 -Wall -static -D_FILE_OFFSET_BITS=64"
ifeq ($(origin CC),default)
CC = python -m ziglang cc -target arm-linux-musleabihf -mcpu=cortex_a7
endif
CFLAGS ?= -O2 -Wall -static

PROGS = memsample poke devmem

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)

.PHONY: all clean
