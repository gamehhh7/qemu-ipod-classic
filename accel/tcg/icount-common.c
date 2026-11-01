/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "hw/core/cpu.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpu-throttle.h"
#include "sysemu/cpu-timers-internal.h"

/*
 * ICOUNT: Instruction Counter
 *
 * this module is split off from cpu-timers because the icount part
 * is TCG-specific, and does not need to be built for other accels.
 */
static bool icount_sleep = true;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10

/*
 * 0 = Do not count executed instructions.
 * 1 = Fixed conversion of insn to ns via "shift" option
 * 2 = Runtime adaptive algorithm to compute shift
 */
int use_icount;

static void icount_enable_precise(void)
{
    use_icount = 1;
}

static void icount_enable_adaptive(void)
{
    use_icount = 2;
}

/*
 * The current number of executed instructions is based on what we
 * originally budgeted minus the current state of the decrementing
 * icount counters in extra/u16.low.
 */
static int64_t icount_get_executed(CPUState *cpu)
{
    return (cpu->icount_budget -
            (cpu->neg.icount_decr.u16.low + cpu->icount_extra));
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
static void icount_update_locked(CPUState *cpu)
{
    int64_t executed = icount_get_executed(cpu);
    cpu->icount_budget -= executed;

    qatomic_set_i64(&timers_state.qemu_icount,
                    timers_state.qemu_icount + executed);
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    icount_update_locked(cpu);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static int64_t icount_get_raw_locked(void)
{
    CPUState *cpu = current_cpu;

    if (cpu && cpu->running) {
        if (!cpu->neg.can_do_io) {
            error_report("Bad icount read");
            exit(1);
        }
        /* Take into account what has run */
        icount_update_locked(cpu);
    }
    /* The read is protected by the seqlock, but needs atomic64 to avoid UB */
    return qatomic_read_i64(&timers_state.qemu_icount);
}

static int64_t icount_get_locked(void)
{
    int64_t icount = icount_get_raw_locked();
    return qatomic_read_i64(&timers_state.qemu_icount_bias) +
        icount_to_ns(icount);
}

int64_t icount_get_raw(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = icount_get_raw_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

/* Return the virtual CPU time, based on the instruction counter.  */
int64_t icount_get(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = icount_get_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

int64_t icount_to_ns(int64_t icount)
{
    return icount << qatomic_read(&timers_state.icount_time_shift);
}

/*
 * Correlation between real and virtual time is always going to be
 * fairly approximate, so ignore small variation.
 * When the guest is idle real and virtual time will be aligned in
 * the IO wait loop.
 */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;

    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running()) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    cur_time = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                   cpu_get_clock_locked());
    cur_icount = icount_get_locked();

    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && timers_state.last_delta + ICOUNT_WOBBLE < delta * 2
        && timers_state.icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift - 1);
    }
    if (delta < 0
        && timers_state.last_delta - ICOUNT_WOBBLE > delta * 2
        && timers_state.icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift + 1);
    }
    timers_state.last_delta = delta;
    qatomic_set_i64(&timers_state.qemu_icount_bias,
                    cur_icount - (timers_state.qemu_icount
                                  << timers_state.icount_time_shift));
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static void icount_adjust_rt(void *opaque)
{
    timer_mod(timers_state.icount_rt_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void *opaque)
{
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    icount_adjust();
}

int64_t icount_round(int64_t count)
{
    int shift = qatomic_read(&timers_state.icount_time_shift);
    return (count + (1 << shift) - 1) >> shift;
}

/*
 * Classic7 audio-pacing diagnostic for H-R123-ICOUNT-NONIDLE-GAP.
 *
 * Opt-in only: CLASSIC7_ICOUNT_GAP=1. When the variable is unset or "0"
 * every hook returns after a once-cached check. Sleep stays on by
 * default, fixed shift is unchanged, and this code never writes guest
 * state, vm_clock_warp_start, qemu_icount, or qemu_icount_bias.
 *
 * Host monotonic time since the previous sample is charged to the bucket
 * latched then:
 *   exec    - some CPU is not idle (warp will not arm)
 *   warp    - all CPUs idle and vm_clock_warp_start > 0
 *             (a warp was recorded; fixed-shift leaves the field 0
 *             until then, and -1 means no warp is in progress)
 *   gap     - all CPUs idle and no warp is recorded
 *             (wall neither executing nor inside an idle warp)
 *   stopped - runstate is not running (kept out of the three buckets)
 *
 * About every 5s a single stderr line reports nanoseconds in each bucket
 * since the previous line, plus how often icount_start_warp_timer
 * returned early for !all_cpu_threads_idle(), for no deadline, or took
 * the sleep-disabled bias jump.
 */
#define C7_GAP_REPORT_NS (5 * NANOSECONDS_PER_SECOND)

enum {
    C7_EXEC = 0,
    C7_WARP,
    C7_GAP,
    C7_STOPPED,
    C7_NBUCKET
};

enum {
    C7_EV_NONE = 0,
    C7_EV_NOT_IDLE,
    C7_EV_NO_DEADLINE,
    C7_EV_SLEEP_OFF
};

/* Static storage is unlocked; qemu_spin_init() stores 0. */
static QemuSpin c7_gap_lock;
static int c7_gap_on = -1;
static int c7_latched = C7_STOPPED;
static int c7_have_sample;
static int64_t c7_last_ns;
static int64_t c7_report_ns;
static int64_t c7_ns[C7_NBUCKET];
static uint64_t c7_ret_not_idle;
static uint64_t c7_ret_no_deadline;
static uint64_t c7_sleep_off;

static bool c7_gap_enabled(void)
{
    int v = qatomic_read(&c7_gap_on);

    if (unlikely(v < 0)) {
        const char *e = getenv("CLASSIC7_ICOUNT_GAP");

        /*
         * "1" enables. Unset and "0" stay off. Any other non-empty
         * value also enables so the flag is not spelling-sensitive.
         */
        v = (e && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        qatomic_set(&c7_gap_on, v);
    }
    return v == 1;
}

static int c7_gap_classify(void)
{
    unsigned seq;
    int64_t warp_start;

    if (!runstate_is_running()) {
        return C7_STOPPED;
    }
    /*
     * Not-idle wins over an armed warp. icount_start_warp_timer refuses
     * to arm while any CPU is runnable, so that wall time is "exec"
     * even if a previous warp has not been accounted yet.
     */
    if (!all_cpu_threads_idle()) {
        return C7_EXEC;
    }
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = timers_state.vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));
    /*
     * icount_warp_rt treats only -1 as "no warp". Fixed shift never
     * stores that sentinel up front (the field is BSS 0), and the
     * arming path does not replace 0, so 0 is not a recorded warp.
     * A recorded arming stores the virtual-RT clock, which is > 0.
     */
    return warp_start > 0 ? C7_WARP : C7_GAP;
}

