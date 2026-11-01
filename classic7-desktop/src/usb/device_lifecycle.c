/* Opt-in DWC device core; diagnostic single-SETUP ingress, no USB host bridge. */
#include "qemu/osdep.h"
#include "target/arm/cpu.h"
#include "hw/arm/ipod_classic/ipod_mmio_observer.h"
#include "qemu/module.h"
#include "hw/arm/ipod_classic/ipod_native_control.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "sysemu/dma.h"
#include "hw/arm/ipod_classic/ipod_classic_usb.h"
#include "hw/arm/ipod_classic/ipod_classic.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_dma.h"
/* Forward declarations let the fixed synthetic test exercise actual callbacks. */
static void usb_write(void *opaque, hwaddr off, uint64_t value, unsigned size);
static void usb_vbus_input(void *opaque, int n, int level);
static void usb_reset_set(Object *obj, bool value, Error **errp);
static void usb_reset_line_apply(IPodClassicUSBState *s, bool value);
static void usb_reset(DeviceState *dev);
static void usb_realize(DeviceState *dev, Error **errp);
static int usb_pre_save(void *opaque);
static int usb_post_load(void *opaque, int version);
static void usb_update(IPodClassicUSBState *s);
static void usb_ep0_cause_observe(IPodClassicUSBState *,hwaddr,uint32_t);
void ipod_classic_usb_rx_journal(IPodClassicUSBState *,const char *,uint32_t,uint32_t,const uint8_t *);
static uint64_t ipod_usb_in_next_owner = 1;
static uint64_t ipod_usb_in_take_counter(uint64_t *next)
{
    return ipod_zlp_shared_take_id(next);
}
/* Transaction invalidation: keep accepted SETUP and let an IN0 write
 * establish/disqualify the fresh arm independently. */
