/* Optional observer only: source-identity guarded, bounded plain-RAM reads.
 * No return value influences USB gates, flags, completions, IRQs or guest RAM.
 */
void ipod_classic_usb_rx_journal(IPodClassicUSBState *s,const char *event,
    uint32_t off,uint32_t value,const uint8_t *setup)
{
    static unsigned hits;
    static const uint8_t expected[12]={0x70,0x40,0x2d,0xe9,0x78,0x30,0x9f,0xe5,0x00,0x50,0xa0,0xe1};
    const char *enabled=getenv("IPOD_USB_RX_JOURNAL");
    uint8_t code[12],literal[4],global[64],buffer[64],irq_literal[4],out_flags[16];unsigned j;
    bool fingerprint=false,global_ok=false,buffer_ok=false,flags_ok=false;
    uint32_t dma=s->regs.out_dma[0],buffer_start=dma>=16?dma-16:0;
    if(!enabled || strcmp(enabled,"1") || hits>=96)return;
    ++hits;
    fingerprint=usb_bulk_dma(&address_space_memory,0x080fc878,code,sizeof(code),false) &&
        !memcmp(code,expected,sizeof(code)) &&
        usb_bulk_dma(&address_space_memory,0x080fc8fc,literal,sizeof(literal),false) &&
        ldl_le_p(literal)==0x08a09f78;
    if(fingerprint)global_ok=usb_bulk_dma(&address_space_memory,ldl_le_p(literal),global,sizeof(global),false);
    if(fingerprint && usb_bulk_dma(&address_space_memory,0x08076abc,irq_literal,4,false) && ldl_le_p(irq_literal)==0x08ae5500)
        flags_ok=usb_bulk_dma(&address_space_memory,ldl_le_p(irq_literal),out_flags,sizeof(out_flags),false);
    if(buffer_start)buffer_ok=usb_bulk_dma(&address_space_memory,buffer_start,buffer,sizeof(buffer),false);
    fprintf(stderr,"RXJOURNAL event=%s hit=%u off=%03x value=%08x cpu_context=%d fingerprint=%d global_ok=%d dma=%08x out_ctl=%08x out_tsiz=%08x out_irq=%08x in_irq=%08x vns=%"PRId64" setup=",
        event,hits,off,value,!!current_cpu,fingerprint,global_ok,dma,s->regs.out_ctl[0],s->regs.out_tsiz[0],s->regs.out_int[0],s->regs.in_int[0],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if(setup)for(j=0;j<8;j++)fprintf(stderr,"%02x",setup[j]);
    fprintf(stderr," global_hex=");if(global_ok)for(j=0;j<sizeof(global);j++)fprintf(stderr,"%02x",global[j]);
    fprintf(stderr," flags_ok=%d out_flags_hex=",flags_ok);if(flags_ok)for(j=0;j<sizeof(out_flags);j++)fprintf(stderr,"%02x",out_flags[j]);
    fprintf(stderr," buffer_start=%08x buffer_ok=%d buffer_hex=",buffer_start,buffer_ok);
    if(buffer_ok)for(j=0;j<sizeof(buffer);j++)fprintf(stderr,"%02x",buffer[j]);
    fprintf(stderr,"\n");
}
