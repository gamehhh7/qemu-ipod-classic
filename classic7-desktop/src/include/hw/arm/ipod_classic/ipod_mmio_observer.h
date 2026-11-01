#ifndef IPOD_MMIO_OBSERVER_H
#define IPOD_MMIO_OBSERVER_H
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/arm/ipod_classic/ipod_mmio_trace_policy.h"

/* Per translation unit, process-lifetime trace budget. No register mutation. */
static IPodMMIOTracePolicy ipod_mmio_observer_policy;

static inline bool ipod_mmio_observer_enabled(void)
{
    const char *s = getenv("IPOD_USB_MMIO_OBSERVER");
    return s && !strcmp(s, "1");
}

static inline void ipod_mmio_observe(unsigned region, hwaddr off, uint64_t value,
                              unsigned size, bool write, bool supported)
{
    unsigned action;
    CPUState *cs;
    uint32_t pc;
    if (!ipod_mmio_observer_enabled()) {
        return;
    }
    action = ipod_mmio_trace_decide(&ipod_mmio_observer_policy, region, off, write);
    if (action == IPOD_TRACE_DROP) {
        return;
    }
    cs = current_cpu;
    pc = cs ? CPU_GET_CLASS(cs)->get_pc(cs) : 0;
    fprintf(stderr, "USBMMIO region=%s seq=%" PRIu64 " kind=%s op=%s"
            " off=%08" HWADDR_PRIx " value=%016" PRIx64
            " size=%u supported=%d pc_approx=%08x cpu_context=%d vns=%" PRId64 "\n",
            region == IPOD_TRACE_PHY ? "phy" : "core",
            ipod_mmio_observer_policy.sequence,
            action == IPOD_TRACE_LIMIT ? "bucket_limit" : "sample",
            write ? "W" : "R", off, value, size, supported, pc, !!cs,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static inline void ipod_mmio_observe_input(const char *kind, bool level, bool accepted)
{
    static unsigned count;
    if (!ipod_mmio_observer_enabled() || count > 64) {
        return;
    }
    ++count;
    fprintf(stderr, "USBMMIO_INPUT kind=%s level=%d accepted=%d count=%u capped=%d vns=%" PRId64 "\n",
            kind, level, accepted, count, count == 65,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
#endif