static void ipod_usb_in_invalidate(IPodClassicUSBState *s)
{
    assert(qemu_mutex_iothread_locked());
    if (s->status_in.constructed) {
        ipod_zlp_advance(&s->status_in);
    } else if (s->in_stage.constructed && s->in_stage.valid) {
        ipod_usb_in_stage_invalidate(&s->in_stage, s->in_stage.owner,
                                     s->in_stage.generation + 1);
    }
}
static void ipod_usb_notify_context_lost(IPodClassicUSBState *s)
{
    void (*notify)(void *,bool);void *opaque;
    assert(qemu_mutex_iothread_locked());
    if(s->context_lost_notifying||!s->context_lost)return;
    s->context_lost_notifying=true;notify=s->context_lost;opaque=s->context_lost_opaque;
    notify(opaque,s->context_lost_dead);
    s->context_lost_notifying=false;
}
bool ipod_classic_usb_observer_register(IPodClassicUSBState *s,
    void (*notify)(void *,bool),void *opaque,uint64_t *token)
{
    uint64_t id;
    assert(qemu_mutex_iothread_locked());
    if(!notify||!opaque||!token||!DEVICE(s)->realized||!s->in_stage.valid||
       s->context_lost_dead||s->context_lost)return false;
    id=ipod_zlp_shared_take_id(&s->next_context_lost_token);if(!id)return false;
    s->context_lost=notify;s->context_lost_opaque=opaque;s->context_lost_token=id;
    *token=id;return true;
}
bool ipod_classic_usb_observer_unregister(IPodClassicUSBState *s,void *opaque,uint64_t token)
{
    assert(qemu_mutex_iothread_locked());
    if(!token||s->context_lost_opaque!=opaque||s->context_lost_token!=token)return false;
    s->context_lost=NULL;s->context_lost_opaque=NULL;s->context_lost_token=0;return true;
}
/* Full lifecycle loss: register restoration never restores a transaction. */
static void ipod_usb_ep0_lifecycle_invalidate(IPodClassicUSBState *s)
{
    assert(qemu_mutex_iothread_locked());
    if (s->status_in.constructed) {
        ipod_zlp_explicit_invalidate(&s->status_in);
    } else {
        ipod_usb_in_invalidate(s);
        ipod_usb_arm_line_drop(&s->arm);
    }
    ipod_classic_usb_bulk_invalidate(s);
    ipod_usb_notify_context_lost(s); /* metadata has already been cleared */
}
static bool ipod_usb_in_dma_read(void *opaque, uint32_t addr, uint8_t *dst, size_t len)
{
    return ipod_usb_dma_read_plain(opaque, addr, dst, len);
}
static IPodUSBInResult ipod_usb_in_begin_as(IPodClassicUSBState *s, AddressSpace *as,
                                           uint8_t *dst, size_t cap,
                                           IPodUSBInStamp *stamp)
{
    uint64_t id;
    IPodUSBInResult result;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return IPOD_USB_IN_BAD_ID; }
    if (!s->in_stage.valid) { return IPOD_USB_IN_BAD_GENERATION; }
    if (s->in_stage.active || s->status_in.active) { return IPOD_USB_IN_BUSY; }
    if (!ipod_usb_arm_begin_allowed(&s->arm)) { return IPOD_USB_IN_NO_ARM; }
    id = ipod_zlp_shared_take_id(&s->next_in_id);
    if (!id) { return IPOD_USB_IN_BAD_ID; }
    result = ipod_usb_in_stage_prepare(&s->in_stage, &s->regs,
        s->in_stage.owner, s->in_stage.generation, id,
        IPOD_USB_IN_STAGE_IN_PID, IPOD_USB_IN_STAGE_SINGLE_BUFFER,
        dst, cap, ipod_usb_in_dma_read, as);
    if (result == IPOD_USB_IN_STAGED) {
        *stamp = (IPodUSBInStamp) {s->in_stage.owner, s->in_stage.generation, id};
    }
    return result;
}
IPodUSBInResult ipod_classic_usb_in_begin(IPodClassicUSBState *s, uint8_t *dst,
                                         size_t cap, IPodUSBInStamp *stamp)
{
    return ipod_usb_in_begin_as(s, &address_space_memory, dst, cap, stamp);
}
IPodUSBInResult ipod_classic_usb_in_retry(IPodClassicUSBState *s,
                                         const IPodUSBInStamp *stamp,
                                         uint8_t *dst, size_t cap)
{
    assert(qemu_mutex_iothread_locked());
    IPodUSBInResult correlated;
    if (!stamp) { return IPOD_USB_IN_BAD_ID; }
    correlated = ipod_usb_in_stage_correlate(&s->in_stage, stamp->owner,
                                             stamp->generation, stamp->id);
    if (correlated != IPOD_USB_IN_STAGED) { return correlated; }
    return ipod_usb_in_stage_prepare(&s->in_stage, &s->regs,
        stamp->owner, stamp->generation, stamp->id,
        IPOD_USB_IN_STAGE_IN_PID, IPOD_USB_IN_STAGE_SINGLE_BUFFER,
        dst, cap, ipod_usb_in_dma_read, &address_space_memory);
}
IPodUSBInResult ipod_classic_usb_in_cancel(IPodClassicUSBState *s,
                                          const IPodUSBInStamp *stamp)
{
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return IPOD_USB_IN_BAD_ID; }
    return ipod_usb_in_stage_cancel(&s->in_stage, stamp->owner, stamp->generation, stamp->id);
}
IPodUSBInResult ipod_classic_usb_in_finish(IPodClassicUSBState *s,
                                          const IPodUSBInStamp *stamp)
{
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return IPOD_USB_IN_BAD_ID; }
    return ipod_usb_in_stage_finish_success(&s->in_stage, stamp->owner, stamp->generation, stamp->id);
}
IPodZLPResult ipod_classic_usb_status_in_begin(IPodClassicUSBState *s,
                                             IPodUSBInStamp *stamp)
{
    assert(qemu_mutex_iothread_locked());
    return ipod_zlp_begin(&s->status_in, &s->regs, 0, stamp);
}
IPodZLPResult ipod_classic_usb_status_in_retry(IPodClassicUSBState *s,
                                             const IPodUSBInStamp *stamp)
{
    assert(qemu_mutex_iothread_locked());
    return ipod_zlp_retry(&s->status_in, &s->regs, stamp);
}
IPodZLPResult ipod_classic_usb_status_in_cancel(IPodClassicUSBState *s,
                                              const IPodUSBInStamp *stamp)
{
    assert(qemu_mutex_iothread_locked());
    return ipod_zlp_cancel(&s->status_in, stamp);
}
bool ipod_classic_usb_ep0_abort_epoch(IPodClassicUSBState *s,uint64_t expected)
{
    assert(qemu_mutex_iothread_locked());
    if (!expected || expected != s->arm.setup_seq) { return false; }
    /* Adapter-owned cancellation must not synchronously re-enter its ledger.
     * Actual external lifecycle callbacks use the notifying helper instead. */
    ipod_zlp_explicit_invalidate(&s->status_in);
    return true;
}
size_t ipod_classic_usb_in_staged_len(IPodClassicUSBState *s)
{
    assert(qemu_mutex_iothread_locked());
    return s->in_stage.active ? s->in_stage.len : 0;
}
uint64_t ipod_classic_usb_setup_epoch(const IPodClassicUSBState *s)
{
    assert(qemu_mutex_iothread_locked());
    return s->arm.setup_seq;
}
uint64_t ipod_classic_usb_in_arm_epoch(const IPodClassicUSBState *s)
{
    assert(qemu_mutex_iothread_locked());
    return s->arm.in_arm_seq;
}
IPodUSBSetupResult ipod_classic_usb_setup_event(IPodClassicUSBState *s,
    const uint8_t setup[8], IPodUSBSetupDMA dma, void *opaque)
{
    IPodUSBSetupResult result;
    assert(qemu_mutex_iothread_locked());
    /* Replaces the old receiver/provenance block: exactly one core event. */
    result = ipod_zlp_setup_event(&s->status_in, &s->regs, setup, dma, opaque);
    usb_update(s); /* Existing derived OUT.STUP IRQ, never an invented IN ACK. */
    return result;
}
#include "ipod_classic_usb_dma_selftest.h"
static void usb_update(IPodClassicUSBState *s)
{
    qemu_set_irq(s->irq, ipod_usb_irq(&s->regs));
}
#include "native_bulk.inc"
static bool usb_setup_dma(void *opaque, uint32_t addr, const uint8_t setup[8])
{
    const uint64_t end = (uint64_t)addr + 8;
    bool in_window =
        (addr >= SDRAM_MEM_BASE && end <= (uint64_t)SDRAM_MEM_BASE + SDRAM_MEM_SIZE) ||
        (addr >= SDRAM_UNCACHED_MEM_BASE && end <= (uint64_t)SDRAM_UNCACHED_MEM_BASE + SDRAM_MEM_SIZE) ||
        (addr >= IRAM0_MEM_BASE && end <= (uint64_t)IRAM0_MEM_BASE + IRAM0_MEM_SIZE) ||
        (addr >= IRAM1_MEM_BASE && end <= (uint64_t)IRAM1_MEM_BASE + IRAM1_MEM_SIZE);
    hwaddr xlat, span = 8;
    MemoryRegion *mr;
    bool ok = false;

    /* QOM/QMP setter runs under BQL. The map stays fixed through preflight
     * and DMA. RCU protects the translated region's lifetime. */
    assert(qemu_mutex_iothread_locked());
    if ((addr & 3) || end > (1ULL << 32) || !in_window) {
        return false;
    }
    rcu_read_lock();
    mr = address_space_translate(&address_space_memory, addr, &xlat, &span,
                                 true, MEMTXATTRS_UNSPECIFIED);
    /* Mapping validity alone accepts io_floor (writes discarded). Require
     * one complete direct writable plain-RAM span, including backing bounds.
     * QEMU physmem direct-RAM path then performs one copy and dirty marking,
     * without MMIO callbacks, map changes or a partial late-error branch. */
    if (span >= 8 && memory_access_is_direct(mr, true) &&
        xlat <= memory_region_size(mr) &&
        memory_region_size(mr) - xlat >= 8) {
        ok = dma_memory_write(&address_space_memory, addr, setup, 8,
                              MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
    }
    rcu_read_unlock();
    return ok;
}
IPodUSBSetupResult ipod_classic_usb_setup_ingress(IPodClassicUSBState *s,
    const uint8_t setup[8])
{
    return ipod_classic_usb_setup_event(s, setup, usb_setup_dma, s);
}
static void usb_host_setup_set(Object *obj, const char *value, Error **errp)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(obj);
    uint8_t setup[8];
    IPodUSBSetupResult result;

    if (strlen(value) != 16) {
        error_setg(errp, "host-setup requires exactly 8 bytes as 16 hex digits");
        return;
    }
    for (unsigned i = 0; i < 8; i++) {
        int hi = g_ascii_xdigit_value(value[2 * i]);
        int lo = g_ascii_xdigit_value(value[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            error_setg(errp, "host-setup contains a non-hex digit");
            return;
        }
        setup[i] = (hi << 4) | lo;
    }
    result = ipod_classic_usb_setup_event(s, setup, usb_setup_dma, s);
    fprintf(stderr, "USBDEV source=test-host setup=%s result=%s dma=%08x tsiz=%08x int=%08x irq=%d\n",
            value, ipod_usb_setup_result_name(result), s->regs.out_dma[0],
            s->regs.out_tsiz[0], s->regs.out_int[0], ipod_usb_irq(&s->regs));
    if (result != IPOD_USB_SETUP_OK) {
        error_setg(errp, "host-setup rejected: %s", ipod_usb_setup_result_name(result));
    }
}

/* Shared causal PHY/link model. No packet/IRQ/DMA/UI action occurs here. */
static void usb_link_refresh(void *opaque)
{
    IPodClassicUSBState *s = opaque;
    bool clocks = false;
    int available, hprt;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->experimental_link_v0) { return; }
    if (s->link_clock) {
        clocks = s->link_clock->usb_gate0_written &&
                 s->link_clock->usb_gate1_written &&
                 !(s->link_clock->reg[S5L8702_CLK_PWRCON0 / 4] & (1u << 2)) &&
                 !(s->link_clock->reg[S5L8702_CLK_PWRCON1 / 4] & (1u << 3));
    }
    ipod_link_update(&s->link_v0, s->regs.vbus, clocks, now);
    available = ipod_link_available(&s->link_v0, now);
    hprt = ipod_link_hprt(&s->link_v0, now, s->regs.dctl,
                         s->regs.pcgcctl, s->regs.gusbcfg);
    if ((available != s->link_last_available || hprt != s->link_last_hprt) &&
        s->link_diagnostics < 256) {
        fprintf(stderr, "USBLINK hypothesis=v0 available=%d hprt=%03x vbus=%d soc_clocks=%d power=%08x reset=%08x ref=%08x dctl=%08x pcgcctl=%08x deadline=%" PRId64 " vns=%" PRId64 "\n",
            available, hprt, s->regs.vbus, clocks, s->link_v0.phy[0],
            s->link_v0.phy[2], s->link_v0.phy[1], s->regs.dctl,
            s->regs.pcgcctl, s->link_v0.deadline, now);
        s->link_diagnostics++;
    }
    s->link_last_available = available; s->link_last_hprt = hprt;
}
void ipod_classic_usb_link_attach(IPodClassicUSBState *s, IPodClassicClkState *clock)
{
    assert(s->experimental_link_v0 && !s->link_clock && clock);
    assert(!clock->usb_link_notify);
    s->link_clock = clock;
    clock->usb_link_notify = usb_link_refresh;
    clock->usb_link_opaque = s;
    usb_link_refresh(s);
}
static uint64_t usb_link_phy_read(void *opaque, hwaddr off, unsigned size)
{
    IPodClassicUSBState *s = opaque;
    uint32_t value = 0;
    bool supported;
    usb_link_refresh(s);
    supported = size == 4 && !(off & 3) &&
        ipod_link_phy_read(&s->link_v0, off,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &value);
    ipod_mmio_observe(IPOD_TRACE_PHY, off, value, size, false, supported);
    if (!supported && s->link_diagnostics++ < 256) {
        fprintf(stderr, "USBLINK hypothesis=v0 unknown-read off=%" HWADDR_PRIx "\n", off);
    }
    return value;
}
static void usb_link_phy_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPodClassicUSBState *s = opaque;
    bool supported;
    usb_link_refresh(s);
    if (size != 4 || (off & 3)) {
        s->link_v0.ancillary_known = false;
        supported = false;
    } else {
        supported = ipod_link_phy_write(&s->link_v0, off, value,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    ipod_mmio_observe(IPOD_TRACE_PHY, off, value, size, true, supported);
    if (!supported && s->link_diagnostics++ < 256) {
        fprintf(stderr, "USBLINK hypothesis=v0 unknown-write off=%" HWADDR_PRIx " value=%08x availability-failclosed=1\n", off, (uint32_t)value);
    }
    usb_link_refresh(s);
}
static const MemoryRegionOps usb_link_phy_ops = {
    .read = usb_link_phy_read, .write = usb_link_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 8,
    .impl.min_access_size = 1, .impl.max_access_size = 8,
    .valid.unaligned = true,
};
static uint64_t usb_read(void *opaque, hwaddr off, unsigned size)
{
    IPodClassicUSBState *s = opaque;
    uint32_t value = 0;
    bool supported = ipod_usb_read(&s->regs, off, &value);
    if (s->experimental_link_v0 && off == 0x440) {
        usb_link_refresh(s);
        value = ipod_link_hprt(&s->link_v0, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            s->regs.dctl, s->regs.pcgcctl, s->regs.gusbcfg);
        supported = true;
    }
    if (!supported) {
        qemu_log_mask(LOG_UNIMP, "USBDEV unsupported read offset=0x%03x\n", (unsigned)off);
    }
    if (getenv("USBTRACE")) {
        fprintf(stderr, "USBDEV read offset=%03x value=%08x supported=%d\n",
                (unsigned)off, value, supported);
    }
    ipod_mmio_observe(IPOD_TRACE_CORE, off, value, size, false, supported);
    if(supported && off==0xb10)ipod_classic_usb_rx_journal(s,"guest_read_tsiz",off,value,NULL);
    return value;
}
static void usb_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPodClassicUSBState *s = opaque;
    /* Replaces the old core-write + manual provenance block, not an extra write. */
    bool supported = ipod_zlp_write_event(&s->status_in, &s->regs, off, value);
    if (supported) usb_bulk_guest_write(s,off,value);
    if (supported) usb_ep0_cause_observe(s,off,value);
    if(supported && (off==0xb08 || off==0xb10 || off==0xb00))ipod_classic_usb_rx_journal(s,"guest_write",off,value,NULL);
    if (supported && s->experimental_link_v0) {
        ipod_link_core_write(&s->link_v0, off, value);
    }
    if(supported&&((off==0x10&&(value&1))||(off==0x804&&(s->regs.dctl&2))))
        ipod_usb_notify_context_lost(s); /* wrapper already performed full clear */
    if (!supported) {
        qemu_log_mask(LOG_UNIMP, "USBDEV unsupported write offset=0x%03x value=%08x\n",
                      (unsigned)off, (uint32_t)value);
    }
    if (supported && off==0x10 && (value&1)) s->desktop_sdis_seen=false;
    if (supported && off==0x804 && (s->regs.dctl&2)) s->desktop_sdis_seen=true;
    ipod_mmio_observe(IPOD_TRACE_CORE, off, value, size, true, supported);
    usb_update(s);
    if (getenv("USBTRACE")) {
        fprintf(stderr, "USBDEV write offset=%03x value=%08x supported=%d pending=%08x mask=%08x global=%d irq=%d\n",
                (unsigned)off, (uint32_t)value, supported,
                ipod_usb_status(&s->regs), s->regs.gintmsk,
                !!(s->regs.gahbcfg & 1), ipod_usb_irq(&s->regs));
        if (off == 0x804 || off == 0xe00) {
            fprintf(stderr, "USBDEV control dctl=%08x pcgcctl=%08x in-nak-requested=%d out-nak-requested=%d\n",
                    s->regs.dctl, s->regs.pcgcctl,
                    s->regs.in_nak_requested, s->regs.out_nak_requested);
        }
    }
    usb_link_refresh(s);
}
static void usb_vbus_input(void *opaque, int n, int level)
{
    IPodClassicUSBState *s = opaque;
    bool was_vbus = s->regs.vbus;
    ipod_usb_vbus(&s->regs, !!level);
    usb_link_refresh(s);
    if (was_vbus && !s->regs.vbus) {
        s->desktop_sdis_seen=false;
        if (s->host_bus_owner) {
            ipod_classic_usb_host_bus_release(s, s->host_bus_owner,
                s->host_bus_token, "pmu-vbus-fall");
        }
        ipod_usb_ep0_lifecycle_invalidate(s);
    }
    ipod_mmio_observe_input("vbus", !!level, true);
    usb_update(s);
    if (getenv("USBTRACE")) {
        fprintf(stderr, "USBDEV vbus=%d pending=%08x irq=%d\n",
                s->regs.vbus, ipod_usb_status(&s->regs), ipod_usb_irq(&s->regs));
    }
}
static bool usb_connected_get(Object *obj, Error **errp)
{
    return IPOD_CLASSIC_USB(obj)->regs.vbus;
}
static bool usb_reset_get(Object *obj, Error **errp)
{
    return IPOD_CLASSIC_USB(obj)->regs.reset_line;
}
static void usb_reset_line_apply(IPodClassicUSBState *s, bool value)
{
    bool was_reset = s->regs.reset_line;
    bool event = ipod_usb_bus_reset(&s->regs, value);
    if (!was_reset && s->regs.reset_line) {
        ipod_usb_ep0_lifecycle_invalidate(s);
    }
    ipod_mmio_observe_input("bus-reset", value, event);
    usb_update(s);
    if (getenv("USBTRACE")) {
        fprintf(stderr, "USBDEV bus-reset-line=%d accepted=%d vbus=%d pending=%08x irq=%d\n",
                value, event, s->regs.vbus, ipod_usb_status(&s->regs),
                ipod_usb_irq(&s->regs));
    }
}

