// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
/* devmem.c - mmap-based 32-bit peek/poke for F286 FPLinux (armv7l)
 * Usage: devmem ADDR [VAL32]
 *   ADDR/VAL accept 0x-prefixed hex or decimal. No VAL = read.
 * Build: see tools/phone/Makefile (static armhf binary).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

static unsigned long parse_ul(const char *s)
{
    return strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    unsigned long addr, val, count = 1;
    off_t pgoff;
    size_t pgsize = (size_t)sysconf(_SC_PAGESIZE);
    int fd, write_mode;
    volatile uint32_t *p;

    if (argc >= 2 && !strcmp(argv[1], "-d") && argc == 4) {
        /* dump mode: devmem -d ADDR COUNT_WORDS */
        addr = parse_ul(argv[2]);
        count = parse_ul(argv[3]);
        fd = open("/dev/mem", O_SYNC | O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "devmem: open /dev/mem: %s\n", strerror(errno));
            return 1;
        }
        for (unsigned long i = 0; i < count; i++) {
            unsigned long a = addr + i * 4;
            pgoff = (off_t)(a & ~(unsigned long)(pgsize - 1));
            p = (volatile uint32_t *)mmap(NULL, pgsize, PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, pgoff);
            if (p == MAP_FAILED) {
                printf("%08lX: MMAP_FAIL\n", a);
                continue;
            }
            p += (a & (pgsize - 1)) / sizeof(uint32_t);
            if (i % 8 == 0)
                printf("\n%08lX:", a);
            printf(" %08X", *p);
            munmap((void *)((uintptr_t)p & ~(pgsize - 1)), pgsize);
        }
        printf("\n");
        close(fd);
        return 0;
    }

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s ADDR [VAL]\n", argv[0]);
        return 2;
    }
    addr = parse_ul(argv[1]);
    write_mode = (argc == 3);
    if (write_mode)
        val = parse_ul(argv[2]);
    if (addr & 3) {
        fprintf(stderr, "devmem: addr 0x%lx not 32-bit aligned\n", addr);
        return 1;
    }

    fd = open("/dev/mem", O_SYNC | O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "devmem: open /dev/mem: %s\n", strerror(errno));
        return 1;
    }
    pgoff = (off_t)(addr & ~(unsigned long)(pgsize - 1));
    p = (volatile uint32_t *)mmap(NULL, pgsize, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, pgoff);
    if (p == MAP_FAILED) {
        fprintf(stderr, "devmem: mmap 0x%lx: %s\n", addr, strerror(errno));
        close(fd);
        return 1;
    }
    p += (addr & (pgsize - 1)) / sizeof(uint32_t);

    if (write_mode) {
        *p = (uint32_t)val;
        /* read-back for the log */
        printf("Write at 0x%08lX = 0x%08lX (readback 0x%08X)\n",
               addr, val, *p);
    } else {
        printf("0x%08X\n", *p);
    }

    munmap((void *)((uintptr_t)p & ~(pgsize - 1)), pgsize);
    close(fd);
    return 0;
}
