/* Internal staging only. No transport, native DMA adapter, ACK/XFRC/IRQ producer.
 * Caller MUST invalidate generation before ANY re-arm (even identical register
 * values), reset, disconnect or session change. Snapshot equality is insufficient.
 * DMA callback MUST preflight a full direct-readable plain-RAM span (not ROM/MMIO),
 * copy exactly len bytes or reject; fixtures do NOT prove the real DMA adapter.
 * This narrow candidate accepts only CTL=EPENA|USBACTEP, TSIZ=1packet|len,
 * len1..64, no NAK/stall/descriptor-DMA/unknown modes. Not a silicon assertion.
 */
#ifndef IPOD_CLASSIC_USB_STAGE_H
#define IPOD_CLASSIC_USB_STAGE_H
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include "hw/arm/ipod_classic/ipod_classic_usb_core.h"
#define IPOD_USB_IN_STAGE_IN_PID 0x69u
#define IPOD_USB_IN_STAGE_SINGLE_BUFFER 1u
#define IPOD_USB_IN_STAGE_MAX 64u
typedef enum { IPOD_USB_IN_STAGED=1, IPOD_USB_IN_RETRIED, IPOD_USB_IN_CANCELLED, IPOD_USB_IN_UNSUPPORTED,
 IPOD_USB_IN_BAD_ID, IPOD_USB_IN_BAD_OWNER, IPOD_USB_IN_BAD_GENERATION, IPOD_USB_IN_BAD_MODE, IPOD_USB_IN_BAD_SIZE, IPOD_USB_IN_BAD_DMA, IPOD_USB_IN_BUSY, IPOD_USB_IN_STALE,
 /* Appended (no renumbering): no IN0 enable act after the currently accepted
  * SETUP; the caller must hold until firmware re-arms (arm provenance). */
 IPOD_USB_IN_NO_ARM } IPodUSBInResult;
typedef bool (*IPodUSBInDMARead)(void *,uint32_t,uint8_t *,size_t);
typedef struct { uint64_t owner, generation, id; } IPodUSBInStamp;
typedef struct {
 uint64_t owner,generation,last_id,id;
 bool constructed,valid,active;
 size_t len;
 IPodUSBRegisters snapshot;
 uint8_t payload[IPOD_USB_IN_STAGE_MAX];
} IPodUSBInStage;
static inline bool ipod_usb_in_stage_number(uint64_t n) { return n && n!=UINT64_MAX; }
/* Constructor ONLY: caller provides a zero-initialized object, never reuses its
 * storage until every old callback is drained, and allocates a non-repeating
 * owner/session namespace across objects. No random number substitutes for that
 * integration obligation. No API clears constructed, even after poison/reset.
 */
