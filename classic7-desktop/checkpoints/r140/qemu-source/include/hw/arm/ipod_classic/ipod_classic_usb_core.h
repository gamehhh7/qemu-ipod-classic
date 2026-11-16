/* Experimental device-only DWC global/endpoint-register subset.
 * Rockbox 616989f6 usb-designware.h/c; generic DWC bit definitions from
 * QEMU dwc2-regs.h. Single SETUP buffer-DMA receive only; no FIFO, IN/status,
 * descriptor response, ACK, ID or enumeration synthesized.
 * Shared production implementation: component tests include this exact file.
 */
#ifndef IPOD_USB_CORE_H
#define IPOD_USB_CORE_H
#include <stdint.h>
#include <stdbool.h>
#define IPOD_DWC_USBRST (1u << 12)
#define IPOD_DWC_OTGINT (1u << 2)
#define IPOD_DWC_SESEND (1u << 2)
#define IPOD_DWC_IEPINT (1u << 18)
#define IPOD_DWC_OEPINT (1u << 19)
#define IPOD_DWC_SUPPORTED (IPOD_DWC_USBRST | IPOD_DWC_OTGINT | \
                            IPOD_DWC_IEPINT | IPOD_DWC_OEPINT)
/* Architectural register-bank capacity, not an S5L endpoint-capability ID. */
#define IPOD_DWC_EP_SLOTS 16
/* Only XFRC, EPDISD, AHBERR and IN timeout / OUT SETUP are modeled flags.
 * Only the single-SETUP receiver below produces STUP. */
#define IPOD_DWC_EP_EVENTS 0x0fu
typedef struct IPodUSBRegisters {
    uint32_t gahbcfg, gusbcfg, gintmsk, pending, gotgint;
    bool vbus, reset_line;
    uint32_t dcfg, diepmsk, doepmsk, daintmsk;
    uint32_t dctl, pcgcctl;
    bool in_nak_requested, out_nak_requested;
    uint32_t in_ctl[IPOD_DWC_EP_SLOTS], out_ctl[IPOD_DWC_EP_SLOTS];
    uint32_t in_int[IPOD_DWC_EP_SLOTS], out_int[IPOD_DWC_EP_SLOTS];
    uint32_t in_tsiz[IPOD_DWC_EP_SLOTS], out_tsiz[IPOD_DWC_EP_SLOTS];
    uint32_t in_dma[IPOD_DWC_EP_SLOTS], out_dma[IPOD_DWC_EP_SLOTS];
} IPodUSBRegisters;
/* Diagnostic host ingress result, not a USB handshake/URB completion. */
typedef enum IPodUSBSetupResult {
    IPOD_USB_SETUP_OK,
    IPOD_USB_SETUP_NO_VBUS,
    IPOD_USB_SETUP_HOST_MODE,
    IPOD_USB_SETUP_RESET_ACTIVE,
    IPOD_USB_SETUP_DISCONNECTED,
    IPOD_USB_SETUP_CLOCK_GATED,
    IPOD_USB_SETUP_DMA_DISABLED,
    IPOD_USB_SETUP_DESCRIPTOR_DMA,
    IPOD_USB_SETUP_NOT_ARMED,
    IPOD_USB_SETUP_HALTED,
    IPOD_USB_SETUP_PENDING,
    IPOD_USB_SETUP_SIZE,
    IPOD_USB_SETUP_DMA_REJECTED,
} IPodUSBSetupResult;
static inline const char *ipod_usb_setup_result_name(IPodUSBSetupResult result)
{
    switch (result) {
    case IPOD_USB_SETUP_OK: return "accepted-setup-only";
    case IPOD_USB_SETUP_NO_VBUS: return "no-vbus";
    case IPOD_USB_SETUP_HOST_MODE: return "host-mode";
    case IPOD_USB_SETUP_RESET_ACTIVE: return "reset-active";
    case IPOD_USB_SETUP_DISCONNECTED: return "soft-disconnected";
    case IPOD_USB_SETUP_CLOCK_GATED: return "clock-gated-policy";
    case IPOD_USB_SETUP_DMA_DISABLED: return "buffer-dma-disabled";
    case IPOD_USB_SETUP_DESCRIPTOR_DMA: return "descriptor-dma-unsupported";
    case IPOD_USB_SETUP_NOT_ARMED: return "ep0-out-not-armed";
    case IPOD_USB_SETUP_HALTED: return "halt-recovery-unsupported";
    case IPOD_USB_SETUP_PENDING: return "setup-already-pending";
    case IPOD_USB_SETUP_SIZE: return "setup-buffer-not-loaded";
    case IPOD_USB_SETUP_DMA_REJECTED: return "dma-ram-span-rejected";
    }
    return "invalid-result";
}
/* Callback must preflight the entire span and either copy all 8 bytes to
 * writable plain system RAM or return false without any memory side effect. */