static void c7_gap_report(const int64_t *ns, uint64_t n_not_idle,
                          uint64_t n_no_deadline, uint64_t n_sleep_off)
{
    int64_t wall = ns[C7_EXEC] + ns[C7_WARP] + ns[C7_GAP] + ns[C7_STOPPED];
    int exec_pct = 0;
    int warp_pct = 0;
    int gap_pct = 0;

    if (wall > 0) {
        exec_pct = (int)((ns[C7_EXEC] * 100) / wall);
        warp_pct = (int)((ns[C7_WARP] * 100) / wall);
        gap_pct = (int)((ns[C7_GAP] * 100) / wall);
    }
    /* stderr, not error_report: one greppable line, no monitor prefix. */
    fprintf(stderr,
            "classic7-icount-gap: wall_ns=%" PRId64
            " exec_ns=%" PRId64
            " warp_ns=%" PRId64
            " gap_ns=%" PRId64
            " stopped_ns=%" PRId64
            " exec_pct=%d warp_pct=%d gap_pct=%d"
            " ret_not_idle=%" PRIu64
            " ret_no_deadline=%" PRIu64
            " sleep_off=%" PRIu64
            " shift=%d sleep=%d\n",
            wall,
            ns[C7_EXEC], ns[C7_WARP], ns[C7_GAP], ns[C7_STOPPED],
            exec_pct, warp_pct, gap_pct,
            n_not_idle, n_no_deadline, n_sleep_off,
            (int)qatomic_read(&timers_state.icount_time_shift),
            icount_sleep ? 1 : 0);
    fflush(stderr);
}

