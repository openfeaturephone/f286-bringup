// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
/* whale.c - F286 whale/CP-die power + GSM DSP boot driver
 * Written before the f286-radio driver; see tools/legacy/README.md.
 *
 * Safety: EVERY individual MMIO register access runs in a fork-isolated
 * child (pipe-acked, 5s cap). A child that dies (SIGBUS on external abort, seen
 * live at PA 0x40310008) or hangs (unclaimed transaction, seen at 0x515d/e)
 * leaves the parent alive: per-access verdict printed, mode continues or aborts.
 *
 * Modes:
 *   whale adi_baseline            Session A read-only (ADI + PMU + 0x4031 probe)
 *   whale adi RD <hexaddr>        arbitrary ADI read  (window 0x40600000-0x4060FFFF)
 *   whale adi WR <hexaddr> <hexval>  arbitrary ADI write + readback
 *   whale adi_enable              Session B batch; env SESSION_B_OK=1 required;
 *                                 env WHALE_FORCE=1 bypasses the die-id gate;
 *                                 0x40310008/20 steps auto-skip on bus-abort
 *   whale whale_request           power request; 0x515d/e only if c4/c0 left 7
 *                                 (then ONE fork-isolated 0x515e0058 probe)
 *   whale dspload <fw> [sec]      GSM DSP boot + queue poll (poll loop child-sandboxed)
 * ADI protocol: READ reg->[0x40600028], poll [0x4060002C]
 * bit31 clear; WRITE poll [0x40600030] bit11 then direct 32-bit store.
 * ADI transactions cannot wedge the bus (slave always completes/times out).
 * Build: arm-linux-gnueabihf-gcc -static -O2 -Wall -o whale whale.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/select.h>

static int memfd = -1;
static double t0;

struct map { unsigned long base, len; volatile uint32_t *p; };
static struct map maps[] = {
    { 0x402b0000, 0x3000, NULL },   /* PMU_APB incl. SET/CLEAR banks */
    { 0x402e0000, 0x3000, NULL },   /* AON_APB + AON2 + 0x402e2xxx gates */
    { 0x60830000, 0x2000, NULL },   /* whale APB bridge */
    { 0x515d0000, 0x1000, NULL },   /* whale-die RF gate  - S3 GATED */
    { 0x515e0000, 0x4000, NULL },   /* whale-die AON/PMU  - S3 GATED */
    { 0x60100000, 0x2000, NULL },   /* whale misc die cfg (proven AP-reachable) */
    { 0x40600000, 0x10000, NULL },  /* ADI window */
    { 0x40310000, 0x1000, NULL },   /* AP-side APB - EXTERNAL-ABORTS on read (on hardware) */
    { 0x80100000, 0x100000, NULL }, /* GSM DSP DRAM first MB */
    { 0x800e0000, 0x12000, NULL },  /* GSM DSP AP window (queue at +0xe400) */
    { 0x50800000, 0x1000, NULL },   /* CM4 IRAM (boot vector) - fork-isolated */
    { 0x404d0000, 0x1000, NULL },   /* 0x404D block radio-on tail) */
    { 0x400a8000, 0x1000, NULL },   /* CP->AP mailbox (FIFO status +0x14) */
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void lg(const char *fmt, ...)
{
    va_list ap;
    printf("[%8.3f] ", now_s() - t0);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void maps_init(void)
{
    size_t pg = 4096;
    memfd = open("/dev/mem", O_SYNC | O_RDWR);
    if (memfd < 0) { fprintf(stderr, "open /dev/mem: %s\n", strerror(errno)); exit(1); }
    for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        off_t pgbase = (off_t)(maps[i].base & ~(pg - 1));
        unsigned long span = maps[i].base - (unsigned long)pgbase + maps[i].len;
        maps[i].p = mmap(NULL, (span + pg - 1) & ~(pg - 1), PROT_READ | PROT_WRITE,
                         MAP_SHARED, memfd, pgbase);
        if (maps[i].p == MAP_FAILED) {
            fprintf(stderr, "mmap 0x%08lX: %s\n", maps[i].base, strerror(errno));
            exit(1);
        }
        maps[i].p += (maps[i].base & (pg - 1)) / 4;
    }
}

static volatile uint32_t *P(unsigned long addr)
{
    for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++)
        if (addr >= maps[i].base && addr < maps[i].base + maps[i].len)
            return maps[i].p + (addr - maps[i].base) / 4;
    fprintf(stderr, "addr 0x%08lX unmapped\n", addr);
    exit(1);
}

/* ---- fork-isolated access core ----
 * op: 0=read, 1=store wval, 2=RMW(set=wval, clr=xval)
 * out: 12-byte packet {a,b,c}: rd {val,-,-}; wr {stored,-,-}; rmw {old,new,rb}
 * returns 0 ok, -1 hung (killed at 5s), -2 child died (SIGBUS/segv)          */
