/* Native buffer-DMA bulk peer. No synthetic endpoint arms or device replies. */
#ifndef IPOD_NATIVE_BULK_H
#define IPOD_NATIVE_BULK_H
#include "hw/arm/ipod_classic/ipod_classic_usb_core.h"
#define IPOD_BULK_MAX_PACKET 512
enum { IPOD_BULK_OK=0, IPOD_BULK_WAIT=1, IPOD_BULK_STALL=2,
       IPOD_BULK_STALE=-1, IPOD_BULK_MODE=-2, IPOD_BULK_SIZE=-3,
       IPOD_BULK_DMA=-4, IPOD_BULK_EXHAUSTED=-5 };
typedef struct {
    bool staged;
    uint64_t nonce, context, generation;
    uint32_t ctl, tsiz, dma, interrupt, length;
    uint8_t bytes[IPOD_BULK_MAX_PACKET];
} IPodBulkPacket;
typedef struct {
    uint64_t context, next_nonce;
    uint64_t generation[2][IPOD_DWC_EP_SLOTS];
    bool armed[2][IPOD_DWC_EP_SLOTS], exhausted;
    IPodBulkPacket in[IPOD_DWC_EP_SLOTS];
} IPodNativeBulk;
typedef struct IPodClassicUSBState IPodClassicUSBState;
int ipod_classic_usb_bulk_out(IPodClassicUSBState *,unsigned,const uint8_t *,size_t);
int ipod_classic_usb_bulk_in(IPodClassicUSBState *,unsigned,IPodBulkPacket **);
int ipod_classic_usb_bulk_ack(IPodClassicUSBState *,unsigned,uint64_t);
void ipod_classic_usb_bulk_invalidate(IPodClassicUSBState *);
#endif
