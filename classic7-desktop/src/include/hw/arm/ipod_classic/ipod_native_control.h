/* Native read/no-data control peer: actual EP0 packets <=64B, bounded4096B.
 * This is an abstract emulated host, not USB/IP URB completion or an HCD.
 * Setup/IN/ACK/status are distinct inputs. No descriptor or guest arm is made up.
 */
#ifndef IPOD_NATIVE_CONTROL_H
#define IPOD_NATIVE_CONTROL_H
#include "hw/arm/ipod_classic/ipod_classic_usb_core.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_arm.h"
#include <string.h>

typedef struct {
    bool open, packet, exposed, data_done, done;
    uint64_t id, epoch, arm_seq, generation, packet_nonce, next_nonce;
    uint8_t setup[8], data[64];
    uint32_t requested, actual, transferred;
    IPodUSBRegisters snapshot;
} IPodNativeControl;
enum { IPOD_PEER_OK=0, IPOD_PEER_WAIT=1, IPOD_PEER_STALE=-1,
       IPOD_PEER_MODE=-2, IPOD_PEER_SIZE=-3, IPOD_PEER_DMA=-4,
       IPOD_PEER_STALL=-5 };

static inline bool ipod_peer_setup_supported(const uint8_t p[8])
{
    unsigned n=p[6]|((unsigned)p[7]<<8), v=p[2]|((unsigned)p[3]<<8);
    unsigned index=p[4]|((unsigned)p[5]<<8), recipient=p[0]&31;
    /* Forward only the actual MSC class/interface and endpoint recovery
     * SETUPs. The guest, not this peer, must clear HALT and emit status/data. */
    if(p[0]==2 && p[1]==1 && !v && !n && !p[5] &&
       !(p[4]&0x70) && (p[4]&15)) return true;
    if(p[0]==0xa1 && p[1]==0xfe && !v && !p[4] && !p[5] && n==1) return true;
    if(p[0]==0x21 && p[1]==0xff && !v && !index && !n) return true;
    /* Standard IN only; vendor/auth and data-OUT requests remain unsupported.
     * Strings carry a language index. Guest may return actual endpoint STALL.
     */
    if(p[0]==0x80 && p[1]==6) return n && n<=4096 &&
        (p[3]==1 || p[3]==2 || p[3]==3 || p[3]==6 || p[3]==7 || p[3]==15);
    if((p[0]&0xe0)==0x80 && recipient<=2 && p[1]==0 && !v && n==2)
        return recipient==0?!index:recipient==1?index<=255:!(index&~0x8f);
    if(p[0]==0x81 && p[1]==10 && !v && index<=255 && n==1) return true;
    if(p[0]==1 && p[1]==11 && v<=255 && index<=255 && !n) return true;
    if (index) return false;
    if (p[0]==0x80 && p[1]==8) return n==1 && !v;
    return p[0]==0 && !n && ((p[1]==5 && v<=127) || (p[1]==9 && v<=255));
}
static inline bool ipod_peer_live(const IPodNativeControl *p,
                                 const IPodUSBArmProvenance *arm)
{
    return p->open && p->epoch && p->epoch==arm->setup_seq;
}
static inline bool ipod_peer_bus(const IPodUSBRegisters *r)
{
    return r->vbus && !r->reset_line && !(r->gusbcfg&(1u<<29)) &&
           !(r->dctl&2) && !(r->pcgcctl&3) && (r->gahbcfg&(1u<<5)) &&
           !(r->dcfg&(1u<<23));
}
static inline int ipod_peer_bind(IPodNativeControl *p, const uint8_t setup[8],
                                uint64_t id, uint64_t epoch)
{
    if (p->open || !id || !epoch || !ipod_peer_setup_supported(setup)) return IPOD_PEER_MODE;
    if(epoch==UINT64_MAX) return IPOD_PEER_MODE;
    memset(p,0,sizeof(*p));p->open=true;p->id=id;p->epoch=epoch;p->next_nonce=epoch;
    p->requested=setup[6]|((unsigned)setup[7]<<8);memcpy(p->setup,setup,8);
    return IPOD_PEER_OK;
}
static inline bool ipod_peer_packet_same(const IPodNativeControl *p,
    const IPodUSBRegisters *r,const IPodUSBArmProvenance *arm,uint64_t generation)
{
    return ipod_peer_live(p,arm) && p->packet && p->generation==generation &&
           p->arm_seq==arm->in_arm_seq &&
           p->snapshot.in_ctl[0]==r->in_ctl[0] &&
           p->snapshot.in_tsiz[0]==r->in_tsiz[0] &&
           p->snapshot.in_dma[0]==r->in_dma[0] &&
           p->snapshot.in_int[0]==r->in_int[0] && ipod_peer_bus(r);
}
/* EP0 is structurally present. EPENA must be an actual post-SETUP guest write.
 * EP0 MPS encoding0 (64B) and NEXTEP are supported, not treated as data/arm.
 * Neither optional USBAEP readback nor NEXTEP creates an enable act.
 */