/* Charge elapsed host time to the previously latched bucket, then latch. */
static void c7_gap_sample(int event)
{
    int64_t now;
    int64_t delta;
    int bucket;
    int64_t report_ns[C7_NBUCKET];
    uint64_t n_not_idle = 0;
    uint64_t n_no_deadline = 0;
    uint64_t n_sleep_off = 0;
    bool do_report = false;
    int i;

    if (!c7_gap_enabled()) {
        return;
    }

    bucket = c7_gap_classify();
    now = get_clock();

    qemu_spin_lock(&c7_gap_lock);
    /*
     * c7_have_sample, not a zero timestamp: get_clock() can be 0 and
     * must still arm the 5s window.
     */
    if (c7_have_sample) {
        delta = now - c7_last_ns;
        if (delta > 0 && c7_latched >= 0 && c7_latched < C7_NBUCKET) {
            c7_ns[c7_latched] += delta;
        }
    } else {
        c7_report_ns = now;
        c7_have_sample = 1;
    }
    c7_last_ns = now;
    c7_latched = bucket;
    if (event == C7_EV_NOT_IDLE) {
        c7_ret_not_idle++;
    } else if (event == C7_EV_NO_DEADLINE) {
        c7_ret_no_deadline++;
    } else if (event == C7_EV_SLEEP_OFF) {
        c7_sleep_off++;
    }
    if (c7_have_sample && now - c7_report_ns >= C7_GAP_REPORT_NS) {
        for (i = 0; i < C7_NBUCKET; i++) {
            report_ns[i] = c7_ns[i];
            c7_ns[i] = 0;
        }
        n_not_idle = c7_ret_not_idle;
        n_no_deadline = c7_ret_no_deadline;
        n_sleep_off = c7_sleep_off;
        c7_ret_not_idle = 0;
        c7_ret_no_deadline = 0;
        c7_sleep_off = 0;
        c7_report_ns = now;
        do_report = true;
    }
    qemu_spin_unlock(&c7_gap_lock);

    if (do_report) {
        c7_gap_report(report_ns, n_not_idle, n_no_deadline, n_sleep_off);
    }
}

void icount_classic7_gap_idle_wait(void)
{
    c7_gap_sample(C7_EV_NONE);
}

/*
 * Classic7 warp-efficiency diagnostic for H-R124-WARP-UNDERADVANCE.
 *
 * Opt-in: CLASSIC7_ICOUNT_WARP_EFF=1. Unset or "0" returns immediately.
 * Does not change shift, sleep, warp_start, or bias. K1's
 * CLASSIC7_ICOUNT_GAP sampler is untouched.
 *
 * Each sleep=on warp that records a start timestamp is paired with the
 * later icount_warp_rt credit:
 *   deadline_ns     - virtual deadline used when that warp was opened
 *   slept_wall_ns   - host monotonic time from that open until credit
 *   bias_advance_ns - amount actually added to qemu_icount_bias
 *   rt_elapsed_ns   - VIRTUAL_RT advance over the same host window
 * Positive short_sleep (slept_wall - bias) is under-advance versus the
 * host. bias_per_rt_x1000 near 1000 with bias_per_sleep_x1000 below that
 * means VIRTUAL_RT itself lagged the host clock.
 */
#define C7_WEFF_REPORT_NS (5 * NANOSECONDS_PER_SECOND)
#define C7_WEFF_CAP 256

enum {
    C7W_EARLY_STOPPED = 0,
    C7W_EARLY_NOT_IDLE,
    C7W_EARLY_QTEST,
    C7W_EARLY_REPLAY,
    C7W_EARLY_NO_DEADLINE,
    C7W_N_EARLY
};

typedef struct C7WeffSnap {
    uint64_t done;
    int64_t deadline_ns;
    int64_t slept_wall_ns;
    int64_t bias_advance_ns;
    int64_t rt_elapsed_ns;
    uint64_t n_deadline0;
    uint64_t n_sleep_off;
    int64_t sleep_off_bias_ns;
    uint64_t n_arm;
    uint64_t n_arm_set;
    uint64_t n_arm_kept;
    uint64_t n_arm_lost;
    uint64_t n_early[C7W_N_EARLY];
    uint64_t n_account_unpaired;
    int64_t bias_unpaired_ns;
    uint64_t n_bias_from_zero;
    int64_t bias_from_zero_ns;
    uint64_t n_neg_bias;
    uint64_t n_runstate_skip;
    uint64_t n_account_replay_skip;
    uint64_t n_account_stopped;
    uint64_t n_rt_inactive;
    uint64_t n_arm_orphaned;
    int have_short;
    int short_n;
    int64_t dl_min, dl_max, sl_min, sl_max;
    int64_t dl_samples[C7_WEFF_CAP];
    int64_t sl_samples[C7_WEFF_CAP];
} C7WeffSnap;

typedef struct C7Weff {
    QemuSpin lock;
    int on;
    int have_sample;
    int open;
    int have_short;
    int short_n;
    uint32_t rng;
    int64_t report_ns;
    int64_t host0;
    int64_t arm_rt;
    int64_t deadline_pending;
    int64_t deadline_ns;
    int64_t slept_wall_ns;
    int64_t bias_advance_ns;
    int64_t rt_elapsed_ns;
    int64_t sleep_off_bias_ns;
    int64_t bias_unpaired_ns;
    int64_t bias_from_zero_ns;
    int64_t dl_min, dl_max, sl_min, sl_max;
    uint64_t done;
    uint64_t short_seen;
    uint64_t n_deadline0;
    uint64_t n_sleep_off;
    uint64_t n_arm;
    uint64_t n_arm_set;
    uint64_t n_arm_kept;
    uint64_t n_arm_lost;
    uint64_t n_early[C7W_N_EARLY];
    uint64_t n_account_unpaired;
    uint64_t n_bias_from_zero;
    uint64_t n_neg_bias;
    uint64_t n_runstate_skip;
    uint64_t n_account_replay_skip;
    uint64_t n_account_stopped;
    uint64_t n_rt_inactive;
    uint64_t n_arm_orphaned;
    int64_t dl_samples[C7_WEFF_CAP];
    int64_t sl_samples[C7_WEFF_CAP];
} C7Weff;