static int fa2(const char *what, unsigned long addr, int op,
               uint32_t wval, uint32_t xval, uint32_t *out)
{
    int pfd[2];
    if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        uint32_t v[3] = {0, 0, 0};
        if (op == 0) {
            v[0] = *P(addr);
        } else if (op == 1) {
            *P(addr) = wval;
            v[0] = wval;
        } else {
            uint32_t old = *P(addr);
            uint32_t nv = (old | wval) & ~xval;
            *P(addr) = nv;
            v[0] = old; v[1] = nv; v[2] = *P(addr);
        }
        ssize_t r = write(pfd[1], v, sizeof(v));
        (void)r;
        _exit(0);
    }
    close(pfd[1]);
    uint32_t v[3] = {0, 0, 0};
    int got = 0, verdict;
    double dl = now_s() + 5.0;
    size_t want = (op == 2) ? 12 : 4;
    while (got < (int)want) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(pfd[0], &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int s = select(pfd[0] + 1, &fds, NULL, NULL, &tv);
        if (s > 0) {
            ssize_t r = read(pfd[0], (char *)v + got, want - got);
            if (r <= 0) break;              /* EOF: child died */
            got += (int)r;
            continue;
        }
        if (now_s() > dl) break;
    }
    close(pfd[0]);
    if (got >= (int)want) {
        verdict = 0;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    } else {
        verdict = (got > 0) ? -2 : -1;
        kill(pid, SIGKILL);
        if (now_s() < dl + 0.2) { /* reap if it already exited; else leave */ }
        waitpid(pid, NULL, WNOHANG);
    }
    if (verdict == 0 && out)
        memcpy(out, v, want);
    if (verdict == -1)
        lg("FA %s 0x%08lX: VERDICT=HUNG (child %d abandoned)", what, addr, pid);
    else if (verdict == -2)
        lg("FA %s 0x%08lX: VERDICT=BUS-ABORT (child %d died, partial=%d)",
           what, addr, pid, got);
    return verdict;
}

/* fork-isolated logged accessors; return 0 on success */
static int RDf(unsigned long a, uint32_t *out)
{
    uint32_t w[3];
    int r = fa2("RD", a, 0, 0, 0, w);
    if (r == 0) {
        if (out) *out = w[0];
        lg("RD 0x%08lX = %08X", a, w[0]);
    }
    return r;
}
static int WRf(unsigned long a, uint32_t v)
{
    uint32_t w[3];
    int r = fa2("WR", a, 1, v, 0, w);
    if (r == 0)
        lg("WR 0x%08lX = %08X (stored)", a, v);
    return r;
}
static int RMWf(unsigned long a, uint32_t set, uint32_t clr, uint32_t *newout)
{
    uint32_t w[3];
    int r = fa2("RMW", a, 2, set, clr, w);
    if (r == 0) {
        lg("RMW 0x%08lX: %08X -> %08X (set %08X clr %08X, rb %08X)",
           a, w[0], w[1], set, clr, w[2]);
        if (newout) *newout = w[1];
    }
    return r;
}

/* ---- ADI helpers (direct: ADI slave cannot wedge the AP bus) ---- */
static int adi_read(unsigned long reg, uint32_t *out, int tmo_ms)
{
    volatile uint32_t *cmd = P(0x40600028);
    volatile uint32_t *dat = P(0x4060002C);
    double dl = now_s() + tmo_ms / 1000.0;
    uint32_t v;
    *cmd = (uint32_t)(reg & 0xFFFFu);
    do {
        v = *dat;
        if (!(v & 0x80000000u)) { if (out) *out = v; return 0; }
    } while (now_s() < dl);
    return -1;
}
/* ADI write per FPLinux spi-sprd-adi.c (authoritative for this chip):
 *   drain:  wait ARM_FIFO_STS bit10 (BIT_FIFO_EMPTY)
 *   gate:   write only when bit11 (BIT_FIFO_FULL) is CLEAR
 *   store:  direct 32-bit store into the slave window (0x40608000+ physicals)
 * plus UMS9117 user-lock: 0x224 acquired by reading zero, released with token */
#define ADI_FIFO_EMPTY 0x400u
#define ADI_FIFO_FULL  0x800u
#define ADI_USER_LOCK  0x40600224UL
#define ADI_USER_UNLOCK 0x5348554Cu

static int adi_lock(int tmo_ms)
{
    volatile uint32_t *lk = P(ADI_USER_LOCK);
    double dl = now_s() + tmo_ms / 1000.0;
    while (*lk != 0) {
        if (now_s() > dl) return -1;
        usleep(500);
    }
    return 0;   /* reading zero acquires the semaphore */
}
static void adi_unlock(void)
{
    *P(ADI_USER_LOCK) = ADI_USER_UNLOCK;
}

static int adi_write(unsigned long reg, uint32_t val, int tmo_ms)
{
    volatile uint32_t *sts = P(0x40600030);
    double dl = now_s() + tmo_ms / 1000.0;
    int locked = (adi_lock(5) == 0);
    if (!locked)
        lg("ADI user-lock not free (0x224 nonzero) - proceeding unlocked (single-master)");
    /* drain is best-effort: live FIFO_STS can read 0 (neither bit set) yet the
     * write still succeeds - the driver's hard requirement is only NOT-FULL */
    { double d2 = now_s() + 0.05; while (!(*sts & ADI_FIFO_EMPTY) && now_s() < d2) ; }
    while (*sts & ADI_FIFO_FULL) {           /* write only when NOT full */
        if (now_s() > dl) { if (locked) adi_unlock(); return -1; }
    }
    *P(reg) = val;                           /* bus-intercepted slave store */
    { double d2 = now_s() + 0.05; while (!(*sts & ADI_FIFO_EMPTY) && now_s() < d2) ; }
    if (locked) adi_unlock();
    return 0;
}
static int adi_rmw16(unsigned long reg, uint32_t set, uint32_t clr)
{
    uint32_t v, v16, nv, rb;
    if (adi_read(reg, &v, 1000)) { lg("ADI RMW 0x%08lX: READ TIMEOUT, SKIPPED", reg); return -1; }
    v16 = v & 0xFFFFu;
    nv = (v16 | set) & ~clr;
    if (adi_write(reg, nv, 1000)) { lg("ADI RMW 0x%08lX: FIFO TIMEOUT, SKIPPED", reg); return -1; }
    if (adi_read(reg, &rb, 1000)) {
        lg("ADI RMW 0x%08lX: %04X -> %04X (rb TIMEOUT)", reg, v16, nv);
        return 0;
    }
    lg("ADI RMW 0x%08lX: %04X -> %04X (rb %04X)", reg, v16, nv, rb & 0xFFFFu);
    return 0;
}