typedef bool (*IPodUSBSetupDMA)(void *opaque, uint32_t bus_addr,
                                const uint8_t setup[8]);
static inline IPodUSBSetupResult ipod_usb_receive_setup(
    IPodUSBRegisters *r, const uint8_t setup[8], IPodUSBSetupDMA dma, void *opaque)
{
    uint32_t size = r->out_tsiz[0];
    unsigned count = (size >> 29) & 3;
    if (!r->vbus) { return IPOD_USB_SETUP_NO_VBUS; }
    if (r->gusbcfg & (1u << 29)) { return IPOD_USB_SETUP_HOST_MODE; }
    if (r->reset_line) { return IPOD_USB_SETUP_RESET_ACTIVE; }
    if (r->dctl & 2) { return IPOD_USB_SETUP_DISCONNECTED; }
    /* Conservative partial-model gate; exact S5L clock behavior unproven. */
    if (r->pcgcctl & 3) { return IPOD_USB_SETUP_CLOCK_GATED; }
    if (!(r->gahbcfg & (1u << 5))) { return IPOD_USB_SETUP_DMA_DISABLED; }
    if (r->dcfg & (1u << 23)) { return IPOD_USB_SETUP_DESCRIPTOR_DMA; }
    if (!(r->out_ctl[0] & (1u << 31))) { return IPOD_USB_SETUP_NOT_ARMED; }
    if (r->out_ctl[0] & (1u << 21)) { return IPOD_USB_SETUP_HALTED; }
    if (r->out_int[0] & 8) { return IPOD_USB_SETUP_PENDING; }
    if (!count || !(size & (1u << 19)) || (size & 0x7f) < 8) {
        return IPOD_USB_SETUP_SIZE;
    }
    /* NAK and interrupt masks deliberately do not gate SETUP reception. */
    if (!dma || !dma(opaque, r->out_dma[0], setup)) {
        return IPOD_USB_SETUP_DMA_REJECTED;
    }
    /* Single-SETUP buffer-DMA policy, not asserted silicon timing: consume
     * one packet, end this receive, residual = loaded - 8. STUP is the only
     * new event; omitting additional XFRC is policy, not a HW prohibition. */
    r->out_tsiz[0] = (size & ~((3u << 29) | (1u << 19) | 0x7fu)) |
                     ((count - 1) << 29) | ((size & 0x7f) - 8);
    r->out_ctl[0] &= ~(1u << 31);
    r->out_int[0] |= 8;
    return IPOD_USB_SETUP_OK;
}
static inline void ipod_usb_ep_regs_reset(IPodUSBRegisters *r)
{
    r->dcfg = r->diepmsk = r->doepmsk = r->daintmsk = 0;
    for (unsigned i = 0; i < IPOD_DWC_EP_SLOTS; i++) {
        r->in_ctl[i] = r->out_ctl[i] = r->in_int[i] = r->out_int[i] = 0;
        r->in_tsiz[i] = r->out_tsiz[i] = r->in_dma[i] = r->out_dma[i] = 0;
    }
}
/* Partial-model empty defaults; silicon reset values are not asserted. */
static inline void ipod_usb_control_regs_reset(IPodUSBRegisters *r)
{
    r->dctl = r->pcgcctl = 0;
    r->in_nak_requested = r->out_nak_requested = false;
}
static inline bool ipod_usb_dctl_write(IPodUSBRegisters *r, uint32_t value)
{
    /* Linux v6.12 hw.h DCTL / Rockbox 616989f6 usb-designware.h.
     * Requested NAK is not effective NAK: no packet engine exists yet.
     * Command-read0 and atomic rejection of conflicting/unsupported commands
     * are fail-closed partial-model policy, not silicon access assertions. */
    const uint32_t rw = 0x802, commands = 0x780, ro = 0xc;
    if ((value & ~(rw | commands | ro)) ||
        (value & 0x180) == 0x180 || (value & 0x600) == 0x600) {
        return false;
    }
    r->dctl = value & rw;
    if (value & 0x80) { r->in_nak_requested = true; }
    if (value & 0x100) { r->in_nak_requested = false; }
    if (value & 0x200) { r->out_nak_requested = true; }
    if (value & 0x400) { r->out_nak_requested = false; }
    return true;
}
static inline void ipod_usb_regs_reset(IPodUSBRegisters *r)
{
    /* Preserve physical input levels; no packet is generated by reset. */
    r->gahbcfg = r->gusbcfg = r->gintmsk = r->pending = r->gotgint = 0;
    ipod_usb_ep_regs_reset(r);
    ipod_usb_control_regs_reset(r);
}
static inline uint32_t ipod_usb_daint(const IPodUSBRegisters *r)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < IPOD_DWC_EP_SLOTS; i++) {
        if (r->in_int[i] & r->diepmsk & IPOD_DWC_EP_EVENTS) {
            value |= 1u << i;
        }
        if (r->out_int[i] & r->doepmsk & IPOD_DWC_EP_EVENTS) {
            value |= 1u << (i + 16);
        }
    }
    return value;
}
static inline uint32_t ipod_usb_status(const IPodUSBRegisters *r)
{
    uint32_t daint = ipod_usb_daint(r) & r->daintmsk;
    return r->pending | (r->gotgint ? IPOD_DWC_OTGINT : 0) |
           ((daint & 0xffff) ? IPOD_DWC_IEPINT : 0) |
           ((daint >> 16) ? IPOD_DWC_OEPINT : 0);
}
static inline bool ipod_usb_irq(const IPodUSBRegisters *r)
{
    return (r->gahbcfg & 1) && (ipod_usb_status(r) & r->gintmsk);
}
static inline void ipod_usb_vbus(IPodUSBRegisters *r, bool value)
{
    if (r->vbus == value) { return; }
    if (r->vbus && !value) { r->gotgint |= IPOD_DWC_SESEND; }
    r->vbus = value;
}
static inline bool ipod_usb_bus_reset(IPodUSBRegisters *r, bool value)
{
    bool event = value && !r->reset_line && r->vbus &&
                 !(r->gusbcfg & (1u << 29)); /* force-host unsupported */
    r->reset_line = value;
    if (event) { r->pending |= IPOD_DWC_USBRST; }
    return event;
}
static inline bool ipod_usb_ep_offset(uint32_t off, bool *in,
                                      unsigned *ep, unsigned *reg)
{
    if (off < 0x900 || off >= 0xd00 || (off & 3)) { return false; }
    *in = off < 0xb00;
    unsigned relative = off - (*in ? 0x900 : 0xb00);
    *ep = relative / 0x20;
    *reg = relative % 0x20;
    return true;
}
static inline bool ipod_usb_ep_ctl_write(uint32_t *ctl, uint32_t value,
                                         unsigned ep, bool in)
{
    /* Linux v6.12 hw.h DXEPCTL, pinned Rockbox usb-designware.h.
     * Commands update only intent/NAK/PID state; no completion is invented. */
    const uint32_t ena = 1u << 31, dis = 1u << 30;
    const uint32_t d1 = 1u << 29, d0 = 1u << 28;
    const uint32_t snak = 1u << 27, cnak = 1u << 26;
    const uint32_t nak = 1u << 17, pid = ep ? 1u << 16 : 0;
    uint32_t type = (value >> 18) & 3;
    if ((!ep && type) || type == 1 ||
        (value & (ena | dis)) == (ena | dis) ||
        (value & (snak | cnak)) == (snak | cnak) ||
        (ep && (value & (d0 | d1)) == (d0 | d1))) { return false; }
    uint32_t config = (1u << 21) | (3u << 18) | (1u << 15) |
                      (ep ? 0x7ffu : 3u);
    if (in) { config |= (15u << 22) | (15u << 11); }
    else { config |= 1u << 20; } /* OUT snoop configuration */
    uint32_t next = (value & config) | (*ctl & (ena | nak | pid));
    if (value & ena) { next |= ena; }
    if (value & dis) { next &= ~ena; }
    if (value & snak) { next |= nak; }
    if (value & cnak) { next &= ~nak; }
    /* SETD0/SETD1 and DPID are reserved for EP0: neither commands nor state. */
    if (ep && (value & d1)) { next |= pid; }
    if (ep && (value & d0)) { next &= ~pid; }
    *ctl = next;
    return true;
}
static inline bool ipod_usb_read(const IPodUSBRegisters *r, uint32_t off,
                                 uint32_t *value)
{
    bool in; unsigned ep, reg;
    if (ipod_usb_ep_offset(off, &in, &ep, &reg)) {
        switch (reg) {
        case 0x00: *value = in ? r->in_ctl[ep] : r->out_ctl[ep]; break;
        case 0x08: *value = (in ? r->in_int[ep] : r->out_int[ep]) & IPOD_DWC_EP_EVENTS; break;
        case 0x10: *value = in ? r->in_tsiz[ep] : r->out_tsiz[ep]; break;
        case 0x14: *value = in ? r->in_dma[ep] : r->out_dma[ep]; break;
        default: return false;
        }
        return true;
    }
    switch (off) {
    case 0x00: *value = (1u << 16) | (r->vbus ? 1u << 19 : 0); break;
    case 0x04: *value = r->gotgint; break;
    case 0x08: *value = r->gahbcfg; break;
    case 0x0c: *value = r->gusbcfg; break;
    case 0x10: *value = 1u << 31; break; /* synchronous DMA has no queued work */
    case 0x14: *value = ipod_usb_status(r); break; /* CMOD=0: device only */
    case 0x18: *value = r->gintmsk; break;
    case 0x800: *value = r->dcfg; break;
    case 0x804: *value = r->dctl; break; /* no NAK-effective state */
    case 0xe00: *value = r->pcgcctl; break;
    case 0x810: *value = r->diepmsk; break;
    case 0x814: *value = r->doepmsk; break;
    case 0x818: *value = ipod_usb_daint(r); break;
    case 0x81c: *value = r->daintmsk; break;
    default: return false;
    }
    return true;
}
static inline bool ipod_usb_write(IPodUSBRegisters *r, uint32_t off,
                                  uint32_t value)
{
    bool in; unsigned ep, reg;
    if (ipod_usb_ep_offset(off, &in, &ep, &reg)) {
        switch (reg) {
        case 0x00: return ipod_usb_ep_ctl_write(in ? &r->in_ctl[ep] : &r->out_ctl[ep], value, ep, in);
        case 0x08:
            if (in) { r->in_int[ep] &= ~(value & IPOD_DWC_EP_EVENTS); }
            else { r->out_int[ep] &= ~(value & IPOD_DWC_EP_EVENTS); }
            break;
        case 0x10: {
            uint32_t mask = ep ? (in ? 0x7fffffffu : 0x1fffffffu) :
                               (in ? 0x0018007fu : 0x6008007fu);
            if (in) { r->in_tsiz[ep] = value & mask; }
            else { r->out_tsiz[ep] = value & mask; }
            break;
        }
        case 0x14:
            /* Programmed bus address only: no host pointer or DMA access. */
            if (in) { r->in_dma[ep] = value; }
            else { r->out_dma[ep] = value; }
            break;
        default: return false;
        }
        return true;
    }
    switch (off) {
    case 0x04: r->gotgint &= ~(value & IPOD_DWC_SESEND); break;
    case 0x08: r->gahbcfg = value & 0x1ff; break;
    case 0x0c: r->gusbcfg = value & 0x60003cff; break;
    case 0x10:
        /* CSRST resets only this empty modeled core, completes synchronously.
         * RX/TX FIFO flush commands intentionally unsupported, not ACKed. */
        if (value & 1) { ipod_usb_regs_reset(r); }
        else if (value) { return false; }
        break;
    case 0x14: r->pending &= ~(value & IPOD_DWC_USBRST); break;
    case 0x18: r->gintmsk = value & IPOD_DWC_SUPPORTED; break;
    /* Retain descriptor-DMA intent so the buffer-only receiver rejects it. */
    case 0x800: r->dcfg = value & ((1u << 23) | (31u << 18) | (3u << 11) | (127u << 4) | 7u); break;
    case 0x804: return ipod_usb_dctl_write(r, value);
    case 0xe00:
        /* STOPPCLK/GATEHCLK intent only. MMIO and physical reset detection
         * remain accessible; no clock/wake/packet engine is claimed. */
        if (value & ~3u) { return false; }
        r->pcgcctl = value;
        break;
    case 0x810: r->diepmsk = value & IPOD_DWC_EP_EVENTS; break;
    case 0x814: r->doepmsk = value & IPOD_DWC_EP_EVENTS; break;
    case 0x81c: r->daintmsk = value; break;
    default: return false;
    }
    return true;
}
#endif