/* Spin value 0 is unlocked. .on == -1 means the env var is not cached. */
static C7Weff c7_weff = {
    .on = -1,
    .rng = 1,
};

static bool c7_weff_enabled(void)
{
    int v = qatomic_read(&c7_weff.on);

    if (unlikely(v < 0)) {
        const char *e = getenv("CLASSIC7_ICOUNT_WARP_EFF");

        v = (e && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        qatomic_set(&c7_weff.on, v);
    }
    return v == 1;
}

static int64_t c7_weff_x1000(int64_t num, int64_t den)
{
    if (den <= 0) {
        return -1;
    }
    if (num > 0 && num > INT64_MAX / 1000) {
        return INT64_MAX;
    }
    if (num < 0 && num < INT64_MIN / 1000) {
        return INT64_MIN;
    }
    return (num * 1000 + (num >= 0 ? den / 2 : -den / 2)) / den;
}

static int c7_weff_cmp(const void *a, const void *b)
{
    int64_t da = *(const int64_t *)a;
    int64_t db = *(const int64_t *)b;

    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

static int64_t c7_weff_p50(int64_t *v, int n)
{
    if (n <= 0) {
        return 0;
    }
    qsort(v, n, sizeof(int64_t), c7_weff_cmp);
    return v[n / 2];
}

static uint32_t c7_weff_rand(void)
{
    c7_weff.rng = c7_weff.rng * 1103515245u + 12345u;
    return c7_weff.rng >> 1;
}

/* Min/max are exact. p50 is exact up to C7_WEFF_CAP, then a reservoir. */
static void c7_weff_note_short(int64_t short_deadline, int64_t short_sleep)
{
    uint64_t seen;

    if (!c7_weff.have_short) {
        c7_weff.dl_min = c7_weff.dl_max = short_deadline;
        c7_weff.sl_min = c7_weff.sl_max = short_sleep;
        c7_weff.have_short = 1;
    } else {
        if (short_deadline < c7_weff.dl_min) {
            c7_weff.dl_min = short_deadline;
        }
        if (short_deadline > c7_weff.dl_max) {
            c7_weff.dl_max = short_deadline;
        }
        if (short_sleep < c7_weff.sl_min) {
            c7_weff.sl_min = short_sleep;
        }
        if (short_sleep > c7_weff.sl_max) {
            c7_weff.sl_max = short_sleep;
        }
    }
    c7_weff.short_seen++;
    seen = c7_weff.short_seen;
    if (c7_weff.short_n < C7_WEFF_CAP) {
        c7_weff.dl_samples[c7_weff.short_n] = short_deadline;
        c7_weff.sl_samples[c7_weff.short_n] = short_sleep;
        c7_weff.short_n++;
        return;
    }
    if ((c7_weff_rand() % seen) < C7_WEFF_CAP) {
        int idx = (int)(c7_weff_rand() % C7_WEFF_CAP);

        c7_weff.dl_samples[idx] = short_deadline;
        c7_weff.sl_samples[idx] = short_sleep;
    }
}

static bool c7_weff_take_report(int64_t now, C7WeffSnap *snap)
{
    if (!c7_weff.have_sample) {
        c7_weff.have_sample = 1;
        c7_weff.report_ns = now;
        return false;
    }
    if (now - c7_weff.report_ns < C7_WEFF_REPORT_NS) {
        return false;
    }

    snap->done = c7_weff.done;
    snap->deadline_ns = c7_weff.deadline_ns;
    snap->slept_wall_ns = c7_weff.slept_wall_ns;
    snap->bias_advance_ns = c7_weff.bias_advance_ns;
    snap->rt_elapsed_ns = c7_weff.rt_elapsed_ns;
    snap->n_deadline0 = c7_weff.n_deadline0;
    snap->n_sleep_off = c7_weff.n_sleep_off;
    snap->sleep_off_bias_ns = c7_weff.sleep_off_bias_ns;
    snap->n_arm = c7_weff.n_arm;
    snap->n_arm_set = c7_weff.n_arm_set;
    snap->n_arm_kept = c7_weff.n_arm_kept;
    snap->n_arm_lost = c7_weff.n_arm_lost;
    memcpy(snap->n_early, c7_weff.n_early, sizeof(snap->n_early));
    snap->n_account_unpaired = c7_weff.n_account_unpaired;
    snap->bias_unpaired_ns = c7_weff.bias_unpaired_ns;
    snap->n_bias_from_zero = c7_weff.n_bias_from_zero;
    snap->bias_from_zero_ns = c7_weff.bias_from_zero_ns;
    snap->n_neg_bias = c7_weff.n_neg_bias;
    snap->n_runstate_skip = c7_weff.n_runstate_skip;
    snap->n_account_replay_skip = c7_weff.n_account_replay_skip;
    snap->n_account_stopped = c7_weff.n_account_stopped;
    snap->n_rt_inactive = c7_weff.n_rt_inactive;
    snap->n_arm_orphaned = c7_weff.n_arm_orphaned;
    snap->have_short = c7_weff.have_short;
    snap->short_n = c7_weff.short_n;
    snap->dl_min = c7_weff.dl_min;
    snap->dl_max = c7_weff.dl_max;
    snap->sl_min = c7_weff.sl_min;
    snap->sl_max = c7_weff.sl_max;
    if (c7_weff.short_n > 0) {
        memcpy(snap->dl_samples, c7_weff.dl_samples,
               c7_weff.short_n * sizeof(int64_t));
        memcpy(snap->sl_samples, c7_weff.sl_samples,
               c7_weff.short_n * sizeof(int64_t));
    }

    c7_weff.done = 0;
    c7_weff.deadline_ns = 0;
    c7_weff.slept_wall_ns = 0;
    c7_weff.bias_advance_ns = 0;
    c7_weff.rt_elapsed_ns = 0;
    c7_weff.n_deadline0 = 0;
    c7_weff.n_sleep_off = 0;
    c7_weff.sleep_off_bias_ns = 0;
    c7_weff.n_arm = 0;
    c7_weff.n_arm_set = 0;
    c7_weff.n_arm_kept = 0;
    c7_weff.n_arm_lost = 0;
    memset(c7_weff.n_early, 0, sizeof(c7_weff.n_early));
    c7_weff.n_account_unpaired = 0;
    c7_weff.bias_unpaired_ns = 0;
    c7_weff.n_bias_from_zero = 0;
    c7_weff.bias_from_zero_ns = 0;
    c7_weff.n_neg_bias = 0;
    c7_weff.n_runstate_skip = 0;
    c7_weff.n_account_replay_skip = 0;
    c7_weff.n_account_stopped = 0;
    c7_weff.n_rt_inactive = 0;
    c7_weff.n_arm_orphaned = 0;
    c7_weff.have_short = 0;
    c7_weff.short_n = 0;
    c7_weff.short_seen = 0;
    c7_weff.report_ns = now;
    return true;
}

static void c7_weff_report(C7WeffSnap *snap)
{
    int64_t dl_p50 = 0;
    int64_t sl_p50 = 0;

    if (snap->have_short && snap->short_n > 0) {
        dl_p50 = c7_weff_p50(snap->dl_samples, snap->short_n);
        sl_p50 = c7_weff_p50(snap->sl_samples, snap->short_n);
    } else {
        snap->dl_min = snap->dl_max = 0;
        snap->sl_min = snap->sl_max = 0;
    }

    fprintf(stderr,
            "classic7-icount-warp-eff: done=%" PRIu64
            " deadline_ns=%" PRId64
            " slept_wall_ns=%" PRId64
            " bias_advance_ns=%" PRId64
            " rt_elapsed_ns=%" PRId64
            " bias_per_sleep_x1000=%" PRId64
            " bias_per_deadline_x1000=%" PRId64
            " sleep_per_deadline_x1000=%" PRId64
            " bias_per_rt_x1000=%" PRId64
            " n_deadline0=%" PRIu64
            " n_sleep_off=%" PRIu64
            " sleep_off_bias_ns=%" PRId64
            " n_arm=%" PRIu64
            " n_arm_set=%" PRIu64
            " n_arm_kept=%" PRIu64
            " n_arm_lost=%" PRIu64
            " n_early_stopped=%" PRIu64
            " n_early_not_idle=%" PRIu64
            " n_early_qtest=%" PRIu64
            " n_early_replay=%" PRIu64
            " n_early_no_deadline=%" PRIu64
            " n_account_unpaired=%" PRIu64
            " bias_unpaired_ns=%" PRId64
            " n_bias_from_zero=%" PRIu64
            " bias_from_zero_ns=%" PRId64
            " n_neg_bias=%" PRIu64
            " n_runstate_skip=%" PRIu64
            " n_account_replay_skip=%" PRIu64
            " n_account_stopped=%" PRIu64
            " n_rt_inactive=%" PRIu64
            " n_arm_orphaned=%" PRIu64
            " short_deadline_min=%" PRId64
            " short_deadline_p50=%" PRId64
            " short_deadline_max=%" PRId64
            " short_sleep_min=%" PRId64
            " short_sleep_p50=%" PRId64
            " short_sleep_max=%" PRId64
            " p50_n=%d"
            " shift=%d sleep=%d\n",
            snap->done,
            snap->deadline_ns,
            snap->slept_wall_ns,
            snap->bias_advance_ns,
            snap->rt_elapsed_ns,
            c7_weff_x1000(snap->bias_advance_ns, snap->slept_wall_ns),
            c7_weff_x1000(snap->bias_advance_ns, snap->deadline_ns),
            c7_weff_x1000(snap->slept_wall_ns, snap->deadline_ns),
            c7_weff_x1000(snap->bias_advance_ns, snap->rt_elapsed_ns),
            snap->n_deadline0,
            snap->n_sleep_off,
            snap->sleep_off_bias_ns,
            snap->n_arm,
            snap->n_arm_set,
            snap->n_arm_kept,
            snap->n_arm_lost,
            snap->n_early[C7W_EARLY_STOPPED],
            snap->n_early[C7W_EARLY_NOT_IDLE],
            snap->n_early[C7W_EARLY_QTEST],
            snap->n_early[C7W_EARLY_REPLAY],
            snap->n_early[C7W_EARLY_NO_DEADLINE],
            snap->n_account_unpaired,
            snap->bias_unpaired_ns,
            snap->n_bias_from_zero,
            snap->bias_from_zero_ns,
            snap->n_neg_bias,
            snap->n_runstate_skip,
            snap->n_account_replay_skip,
            snap->n_account_stopped,
            snap->n_rt_inactive,
            snap->n_arm_orphaned,
            snap->dl_min,
            dl_p50,
            snap->dl_max,
            snap->sl_min,
            sl_p50,
            snap->sl_max,
            snap->short_n,
            (int)qatomic_read(&timers_state.icount_time_shift),
            icount_sleep ? 1 : 0);
    fflush(stderr);
}

/* Caller holds c7_weff.lock. May release it to print. */
static void c7_weff_maybe_report(int64_t now)
{
    C7WeffSnap snap;
    bool do_report;

    do_report = c7_weff_take_report(now, &snap);
    qemu_spin_unlock(&c7_weff.lock);
    if (do_report) {
        c7_weff_report(&snap);
    }
}

static void c7_weff_event(int which)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    if (which >= 0 && which < C7W_N_EARLY) {
        c7_weff.n_early[which]++;
    }
    c7_weff_maybe_report(now);
}

static void c7_weff_on_deadline0(void)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    c7_weff.n_deadline0++;
    c7_weff_maybe_report(now);
}