/* Sole external producer binding. Tokens are lifetime identifiers, not secrets.
 * The one terminal native session and exact token/sequence prevent replay.
 * Existing core emits modeled USBRST at ASSERT, not after qualified duration.
 * Neither short-pulse reporting nor release can undo that existing limitation. */
static uint64_t host_bus_next_token = 1;
bool ipod_classic_usb_host_bus_acquire(IPodClassicUSBState *s, void *owner,
    uint64_t *token)
{
    assert(qemu_mutex_iothread_locked());
    if (!owner || !token || !DEVICE(s)->realized || s->context_lost_dead ||
        !s->in_stage.valid || s->host_bus_owner || s->regs.reset_line ||
        !host_bus_next_token) { return false; }
    s->host_bus_owner = owner;
    s->host_bus_token = host_bus_next_token++;
    memset(&s->host_bus_policy, 0, sizeof(s->host_bus_policy));
    object_ref(OBJECT(s)); /* released exactly once on abort/close/unrealize */
    *token = s->host_bus_token;
    return true;
}
IPodHostBusResult ipod_classic_usb_host_bus_event(IPodClassicUSBState *s,
    void *owner, uint64_t token, uint64_t sequence, bool asserted)
{
    IPodHostBusResult result;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    assert(qemu_mutex_iothread_locked());
    if (!owner || s->host_bus_owner != owner || !token ||
        s->host_bus_token != token || s->context_lost_dead ||
        !DEVICE(s)->realized) { return IPOD_HOST_BUS_OWNER; }
    if (s->regs.reset_line != s->host_bus_policy.asserted) {
        return IPOD_HOST_BUS_STATE;
    }
    if (asserted && getenv("IPOD_USB_DESKTOP_SERVICE") &&
        !strcmp(getenv("IPOD_USB_DESKTOP_SERVICE"),"1")) {
        uint32_t state[8];uint64_t observed;
        ipod_classic_usb_link_snapshot(s,state,&observed);
        if (!state[6] || !state[7] || !state[1] || state[2] || state[3])
            return IPOD_HOST_BUS_STATE;
        fprintf(stderr,"HOSTRECEIVER token=%"PRIu64" available=%u soft_qualified=%u dctl=%08x pcgcctl=%08x vbus=%u reset=%u host_mode=%u vns=%"PRIu64"\n",
            token,state[6],state[7],state[4],state[5],state[1],state[2],state[3],observed);
    }
    now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    result = ipod_host_bus_event_min(&s->host_bus_policy, sequence, asserted, now,
        s->regs.vbus, !!(s->regs.gusbcfg & (1u << 29)),
        s->root_port_reset ? INT64_C(50000000) : IPOD_HOST_BUS_MIN_RESET_NS);
    if (result == IPOD_HOST_BUS_OK || result == IPOD_HOST_BUS_SHORT) {
        object_ref(OBJECT(s)); /* lifecycle/IRQ callbacks may revoke lease */
        usb_reset_line_apply(s, asserted);
        if (s->host_bus_owner != owner || s->host_bus_token != token ||
            s->regs.reset_line != asserted || s->context_lost_dead) {
            result = IPOD_HOST_BUS_OWNER;
        }
        fprintf(stderr, "HOSTBUS source=%s token=%" PRIu64
            " seq=%" PRIu64 " asserted=%d result=%u vns=%" PRId64 "\n",
            s->root_port_reset ? "emulated-root-port" : "emulated-downstream-hub",
            token, sequence, asserted, result, now);
        object_unref(OBJECT(s));
    }
    return result;
}
bool ipod_classic_usb_host_bus_ready(IPodClassicUSBState *s, void *owner,
    uint64_t token)
{
    assert(qemu_mutex_iothread_locked());
    return owner && s->host_bus_owner == owner && token &&
        s->host_bus_token == token && !s->context_lost_dead &&
        DEVICE(s)->realized && s->regs.vbus && !s->regs.reset_line &&
        !(s->regs.gusbcfg & (1u << 29)) &&
        ipod_host_bus_ready(&s->host_bus_policy,
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
bool ipod_classic_usb_host_bus_release(IPodClassicUSBState *s, void *owner,
    uint64_t token, const char *reason)
{
    bool withdrew;
    assert(qemu_mutex_iothread_locked());
    if (!owner || s->host_bus_owner != owner || !token ||
        s->host_bus_token != token) { return false; }
    withdrew = s->host_bus_policy.asserted && s->regs.reset_line;
    if (withdrew) { usb_reset_line_apply(s, false); }
    ipod_classic_usb_bulk_invalidate(s);
    s->root_port_reset=false;
    s->host_bus_owner = NULL;
    s->host_bus_token = 0;
    memset(&s->host_bus_policy, 0, sizeof(s->host_bus_policy));
    fprintf(stderr, "HOSTBUS abort=1 completed=0 reason=%s withdrew=%d token=%"
        PRIu64 " vns=%" PRId64 "\n", reason, withdrew, token,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    object_unref(OBJECT(s));
    return true;
}
static void usb_reset_set(Object *obj, bool value, Error **errp)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(obj);
    if (s->host_bus_owner) {
        error_setg(errp, "bus-reset test property unavailable while native producer owns line");
        return;
    }
    usb_reset_line_apply(s, value);
}
static const MemoryRegionOps usb_ops = {
    .read = usb_read, .write = usb_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
};
static void usb_reset(DeviceState *dev)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(dev);
    if (s->host_bus_owner) {
        ipod_classic_usb_host_bus_release(s, s->host_bus_owner,
            s->host_bus_token, "machine-reset");
    }
    ipod_usb_ep0_lifecycle_invalidate(s);
    ipod_usb_regs_reset(&s->regs);
    s->desktop_sdis_seen=false;
    if (s->experimental_link_v0) {
        ipod_link_init(&s->link_v0);
        s->link_last_available = s->link_last_hprt = -1;
        usb_link_refresh(s);
    }
    usb_update(s);
}
static int usb_pre_save(void *opaque)
{
    IPodClassicUSBState *s = opaque;
    return (s->experimental_link_v0 || s->in_stage.active || s->status_in.active || s->host_bus_owner) ? -EBUSY : 0;
}
static int usb_post_load(void *opaque, int version)
{
    IPodClassicUSBState *s = opaque;
    if (s->experimental_link_v0) { return -EINVAL; }
    ipod_usb_ep0_lifecycle_invalidate(s);
    if (version < 2) { ipod_usb_ep_regs_reset(&s->regs); }
    if (version < 3) { ipod_usb_control_regs_reset(&s->regs); }
    usb_update(s);
    return 0;
}
static const VMStateDescription usb_vmstate = {
    .name = TYPE_IPOD_CLASSIC_USB, .version_id = 3, .minimum_version_id = 1,
    .pre_save = usb_pre_save, .post_load = usb_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(regs.gahbcfg, IPodClassicUSBState),
        VMSTATE_UINT32(regs.gusbcfg, IPodClassicUSBState),
        VMSTATE_UINT32(regs.gintmsk, IPodClassicUSBState),
        VMSTATE_UINT32(regs.pending, IPodClassicUSBState),
        VMSTATE_UINT32(regs.gotgint, IPodClassicUSBState),
        VMSTATE_BOOL(regs.vbus, IPodClassicUSBState),
        VMSTATE_BOOL(regs.reset_line, IPodClassicUSBState),
        VMSTATE_UINT32_V(regs.dcfg, IPodClassicUSBState, 2),
        VMSTATE_UINT32_V(regs.diepmsk, IPodClassicUSBState, 2),
        VMSTATE_UINT32_V(regs.doepmsk, IPodClassicUSBState, 2),
        VMSTATE_UINT32_V(regs.daintmsk, IPodClassicUSBState, 2),
        VMSTATE_UINT32_ARRAY_V(regs.in_ctl, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.out_ctl, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.in_int, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.out_int, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.in_tsiz, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.out_tsiz, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.in_dma, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_ARRAY_V(regs.out_dma, IPodClassicUSBState, IPOD_DWC_EP_SLOTS, 2),
        VMSTATE_UINT32_V(regs.dctl, IPodClassicUSBState, 3),
        VMSTATE_UINT32_V(regs.pcgcctl, IPodClassicUSBState, 3),
        VMSTATE_BOOL_V(regs.in_nak_requested, IPodClassicUSBState, 3),
        VMSTATE_BOOL_V(regs.out_nak_requested, IPodClassicUSBState, 3),
        VMSTATE_END_OF_LIST()
    }
};
static void usb_init(Object *obj)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(obj);
    /* One-time instance initialization, never a realize/reset shortcut. */
    memset(&s->status_in, 0, sizeof(s->status_in));
    s->next_context_lost_token=1;
    memory_region_init_io(&s->iomem, obj, &usb_ops, s, TYPE_IPOD_CLASSIC_USB, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    memory_region_init_io(&s->phy_iomem, obj, &usb_link_phy_ops, s,
                          "s5l8702-usb-link-v0-phy", 0x104);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->phy_iomem);
    ipod_link_init(&s->link_v0);
    s->link_last_available = s->link_last_hprt = -1;
    qdev_init_gpio_in_named(DEVICE(s), usb_vbus_input, "vbus", 1);
    object_property_add_bool(obj, "vbus-present", usb_connected_get, NULL);
    object_property_add_bool(obj, "bus-reset", usb_reset_get, usb_reset_set);
    object_property_set_description(obj, "bus-reset",
        "External host bus-reset line (test API); no packets or enumeration synthesized");
    if (g_strcmp0(getenv("IPOD_USB_TEST_HOST"), "1") == 0) {
        object_property_add_bool(obj, "dma-read-selftest", NULL, ipod_usb_dma_selftest_set);
        object_property_add_str(obj, "dma-read-selftest-result", ipod_usb_dma_selftest_get, NULL);
        object_property_add_str(obj, "host-setup", NULL, usb_host_setup_set);
        object_property_set_description(obj, "host-setup",
            "Diagnostic external 8-byte SETUP ingress only; no USB ACK, response or URB completion");
    }
}
static void usb_realize(DeviceState *dev, Error **errp)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(dev);
    uint64_t owner;
    assert(qemu_mutex_iothread_locked());
    if (s->in_stage.constructed) {
        error_setg(errp, "USB IN staging constructor cannot be repeated"); return;
    }
    owner = ipod_usb_in_take_counter(&ipod_usb_in_next_owner);
    if (!owner || !ipod_usb_in_stage_init(&s->in_stage, owner, 1)) {
        error_setg(errp, "USB IN owner namespace exhausted"); return;
    }
    s->next_in_id = 1;
    if (!ipod_zlp_init(&s->status_in, &s->in_stage, &s->arm, &s->next_in_id)) {
        s->in_stage.valid = false;
        error_setg(errp, "USB ZLP pending constructor cannot be repeated");
        return;
    }
}
static void usb_unrealize(DeviceState *dev)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(dev);
    if (s->host_bus_owner) {
        ipod_classic_usb_host_bus_release(s, s->host_bus_owner,
            s->host_bus_token, "controller-unrealize");
    }
    if (s->link_clock) {
        s->link_clock->usb_link_notify = NULL;
        s->link_clock->usb_link_opaque = NULL;
        s->link_clock = NULL;
    }
    s->context_lost_dead=true; /* no new observer may bind during teardown */
    ipod_usb_ep0_lifecycle_invalidate(s);
    s->context_lost=NULL;s->context_lost_opaque=NULL;s->context_lost_token=0;
    s->in_stage.valid = false;
    /* No backend exists now. A future backend must retain QOM refs and drain
     * its callbacks before releasing the final device reference/unrealizing. */
}
static Property usb_link_properties[] = {
    DEFINE_PROP_BOOL("experimental-link-v0", IPodClassicUSBState, experimental_link_v0, false),
    DEFINE_PROP_END_OF_LIST(),
};
static void usb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, usb_link_properties);
    dc->realize = usb_realize;
    dc->unrealize = usb_unrealize;
    dc->reset = usb_reset;
    dc->vmsd = &usb_vmstate;
}
static const TypeInfo usb_type = {
    .name = TYPE_IPOD_CLASSIC_USB, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicUSBState),
    .instance_init = usb_init, .class_init = usb_class_init,
};
static void usb_register_types(void) { type_register_static(&usb_type); }
type_init(usb_register_types)


