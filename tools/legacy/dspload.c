// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
/* dspload.c - F286 GSM XDSP bring-up from live FPLinux (UMS9117)
 * The stock GSM boot sequence + param blocks from user space, restricted to
 * writes below 0x80200000 (Linux RAM started there when this was written).
 *
 * Usage: dspload <fwfile> [poll_seconds]        (default poll 60)
 * Loads first 0x100000 bytes of fw into DRAM 0x80100000 (upper fw sections
 * map to 0x80200000+ = kernel RAM, MUST NOT be written; entry+params
 * are all inside the first MB).
 *
 * Build: arm-linux-gnueabihf-gcc -static -O2 -o dspload dspload.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

#define DRAM_BASE   0x80100000UL
#define DRAM_LEN    0x00100000UL   /* ONLY first MB: 0x80200000+ was Linux RAM */
#define WIN_BASE    0x800e0000UL   /* GSM DSP AP window */
#define WIN_LEN     0x00012000UL
#define AON_BASE    0x402e0000UL   /* AON_APB */
#define AON_LEN     0x00002000UL
#define PMU_BASE    0x402b0000UL   /* PMU_APB incl. SET/CLEAR banks (+0x2000) */
#define PMU_LEN     0x00003000UL
#define ADI_BASE    0x40600000UL   /* ADI window (read-only use: die-id pre-check) */
#define ADI_LEN     0x00010000UL

#define Q_OFF       0xe400UL       /* DSP-relative cmd/resp queue ctl */
#define Q_WATCH     0xc00UL        /* watch 0xe400..0xf000 for changes */

static int memfd = -1;

static volatile uint32_t *map_range(unsigned long base, unsigned long len)
{
    off_t pg = (off_t)(base & ~0xfffUL);
    unsigned long span = (base - (unsigned long)pg) + len;
    volatile uint32_t *p = (volatile uint32_t *)mmap(NULL, (span + 0xfffUL) & ~0xfffUL,
                              PROT_READ | PROT_WRITE, MAP_SHARED, memfd, pg);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap 0x%08lX failed: %s\n", base, strerror(errno));
        exit(1);
    }
    p += (base & 0xfffUL) / 4;
    return p;
}

