/* Read-only event observer. It is not a ready/state/IRQ input. */
static void usb_ep0_cause_observe(IPodClassicUSBState *s,hwaddr off,uint32_t value)
{
    static unsigned hits;
    const char *enabled=getenv("IPOD_USB_EP0_CAUSE");
    CPUState *cs=current_cpu;
    CPUARMState *env;
    uint8_t bytes[64];unsigned i,j;
    const unsigned pointers[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14};
    bool stall=off==0x900 && (value&(1u<<21));
    bool address=off==0x800 && ((value>>4)&127);
    if(!enabled || strcmp(enabled,"1") || (!stall && !address) || hits>=16)return;
    ++hits;
    fprintf(stderr,"EP0CAUSE event=%s hit=%u off=%03"HWADDR_PRIx" value=%08x cpu_context=%d dcfg=%08x in_ctl=%08x out_ctl=%08x vns=%"PRId64,
        stall?"actual_guest_STALL":"actual_guest_ADDRESS",hits,off,value,!!cs,
        s->regs.dcfg,s->regs.in_ctl[0],s->regs.out_ctl[0],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if(!cs){fprintf(stderr,"\n");return;}
    env=&container_of(cs,ARMCPU,parent_obj)->env;
    for(i=0;i<16;i++)fprintf(stderr," r%u=%08x",i,env->regs[i]);
    fprintf(stderr,"\n");
    for(i=0;i<ARRAY_SIZE(pointers);i++) {
        unsigned reg=pointers[i];uint32_t addr=env->regs[reg]&~3u;
        bool ok=usb_bulk_dma(&address_space_memory,addr,bytes,sizeof(bytes),false);
        fprintf(stderr,"EP0CAUSE_RAM hit=%u reg=r%u addr=%08x plain_ram_read=%d bytes=%u hex=",
            hits,reg,addr,ok,ok?(unsigned)sizeof(bytes):0);
        if(ok)for(j=0;j<sizeof(bytes);j++)fprintf(stderr,"%02x",bytes[j]);
        fprintf(stderr,"\n");
    }
    /* The actual STALL helper pushes r4/r5/r6/LR. The first saved word
     * is the observed caller object, not a fixed guessed firmware address.
     * Follow at most256bytes and the saved LR's64code bytes, read-only.
     */
    if(stall && usb_bulk_dma(&address_space_memory,env->regs[13]&~3u,bytes,16,false)) {
        uint32_t object=ldl_le_p(bytes),caller=ldl_le_p(bytes+12)&~3u;
        uint8_t object_bytes[256],code[64];
        bool object_ok=usb_bulk_dma(&address_space_memory,object,object_bytes,sizeof(object_bytes),false);
        bool code_ok=usb_bulk_dma(&address_space_memory,caller,code,sizeof(code),false);
        fprintf(stderr,"EP0CAUSE_OBJECT hit=%u actual_saved_r4=%08x plain_ram_read=%d bytes=%u hex=",hits,object,object_ok,object_ok?256:0);
        if(object_ok)for(j=0;j<sizeof(object_bytes);j++)fprintf(stderr,"%02x",object_bytes[j]);
        fprintf(stderr,"\nEP0CAUSE_CALLER hit=%u actual_saved_lr=%08x plain_ram_read=%d hex=",hits,caller,code_ok);
        if(code_ok)for(j=0;j<sizeof(code);j++)fprintf(stderr,"%02x",code[j]);
        fprintf(stderr,"\n");
    }
}
