// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
/* memsample: watch physical memory/registers on the phone through /dev/mem and
 * print every 32-bit word that changes, as fast as the A7 allows.
 *
 * Written for the LTE DSP post-release hang (docs/bringup-status.md): the AP
 * dies ~1.1 s after the DSP is released, so this streams what the DSP writes in that
 * window to stdout (SSH -> PC) and a one-line summary per change burst to
 * /dev/kmsg (mirrored onto the phone screen).
 *
 * Arming: polls the LTE boot-address register (0x402e0248) until the driver
 * writes the DSP entry (step [7]); the sampler runs right after the
 * release, then baselines the small regions at once. Big regions are walked in
 * 16 KB slices round-robin; each slice's first visit is its baseline, so no
 * slow full-size copy delays the start (uncached /dev/mem runs at ~6 MB/s).
 * The baseline line prints the queue/handshake words found right after release.
 *
 * Build: see tools/phone/Makefile (static armhf binary).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct region {
	const char *name;
	uint32_t phys;
	uint32_t size;
	int big;			/* compared in slices */
	char only;			/* 'l'/'g': only in that mode */
	volatile uint32_t *map;
	uint32_t *prev;
	uint32_t cursor;
	unsigned char *seen;		/* big: slice baselined yet */
};

static struct region regions[] = {
	{ "pmu",     0x402b0000, 0x300,    0 },	/* c0/c4/d4 power status */
	{ "aon",     0x402e0000, 0x300,    0 },	/* 0x130 reset, 0x248 boot addr */
	{ "ltewin",  0x80400000, 0x12000,  0 },	/* queue + LDSP share */
	{ "hs",      0x80447000, 0x1000,   0 },	/* handshake cells */
	{ "slots",   0x8072c000, 0x34000,  1 },	/* gap, split + UL slots */
	{ "tail",    0x80760000, 0xa0000,  1 },	/* below Linux RAM */
	{ "lteimg",  0x80448000, 0x2e4000, 1, 'l' },	/* DSP image / data */
	{ "gsmwin",  0x800e0000, 0x20000,  0 },	/* GSM window, queue, params */
	{ "gsmimg",  0x80100000, 0x300000, 1, 'g' },	/* GSM DSP image / data */
};
#define NREG (sizeof(regions) / sizeof(regions[0]))
#define SLICE 0x4000u
#define MAX_LINES 24			/* per region per pass */

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int kmsg = -1;
static char mode = 'l';		/* argv[1]: lte (default) or gsm */

static void klog(const char *s)
{
	char buf[256];
	int n = snprintf(buf, sizeof(buf), "<4>ms %s\n", s);

	if (kmsg >= 0)
		if (write(kmsg, buf, n) < 0)
			kmsg = -1;
}

static void map_all(int fd)
{
	for (unsigned i = 0; i < NREG; i++) {
		struct region *r = &regions[i];
		void *p;

		if (r->only && r->only != mode)
			continue;
		p = mmap(NULL, r->size, PROT_READ, MAP_SHARED, fd, r->phys);

		if (p == MAP_FAILED) {
			printf("map %s %08x failed\n", r->name, r->phys);
			exit(1);
		}
		r->map = p;
		r->prev = malloc(r->size);
		r->seen = calloc(r->size / SLICE + 1, 1);
	}
}

static void snapshot(struct region *r)
{
	for (uint32_t i = 0; i < r->size / 4; i++)
		r->prev[i] = r->map[i];
}

/* Compare words [from, to) of one region; return the number changed. */
static unsigned scan(struct region *r, uint32_t from, uint32_t to, double t)
{
	unsigned changed = 0;

	for (uint32_t i = from; i < to; i++) {
		uint32_t v = r->map[i];

		if (v == r->prev[i])
			continue;
		if (changed < MAX_LINES)
			printf("%.6f %s %08x %08x -> %08x\n", t, r->name,
			       r->phys + i * 4, r->prev[i], v);
		r->prev[i] = v;
		changed++;
	}
	if (changed > MAX_LINES)
		printf("%.6f %s ... %u words changed this pass\n", t, r->name,
		       changed);
	return changed;
}

