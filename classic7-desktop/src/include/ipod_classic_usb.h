#ifndef IPOD_CLASSIC_USB_H
#define IPOD_CLASSIC_USB_H
#include "hw/sysbus.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_core.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_stage.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_arm.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_zlp_pending.h"
#define TYPE_IPOD_CLASSIC_USB "s5l8702-usb-device-core"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicUSBState, IPOD_CLASSIC_USB)
struct IPodClassicUSBState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    IPodUSBRegisters regs;
    IPodUSBInStage in_stage; /* Ephemeral, never migrated or restored. */
    uint64_t next_in_id; /* Single persistent counter for data-IN and ZLP. */
    IPodZLPPending status_in; /* Ephemeral; never migrated or restored. */
    IPodUSBArmProvenance arm; /* Control-EP0 arming provenance; ephemeral. */
    /* Appended metadata only: preexisting field offsets retained. Weak observer
     * opaque is owned/drained by its bridge, which holds a strong controller ref. */
    void (*context_lost)(void *opaque,bool dead);
    void *context_lost_opaque;
    uint64_t context_lost_token,next_context_lost_token;
    bool context_lost_notifying,context_lost_dead;
};
/* Internal transport contract only: no runtime backend/ACK producer is attached.
 * Backend callbacks must hold a QOM reference, run under BQL and be cancelled/
 * drained before unrealize releases their references. That backend is not built. */
IPodUSBInResult ipod_classic_usb_in_begin(IPodClassicUSBState *s, uint8_t *dst,
                                         size_t cap, IPodUSBInStamp *stamp);
IPodUSBInResult ipod_classic_usb_in_retry(IPodClassicUSBState *s,
                                         const IPodUSBInStamp *stamp,
                                         uint8_t *dst, size_t cap);
IPodUSBInResult ipod_classic_usb_in_cancel(IPodClassicUSBState *s,
                                          const IPodUSBInStamp *stamp);
IPodUSBInResult ipod_classic_usb_in_finish(IPodClassicUSBState *s,
                                          const IPodUSBInStamp *stamp);
/* Every accepted SETUP supersedes old EP0 IN staging. Future real ingress must
 * use this event wrapper; DMA callback is internal, never a new QOM parameter. */
IPodUSBSetupResult ipod_classic_usb_setup_event(IPodClassicUSBState *s,
    const uint8_t setup[8], IPodUSBSetupDMA dma, void *opaque);
/* Transport ingress using the existing audited DMA path; BQL only. */
IPodUSBSetupResult ipod_classic_usb_setup_ingress(IPodClassicUSBState *s,
    const uint8_t setup[8]);
size_t ipod_classic_usb_in_staged_len(IPodClassicUSBState *s);
/* Arming provenance epochs (patch 0002; BQL only). setup_epoch: seq of the
 * currently accepted SETUP, 0 = none. in_arm_epoch: seq of the last valid
 * IN0 enable act for that SETUP, 0 = not armed. A staged IN read is
 * authorized only from an enable act strictly after the current SETUP. */
uint64_t ipod_classic_usb_setup_epoch(const IPodClassicUSBState *s);
uint64_t ipod_classic_usb_in_arm_epoch(const IPodClassicUSBState *s);
/* Pending-only status-IN API: no payload, DMA, commit or completion producer.
 * Same BQL/QOM lifetime obligations as data-IN. */
IPodZLPResult ipod_classic_usb_status_in_begin(IPodClassicUSBState *s,
                                             IPodUSBInStamp *stamp);
IPodZLPResult ipod_classic_usb_status_in_retry(IPodClassicUSBState *s,
                                             const IPodUSBInStamp *stamp);
IPodZLPResult ipod_classic_usb_status_in_cancel(IPodClassicUSBState *s,
                                              const IPodUSBInStamp *stamp);
/* Metadata-only abort. False if expected epoch no longer owns the context. */
bool ipod_classic_usb_ep0_abort_epoch(IPodClassicUSBState *s,uint64_t expected);
bool ipod_classic_usb_observer_register(IPodClassicUSBState *s,
    void (*notify)(void *,bool),void *opaque,uint64_t *token);
bool ipod_classic_usb_observer_unregister(IPodClassicUSBState *s,
    void *opaque,uint64_t token);
#endif
