/* Experimental S5L device compatibility hypothesis, not a silicon definition.
 * Pure state: callers supply virtual time and real modeled input/config intent.
 * No packet engine, IRQ, guest address, PC, UI, or DMA access exists here. */
#ifndef IPOD_USB_LINK_V0_H
#define IPOD_USB_LINK_V0_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define IPOD_LINK_SETTLE_NS INT64_C(400000)
typedef struct IPodUSBLinkV0 {
    uint32_t phy[65];
    bool power_written, reset_written, reference_written;
    bool power_known, reset_known, reference_known, ancillary_known;
    bool vbus, soc_clocks, eligible;
    bool core_reset_seen, guest_connected;
    int64_t deadline;
} IPodUSBLinkV0;
static inline void ipod_link_init(IPodUSBLinkV0 *m)
{
    memset(m, 0, sizeof(*m));
    m->ancillary_known = true;
    m->deadline = -1;
}
static inline void ipod_link_update(IPodUSBLinkV0 *m, bool vbus,
                                    bool clocks, int64_t now)
{
    bool eligible;
    m->vbus = vbus; m->soc_clocks = clocks;
    if (!vbus || (m->power_written && m->phy[0] != 0) ||
        (m->reset_written && m->phy[2] != 0)) {
        m->core_reset_seen = m->guest_connected = false;
    }
    eligible = vbus && clocks && m->power_written && m->reset_written &&
        m->reference_written && m->power_known && m->reset_known &&
        m->reference_known && m->ancillary_known &&
        m->phy[0] == 0 && m->phy[2] == 0;
    if (!eligible) { m->deadline = -1; }
    else if (!m->eligible) { m->deadline = now + IPOD_LINK_SETTLE_NS; }
    m->eligible = eligible;
}
static inline bool ipod_link_available(const IPodUSBLinkV0 *m, int64_t now)
{
    return m->eligible && m->deadline >= 0 && now >= m->deadline;
}
/* Unknown ancillary configuration is sticky until model/machine reset.
 * No subsequent unrelated supported write may erase that uncertainty. */
static inline bool ipod_link_phy_write(IPodUSBLinkV0 *m, uint32_t off,
                                        uint32_t value, int64_t now)
{
    bool known = true;
    switch (off) {
    case 0x00:
        m->power_written = true;
        m->power_known = value == 0 || value == 0xf || value == 0x1f;
        known = m->power_known; break;
    case 0x04:
        m->reference_written = true;
        m->reference_known = value == 0;
        known = m->reference_known; break;
    case 0x08:
        m->reset_written = true;
        m->reset_known = value == 0 || value == 1 || value == 7;
        known = m->reset_known; break;
    case 0x18: known = value == 0 || value == 0x400 || value == 0x600; break;
    case 0x1c: known = value == 0 || value == 1; break;
    case 0x30: known = value == 0; break;
    case 0x40: known = !(value & ~UINT32_C(0x347)); break;
    case 0x44: known = !(value & ~UINT32_C(0xfff)); break;
    case 0x100: known = value == 0 || value == 1; break;
    default:
        m->ancillary_known = false;
        ipod_link_update(m, m->vbus, m->soc_clocks, now);
        return false;
    }
    m->phy[off / 4] = value;
    if (off != 0 && off != 4 && off != 8 && !known) {
        m->ancillary_known = false;
    }
    ipod_link_update(m, m->vbus, m->soc_clocks, now);
    return known;
}
static inline bool ipod_link_phy_read(const IPodUSBLinkV0 *m, uint32_t off,
                                       int64_t now, uint32_t *value)
{
    switch (off) {
    case 0x28: *value = ipod_link_available(m, now) ? 1 : 0; return true;
    case 0x00: case 0x04: case 0x08: case 0x18: case 0x1c:
    case 0x30: case 0x40: case 0x44: case 0x100:
        *value = m->phy[off / 4]; return true;
    default: *value = 0; return false;
    }
}
static inline void ipod_link_core_write(IPodUSBLinkV0 *m, uint32_t off,
                                         uint32_t value)
{
    if (off == 0x10 && (value & 1)) {
        m->core_reset_seen = true;
        m->guest_connected = false;
    }
    if (off == 0x804) {
        m->guest_connected = m->core_reset_seen && !(value & 2);
    }
}
/* HPRT bits11:10 here are a hypothetical device-view connection phase.
 * They are explicitly NOT a claim that USB D+/D- carry electrical SE1. */
static inline uint32_t ipod_link_hprt(const IPodUSBLinkV0 *m, int64_t now,
                                      uint32_t dctl, uint32_t pcgcctl,
                                      uint32_t gusbcfg)
{
    (void)pcgcctl; /* Packet clock gating does not disconnect the receiver. */
    return m->core_reset_seen && m->guest_connected &&
        ipod_link_available(m, now) && !(dctl & 2) &&
        !(gusbcfg & (1u << 29)) ? 0xc00 : 0;
}
#endif