static void mdelay(int ms) { usleep(ms * 1000); }

/* fork-isolated poll; returns 1 hit, 0 timeout, -1 access failed */
static int poll5f(const char *what, unsigned long a, uint32_t mask, uint32_t want)
{
    double dl = now_s() + 5.0;
    uint32_t v = 0, w[3];
    int n = 0;
    while (now_s() < dl) {
        int r = fa2("poll", a, 0, 0, 0, w);
        if (r != 0) {
            lg("POLL %s: access %s after %d reads - aborting poll",
               what, r == -1 ? "HUNG" : "BUS-ABORT", n);
            return -1;
        }
        v = w[0];
        n++;
        if ((v & mask) == want) {
            lg("POLL %s: OK after %d reads, val %08X", what, n, v);
            return 1;
        }
        usleep(20000);
    }
    lg("POLL %s (0x%08lX & %04X == %04X): TIMEOUT 5s, %d reads, last %08X",
       what, a, mask, want, n, v);
    return 0;
}

static void status_dump(const char *tag)
{
    uint32_t c0, c4, d4;
    if (RDf(0x402b00c0, &c0)) { lg("STATUS %s: c0 unreadable", tag); return; }
    if (RDf(0x402b00c4, &c4)) { lg("STATUS %s: c4 unreadable", tag); return; }
    if (RDf(0x402b00d4, &d4)) { lg("STATUS %s: d4 unreadable", tag); return; }
    lg("STATUS %s: c0=%08X (WHu3g[4:0]=%u XDSPA=%u XDSPB=%u CM4=%u) "
       "c4=%08X (WTLCP[4:0]=%u) d4=%08X (state=%u)",
       tag, c0, c0 & 0x1f, (c0 >> 5) & 0x1f, (c0 >> 10) & 0x1f, (c0 >> 15) & 0x1f,
       c4, c4 & 0x1f, d4, (d4 >> 4) & 0xf);
}

static int die_id_check(int force)
{
    uint32_t v;
    if (adi_read(0x40608E24, &v, 1000) != 0) {
        lg("die-id READ TIMEOUT -> ABORT (no ADI writes without confirmed die-id)");
        return -1;
    }
    unsigned id = (v >> 8) & 0xFF;
    lg("die id full=%08X [15:8]=%02X", v, id);
    if (id == 0x5A || id == 0x6A || id == 0x7A)
        return 0;
    if (force) {
        lg("WHALE_FORCE=1: die-id gate BYPASSED (id=%02X != 5A/6A/7A)", id);
        return 0;
    }
    lg("die-id NOT 5A/6A/7A -> ABORT (set WHALE_FORCE=1 to override)");
    return -1;
}

/* ---------------- adi_baseline (read-only) ---------------- */
static void adi_baseline(void)
{
    lg("=== ADI BASELINE (read-only; every APB access fork-isolated) ===");
    unsigned long adiregs[] = {
        0x40608E24, 0x40608050, 0x40608E30, 0x40608C08, 0x40608C0C,
        0x40608404, 0x40608450, 0x40608454, 0x40608458, 0x40608DE8,
        0x40608E88, 0x40608090, 0x40608094,
    };
    for (unsigned i = 0; i < sizeof(adiregs) / sizeof(adiregs[0]); i++) {
        uint32_t v;
        if (adi_read(adiregs[i], &v, 1000) == 0)
            lg("ADI RD 0x%08lX = %04X (full %08X)", adiregs[i], v & 0xFFFFu, v);
        else
            lg("ADI RD 0x%08lX: TIMEOUT", adiregs[i]);
    }
    die_id_check(0);
    lg("--- plain APB probes (fork-isolated) ---");
    uint32_t v;
    if (RDf(0x40310008, &v) == 0) lg("0x40310008 READABLE = %08X", v);
    if (RDf(0x40310020, &v) == 0) lg("0x40310020 READABLE = %08X", v);
    status_dump("adi_baseline end");
}

