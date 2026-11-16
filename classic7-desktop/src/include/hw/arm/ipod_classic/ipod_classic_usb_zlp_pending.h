/* Additive, synthetic-host prototype only. No commit/finish/ACK/XFRC/IRQ path.
 * Caller owns one serialized data-stage session and one monotonic ID counter.
 * ALL data-IN and ZLP allocations must use that SAME counter. No runtime wiring.
 * Lifecycle callbacks must call explicit invalidate before object drain/reuse.
 */
#ifndef IPOD_CLASSIC_USB_ZLP_PENDING_H
#define IPOD_CLASSIC_USB_ZLP_PENDING_H
#include "hw/arm/ipod_classic/ipod_classic_usb_stage.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_arm.h"

typedef enum { IPOD_ZLP_PENDING=1, IPOD_ZLP_RETRIED, IPOD_ZLP_CANCELLED,
 IPOD_ZLP_BAD_SESSION, IPOD_ZLP_BAD_SETUP, IPOD_ZLP_NO_ARM,
 IPOD_ZLP_BAD_MODE, IPOD_ZLP_BAD_SIZE, IPOD_ZLP_BUSY,
 IPOD_ZLP_STALE, IPOD_ZLP_EXHAUSTED } IPodZLPResult;
typedef struct {
 IPodUSBInStage *data; IPodUSBArmProvenance *arm; uint64_t *shared_next_id;
 bool constructed,bound,active;
 uint8_t setup[8]; uint64_t setup_seq,arm_seq,used_setup_seq;
 IPodUSBInStamp stamp; IPodUSBRegisters snapshot;
} IPodZLPPending;

/* Matches existing adapter's non-wrapping next-ID semantics. Shared by caller's
 * data-IN path too; the latch never has a private ID counter. */
static inline uint64_t ipod_zlp_shared_take_id(uint64_t *next) {
 if(!next||!*next||*next==UINT64_MAX) return 0;
 return (*next)++;
}
static inline bool ipod_zlp_session(const IPodZLPPending *s) {
 return s->constructed&&s->data&&s->arm&&s->shared_next_id&&
 s->data->constructed&&s->data->valid;
}
/* Constructor: IPodZLPPending MUST be zero-initialized before first init.
 * Repeated init is refused. Caller keeps storage alive and drains all callbacks
 * before any reuse; arbitrary uninitialized storage is not a supported input. */
static inline bool ipod_zlp_init(IPodZLPPending *s,IPodUSBInStage *data,
 IPodUSBArmProvenance *arm,uint64_t *shared_next_id) {
 if(s->constructed||!data||!data->constructed||!data->valid||!arm||!shared_next_id)
  return false;
 memset(s,0,sizeof(*s));s->constructed=true;s->data=data;s->arm=arm;
 s->shared_next_id=shared_next_id;return true;
}
static inline void ipod_zlp_drop(IPodZLPPending *s) {
 s->active=false;memset(&s->stamp,0,sizeof(s->stamp));s->arm_seq=0;
 memset(&s->snapshot,0,sizeof(s->snapshot));
}
/* Shared generation invalidates BOTH data and ZLP latches; overflow poisons
 * the existing data session, never resets its owner or allocator namespace. */
static inline bool ipod_zlp_advance(IPodZLPPending *s) {
 ipod_zlp_drop(s);
 if(!ipod_zlp_session(s)) return false;
 return ipod_usb_in_stage_invalidate(s->data,s->data->owner,
  s->data->generation+1)==IPOD_USB_IN_CANCELLED;
}
/* Call for reset/disconnect/VBUS fall/migration/unrealize/session changes.
 * Not wired into production callbacks by this standalone prototype. */
static inline void ipod_zlp_explicit_invalidate(IPodZLPPending *s) {
 if(!s->constructed) return;
 ipod_zlp_advance(s);s->bound=false;s->setup_seq=0;
 memset(s->setup,0,sizeof(s->setup));ipod_usb_arm_line_drop(s->arm);
}
/* Controller EP0 SETUP path: actual copied receiver must accept before metadata
 * or provenance is bound. No caller-supplied accepted flag/epoch is trusted. */