static inline int ipod_peer_stage(IPodNativeControl *p,const IPodUSBRegisters *r,
    const IPodUSBArmProvenance *arm,uint64_t generation,
    bool (*read)(void *,uint32_t,uint8_t *,size_t),void *opaque)
{
    uint32_t ctl=r->in_ctl[0], remain=r->in_tsiz[0]&0x7f;
    uint32_t packets=(r->in_tsiz[0]>>19)&3, n=remain>64?64:remain;
    const uint32_t supported=(1u<<31)|(1u<<16)|(1u<<15)|(0xfu<<11);
    if (!ipod_peer_live(p,arm) || p->data_done) return IPOD_PEER_STALE;
    if (p->packet) return ipod_peer_packet_same(p,r,arm,generation)?IPOD_PEER_OK:IPOD_PEER_STALE;
    if (!ipod_peer_bus(r)) return IPOD_PEER_WAIT;
    if(ctl&(1u<<21)) {p->open=false;return IPOD_PEER_STALL;}
    if (!ipod_usb_arm_begin_allowed(arm) || !(ctl&(1u<<31))) return IPOD_PEER_WAIT;
    if ((ctl&~supported) || r->in_nak_requested || r->in_int[0]) return IPOD_PEER_MODE;
    if ((r->in_tsiz[0]&~((3u<<19)|0x7fu)) || !packets ||
        (remain && (remain>packets*64 || remain<=(packets-1)*64)) ||
        (!remain && packets!=1) ||
        (p->requested ? remain>p->requested-p->transferred : remain!=0)) return IPOD_PEER_SIZE;
    if (n && (!r->in_dma[0] || (r->in_dma[0]&3) ||
        !read || !read(opaque,r->in_dma[0],p->data,n))) return IPOD_PEER_DMA;
    p->snapshot=*r;p->arm_seq=arm->in_arm_seq;p->generation=generation;
    if(p->next_nonce==UINT64_MAX) return IPOD_PEER_MODE;
    p->packet_nonce=++p->next_nonce;
    p->actual=n;p->packet=true;p->exposed=true;return IPOD_PEER_OK;
}
static inline int ipod_peer_ack(IPodNativeControl *p,IPodUSBRegisters *r,
    IPodUSBArmProvenance *arm,uint64_t generation,uint64_t nonce)
{
    if (!p->exposed || nonce!=p->packet_nonce ||
        !ipod_peer_packet_same(p,r,arm,generation)) return IPOD_PEER_STALE;
    /* ACK consumes only the packet actually DMA-read (or the guest-armed ZLP).
     * The IRQ output is subsequently derived by the normal core update helper.
     */
    {
        unsigned remain=(r->in_tsiz[0]&0x7f)-p->actual;
        unsigned packets=((r->in_tsiz[0]>>19)&3)-1;
        bool end=!remain || !packets || p->actual<64;
        r->in_tsiz[0]=(packets<<19)|remain;
        r->in_dma[0]+=p->actual;r->in_ctl[0]^=1u<<16;
        if(end) {r->in_ctl[0]&=~(1u<<31);r->in_int[0]|=1;ipod_usb_arm_in_disqualify(arm);}
    }
    p->transferred+=p->actual;p->packet=false;p->exposed=false;
    p->data_done=!p->requested || p->transferred==p->requested || p->actual<64;
    if (!p->requested) {p->done=true;p->open=false;}
    return IPOD_PEER_OK;
}
static inline int ipod_peer_status_out(IPodNativeControl *p,IPodUSBRegisters *r,
                                       const IPodUSBArmProvenance *arm)
{
    uint32_t n=r->out_tsiz[0]&0x7f;
    if (!ipod_peer_live(p,arm) || !p->requested || !p->data_done || p->packet) return IPOD_PEER_STALE;
    if (!ipod_peer_bus(r) || !(r->out_ctl[0]&(1u<<31))) return IPOD_PEER_WAIT;
    if (r->out_nak_requested || r->out_ctl[0]&(1u<<17)) return IPOD_PEER_WAIT;
    if ((r->out_ctl[0]&~((1u<<31)|(1u<<15)|3u)) || (r->out_int[0]&8) ||
        (r->out_tsiz[0]&~((3u<<29)|(1u<<19)|0x7fu)) ||
        !(r->out_tsiz[0]&(1u<<19))) return IPOD_PEER_MODE;
    /* Zero-length STATUS OUT consumes one loaded packet, no RAM write/data.
     * A loaded receive capacity (0..64B) remains a residual of that capacity.
     */
    if (n>64) return IPOD_PEER_SIZE;
    r->out_tsiz[0]&=~(1u<<19);r->out_ctl[0]&=~(1u<<31);r->out_int[0]|=1;
    p->done=true;p->open=false;return IPOD_PEER_OK;
}

typedef struct IPodClassicUSBState IPodClassicUSBState;
int ipod_classic_usb_peer_begin(IPodClassicUSBState *,IPodNativeControl *,const uint8_t[8],uint64_t);
int ipod_classic_usb_peer_poll(IPodClassicUSBState *,IPodNativeControl *);
int ipod_classic_usb_peer_ack(IPodClassicUSBState *,IPodNativeControl *,uint64_t);
int ipod_classic_usb_peer_status_out(IPodClassicUSBState *,IPodNativeControl *);
#endif