/* ---------------- adi_enable (Session B) ---------------- */
static void adi_enable(void)
{
    lg("=== ADI ENABLE (Session B analog-die batch) ===");
    if (die_id_check(getenv("WHALE_FORCE") && !strcmp(getenv("WHALE_FORCE"), "1")))
        return;
    /* S2a: AP-side APB config - skip individual steps that bus-abort */
    uint32_t v;
    if (RDf(0x40310008, &v) == 0) {
        RMWf(0x40310008, 0, 0xB, NULL);
    } else {
        lg("0x40310008 skipped, bus-abort (read %s)",
           "failed");
    }
    if (RDf(0x40310020, &v) == 0) {
        WRf(0x40310020, 0xFFFF1AAE);
    } else {
        lg("0x40310020 skipped, bus-abort");
    }
    /* S2b..g analog-die batch (stock 0x811D05B4 + radio-on tail); ADI is safe */
    adi_rmw16(0x40608C08, 0x20, 0);
    adi_rmw16(0x40608C0C, 0x40, 0);
    adi_rmw16(0x40608C0C, 0x20, 0);
    adi_rmw16(0x40608DE8, 0x100, 0);
    adi_rmw16(0x40608454, 0x1, 0);
    adi_rmw16(0x40608458, 0x1, 0);
    adi_rmw16(0x40608404, 0x1, 0);
    adi_rmw16(0x40608404, 0x1000, 0);
    adi_rmw16(0x40608404, 0, 0x700);
    adi_rmw16(0x40608E88, 0x20, 0x8);
    lg("ADI batch done; verification re-read:");
    unsigned long regs[] = { 0x40608C08, 0x40608C0C, 0x40608DE8, 0x40608454,
                             0x40608458, 0x40608404, 0x40608E88 };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (adi_read(regs[i], &v, 1000) == 0)
            lg("ADI RB 0x%08lX = %04X", regs[i], v & 0xFFFFu);
        else
            lg("ADI RB 0x%08lX: TIMEOUT", regs[i]);
    }
    status_dump("after adi_enable");
    lg("NEXT STEP: run 'whale whale_request'");
}

/* ---------------- whale_request ---------------- */
static void whale_request(void)
{
    lg("=== WHALE REQUEST (power request; 0x515d/e S3-gated) ===");
    uint32_t v;
    WRf(0x402e10b0, 1);
    RMWf(0x60830044, 0x2000, 0, NULL);
    mdelay(5);
    RMWf(0x60830044, 0x40, 0, NULL);
    for (int round = 0; round < 2; round++) {
        int ok = poll5f("WTLCP c4[4:0]==0", 0x402b00c4, 0x1f, 0) > 0
                 && RDf(0x402b00d4, &v) == 0
                 && ((v >> 4) & 0xf) == 6;
        if (ok) break;
        lg("round %d failed", round);
        uint32_t c4;
        if (RDf(0x402b00c4, &c4) == 0 && (c4 & 0x1f) == 7) {
            lg("FORCE: 0x402b2044=0x1000000 + 0x60830044|=0x40");
            WRf(0x402b2044, 0x1000000);
            RMWf(0x60830044, 0x40, 0, NULL);
        }
    }
    if (RDf(0x6010102c, &v) == 0)
        WRf(0x6010102c, (v & ~0x1000000u) | 0x2000000u);
    RMWf(0x60830044, 0, 0x2000, NULL);      /* release isolation */
    RMWf(0x60830044, 0x80, 0, NULL);        /* follow-up enable */
    status_dump("after whale_request");
    /* S3 gate */
    uint32_t c0 = 0xFFFFFFFFu, c4 = 0xFFFFFFFFu;
    RDf(0x402b00c0, &c0);
    RDf(0x402b00c4, &c4);
    if (((c4 & 0x1f) != 7 || (c0 & 0x1f) != 7) && c0 != 0xFFFFFFFFu) {
        lg("DIE POWERING (c0[4:0]=%u c4[4:0]=%u) -> ONE fork-isolated 0x515e0058 probe",
           c0 & 0x1f, c4 & 0x1f);
        uint32_t pv;
        if (fa2("probe 0x515e0058", 0x515e0058, 0, 0, 0, &pv) == 0)
            lg("PROBE 0x515E0058 = %08X (page ALIVE)", pv);
        lg("S3 remainder deferred: run explicit follow-up after reviewing this log");
    } else {
        lg("die still OFF (c0=%08X c4=%08X) -> NOT touching 0x515d/0x515e", c0, c4);
    }
}

/* ---------------- dspload (GSM DSP boot, sequence unchanged) ---------------- */
#define DRAM_BASE   0x80100000UL
#define DRAM_LEN    0x00100000UL
#define Q_OFF       0xe400UL
#define Q_WATCH     0xc00UL

/* one child snapshots the whole watched window; parent survives bus death */
static int snap_window(volatile uint32_t *win, uint32_t *buf)
{
    int pfd[2];
    if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        uint32_t local[Q_WATCH / 4];
        memcpy(local, (const void *)(win + Q_OFF / 4), Q_WATCH);
        ssize_t r = write(pfd[1], local, sizeof(local));
        (void)r;
        _exit(0);
    }
    close(pfd[1]);
    int got = 0;
    double dl = now_s() + 3.0;
    while (got < (int)Q_WATCH) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(pfd[0], &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int s = select(pfd[0] + 1, &fds, NULL, NULL, &tv);
        if (s > 0) {
            ssize_t r = read(pfd[0], (char *)buf + got, Q_WATCH - got);
            if (r <= 0) break;
            got += (int)r;
            continue;
        }
        if (now_s() > dl) break;
    }
    close(pfd[0]);
    if (got == (int)Q_WATCH) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 0;
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, WNOHANG);
    lg("POLL: window snapshot %s - STOPPING POLL LOOP",
       got ? "child died (BUS-ABORT)" : "HUNG");
    return -1;
}