static inline bool ipod_usb_in_stage_init(IPodUSBInStage *s,uint64_t owner,uint64_t generation) {
 if(s->constructed||!ipod_usb_in_stage_number(owner)||!ipod_usb_in_stage_number(generation)) return false;
 memset(s,0,sizeof(*s));
 s->constructed=true;s->owner=owner;s->generation=generation;s->valid=true;return true;
}
static inline void ipod_usb_in_stage_drop(IPodUSBInStage *s) {
 s->active=false;s->id=0;s->len=0;memset(s->payload,0,sizeof(s->payload));
 memset(&s->snapshot,0,sizeof(s->snapshot));
}
/* Invalid generation advancement poisons this session fail-closed. */
static inline IPodUSBInResult ipod_usb_in_stage_invalidate(IPodUSBInStage *s,uint64_t owner,uint64_t next) {
 if(!s->valid) return IPOD_USB_IN_BAD_GENERATION;
 if(owner!=s->owner) return IPOD_USB_IN_BAD_OWNER;
 ipod_usb_in_stage_drop(s);
 if(!ipod_usb_in_stage_number(next)||next<=s->generation) {s->valid=false;return IPOD_USB_IN_BAD_GENERATION;}
 s->generation=next;return IPOD_USB_IN_CANCELLED;
}
static inline bool ipod_usb_in_stage_same(const IPodUSBRegisters *a,const IPodUSBRegisters *b) {
 return a->vbus==b->vbus&&a->reset_line==b->reset_line&&a->gahbcfg==b->gahbcfg&&
 a->gusbcfg==b->gusbcfg&&a->dcfg==b->dcfg&&a->dctl==b->dctl&&a->pcgcctl==b->pcgcctl&&
 a->in_nak_requested==b->in_nak_requested&&a->in_ctl[0]==b->in_ctl[0]&&
 a->in_tsiz[0]==b->in_tsiz[0]&&a->in_dma[0]==b->in_dma[0]&&a->in_int[0]==b->in_int[0];
}
static inline IPodUSBInResult ipod_usb_in_stage_prepare(IPodUSBInStage *s,const IPodUSBRegisters *r,
 uint64_t owner,uint64_t generation,uint64_t id,unsigned pid,unsigned mode,
 uint8_t *output,size_t cap,IPodUSBInDMARead read,void *opaque) {
 size_t len=r->in_tsiz[0]&0x7fu;
 if(!s->valid||generation!=s->generation||!ipod_usb_in_stage_number(generation)) return IPOD_USB_IN_BAD_GENERATION;
 if(owner!=s->owner) return IPOD_USB_IN_BAD_OWNER;
 if(!ipod_usb_in_stage_number(id)) return IPOD_USB_IN_BAD_ID;
 if(pid!=IPOD_USB_IN_STAGE_IN_PID||mode!=IPOD_USB_IN_STAGE_SINGLE_BUFFER||!r->vbus||r->reset_line||
 (r->gusbcfg&(1u<<29))||(r->dctl&2)||r->pcgcctl||!(r->gahbcfg&(1u<<5))||
 (r->dcfg&(1u<<23))||r->in_nak_requested||r->in_int[0]||
 r->in_ctl[0]!=((1u<<31)|(1u<<15))) return IPOD_USB_IN_BAD_MODE;
 if(!len||len>IPOD_USB_IN_STAGE_MAX||r->in_tsiz[0]!=((1u<<19)|len)||!output||cap<len) return IPOD_USB_IN_BAD_SIZE;
 if(!r->in_dma[0]||(r->in_dma[0]&3)||((uint64_t)r->in_dma[0]+len>(1ULL<<32))) return IPOD_USB_IN_BAD_DMA;
 if(s->active) {
  if(id!=s->id) return IPOD_USB_IN_BUSY;
  if(!ipod_usb_in_stage_same(r,&s->snapshot)||len!=s->len) return IPOD_USB_IN_STALE;
  memcpy(output,s->payload,len);return IPOD_USB_IN_RETRIED;
 }
 if(id<=s->last_id) return IPOD_USB_IN_BAD_ID;
 /* Consume a fresh transaction identity even on DMA rejection; never reuse IDs. */
 s->last_id=id;
 if(!read||!read(opaque,r->in_dma[0],s->payload,len)) {ipod_usb_in_stage_drop(s);return IPOD_USB_IN_BAD_DMA;}
 s->id=id;s->len=len;s->snapshot=*r;s->active=true;
 memcpy(output,s->payload,len);return IPOD_USB_IN_STAGED;
}
static inline IPodUSBInResult ipod_usb_in_stage_correlate(const IPodUSBInStage *s,uint64_t owner,uint64_t generation,uint64_t id) {
 if(!s->valid||generation!=s->generation||!ipod_usb_in_stage_number(generation)) return IPOD_USB_IN_BAD_GENERATION;
 if(owner!=s->owner) return IPOD_USB_IN_BAD_OWNER;
 if(!ipod_usb_in_stage_number(id)) return IPOD_USB_IN_BAD_ID;
 if(!s->active||id!=s->id) return IPOD_USB_IN_STALE;
 return IPOD_USB_IN_STAGED;
}
static inline IPodUSBInResult ipod_usb_in_stage_cancel(IPodUSBInStage *s,uint64_t owner,uint64_t generation,uint64_t id) {
 IPodUSBInResult r=ipod_usb_in_stage_correlate(s,owner,generation,id);
 if(r!=IPOD_USB_IN_STAGED) return r;
 ipod_usb_in_stage_drop(s);return IPOD_USB_IN_CANCELLED;
}
/* No backend is attached: even correctly correlated success is NOT committed. */
static inline IPodUSBInResult ipod_usb_in_stage_finish_success(const IPodUSBInStage *s,uint64_t owner,uint64_t generation,uint64_t id) {
 IPodUSBInResult r=ipod_usb_in_stage_correlate(s,owner,generation,id);
 return r==IPOD_USB_IN_STAGED?IPOD_USB_IN_UNSUPPORTED:r;
}
#endif
