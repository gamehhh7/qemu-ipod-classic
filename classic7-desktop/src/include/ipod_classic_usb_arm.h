/* Control-EP0 arming provenance: order-of-events metadata about which
 * accepted controller write events happened after the accepted SETUP. Only
 * defined controller event order is proven; firmware intent and the semantic
 * correctness of response content are NOT proven.
 *
 * First-hand evidence for the modeled enable act (DWC device mode, Rockbox
 * 616989f6 firmware/drivers/usb-designware.c, the revision this device model
 * cites): usb_dw_epstart starts a transfer by writing DIEPDMA, then
 * DIEPTSIZ, then DIEPCTL |= EPENA|CNAK — the enable write is last and its
 * written value has EPENA set. usb_dw_handle_setup_received flushes EP0
 * IN/OUT (CTL writes without EPENA) before the USB stack arms the data
 * stage, so an armed configuration is re-established by a fresh EPENA write
 * after every accepted SETUP.
 */
#ifndef IPOD_CLASSIC_USB_ARM_H
#define IPOD_CLASSIC_USB_ARM_H
#include <stdint.h>
#include <stdbool.h>

typedef struct IPodUSBArmProvenance {
    uint64_t next_seq;   /* last issued provenance event seq; 0 = fresh */
    uint64_t setup_seq;  /* accepted SETUP event seq; 0 = no current transfer */
    uint64_t in_arm_seq; /* valid IN0 enable-act seq; 0 = not armed */
} IPodUSBArmProvenance;

/* Monotonic, non-wrapping seq namespace. 0 = exhausted (fail closed; the
 * namespace is never reused). */
static inline uint64_t ipod_usb_arm_take_seq(IPodUSBArmProvenance *p)
{
    if (p->next_seq == UINT64_MAX) { return 0; }
    return ++p->next_seq;
}

/* Accepted SETUP (IPOD_USB_SETUP_OK): opens the new transfer's provenance
 * window and supersedes any earlier IN0 arming. Returns false only when the
 * seq namespace is exhausted; the caller must then fail closed permanently. */
static inline bool ipod_usb_arm_setup_accepted(IPodUSBArmProvenance *p)
{
    uint64_t seq = ipod_usb_arm_take_seq(p);
    if (!seq) {
        p->setup_seq = 0;
        p->in_arm_seq = 0;
        return false;
    }
    p->setup_seq = seq;
    p->in_arm_seq = 0;
    return true;
}

/* Accepted IN0 DIEPCTL0 write whose written value requests EPENA: the DWC
 * transfer-enable act. */
static inline void ipod_usb_arm_in_act(IPodUSBArmProvenance *p)
{
    p->in_arm_seq = ipod_usb_arm_take_seq(p); /* 0 on exhaustion: disarmed */
}

/* Accepted IN0 programming that is not the enable act (DIEPTSIZ0/DIEPDMA0,
 * or DIEPCTL0 without EPENA): the armed transfer configuration was touched
 * after the enable, so a fresh enable act is required. Explicit contract: a
 * DMA or length change alone is NOT a valid arm. IRQ/endpoint mask writes
 * are not IN0 transfer programming and never reach this function. */
static inline void ipod_usb_arm_in_disqualify(IPodUSBArmProvenance *p)
{
    p->in_arm_seq = 0;
}

/* Bus reset line rise, soft disconnect, VBUS fall, CSRST, core reset,
 * migration post_load, unrealize: no control-transfer provenance survives. */
static inline void ipod_usb_arm_line_drop(IPodUSBArmProvenance *p)
{
    p->setup_seq = 0;
    p->in_arm_seq = 0;
}

/* A fresh IN begin is allowed only from an enable act strictly after the
 * currently accepted SETUP. */
static inline bool ipod_usb_arm_begin_allowed(const IPodUSBArmProvenance *p)
{
    return p->setup_seq && p->in_arm_seq && p->in_arm_seq > p->setup_seq;
}
#endif