static void dspload_mode(const char *fwpath, int poll_sec)
{
    lg("dspload start: fw=%s poll=%ds (first MB; entry+params inside; all MMIO fork-isolated)",
       fwpath, poll_sec);
    volatile uint32_t *dram = P(DRAM_BASE);
    volatile uint32_t *win  = P(0x800e0000);

    /* ADI die-id pre-check (read-only, informative) */
    {
        uint32_t did;
        if (adi_read(0x40608E24, &did, 1000) == 0)
            lg("ADI die-id 0x40608E24 [15:8]=%02X (pre-check)",
               (did >> 8) & 0xFF);
        else
            lg("ADI die-id read TIMEOUT (pre-check)");
    }

    FILE *f = fopen(fwpath, "rb");
    if (!f) { lg("FATAL: open fw: %s", strerror(errno)); return; }
    {
        static uint32_t buf[DRAM_LEN / 4];
        size_t got = fread(buf, 1, DRAM_LEN, f);
        fclose(f);
        if (got != DRAM_LEN) { lg("FATAL: fw short: %zu", got); return; }
        memcpy((void *)dram, buf, DRAM_LEN);
        int bad = 0;
        volatile uint32_t *v = (volatile uint32_t *)buf;
        for (unsigned i = 0; i < DRAM_LEN / 4; i += 997)
            if (dram[i] != v[i]) bad++;
        lg("fw copied: %lu bytes to 0x80100000, sample-mismatch words=%d",
           (unsigned long)DRAM_LEN, bad);
        lg("fw header: %08X %08X; entry @0x80120080: %08X %08X",
           dram[0], dram[1], dram[0x20080 / 4], dram[0x20084 / 4]);
    }

    /* register sequence - every MMIO access fork-isolated */
    uint32_t v;
    if (RMWf(0x402e0130, 0x20000, 0, &v))
        lg("FATAL: cannot hold GSM DSP in reset (AON page dead) - aborting");
    lg("param writes into safe DRAM (0x80100000..0x80200000)");
#define PO(o, val) do { dram[(o) / 4] = (val); } while (0)
    PO(0x20044, 0);
    PO(0xb8000, 0);
    PO(0x20044, 0);
    PO(0x20048, 0xbb);
    memset((void *)(dram + 0x19670 / 4), 0, 0x6f8);
    PO(0x19670, 1);
    PO(0x19670 + 0x638, 0);
    PO(0x19670 + 0x63c, 0);
    PO(0x19670 + 0x6e4, 0);
    PO(0x19670 + 0x6ec, 0);
    PO(0x19670 + 0x6f4, 0);
    PO(0x19670 + 0x774, 0);
    lg("params written: 0x20044/0xb8000=0, cfg 0x20048=0xbb, params[0]=1, rest 0");

    if (RMWf(0x402e0254, 0, 1, NULL)) lg("0x402e0254 bit0=0 FAILED (continuing)");
    RMWf(0x402e0130, 0x20000, 0, NULL);          /* hold again (stock order) */
    if (RDf(0x402e006c, &v) == 0)
        WRf(0x402e006c, v | 0x9620);
    uint32_t cc;
    if (RDf(0x402b00cc, &cc) == 0)
        lg("0x402b00cc pre=%08X", cc);
    WRf(0x402b20cc, 0x20000);                    /* CLEAR deep-sleep force bit17 */
    uint32_t p44;
    if (RDf(0x402b0044, &p44) == 0)
        lg("0x402b0044 pre=%08X", p44);
    WRf(0x402b2044, 0x30000);                    /* CLEAR bits17/16 */
    WRf(0x402e0250, 0x04009040);                 /* boot addr (0x80120080>>1) */
    { volatile int i; for (i = 0; i < 100; i++) ; }
    RMWf(0x402e0254, 1, 0, NULL);                /* start-ctrl high */
    RMWf(0x402e0130, 0, 0x20000, NULL);          /* RELEASE */
    lg("=== GSM DSP RELEASED, polling queue 0x800ee400 ===");

    static uint32_t snap[Q_WATCH / 4];
    if (snap_window(win, snap)) { lg("initial snapshot failed - aborting poll"); return; }
    lg("queue initial: in=%04X out=%04X",
       snap[0] & 0xffff, (snap[0] >> 16) & 0xffff);

    int doorbell_done = 0;
    double t_end = now_s() + poll_sec;
    int iter = 0;
    while (now_s() < t_end) {
        usleep(500 * 1000);
        iter++;
        static uint32_t cur[Q_WATCH / 4];
        if (snap_window(win, cur)) return;
        for (unsigned i = 0; i < Q_WATCH / 4; i++) {
            if (cur[i] != snap[i]) {
                lg("CHANGE win+0x%04lX: %08X -> %08X  <<< DSP ACTIVITY",
                   (unsigned long)(Q_OFF + i * 4), snap[i], cur[i]);
                snap[i] = cur[i];
            }
        }
        if (iter % 10 == 0)
            lg("poll %ds: ctl=%08X", iter / 2, cur[0]);
        if (!doorbell_done && (now_s() - t0) > 45.0) {
            doorbell_done = 1;
            if (RDf(0x402e1144, &v) == 0 && v != 0x1a) {
                WRf(0x402e1144, 0x1a);
                lg("DOORBELL: 0x402e1144 = 0x1a (once)");
            } else
                lg("0x402e1144 already 0x1a or unreadable, skipping doorbell");
        }
    }
    lg("=== FINAL: queue ctl in=%04X out=%04X (poll done) ===",
       snap[0] & 0xffff, (snap[0] >> 16) & 0xffff);
    status_dump("after dspload");
}