static void c7_weff_on_sleep_off(int64_t deadline)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    c7_weff.n_sleep_off++;
    c7_weff.sleep_off_bias_ns += deadline;
    c7_weff_maybe_report(now);
}

static void c7_weff_on_arm(int64_t rt_clock, int64_t deadline, bool refreshed)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    c7_weff.n_arm++;
    if (!c7_weff.open || refreshed) {
        if (c7_weff.open && refreshed) {
            /* warp_start was replaced without an account. */
            c7_weff.n_arm_lost++;
        }
        if (refreshed) {
            c7_weff.n_arm_set++;
        } else {
            c7_weff.n_arm_kept++;
        }
        c7_weff.open = 1;
        c7_weff.host0 = now;
        c7_weff.arm_rt = rt_clock;
        c7_weff.deadline_pending = deadline;
    } else {
        c7_weff.n_arm_kept++;
    }
    c7_weff_maybe_report(now);
}

static void c7_weff_on_account(int64_t rt_now, int64_t warp_start_used,
                               int64_t bias_applied, bool applied)
{
    int64_t now;
    int64_t slept;
    int64_t rt_elapsed;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);

    /*
     * Fixed shift leaves vm_clock_warp_start at 0 until the first
     * account. That credit is clock-0, not a paired sleep.
     */
    if (warp_start_used == 0) {
        c7_weff.n_bias_from_zero++;
        c7_weff.bias_from_zero_ns += bias_applied;
        c7_weff.open = 0;
    } else if (!c7_weff.open) {
        c7_weff.n_account_unpaired++;
        c7_weff.bias_unpaired_ns += bias_applied;
    } else {
        slept = now - c7_weff.host0;
        if (slept < 0) {
            slept = 0;
        }
        rt_elapsed = 0;
        if (applied) {
            rt_elapsed = rt_now - c7_weff.arm_rt;
        }
        c7_weff.done++;
        c7_weff.deadline_ns += c7_weff.deadline_pending;
        c7_weff.slept_wall_ns += slept;
        c7_weff.bias_advance_ns += bias_applied;
        c7_weff.rt_elapsed_ns += rt_elapsed;
        if (!applied) {
            c7_weff.n_runstate_skip++;
        }
        if (bias_applied < 0) {
            c7_weff.n_neg_bias++;
        }
        c7_weff_note_short(c7_weff.deadline_pending - bias_applied,
                           slept - bias_applied);
        c7_weff.open = 0;
    }
    c7_weff_maybe_report(now);
}

