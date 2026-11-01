// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
/* poke: read or write one 32-bit physical register through /dev/mem.
 *   poke ADDR          print the value
 *   poke ADDR VALUE    write VALUE, then print the readback
 * The phone's BusyBox has no devmem; used for one-off radio register steps.
 *
 * Build: see tools/phone/Makefile (static armhf binary).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	uint32_t addr, page;
	volatile uint32_t *reg;
	void *map;
	int fd;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: poke ADDR [VALUE]\n");
		return 2;
	}
	addr = strtoul(argv[1], NULL, 0);
	page = addr & ~0xfffu;
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	reg = (volatile uint32_t *)((char *)map + (addr - page));
	if (argc == 3) {
		uint32_t value = strtoul(argv[2], NULL, 0);

		*reg = value;
		printf("%08x <- %08x, rb %08x\n", addr, value, *reg);
	} else {
		printf("%08x = %08x\n", addr, *reg);
	}
	return 0;
}