/* ---------------- radio_on: A+B+C full stock radio-on set ---------------- */
static void radio_on_mode(void)
{
    uint32_t v;
    lg("=== RADIO-ON bring-up (A gates, B clears, C two-stage request) ===");
    lg("--- A: AON gate pairs (re-assert all four) ---");
    WRf(0x402e1004, 0x100);
    WRf(0x402e1010, 0x200);
    WRf(0x402e2004, 0x100);
    WRf(0x402e2010, 0x200);
    lg("--- B: whale die reset/bridge clears (stock FUN_80d0f186 order) ---");
    WRf(0x402b20cc, 0x20000);      /* bank2 CLEAR: 0x402b00cc bit17 (DSP deep-sleep) */
    WRf(0x402b20cc, 0x200000);     /* bank2 CLEAR: 0x402b00cc bit21 (CM4) */
    WRf(0x402e2144, 0x10);         /* whale-side system reset clear, stage 1 */
    WRf(0x402e2144, 1);            /* stage 2 */
    WRf(0x402e1144, 2);            /* AON2 reset clear bit1 */
    WRf(0x402e10b0, 1);            /* whale AON bridge enable */
    WRf(0x402e20b0, 0xF);          /* AON3 whale-side bridge enable */
    WRf(0x402e0244, 0x00010100);
    lg("--- 0x404d0008 |= 0x80 (new page) ---");
    if (RDf(0x404d0008, &v) == 0)
        WRf(0x404d0008, v | 0x80);
    lg("--- C: two-stage power request ---");
    WRf(0x402b2044, 0x2000000);
    WRf(0x402b2044, 0x1000000);
    RMWf(0x60830044, 0x40, 0, NULL);
    lg("--- polling c4[4:0] and d4[7:4] for 30s ---");
    uint32_t c4 = 0xFFFFFFFF, c0 = 0xFFFFFFFF, d4 = 0xFFFFFFFF;
    double dl = now_s() + 30.0;
    int moved = 0;
    while (now_s() < dl) {
        uint32_t a, b, c;
        if (RDf(0x402b00c4, &a) == 0) c4 = a;
        if (RDf(0x402b00d4, &b) == 0) d4 = b;
        if (RDf(0x402b00c0, &c) == 0) c0 = c;
        if ((c4 & 0x1f) != 7 || (c0 & 0x1f) != 7) { moved = 1; break; }
        usleep(1000000);
    }
    lg("poll end: c4=%08X d4=%08X c0=%08X moved=%d", c4, d4, c0, moved);
    if (moved) {
        lg("--- D: die-local first touches (c4/c0 LEFT 7) ---");
        if (RDf(0x515e0058, &v) == 0)
            WRf(0x515e0058, v | 1);
        WRf(0x515e0068, 0xDD00DD);
        WRf(0x515e2000, 0x380);
        WRf(0x515e1028, 0x380);
        WRf(0x60831044, 0x80);
        WRf(0x60831034, 0x410);
        WRf(0x515e2000, 0x300000);
        if (RDf(0x515d0020, &v) == 0 && v == 0)
            WRf(0x515d0020, 2);
        RMWf(0x402b003c, 0, 0x2000000, NULL);
        poll5f("WHu3g c0[4:0]==0", 0x402b00c0, 0x1f, 0);
    } else {
        lg("die still OFF after A+B+C -> per recipe, D touches stay forbidden");
    }
    status_dump("after radio_on");
}

/* ---------------- a4031: stock-keyed access to the 0x40310000 block ----------------
 * RE (payload 0x815abf5c-0x815ac028): 0x40310000 = 0x402E1000 + 0x2F000; every
 * access = key 0xE551 -> [0x40310020], then [0x40310008], then [0x40310020] =
 * 0xFFFF1AAE. Companion gate SETs: [0x402e1004]=0x100, [0x402e1010]=0x200,
 * [0x402e2004]=0x100, [0x402e2010]=0x200 (SET aliases, unnamed bits 8/9). */
static int a4031_key(int v) { return WRf(0x40310020, (uint32_t)v); }