static void c7_weff_on_rt_inactive(void)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    c7_weff.n_rt_inactive++;
    if (c7_weff.open) {
        c7_weff.n_arm_orphaned++;
        c7_weff.open = 0;
    }
    c7_weff_maybe_report(now);
}

static void c7_weff_on_account_stopped(void)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    c7_weff.n_account_stopped++;
    c7_weff_maybe_report(now);
}

static void c7_weff_on_account_replay_skip(void)
{
    int64_t now;

    if (!c7_weff_enabled()) {
        return;
    }
    now = get_clock();
    qemu_spin_lock(&c7_weff.lock);
    c7_weff.n_account_replay_skip++;
    c7_weff_maybe_report(now);
}

static void icount_warp_rt(void)
{
    unsigned seq;
    int64_t warp_start;
    int64_t bias_applied = 0;
    int64_t rt_now = 0;
    int64_t warp_used;
    bool applied = false;

    /*
     * The icount_warp_timer is rescheduled soon after vm_clock_warp_start
     * changes from -1 to another value, so the race here is okay.
     */
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = timers_state.vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));

    if (warp_start == -1) {
        c7_gap_sample(C7_EV_NONE);
        c7_weff_on_rt_inactive();
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    warp_used = timers_state.vm_clock_warp_start;
    if (runstate_is_running()) {
        int64_t clock = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                            cpu_get_clock_locked());
        int64_t warp_delta;

        warp_delta = clock - warp_used;
        if (icount_enabled() == 2) {
            /*
             * In adaptive mode, do not let QEMU_CLOCK_VIRTUAL run too far
             * ahead of real time (it might already be ahead so careful not
             * to go backwards).
             */
            int64_t cur_icount = icount_get_locked();
            int64_t delta = clock - cur_icount;

            if (delta < 0) {
                delta = 0;
            }
            warp_delta = MIN(warp_delta, delta);
        }
        qatomic_set_i64(&timers_state.qemu_icount_bias,
                        timers_state.qemu_icount_bias + warp_delta);
        rt_now = clock;
        bias_applied = warp_delta;
        applied = true;
    }
    timers_state.vm_clock_warp_start = -1;
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);

    /* Before notify: a re-entered arm must not pair with this credit. */
    c7_weff_on_account(rt_now, warp_used, bias_applied, applied);

    if (qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
    c7_gap_sample(C7_EV_NONE);
}

