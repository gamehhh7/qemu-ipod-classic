/* Transport batching only: every 512B token still uses the same controller
 * DMA/arm/NAK/PID/IRQ path. Virtual timers let the guest handle its own IRQs.
 * One outstanding RPC, bounded to 100ms wall time, no uncertain-OUT replay. */
static void server_batch_cancel(IPodHostServer *srv)
{
    if (srv->batch_timer) timer_del(srv->batch_timer);
    if (srv->batch_deadline) timer_del(srv->batch_deadline);
    srv->batch_pending=false;
}
static void server_batch_finish(IPodHostServer *srv, int rc)
{
    uint8_t reply[16]={0};
    static unsigned logged;
    server_batch_cancel(srv);
    put_u32(reply,rc);put_u32(reply+4,srv->batch_at);
    put_u64(reply+8,srv->bulk_out_sequence);
    if (getenv("CLASSIC7_USB_TIMING") && logged++<256) {
        fprintf(stderr,"BULKBATCH rc=%d bytes=%u requested=%u seq=%"PRIu64
                " naks=%u start_vns=%"PRId64" end_vns=%"PRId64"\n",
                rc,srv->batch_at,srv->batch_size,srv->bulk_out_sequence,
                srv->batch_naks,srv->batch_started,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    /* rc may describe a terminal controller failure after an accepted prefix.
     * Keep that exact prefix in the response, never call it unaccepted. */
    if (server_reply_ok(srv,0x3b,reply,sizeof(reply))<0) {
        server_close_client(srv);server_request_stop(srv);
    }
}
static void server_batch_expire(void *opaque)
{
    IPodHostServer *srv=opaque;
    if (srv->batch_pending) server_batch_finish(srv,IPOD_BULK_WAIT);
}
static void server_batch_tick(void *opaque)
{
    IPodHostServer *srv=opaque;
    unsigned n;
    int rc;
    assert(qemu_mutex_iothread_locked());
    if (!srv->batch_pending) return;
    if (!srv->client || srv->closing || !srv->session_open ||
        srv->session_closed || srv->session_serial!=srv->batch_serial ||
        srv->host_bus_token!=srv->batch_token || !server_bus_tokens_ready(srv)) {
        server_batch_finish(srv,-1);return;
    }
    n=MIN(512u,srv->batch_size-srv->batch_at);
    rc=ipod_classic_usb_bulk_out(srv->controller,srv->batch_ep,
                                srv->batch_data+srv->batch_at,n);
    if (rc==IPOD_BULK_OK) {
        ++srv->bulk_out_sequence;srv->batch_at+=n;
        if (srv->batch_at==srv->batch_size) {
            server_batch_finish(srv,IPOD_BULK_OK);return;
        }
    } else if (rc==IPOD_BULK_WAIT) {
        ++srv->batch_naks;
    } else {
        server_batch_finish(srv,rc);return;
    }
    /* 512B at HS takes about8.6us before protocol overhead. Each token gets
     * a separate service opportunity; the guest handles its own interrupts. */
    timer_mod(srv->batch_timer,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+10000);
}
static int server_op_batch_out(IPodHostServer *srv,const uint8_t *req,uint32_t length)
{
    unsigned ep,n,packets;
    if (srv->mode!=IPOD_HOST_MODE_NATIVE)
        return server_reply_unsupported(srv,0x3b,IPOD_HOST_REASON_WRONG_MODE);
    if (length<32 || !(ep=get_u32(req+16)) || ep>=IPOD_DWC_EP_SLOTS ||
        (n=get_u32(req+20))<=512 || n>sizeof(srv->batch_data) || length!=32+n)
        return server_reply_refused(srv,0x3b,IPOD_HOST_REASON_MALFORMED);
    if (!srv->session_open || srv->session_closed || !srv->host_bus_token || srv->closing)
        return server_reply_refused(srv,0x3b,IPOD_HOST_REASON_NO_SESSION);
    if (get_u64(req)!=srv->session_serial || get_u64(req+8)!=srv->host_bus_token)
        return server_reply_refused(srv,0x3b,IPOD_HOST_REASON_IDENTITY);
    if (srv->have_control || srv->peer.open || srv->pulse_pending)
        return server_reply_refused(srv,0x3b,IPOD_HOST_REASON_SESSION_BUSY);
    if (!server_bus_tokens_ready(srv))
        return server_reply_refused(srv,0x3b,IPOD_HOST_REASON_RESET_RECOVERY);
    packets=(n+511)/512;
    if (srv->bulk_out_sequence>UINT64_MAX-packets || get_u64(req+24)!=srv->bulk_out_sequence+1)
        return server_reply_refused(srv,0x3b,IPOD_HOST_REASON_UNKNOWN_HANDLE);
    if (!srv->batch_timer) srv->batch_timer=timer_new_ns(QEMU_CLOCK_VIRTUAL,server_batch_tick,srv);
    if (!srv->batch_deadline) srv->batch_deadline=timer_new_ms(QEMU_CLOCK_REALTIME,server_batch_expire,srv);
    memcpy(srv->batch_data,req+32,n);
    srv->batch_ep=ep;srv->batch_size=n;srv->batch_at=0;srv->batch_naks=0;
    srv->batch_token=srv->host_bus_token;srv->batch_serial=srv->session_serial;
    srv->batch_started=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);srv->batch_pending=true;
    timer_mod(srv->batch_deadline,qemu_clock_get_ms(QEMU_CLOCK_REALTIME)+100);
    timer_mod(srv->batch_timer,srv->batch_started+10000);
    return 0; /* response belongs to the timer completion, not this dispatch */
}
