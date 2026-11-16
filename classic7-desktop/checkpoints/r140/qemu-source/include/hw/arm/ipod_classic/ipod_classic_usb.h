#ifndef IPOD_CLASSIC_USB_H
#define IPOD_CLASSIC_USB_H
#include "hw/sysbus.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_core.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_stage.h"
#define TYPE_IPOD_CLASSIC_USB "s5l8702-usb-device-core"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicUSBState, IPOD_CLASSIC_USB)
struct IPodClassicUSBState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    IPodUSBRegisters regs;
    IPodUSBInStage in_stage; /* Ephemeral, never migrated or restored. */
    uint64_t next_in_id;
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
#endif
