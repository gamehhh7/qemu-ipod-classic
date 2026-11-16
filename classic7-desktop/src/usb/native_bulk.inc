/* Included by the actual controller translation unit, always under BQL. */
void ipod_classic_usb_bulk_invalidate(IPodClassicUSBState *s)
{
    IPodNativeBulk *b=&s->bulk;
    assert(qemu_mutex_iothread_locked());
    if (b->context==UINT64_MAX) b->exhausted=true;
    else ++b->context;
    memset(b->armed,0,sizeof(b->armed));
    memset(b->in,0,sizeof(b->in));
}
static void usb_bulk_guest_write(IPodClassicUSBState *s,hwaddr off,uint32_t value)
{
    bool in;unsigned ep,reg;IPodNativeBulk *b=&s->bulk;
    if ((off==0x10 && (value&1)) || (off==0x804 && (s->regs.dctl&2))) {
        ipod_classic_usb_bulk_invalidate(s);return;
    }
    if (!ipod_usb_ep_offset(off,&in,&ep,&reg) || !ep) return;
    if (reg!=0 && reg!=8 && reg!=0x10 && reg!=0x14) return;
    if (b->generation[in][ep]==UINT64_MAX) b->exhausted=true;
    else ++b->generation[in][ep];
    b->armed[in][ep]=reg==0 && (value&(1u<<31)) && !(value&(1u<<30));
    if (in) memset(&b->in[ep],0,sizeof(b->in[ep]));
}
/* Full-span RAM preflight before a single DMA copy. Reject readonly aliases,
 * ROMD, MMIO, holes and backing boundaries. No partial writes on bad spans. */