static void a4031_mode(void)
{
    uint32_t v;
    int alive = 0;
    lg("=== 0x4031 ACCESS (stock protocol: gates -> 0xE551 key -> step-0) ===");
    lg("--- rfa rail re-read (mission ask) ---");
    adi_read(0x40608C94, &v, 1000); lg("ADI C94 = %04X", v & 0xFFFF);
    adi_read(0x40608CAC, &v, 1000); lg("ADI CAC = %04X", v & 0xFFFF);
    adi_read(0x40608C60, &v, 1000); lg("ADI C60 = %04X", v & 0xFFFF);
    lg("--- gate state (base regs = aliases -0x1000) ---");
    RDf(0x402e0004, &v); RDf(0x402e0010, &v);
    RDf(0x402e1004, &v); RDf(0x402e1010, &v);
    RDf(0x402e2004, &v); RDf(0x402e2010, &v);
    lg("--- candidate 1: AON_APB2 SET aliases bit8@0x1004, bit9@0x1010 ---");
    WRf(0x402e1004, 0x100);
    WRf(0x402e1010, 0x200);
    if (fa2("probe 0x40310008", 0x40310008, 0, 0, 0, &v) == 0) {
        alive = 1;
        lg("0x40310008 ALIVE = %08X", v);
    }
    if (!alive) {
        lg("--- candidate 2: AON SET aliases 0x402e2004/0x402e2010 ---");
        WRf(0x402e2004, 0x100);
        WRf(0x402e2010, 0x200);
        if (fa2("probe 0x40310008", 0x40310008, 0, 0, 0, &v) == 0) {
            alive = 1;
            lg("0x40310008 ALIVE = %08X", v);
        }
    }
    if (!alive) {
        lg("0x40310008 STILL DEAD after gate candidates - stopping (no step-0)");
        status_dump("a4031 end (dead)");
        return;
    }
    lg("--- page ALIVE: stock step-0 with key protocol ---");
    a4031_key(0xE551);
    if (RDf(0x40310008, &v) == 0) {
        WRf(0x40310008, v & 0xFFFFFFF4u);      /* &= ~0xB */
    }
    WRf(0x40310020, 0xFFFF1AAE);
    /* companion gate sets (stock 0x815abff8 tail) */
    WRf(0x402e2004, 0x100);
    WRf(0x402e2010, 0x200);
    RDf(0x40310008, &v);
    RDf(0x40310020, &v);
    status_dump("a4031 end");
}

/* ---------------- s3: die-local tail (ONLY after a live 0x515e probe) ---------------- */
static void s3_mode(void)
{
    uint32_t v;
    lg("=== S3 DIE-LOCAL TAIL (precondition: 0x515e probed alive) ===");
    if (RDf(0x515e0058, &v) == 0)
        WRf(0x515e0058, v | 1);            /* whale AON enable */
    WRf(0x515e2000, 0x380);
    WRf(0x515e1028, 0x380);
    WRf(0x60831034, 0x410);
    WRf(0x60831044, 0x80);
    WRf(0x515e2000, 0x300000);
    if (RDf(0x515d0020, &v) == 0) {
        if (v == 0) WRf(0x515d0020, 2);
        else lg("0x515d0020 = %08X (nonzero, skip)", v);
    } else {
        lg("0x515d0020 unreadable - aborting rest");
        return;
    }
    RMWf(0x402b003c, 0, 0x2000000, NULL);  /* WHu3g power-UP */
    poll5f("WHu3g c0[4:0]==0", 0x402b00c0, 0x1f, 0);
    status_dump("after s3");
}

/* ---------------- adisweep: READ-ONLY raw ADI address sweep ----------------
 * The ADI RD cmd register (0x40600028) takes a raw 16-bit ADI address; the
 * live-proven analog-AON reads are words 0x8000-0x8FFF (index =
 * (word-0x8000)>>2, 0x000-0x3FF). Slaves beyond the analog window (PMIC,
 * possibly whale-die) live at other raw words. adisweep writes raw words
 * START, START+STRIDE, ... for COUNT reads and logs EVERY sample; it only
 * ever calls adi_read (no cmd/write path), so it cannot wedge anything. */
static void adisweep_mode(unsigned long start, unsigned count, unsigned stride)
{
    unsigned nz = 0, to = 0;

    lg("=== ADI SWEEP raw start=%04lX count=%u stride=%u (READ-ONLY) ===",
       start, count, stride);
    for (unsigned i = 0; i < count; i++) {
        unsigned long a = start + (unsigned long)i * stride;
        uint32_t v;

        if (adi_read(a, &v, 1000) == 0) {
            uint16_t v16 = (uint16_t)(v & 0xFFFFu);

            lg("ADISWEEP adi=%04lX full=%08X val16=%04X%s",
               a, v, v16,
               (v16 != 0x0000 && v16 != 0xFFFF) ? "  <<< NONZERO" : "");
            if (v16 != 0x0000 && v16 != 0xFFFF)
                nz++;
        } else {
            lg("ADISWEEP adi=%04lX: TIMEOUT", a);
            to++;
        }
        usleep(2000);       /* gentle pacing between transactions */
    }
    lg("=== ADI SWEEP start=%04lX done: nonzero=%u timeouts=%u of %u ===",
       start, nz, to, count);
}

