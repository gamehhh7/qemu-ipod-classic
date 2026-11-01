#ifndef IPOD_MMIO_TRACE_POLICY_H
#define IPOD_MMIO_TRACE_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* Observer bookkeeping only; never part of guest-visible register state. */
enum { IPOD_TRACE_PHY, IPOD_TRACE_CORE };
enum { IPOD_TRACE_DROP, IPOD_TRACE_SAMPLE, IPOD_TRACE_LIMIT };
enum { IPOD_TRACE_KNOWN_BUCKETS = 21, IPOD_TRACE_BUCKETS = 22 };
typedef struct IPodMMIOTracePolicy {
    unsigned counts[IPOD_TRACE_BUCKETS][2];
    uint64_t sequence;
} IPodMMIOTracePolicy;

static inline unsigned ipod_mmio_trace_bucket(unsigned region, uint64_t off)
{
    static const unsigned phy[] = {0, 4, 8, 0x18, 0x28, 0x30, 0x40, 0x44, 0x100};
    static const unsigned core[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x440,
                                    0x800, 0x804, 0xe00, 0x838};
    const unsigned *table = region == IPOD_TRACE_PHY ? phy : core;
    unsigned len = region == IPOD_TRACE_PHY ? 9 : 12;
    unsigned base = region == IPOD_TRACE_PHY ? 0 : 9;
    if (region > IPOD_TRACE_CORE) {
        return IPOD_TRACE_KNOWN_BUCKETS;
    }
    for (unsigned i = 0; i < len; ++i) {
        if (off == table[i]) {
            return base + i;
        }
    }
    return IPOD_TRACE_KNOWN_BUCKETS;
}

static inline unsigned ipod_mmio_trace_decide(IPodMMIOTracePolicy *p,
                                             unsigned region, uint64_t off,
                                             bool write)
{
    unsigned bucket = ipod_mmio_trace_bucket(region, off);
    unsigned cap = bucket == IPOD_TRACE_KNOWN_BUCKETS ? 16 : 32;
    unsigned *count = &p->counts[bucket][!!write];
    if (*count > cap) {
        return IPOD_TRACE_DROP;
    }
    ++*count;
    ++p->sequence;
    return *count == cap + 1 ? IPOD_TRACE_LIMIT : IPOD_TRACE_SAMPLE;
}
#endif