static void icount_timer_cb(void *opaque)
{
    /*
     * No need for a checkpoint because the timer already synchronizes
     * with CHECKPOINT_CLOCK_VIRTUAL_RT.
     */
    icount_warp_rt();
}

void icount_start_warp_timer(void)
{
    int64_t clock;
    int64_t deadline;

    assert(icount_enabled());

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        c7_gap_sample(C7_EV_NONE);
        c7_weff_event(C7W_EARLY_STOPPED);
        return;
    }

    if (replay_mode != REPLAY_MODE_PLAY) {
        if (!all_cpu_threads_idle()) {
            c7_gap_sample(C7_EV_NOT_IDLE);
            c7_weff_event(C7W_EARLY_NOT_IDLE);
            return;
        }

        if (qtest_enabled()) {
            /* When testing, qtest commands advance icount.  */
            c7_gap_sample(C7_EV_NONE);
            c7_weff_event(C7W_EARLY_QTEST);
            return;
        }

        replay_checkpoint(CHECKPOINT_CLOCK_WARP_START);
    } else {
        /* warp clock deterministically in record/replay mode */
        if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
            /*
             * vCPU is sleeping and warp can't be started.
             * It is probably a race condition: notification sent
             * to vCPU was processed in advance and vCPU went to sleep.
             * Therefore we have to wake it up for doing something.
             */
            if (replay_has_event()) {
                qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            }
            c7_gap_sample(C7_EV_NONE);
            c7_weff_event(C7W_EARLY_REPLAY);
            return;
        }
    }

    /* We want to use the earliest deadline from ALL vm_clocks */
    clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline < 0) {
        static bool notified;
        if (!icount_sleep && !notified) {
            warn_report("icount sleep disabled and no active timers");
            notified = true;
        }
        c7_gap_sample(C7_EV_NO_DEADLINE);
        c7_weff_event(C7W_EARLY_NO_DEADLINE);
        return;
    }

    if (deadline > 0) {
        /*
         * Ensure QEMU_CLOCK_VIRTUAL proceeds even when the virtual CPU goes to
         * sleep.  Otherwise, the CPU might be waiting for a future timer
         * interrupt to wake it up, but the interrupt never comes because
         * the vCPU isn't running any insns and thus doesn't advance the
         * QEMU_CLOCK_VIRTUAL.
         */
        if (!icount_sleep) {
            /*
             * We never let VCPUs sleep in no sleep icount mode.
             * If there is a pending QEMU_CLOCK_VIRTUAL timer we just advance
             * to the next QEMU_CLOCK_VIRTUAL event and notify it.
             * It is useful when we want a deterministic execution time,
             * isolated from host latencies.
             */
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            qatomic_set_i64(&timers_state.qemu_icount_bias,
                            timers_state.qemu_icount_bias + deadline);
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            c7_gap_sample(C7_EV_SLEEP_OFF);
            c7_weff_on_sleep_off(deadline);
        } else {
            /*
             * We do stop VCPUs and only advance QEMU_CLOCK_VIRTUAL after some
             * "real" time, (related to the time left until the next event) has
             * passed. The QEMU_CLOCK_VIRTUAL_RT clock will do this.
             * This avoids that the warps are visible externally; for example,
             * you will not be sending network packets continuously instead of
             * every 100ms.
             */
            bool refreshed = false;

            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            if (timers_state.vm_clock_warp_start == -1
                || timers_state.vm_clock_warp_start > clock) {
                timers_state.vm_clock_warp_start = clock;
                refreshed = true;
            }
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            timer_mod_anticipate(timers_state.icount_warp_timer,
                                 clock + deadline);
            c7_gap_sample(C7_EV_NONE);
            c7_weff_on_arm(clock, deadline, refreshed);
        }
    } else if (deadline == 0) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        c7_gap_sample(C7_EV_NONE);
        c7_weff_on_deadline0();
    }
}