static bool usb_bulk_dma(AddressSpace *as,uint32_t addr,uint8_t *data,size_t len,bool write)
{
    uint8_t scratch[IPOD_BULK_MAX_PACKET];
    uint64_t end=(uint64_t)addr+len;hwaddr xlat,span=len;
    MemoryRegion *mr;MemoryRegionSection section;bool ok=false;
    bool window=(addr>=SDRAM_MEM_BASE && end<=(uint64_t)SDRAM_MEM_BASE+SDRAM_MEM_SIZE) ||
        (addr>=SDRAM_UNCACHED_MEM_BASE && end<=(uint64_t)SDRAM_UNCACHED_MEM_BASE+SDRAM_MEM_SIZE) ||
        (addr>=IRAM0_MEM_BASE && end<=(uint64_t)IRAM0_MEM_BASE+IRAM0_MEM_SIZE) ||
        (addr>=IRAM1_MEM_BASE && end<=(uint64_t)IRAM1_MEM_BASE+IRAM1_MEM_SIZE);
    assert(qemu_mutex_iothread_locked());
    if (!as || !as->root || !data || !len || len>sizeof(scratch) || !addr ||
        (addr&3) || end>(1ULL<<32) || !window) return false;
    rcu_read_lock();section=memory_region_find(as->root,addr,len);
    mr=address_space_translate(as,addr,&xlat,&span,write,MEMTXATTRS_UNSPECIFIED);
    if (section.mr==mr && !section.readonly &&
        section.offset_within_address_space==addr && section.offset_within_region==xlat &&
        int128_eq(section.size,int128_make64(len)) && mr && !mr->alias &&
        memory_region_is_ram(mr) && !mr->readonly && !mr->rom_device &&
        !memory_region_is_ram_device(mr) && mr->ram_block &&
        memory_access_is_direct(mr,write) && span>=len &&
        xlat<=memory_region_size(mr) && memory_region_size(mr)-xlat>=len &&
        xlat<=qemu_ram_get_used_length(mr->ram_block) &&
        qemu_ram_get_used_length(mr->ram_block)-xlat>=len) {
        if (write) ok=dma_memory_write(as,addr,data,len,MEMTXATTRS_UNSPECIFIED)==MEMTX_OK;
        else ok=dma_memory_read(as,addr,scratch,len,MEMTXATTRS_UNSPECIFIED)==MEMTX_OK;
    }
    if(section.mr) memory_region_unref(section.mr);
    rcu_read_unlock();if(ok && !write) memcpy(data,scratch,len);return ok;
}
static int usb_bulk_gate(IPodClassicUSBState *s,unsigned ep,bool in)
{
    IPodUSBRegisters *r=&s->regs;
    uint32_t ctl,tsiz,irq,mps;
    if (!ep || ep>=IPOD_DWC_EP_SLOTS) return IPOD_BULK_MODE;
    if (s->bulk.exhausted) return IPOD_BULK_EXHAUSTED;
    if (!DEVICE(s)->realized || !ipod_peer_bus(r)) return IPOD_BULK_WAIT;
    ctl=in?r->in_ctl[ep]:r->out_ctl[ep];tsiz=in?r->in_tsiz[ep]:r->out_tsiz[ep];
    irq=in?r->in_int[ep]:r->out_int[ep];mps=ctl&0x7ff;
    if (ctl&(1u<<21)) return IPOD_BULK_STALL;
    if (!(ctl&(1u<<31)) || !s->bulk.armed[in][ep] || (ctl&(1u<<17)) ||
        (in?r->in_nak_requested:r->out_nak_requested)) return IPOD_BULK_WAIT;
    /* Linux DWC2 gadget.c programs MC(1) for ordinary nonzero bulk IN.
     * Support one transaction; retain that field, never count it as bytes. */
    /* An outstanding real completion is temporary endpoint backpressure.
     * Let the guest ISR clear it; no separate host status RPC is required. */
    if (irq) return IPOD_BULK_WAIT;
    if (!(ctl&(1u<<15)) || ((ctl>>18)&3)!=2 || !mps || mps>512 ||
        !((tsiz>>19)&0x3ff) || (in && ((tsiz>>29)&3)>1)) return IPOD_BULK_MODE;
    return IPOD_BULK_OK;
}
static void usb_bulk_commit(IPodClassicUSBState *s,unsigned ep,bool in,unsigned n)
{
    IPodUSBRegisters *r=&s->regs;
    uint32_t *ctl=in?&r->in_ctl[ep]:&r->out_ctl[ep];
    uint32_t *tsiz=in?&r->in_tsiz[ep]:&r->out_tsiz[ep];
    uint32_t *dma=in?&r->in_dma[ep]:&r->out_dma[ep];
    uint32_t *irq=in?&r->in_int[ep]:&r->out_int[ep];
    unsigned remain=(*tsiz&0x7ffff)-n,packets=((*tsiz>>19)&0x3ff)-1;
    bool complete=!remain || !packets || n<(*ctl&0x7ff);
    *tsiz=(*tsiz&~0x1fffffffu)|(packets<<19)|remain;
    *dma+=n;*ctl^=1u<<16; /* data PID advances only for accepted/ACKed packet */
    if (complete) {*ctl&=~(1u<<31);*irq|=1;s->bulk.armed[in][ep]=false;}
    usb_update(s); /* normal core masks and GAHBCFG determine IRQ level */
}
int ipod_classic_usb_bulk_out(IPodClassicUSBState *s,unsigned ep,const uint8_t *data,size_t n)
{
    int rc;uint32_t ctl,dma,remain;
    assert(qemu_mutex_iothread_locked());
    if (n>512 || (n && !data)) return IPOD_BULK_SIZE;
    rc=usb_bulk_gate(s,ep,false);if(rc) return rc;
    ctl=s->regs.out_ctl[ep];dma=s->regs.out_dma[ep];remain=s->regs.out_tsiz[ep]&0x7ffff;
    if(n>(ctl&0x7ff) || n>remain || (uint64_t)dma+n>UINT32_MAX) return IPOD_BULK_SIZE;
    if(n && !usb_bulk_dma(&address_space_memory,dma,(uint8_t *)data,n,true)) return IPOD_BULK_DMA;
    usb_bulk_commit(s,ep,false,n);return IPOD_BULK_OK;
}
int ipod_classic_usb_bulk_in(IPodClassicUSBState *s,unsigned ep,IPodBulkPacket **out)
{
    int rc;IPodBulkPacket *p;uint32_t ctl,tsiz,dma,n;
    assert(qemu_mutex_iothread_locked());
    if(!out) return IPOD_BULK_MODE;
    *out=NULL;
    rc=usb_bulk_gate(s,ep,true);if(rc) return rc;
    p=&s->bulk.in[ep];
    if(p->staged) {*out=p;return IPOD_BULK_OK;} /* pinned packet, never reread */
    if(s->bulk.next_nonce==UINT64_MAX) return IPOD_BULK_EXHAUSTED;
    ctl=s->regs.in_ctl[ep];tsiz=s->regs.in_tsiz[ep];dma=s->regs.in_dma[ep];
    n=MIN(tsiz&0x7ffff,ctl&0x7ff);
    if((uint64_t)dma+n>UINT32_MAX) return IPOD_BULK_SIZE;
    if(n && !usb_bulk_dma(&address_space_memory,dma,p->bytes,n,false)) return IPOD_BULK_DMA;
    p->nonce=++s->bulk.next_nonce;p->context=s->bulk.context;
    p->generation=s->bulk.generation[true][ep];p->ctl=ctl;p->tsiz=tsiz;p->dma=dma;
    p->interrupt=s->regs.in_int[ep];p->length=n;p->staged=true;*out=p;
    return IPOD_BULK_OK;
}
int ipod_classic_usb_bulk_ack(IPodClassicUSBState *s,unsigned ep,uint64_t nonce)
{
    IPodBulkPacket *p;int rc;
    assert(qemu_mutex_iothread_locked());
    if(!ep || ep>=IPOD_DWC_EP_SLOTS) return IPOD_BULK_MODE;
    p=&s->bulk.in[ep];
    if(!nonce || !p->staged || nonce!=p->nonce || p->context!=s->bulk.context ||
        p->generation!=s->bulk.generation[true][ep] || p->ctl!=s->regs.in_ctl[ep] ||
        p->tsiz!=s->regs.in_tsiz[ep] || p->dma!=s->regs.in_dma[ep] ||
        p->interrupt!=s->regs.in_int[ep]) return IPOD_BULK_STALE;
    rc=usb_bulk_gate(s,ep,true);if(rc) return rc;
    usb_bulk_commit(s,ep,true,p->length);memset(p,0,sizeof(*p));return IPOD_BULK_OK;
}
