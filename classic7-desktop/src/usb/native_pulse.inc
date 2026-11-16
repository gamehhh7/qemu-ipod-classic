/* Emulated downstream host port timer, not guest register programming.
 * One accepted start schedules one release in the same virtual clock domain
 * that measures the physical reset input. Abort cancels; never retries/extends.
 */
static void server_pulse_cancel(IPodHostServer *srv)
{
    if(srv->pulse_timer) timer_del(srv->pulse_timer);
    srv->pulse_pending=false;
}
static void server_pulse_release(void *opaque)
{
    IPodHostServer *srv=opaque;IPodHostBusResult rc;
    assert(qemu_mutex_iothread_locked());
    if(!srv->pulse_pending) return;
    srv->pulse_pending=false;
    rc=ipod_classic_usb_host_bus_event(srv->controller,srv,srv->pulse_token,srv->pulse_sequence+1,false);
    fprintf(stderr,"HOSTPULSE release token=%"PRIu64" seq=%"PRIu64" result=%u vns=%"PRId64"\n",srv->pulse_token,srv->pulse_sequence+1,rc,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
static int server_op_pulse(IPodHostServer *srv,const uint8_t *req,uint32_t length)
{
    IPodHostBusResult rc;uint64_t sequence,duration;uint8_t reply[32]={0};
    if(srv->mode!=IPOD_HOST_MODE_NATIVE) return server_reply_unsupported(srv,0x38,IPOD_HOST_REASON_WRONG_MODE);
    if(length!=32 || get_u64(req+24)!=60000000 || get_u64(req+16)!=1)
        return server_reply_refused(srv,0x38,IPOD_HOST_REASON_MALFORMED);
    if(!srv->session_open || srv->session_closed || !srv->host_bus_token || srv->closing)
        return server_reply_refused(srv,0x38,IPOD_HOST_REASON_NO_SESSION);
    if(get_u64(req)!=srv->session_serial || get_u64(req+8)!=srv->host_bus_token)
        return server_reply_refused(srv,0x38,IPOD_HOST_REASON_IDENTITY);
    if(srv->pulse_pending || srv->have_control || srv->peer.open)
        return server_reply_refused(srv,0x38,IPOD_HOST_REASON_SESSION_BUSY);
    sequence=get_u64(req+16);duration=get_u64(req+24);
    if(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)>INT64_MAX-(int64_t)duration)
        return server_reply_refused(srv,0x38,IPOD_HOST_REASON_MALFORMED);
    if(!srv->pulse_timer) srv->pulse_timer=timer_new_ns(QEMU_CLOCK_VIRTUAL,server_pulse_release,srv);
    /* This explicit op models a root-port pulse: target60ms, actual minimum50ms.
     * Legacy manual downstream inputs retain the original10ms policy. */
    srv->controller->root_port_reset=true;
    rc=ipod_classic_usb_host_bus_event(srv->controller,srv,srv->host_bus_token,sequence,true);
    if(rc==IPOD_HOST_BUS_OK) {
        usb_device_reset(srv->bridge);(void)ipod_host_xfer_cancel(srv->xfer);
        srv->have_control=false;srv->control_handle=0;memset(&srv->peer,0,sizeof(srv->peer));
        srv->pulse_pending=true;srv->pulse_token=srv->host_bus_token;srv->pulse_sequence=sequence;
        timer_mod(srv->pulse_timer,srv->controller->host_bus_policy.asserted_ns+duration);
    }
    put_u32(reply,rc);put_u32(reply+4,rc==IPOD_HOST_BUS_OK);
    put_u64(reply+8,sequence);put_u64(reply+16,srv->controller->host_bus_policy.asserted_ns);put_u64(reply+24,duration);
    return server_send(srv,0x38,rc==IPOD_HOST_BUS_OK?IPOD_HOST_ST_OK:IPOD_HOST_ST_NEGATIVE,reply,sizeof(reply));
}