static inline IPodUSBSetupResult ipod_zlp_setup_event(IPodZLPPending *s,
 IPodUSBRegisters *r,const uint8_t setup[8],IPodUSBSetupDMA dma,void *opaque) {
 IPodUSBSetupResult result=ipod_usb_receive_setup(r,setup,dma,opaque);
 if(result!=IPOD_USB_SETUP_OK) return result;
 if(!ipod_zlp_advance(s)||!ipod_usb_arm_setup_accepted(s->arm)) {
  s->bound=false;s->data->valid=false;ipod_usb_arm_line_drop(s->arm);return result;
 }
 memcpy(s->setup,setup,8);s->setup_seq=s->arm->setup_seq;s->bound=true;
 return result;
}
/* Actual copied register-write acceptance drives provenance. Integration must
 * additionally route all external lifecycle events through invalidate above. */
static inline bool ipod_zlp_write_event(IPodZLPPending *s,IPodUSBRegisters *r,
 uint32_t off,uint32_t value) {
 if(!ipod_usb_write(r,off,value)) return false;
 if(off==0x900||off==0x910||off==0x914) {
  ipod_zlp_advance(s);
  if(off==0x900&&(value&(1u<<31))) ipod_usb_arm_in_act(s->arm);
  else ipod_usb_arm_in_disqualify(s->arm);
 }
 if((off==0x10&&(value&1))||(off==0x804&&(r->dctl&2)))
  ipod_zlp_explicit_invalidate(s);
 return true;
}
static inline IPodZLPResult ipod_zlp_gate(const IPodZLPPending *s,
 const IPodUSBRegisters *r,unsigned ep) {
 if(!ipod_zlp_session(s)) return IPOD_ZLP_BAD_SESSION;
 if(ep||!s->bound||s->setup_seq!=s->arm->setup_seq||
  (s->setup[0]&0x80)||s->setup[6]||s->setup[7]) return IPOD_ZLP_BAD_SETUP;
 if(!ipod_usb_arm_begin_allowed(s->arm)) return IPOD_ZLP_NO_ARM;
 if(!r->vbus||r->reset_line||(r->gusbcfg&(1u<<29))||(r->dctl&2)||
  r->pcgcctl||!(r->gahbcfg&(1u<<5))||(r->dcfg&(1u<<23))||
  r->in_nak_requested||r->in_int[0]||
  r->in_ctl[0]!=((1u<<31)|(1u<<15))) return IPOD_ZLP_BAD_MODE;
 if(r->in_tsiz[0]!=(1u<<19)) return IPOD_ZLP_BAD_SIZE;
 return IPOD_ZLP_PENDING;
}
static inline IPodZLPResult ipod_zlp_begin(IPodZLPPending *s,
 const IPodUSBRegisters *r,unsigned ep,IPodUSBInStamp *out) {
 IPodZLPResult result=ipod_zlp_gate(s,r,ep);uint64_t id;
 if(result!=IPOD_ZLP_PENDING) return result;
 if(!out) return IPOD_ZLP_BAD_SESSION;
 if(s->active||s->data->active) return IPOD_ZLP_BUSY;
 if(s->used_setup_seq==s->setup_seq) return IPOD_ZLP_STALE;
 id=ipod_zlp_shared_take_id(s->shared_next_id);if(!id) return IPOD_ZLP_EXHAUSTED;
 s->stamp=(IPodUSBInStamp){s->data->owner,s->data->generation,id};
 s->snapshot=*r;s->arm_seq=s->arm->in_arm_seq;s->used_setup_seq=s->setup_seq;
 s->active=true;*out=s->stamp;return IPOD_ZLP_PENDING;
}
static inline bool ipod_zlp_correlate(const IPodZLPPending *s,
 const IPodUSBInStamp *stamp) {
 return stamp&&ipod_zlp_session(s)&&s->active&&
 stamp->owner==s->data->owner&&stamp->generation==s->data->generation&&
 stamp->owner==s->stamp.owner&&stamp->generation==s->stamp.generation&&
 stamp->id==s->stamp.id;
}
static inline IPodZLPResult ipod_zlp_retry(const IPodZLPPending *s,
 const IPodUSBRegisters *r,const IPodUSBInStamp *stamp) {
 if(!ipod_zlp_correlate(s,stamp)) return IPOD_ZLP_STALE;
 if(ipod_zlp_gate(s,r,0)!=IPOD_ZLP_PENDING||s->arm_seq!=s->arm->in_arm_seq||
  !ipod_usb_in_stage_same(r,&s->snapshot)) return IPOD_ZLP_STALE;
 return IPOD_ZLP_RETRIED;
}
static inline IPodZLPResult ipod_zlp_cancel(IPodZLPPending *s,
 const IPodUSBInStamp *stamp) {
 if(!ipod_zlp_correlate(s,stamp)) return IPOD_ZLP_STALE;
 ipod_zlp_drop(s);return IPOD_ZLP_CANCELLED;
}
#endif