int main(int argc, char **argv)
{
	unsigned long passes = 0;
	double t0, last_beat;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);

	if (argc > 1 && argv[1][0] == 'g')
		mode = 'g';
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	kmsg = open("/dev/kmsg", O_WRONLY);
	setvbuf(stdout, NULL, _IOLBF, 0);
	map_all(fd);

	/* regions[1] is the AON page: wait for the boot address (step [7]). The
	 * load thread runs [1]..[9] without yielding, so a transient state (like
	 * start-ctrl low) is never seen; the boot address stays set, and the
	 * sampler gets the CPU as soon as the thread sleeps after the release. */
	if (mode == 'g') {
		/* GSM: the driver writes the TDSP boot vector 0x402e0250 (reset
		 * 0x18000000) just before its release, same no-yield pattern. */
		printf("armed (gsm): waiting for 0x402e0250 != 0x18000000\n");
		klog("armed gsm");
		while (regions[1].map[0x250 / 4] == 0x18000000)
			usleep(200);
	} else {
		printf("armed: waiting for 0x402e0248 = 0x40224200\n");
		klog("armed");
		while (regions[1].map[0x248 / 4] != 0x40224200)
			usleep(200);
	}
	for (unsigned i = 0; i < NREG; i++)
		if (regions[i].map && !regions[i].big)
			snapshot(&regions[i]);
	/* Queue ctl, resp ctl and the handshake cells as found at baseline. */
	printf("base qctl=%08x rctl=%08x hsC40=%08x hsE00=%08x hsE14=%08x\n",
	       regions[2].prev[0xe400 / 4], regions[2].prev[0xe478 / 4],
	       regions[3].prev[0xc40 / 4], regions[3].prev[0xe00 / 4],
	       regions[3].prev[0xe14 / 4]);
	/* GSM queue ctl (win+0xe400) and run word (win+0x19ca4). */
	printf("base gsm qctl=%08x run=%08x aon250=%08x\n",
	       regions[7].prev[0xe400 / 4], regions[7].prev[0x19ca4 / 4],
	       regions[1].map[0x250 / 4]);
	t0 = last_beat = now();
	printf("%.6f baseline taken (aon130=%08x)\n", t0, regions[1].map[0x130 / 4]);
	klog("baseline");

	for (;;) {
		double t = now();
		unsigned total = 0;
		char line[200];
		int n = 0;

		for (unsigned i = 0; i < NREG; i++) {
			struct region *r = &regions[i];
			unsigned c;

			if (!r->map)
				continue;

			if (r->big) {
				uint32_t from = r->cursor / 4;
				uint32_t len = SLICE;

				if (r->cursor + len > r->size)
					len = r->size - r->cursor;
				if (r->seen[r->cursor / SLICE]) {
					c = scan(r, from, from + len / 4, t);
				} else {
					for (uint32_t k = from; k < from + len / 4; k++)
						r->prev[k] = r->map[k];
					r->seen[r->cursor / SLICE] = 1;
					c = 0;
				}
				r->cursor = (r->cursor + len) % r->size;
			} else {
				c = scan(r, 0, r->size / 4, t);
			}
			if (c && n < (int)sizeof(line) - 24)
				n += snprintf(line + n, sizeof(line) - n,
					      "%s:%u ", r->name, c);
			total += c;
		}
		passes++;
		if (total) {
			char msg[240];

			snprintf(msg, sizeof(msg), "+%.0fms %s", (t - t0) * 1e3, line);
			klog(msg);
		}
		if (t - last_beat >= 0.1) {
			printf("%.6f beat +%.0f ms passes %lu\n", t, (t - t0) * 1e3,
			       passes);
			last_beat = t;
		}
	}
}