/* Abstract native control peer, distinct from the legacy USB/IP URB adapter.
 * Caller owns a live bus lease; effects below are ordinary controller tokens.
 */
int ipod_classic_usb_peer_begin(IPodClassicUSBState *s,IPodNativeControl *p,
                               const uint8_t setup[8],uint64_t id)
{
    IPodUSBSetupResult result;
    assert(qemu_mutex_iothread_locked());
    usb_link_refresh(s);
    if ((s->experimental_link_v0 && !s->link_last_available) ||
        p->open || !id || !ipod_peer_setup_supported(setup)) return IPOD_PEER_MODE;
    result=ipod_classic_usb_setup_ingress(s,setup);
    if (result!=IPOD_USB_SETUP_OK) return 100+(int)result;
    return ipod_peer_bind(p,setup,id,s->arm.setup_seq);
}
int ipod_classic_usb_peer_poll(IPodClassicUSBState *s,IPodNativeControl *p)
{
    assert(qemu_mutex_iothread_locked());
    usb_link_refresh(s);
    if (s->experimental_link_v0 && !s->link_last_available) return IPOD_PEER_MODE;
    return ipod_peer_stage(p,&s->regs,&s->arm,s->in_stage.generation,
                           ipod_usb_in_dma_read,&address_space_memory);
}
int ipod_classic_usb_peer_ack(IPodClassicUSBState *s,IPodNativeControl *p,
                             uint64_t nonce)
{
    int result;
    assert(qemu_mutex_iothread_locked());
    usb_link_refresh(s);
    if (s->experimental_link_v0 && !s->link_last_available) return IPOD_PEER_MODE;
    result=ipod_peer_ack(p,&s->regs,&s->arm,s->in_stage.generation,nonce);
    if (result==IPOD_PEER_OK) {ipod_usb_in_invalidate(s);usb_update(s);}
    return result;
}
int ipod_classic_usb_peer_status_out(IPodClassicUSBState *s,IPodNativeControl *p)
{
    int result;
    assert(qemu_mutex_iothread_locked());
    usb_link_refresh(s);
    if (s->experimental_link_v0 && !s->link_last_available) return IPOD_PEER_MODE;
    result=ipod_peer_status_out(p,&s->regs,&s->arm);
    if (result==IPOD_PEER_OK) usb_update(s);
    return result;
}

/* Current facts read under the same BQL as ASSERT. No register/RAM/IRQ writes.
 * Packet clock/arming gates are deliberately separate and remain unchanged. */
bool ipod_classic_usb_link_snapshot(IPodClassicUSBState *s,uint32_t out[8],uint64_t *vns)
{
    assert(qemu_mutex_iothread_locked());
    usb_link_refresh(s);
    out[0]=2;out[1]=s->regs.vbus;out[2]=s->regs.reset_line;
    out[3]=!!(s->regs.gusbcfg&(1u<<29));out[4]=s->regs.dctl;out[5]=s->regs.pcgcctl;
    out[6]=s->experimental_link_v0 && s->link_last_available;
    out[7]=s->experimental_link_v0 && s->desktop_sdis_seen &&
        s->link_v0.core_reset_seen && s->link_v0.guest_connected && !(s->regs.dctl&2);
    *vns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return true;
}

#include "ep0_cause.inc"

#include "rx_journal.inc"