/* ---- ADI helpers (read path cannot wedge the AP bus) ---- */
static double now_s(void);
static void logf_ts(const char *fmt, ...);
static volatile uint32_t *adi;
#define ADIP(reg) (adi + ((reg) - ADI_BASE) / 4)
static int adi_read(unsigned long reg, uint32_t *out, int tmo_ms)
{
    double dl = now_s() + tmo_ms / 1000.0;
    uint32_t v;
    *ADIP(0x40600028) = (uint32_t)(reg & 0xFFFFu);
    do {
        v = *ADIP(0x4060002C);
        if (!(v & 0x80000000u)) { if (out) *out = v; return 0; }
    } while (now_s() < dl);
    return -1;
}
__attribute__((unused))
static int adi_write(unsigned long reg, uint32_t val, int tmo_ms)
{
    double dl = now_s() + tmo_ms / 1000.0;
    while (!(*ADIP(0x40600030) & 0x800u)) { if (now_s() > dl) return -1; }
    *ADIP(reg) = val;
    return 0;
}
__attribute__((unused))
static int adi_rmw16(unsigned long reg, uint32_t set, uint32_t clr)
{
    uint32_t v, v16, nv, rb;
    if (adi_read(reg, &v, 1000)) return -1;
    v16 = v & 0xFFFFu;
    nv = (v16 | set) & ~clr;
    if (adi_write(reg, nv, 1000)) return -1;
    if (adi_read(reg, &rb, 1000)) return -1;
    logf_ts("ADI RMW 0x%08lX: %04X -> %04X (rb %04X)", reg, v16, nv, rb & 0xFFFFu);
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

#define R32(a) (*(volatile uint32_t *)(a))   /* only for addresses inside an mmap */
static unsigned long t0_abs;

static void logf_ts(const char *fmt, ...)
{
    va_list ap;
    printf("[%8.3f] ", now_s() - t0_abs);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    t0_abs = now_s();
    const char *fwpath = (argc > 1) ? argv[1] : "/tmp/fw_gsm.bin";
    int poll_sec = (argc > 2) ? atoi(argv[2]) : 60;

    memfd = open("/dev/mem", O_SYNC | O_RDWR);
    if (memfd < 0) {
        fprintf(stderr, "open /dev/mem: %s\n", strerror(errno));
        return 1;
    }

    volatile uint32_t *dram = map_range(DRAM_BASE, DRAM_LEN);
    volatile uint32_t *win  = map_range(WIN_BASE, WIN_LEN);
    volatile uint32_t *aon  = map_range(AON_BASE, AON_LEN);
    volatile uint32_t *pmu  = map_range(PMU_BASE, PMU_LEN);
    adi = map_range(ADI_BASE, ADI_LEN);

    logf_ts("dspload start: fw=%s poll=%ds", fwpath, poll_sec);
    {
        uint32_t did;
        if (adi_read(0x40608E24, &did, 1000) == 0) {
            unsigned id = (did >> 8) & 0xFF;
            logf_ts("ADI die-id 0x40608E24 = %04X ([15:8]=%02X, %s)",
                    did & 0xFFFFu, id,
                    (id == 0x5A || id == 0x6A || id == 0x7A) ? "OK" : "UNEXPECTED");
        } else
            logf_ts("ADI die-id read TIMEOUT (analog die off; DSP boot unlikely to respond)");
    }

    /* ---- 1. copy firmware (first 1MB only) ---- */
    FILE *f = fopen(fwpath, "rb");
    if (!f) { logf_ts("FATAL: open fw: %s", strerror(errno)); return 1; }
    {
        static uint32_t buf[DRAM_LEN / 4];
        size_t got = fread(buf, 1, DRAM_LEN, f);
        fclose(f);
        if (got != DRAM_LEN) { logf_ts("FATAL: fw short: %zu", got); return 1; }
        memcpy((void *)dram, buf, DRAM_LEN);
        /* verify a sample */
        int bad = 0;
        volatile uint32_t *v = (volatile uint32_t *)buf;
        for (unsigned i = 0; i < DRAM_LEN / 4; i += 997)
            if (dram[i] != v[i]) bad++;
        logf_ts("fw copied: %lu bytes to 0x80100000, sample-mismatch words=%d",
                (unsigned long)DRAM_LEN, bad);
        logf_ts("fw header: %08X %08X %08X %08X (expect V1GG ASCII + sizes)",
                dram[0], dram[1], dram[2], dram[3]);
        logf_ts("entry bytes @0x80120080: %08X %08X",
                dram[0x20080 / 4], dram[0x20084 / 4]);
    }

    /* ---- 2. hold GSM DSP in reset (AON_APB+0x130 bit17, RMW) ---- */
    uint32_t r130 = aon[0x130 / 4];
    logf_ts("0x402e0130 pre=%08X", r130);
    aon[0x130 / 4] = r130 | 0x20000;
    logf_ts("0x402e0130 |= 0x20000 (HOLD GSM) -> %08X", aon[0x130 / 4]);

    /* ---- 3. param blocks (DSPBOOT_SEQ 5, GSM) ---- */
#define PO(o, v) do { dram[(o) / 4] = (v); \
        logf_ts("param [0x801%05lX] = %08X", (unsigned long)(o), (uint32_t)(v)); } while (0)
    PO(0x20044, 0);
    PO(0xb8000, 0);
    PO(0x20044, 0);          /* cfg+0x44 */
    PO(0x20048, 0xbb);       /* cfg+0x48 default selector */
    /* params @base+0x19670, 0x6f8 bytes zeroed, then documented fields */
    memset((void *)(dram + 0x19670 / 4), 0, 0x6f8);
    logf_ts("params zeroed: base+0x19670 len 0x6f8");
    PO(0x19670, 1);          /* [base+0x19000+0x670] = 1 */
    PO(0x19670 + 0x638, 0);  /* clock/time pair (runtime-derived; zeros) */
    PO(0x19670 + 0x63c, 0);
    PO(0x19670 + 0x6e4, 0);  /* {u8 flag,u16 val} */
    PO(0x19670 + 0x6ec, 0);  /* feature flag (boot-check unknown -> 0) */
    PO(0x19670 + 0x6f4, 0);
    PO(0x19670 + 0x774, 0);  /* tick */

    /* ---- 4. start-ctrl low ---- */
    aon[0x254 / 4] &= ~1u;
    logf_ts("0x402e0254 bit0=0 -> %08X", aon[0x254 / 4]);

    /* hold again (idempotent, stock order) */
    aon[0x130 / 4] |= 0x20000;
    logf_ts("0x402e0130 hold again -> %08X", aon[0x130 / 4]);

    /* ---- 5. clocks ---- */
    uint32_t r6c = aon[0x6c / 4];
    logf_ts("0x402e006c pre=%08X", r6c);
    aon[0x6c / 4] = r6c | 0x9620;
    logf_ts("0x402e006c |= 0x9620 -> %08X", aon[0x6c / 4]);

    /* ---- 6. PMU: clear DSP deep-sleep force (bit17 via CLEAR bank) and
     *         clear 0x402b0044 bits17/16 (per-DSP force bits, defensive) ---- */
    logf_ts("0x402b00cc pre=%08X", pmu[0xcc / 4]);
    pmu[0x20cc / 4] = 0x20000;
    logf_ts("0x402b20cc(CLEAR) = 0x20000 -> underlying %08X", pmu[0xcc / 4]);
    logf_ts("0x402b0044 pre=%08X", pmu[0x44 / 4]);
    pmu[0x2044 / 4] = 0x30000;
    logf_ts("0x402b2044(CLEAR) = 0x30000 -> underlying %08X", pmu[0x44 / 4]);

    /* ---- 7. boot address = (0x80100000+0x20080)>>1 ---- */
    aon[0x250 / 4] = 0x04009040;
    logf_ts("0x402e0250 = 0x04009040 -> %08X", aon[0x250 / 4]);

    { volatile int i; for (i = 0; i < 100; i++) ; }   /* stock ~100-iter delay */

    /* ---- 8. start-ctrl high ---- */
    aon[0x254 / 4] |= 1u;
    logf_ts("0x402e0254 bit0=1 -> %08X", aon[0x254 / 4]);

    /* ---- 9. RELEASE ---- */
    aon[0x130 / 4] &= ~0x20000u;
    logf_ts("0x402e0130 &= ~0x20000 (RELEASE) -> %08X", aon[0x130 / 4]);
    logf_ts("=== GSM DSP RELEASED, polling queue 0x800ee400 ===");

    /* ---- 10. poll queue region ---- */
    static uint32_t snap[Q_WATCH / 4];
    memcpy(snap, (const void *)(win + Q_OFF / 4), Q_WATCH);
    logf_ts("queue initial: in=%04X out=%04X",
            win[Q_OFF / 4] & 0xffff, (win[Q_OFF / 4] >> 16) & 0xffff);
    logf_ts("win+0xe14 (sleep sync) = %08X, win+0x10bf8 (dumpctl) = %08X",
            win[0xe14 / 4], win[0x10bf8 / 4]);

    int doorbell_done = 0;
    double t_end = now_s() + poll_sec;
    int iter = 0;
    while (now_s() < t_end) {
        usleep(500 * 1000);
        iter++;
        /* any word change in watched region? */
        for (unsigned i = 0; i < Q_WATCH / 4; i++) {
            uint32_t nw = win[Q_OFF / 4 + i];
            if (nw != snap[i]) {
                logf_ts("CHANGE win+0x%04lX: %08X -> %08X",
                        (unsigned long)(Q_OFF + i * 4), snap[i], nw);
                snap[i] = nw;
            }
        }
        if (iter % 10 == 0) {
            logf_ts("poll: ctl=%08X e14=%08X 10bf8=%08X dram0=%08X dram20080=%08X",
                    win[Q_OFF / 4], win[0xe14 / 4], win[0x10bf8 / 4],
                    dram[0], dram[0x20080 / 4]);
        }
        if (!doorbell_done && (now_s() - t0_abs) > 45.0) {
            doorbell_done = 1;
            if (aon[0x1144 / 4] != 0x1a) {
                aon[0x1144 / 4] = 0x1a;
                logf_ts("DOORBELL experiment: 0x402e1144 = 0x1a (once) -> %08X",
                        aon[0x1144 / 4]);
            } else {
                logf_ts("0x402e1144 already 0x1a, skipping doorbell write");
            }
        }
    }

    /* ---- 11. final dumps ---- */
    logf_ts("=== FINAL STATE ===");
    logf_ts("0x402e0130=%08X 0x402e006c=%08X 0x402e0250=%08X 0x402e0254=%08X",
            aon[0x130 / 4], aon[0x6c / 4], aon[0x250 / 4], aon[0x254 / 4]);
    logf_ts("queue ctl: in=%04X out=%04X",
            win[Q_OFF / 4] & 0xffff, (win[Q_OFF / 4] >> 16) & 0xffff);
    logf_ts("queue region hexdump 0x800ee400..0x800ee700:");
    for (unsigned o = 0; o < 0x300; o += 16) {
        printf("[%8.3f] %04lx:", now_s() - t0_abs, (unsigned long)(Q_OFF + o));
        for (unsigned j = 0; j < 4; j++)
            printf(" %08X", win[(Q_OFF + o) / 4 + j]);
        printf("\n");
    }
    fflush(stdout);
    logf_ts("DSP DRAM first 0x40 bytes:");
    for (unsigned o = 0; o < 0x40; o += 16) {
        printf("[%8.3f] %06lx:", now_s() - t0_abs, DRAM_BASE + o);
        for (unsigned j = 0; j < 4; j++)
            printf(" %08X", dram[o / 4 + j]);
        printf("\n");
    }
    fflush(stdout);
    logf_ts("dspload done");
    return 0;
}