/* ---------------- cm4boot: stock CM4 boot pre-step (fallback) ---------------- */
static void cm4boot_mode(void)
{
    uint32_t v;
    lg("=== CM4 BOOT PRE-STEP (stock recipe) ===");
    lg("0x402b00cc pre: (bank2 clears CM4 force-deep-sleep bit21)");
    WRf(0x402b20cc, 0x200000);
    /* 8-byte boot vector {sp=0x200, pc=0x01020001} -> CM4 IRAM 0x50800000
     * (fork-isolated: CM4 bus may be dead) */
    uint32_t pv;
    if (fa2("wr 0x50800000", 0x50800000, 1, 0x00000200, 0, &pv) != 0 ||
        fa2("wr 0x50800004", 0x50800004, 1, 0x01020001, 0, &pv) != 0) {
        lg("CM4 IRAM write failed (bus dead) - stopping cm4boot");
        return;
    }
    lg("CM4 IRAM 0x50800000 <- {sp=0x200, pc=0x01020001}");
    WRf(0x402b20b0, 0x100);       /* clear BT system reset bit8 */
    WRf(0x402e2114, 0x10);        /* clear CM4 system reset bit4 */
    WRf(0x402e2114, 0x1);         /* clear CM4 core reset bit0 */
    lg("CM4 released; domain status:");
    if (RDf(0x402b00c0, &v) == 0)
        lg("c0=%08X CM4[19:15]=%u %s", v, (v >> 15) & 0x1f,
           ((v >> 15) & 0x1f) != 7 ? "*** CM4 DOMAIN MOVED ***" : "(still 7=off)");
}

int main(int argc, char **argv)
{
    t0 = now_s();
    const char *mode = (argc > 1) ? argv[1] : "";
    maps_init();
    lg("whale %s start", mode);

    if (!strcmp(mode, "adi_baseline")) { adi_baseline(); return 0; }
    if (!strcmp(mode, "adi")) {
        if (argc < 4) { lg("usage: whale adi RD <hexaddr> | whale adi WR <hexaddr> <hexval>"); return 2; }
        unsigned long reg = strtoul(argv[3], NULL, 16);
        if (reg < 0x40600000UL || reg >= 0x40610000UL) {
            lg("addr 0x%08lX outside ADI window 0x40600000-0x4060FFFF", reg);
            return 2;
        }
        if (!strcmp(argv[2], "RD")) {
            uint32_t v;
            if (adi_read(reg, &v, 1000) == 0)
                lg("ADI RD 0x%08lX = %04X (full %08X)", reg, v & 0xFFFFu, v);
            else
                lg("ADI RD 0x%08lX: TIMEOUT", reg);
        } else if (!strcmp(argv[2], "WR")) {
            if (argc < 4) { lg("usage: whale adi WR <hexaddr> <hexval>"); return 2; }
            uint32_t val = (uint32_t)strtoul(argv[3], NULL, 16), rb;
            if (adi_write(reg, val, 1000) == 0) {
                lg("ADI WR 0x%08lX = %04X stored", reg, val & 0xFFFFu);
                if (adi_read(reg, &rb, 1000) == 0)
                    lg("ADI RB 0x%08lX = %04X", reg, rb & 0xFFFFu);
                else
                    lg("ADI RB 0x%08lX: TIMEOUT", reg);
            } else
                lg("ADI WR 0x%08lX: FIFO TIMEOUT", reg);
        } else {
            lg("unknown ADI op '%s' (RD|WR)", argv[2]);
            return 2;
        }
        return 0;
    }
    if (!strcmp(mode, "adi_enable")) {
        const char *ok = getenv("SESSION_B_OK");
        if (!ok || strcmp(ok, "1") != 0) {
            lg("REFUSED: adi_enable requires env SESSION_B_OK=1");
            return 1;
        }
        adi_enable();
        return 0;
    }
    if (!strcmp(mode, "whale_request")) { whale_request(); return 0; }
    if (!strcmp(mode, "dspload")) {
        dspload_mode(argc > 2 ? argv[2] : "/tmp/fw_gsm.bin",
                     argc > 3 ? atoi(argv[3]) : 60);
        return 0;
    }
    if (!strcmp(mode, "adisweep") && argc >= 3) {
        unsigned long start = strtoul(argv[2], NULL, 16);
        unsigned count = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 16;
        unsigned stride = (argc > 4) ? (unsigned)strtoul(argv[4], NULL, 0) : 4;

        adisweep_mode(start, count, stride);
        return 0;
    }
    if (!strcmp(mode, "a4031")) { a4031_mode(); return 0; }
    if (!strcmp(mode, "s3")) { s3_mode(); return 0; }
    if (!strcmp(mode, "radio_on")) { radio_on_mode(); return 0; }
    if (!strcmp(mode, "cm4boot")) { cm4boot_mode(); return 0; }
    if (!strcmp(mode, "mmio") && argc >= 4) {
        unsigned long a2 = strtoul(argv[3], NULL, 16);
        uint32_t v2;
        if (!strcmp(argv[2], "RD")) {
            if (RDf(a2, &v2) == 0) lg("MMIO RD 0x%08lX = %08X", a2, v2);
        } else if (!strcmp(argv[2], "WR") && argc >= 5) {
            WRf(a2, (uint32_t)strtoul(argv[4], NULL, 16));
        } else { lg("usage: whale mmio RD <addr> | WR <addr> <val>"); return 2; }
        return 0;
    }
    if (!strcmp(mode, "probe") && argc >= 3) {
        unsigned long a3 = strtoul(argv[2], NULL, 16);
        uint32_t v3;
        if (fa2("probe", a3, 0, 0, 0, &v3) == 0)
            lg("PROBE 0x%08lX = %08X (ALIVE)", a3, v3);
        return 0;
    }
    lg("usage: whale adi_baseline|adi_enable|whale_request|dspload <fw> [sec]|"
       "adi RD|WR <addr> [val]|adisweep <start_hex> [count] [stride]|"
       "a4031|cm4boot|mmio RD|WR <addr> [val]|probe <addr>");
    return 2;
}