void icount_account_warp_timer(void)
{
    if (!icount_sleep) {
        c7_gap_sample(C7_EV_NONE);
        return;
    }

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        c7_gap_sample(C7_EV_NONE);
        c7_weff_on_account_stopped();
        return;
    }

    replay_async_events();

    /* warp clock deterministically in record/replay mode */
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_ACCOUNT)) {
        c7_gap_sample(C7_EV_NONE);
        c7_weff_on_account_replay_skip();
        return;
    }

    timer_del(timers_state.icount_warp_timer);
    icount_warp_rt();
}

void icount_configure(QemuOpts *opts, Error **errp)
{
    const char *option = qemu_opt_get(opts, "shift");
    bool sleep = qemu_opt_get_bool(opts, "sleep", true);
    bool align = qemu_opt_get_bool(opts, "align", false);
    long time_shift = -1;

    if (!option) {
        if (qemu_opt_get(opts, "align") != NULL) {
            error_setg(errp, "Please specify shift option when using align");
        }
        return;
    }

    if (align && !sleep) {
        error_setg(errp, "align=on and sleep=off are incompatible");
        return;
    }

    if (strcmp(option, "auto") != 0) {
        if (qemu_strtol(option, NULL, 0, &time_shift) < 0
            || time_shift < 0 || time_shift > MAX_ICOUNT_SHIFT) {
            error_setg(errp, "icount: Invalid shift value");
            return;
        }
    } else if (icount_align_option) {
        error_setg(errp, "shift=auto and align=on are incompatible");
        return;
    } else if (!icount_sleep) {
        error_setg(errp, "shift=auto and sleep=off are incompatible");
        return;
    }

    icount_sleep = sleep;
    if (icount_sleep) {
        timers_state.icount_warp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                         icount_timer_cb, NULL);
    }

    icount_align_option = align;

    if (time_shift >= 0) {
        timers_state.icount_time_shift = time_shift;
        icount_enable_precise();
        return;
    }

    icount_enable_adaptive();

    /*
     * 125MIPS seems a reasonable initial guess at the guest speed.
     * It will be corrected fairly quickly anyway.
     */
    timers_state.icount_time_shift = 3;

    /*
     * Have both realtime and virtual time triggers for speed adjustment.
     * The realtime trigger catches emulated time passing too slowly,
     * the virtual time trigger catches emulated time passing too fast.
     * Realtime triggers occur even when idle, so use them less frequently
     * than VM triggers.
     */
    timers_state.vm_clock_warp_start = -1;
    timers_state.icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT,
                                   icount_adjust_rt, NULL);
    timer_mod(timers_state.icount_rt_timer,
                   qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    timers_state.icount_vm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        icount_adjust_vm, NULL);
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
}

void icount_notify_exit(void)
{
    if (icount_enabled() && current_cpu) {
        qemu_cpu_kick(current_cpu);
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}
