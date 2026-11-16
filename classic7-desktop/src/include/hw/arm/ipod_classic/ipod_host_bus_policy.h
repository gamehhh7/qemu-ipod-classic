#ifndef IPOD_HOST_BUS_POLICY_H
#define IPOD_HOST_BUS_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* Default downstream-hub timing retained. Explicit root-port input uses50ms
 * minimum under USB2.0 section7.1.7.5. No HCD/PHY identity is claimed.
 * USB 2.0 section 7.1.7.5: TDRST >=10ms, TRSTRCY=10ms. Timing is virtual.
 * No pulse is generated, delayed, extended or completed by this policy. */
#define IPOD_HOST_BUS_MIN_RESET_NS INT64_C(10000000)
#define IPOD_HOST_BUS_RECOVERY_NS INT64_C(10000000)
#define IPOD_HOST_BUS_MAX_EVENTS UINT64_C(8) /* max4 explicit pulses per lease */
typedef enum {
    IPOD_HOST_BUS_OK = 0,
    IPOD_HOST_BUS_SHORT = 1, /* actual deassert delivered; invalid short pulse */
    IPOD_HOST_BUS_OWNER = 2,
    IPOD_HOST_BUS_SEQUENCE = 3,
    IPOD_HOST_BUS_STATE = 4,
    IPOD_HOST_BUS_NO_VBUS = 5,
    IPOD_HOST_BUS_HOST_MODE = 6,
    IPOD_HOST_BUS_CLOCK = 7,
} IPodHostBusResult;

typedef struct IPodHostBusPolicy {
    uint64_t last_sequence;
    int64_t asserted_ns, released_ns, last_ns;
    bool asserted, completed;
} IPodHostBusPolicy;

static inline bool ipod_host_bus_ready(const IPodHostBusPolicy *p, int64_t now)
{
    return p->completed && !p->asserted && now >= p->released_ns &&
           now - p->released_ns >= IPOD_HOST_BUS_RECOVERY_NS;
}

/* Apply only after the controller has checked live owner+token+line state.
 * Refusals do not consume sequence. SHORT consumes the actual deassert edge. */
static inline IPodHostBusResult ipod_host_bus_event_min(IPodHostBusPolicy *p,
    uint64_t sequence, bool assert_line, int64_t now, bool vbus, bool host_mode, int64_t minimum_reset_ns)
{
    if (!sequence || p->last_sequence >= IPOD_HOST_BUS_MAX_EVENTS ||
        sequence != p->last_sequence + 1) { return IPOD_HOST_BUS_SEQUENCE; }
    if (now < 0 || now < p->last_ns) { return IPOD_HOST_BUS_CLOCK; }
    if (assert_line == p->asserted) { return IPOD_HOST_BUS_STATE; }
    if (assert_line && !vbus) { return IPOD_HOST_BUS_NO_VBUS; }
    if (assert_line && host_mode) { return IPOD_HOST_BUS_HOST_MODE; }
    p->last_sequence = sequence;
    p->last_ns = now;
    p->asserted = assert_line;
    p->completed = false;
    if (assert_line) {
        p->asserted_ns = now;
        return IPOD_HOST_BUS_OK;
    }
    p->released_ns = now;
    if (now - p->asserted_ns < minimum_reset_ns) {
        return IPOD_HOST_BUS_SHORT;
    }
    p->completed = true;
    return IPOD_HOST_BUS_OK;
}
static inline IPodHostBusResult ipod_host_bus_event(IPodHostBusPolicy *p,
    uint64_t sequence,bool assert_line,int64_t now,bool vbus,bool host_mode)
{
    return ipod_host_bus_event_min(p,sequence,assert_line,now,vbus,host_mode,IPOD_HOST_BUS_MIN_RESET_NS);
}
#endif
