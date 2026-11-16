/* Opt-in DWC device core; diagnostic single-SETUP ingress, no USB host bridge. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/irq.h"
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
static void usb_reset(DeviceState *dev);
static void usb_realize(DeviceState *dev, Error **errp);
static int usb_pre_save(void *opaque);
static int usb_post_load(void *opaque, int version);
static void usb_update(IPodClassicUSBState *s);
static uint64_t ipod_usb_in_next_owner = 1;
static uint64_t ipod_usb_in_take_counter(uint64_t *next)
{
    uint64_t value = *next;
    if (!value || value == UINT64_MAX) { return 0; }
    *next = value + 1;
    return value;
}
static void ipod_usb_in_invalidate(IPodClassicUSBState *s)
{
    assert(qemu_mutex_iothread_locked());
    if (s->in_stage.constructed && s->in_stage.valid) {
        /* Invalid/overflow generation poisons and drops the transaction. */
        ipod_usb_in_stage_invalidate(&s->in_stage, s->in_stage.owner,
                                     s->in_stage.generation + 1);
    }
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
    if (s->in_stage.active) { return IPOD_USB_IN_BUSY; }
    id = ipod_usb_in_take_counter(&s->next_in_id);
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
IPodUSBSetupResult ipod_classic_usb_setup_event(IPodClassicUSBState *s,
    const uint8_t setup[8], IPodUSBSetupDMA dma, void *opaque)
{
    IPodUSBSetupResult result;
    assert(qemu_mutex_iothread_locked());
    result = ipod_usb_receive_setup(&s->regs, setup, dma, opaque);
    if (result == IPOD_USB_SETUP_OK) { ipod_usb_in_invalidate(s); }
    usb_update(s); /* Existing derived OUT.STUP IRQ, never an invented IN ACK. */
    return result;
}
#include "ipod_classic_usb_dma_selftest.h"
static void usb_update(IPodClassicUSBState *s)
{
    qemu_set_irq(s->irq, ipod_usb_irq(&s->regs));
}
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
static uint64_t usb_read(void *opaque, hwaddr off, unsigned size)
{
    IPodClassicUSBState *s = opaque;
    uint32_t value = 0;
    bool supported = ipod_usb_read(&s->regs, off, &value);
    if (!supported) {
        qemu_log_mask(LOG_UNIMP, "USBDEV unsupported read offset=0x%03x\n", (unsigned)off);
    }
    if (getenv("USBTRACE")) {
        fprintf(stderr, "USBDEV read offset=%03x value=%08x supported=%d\n",
                (unsigned)off, value, supported);
    }
    return value;
}
static void usb_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPodClassicUSBState *s = opaque;
    bool supported = ipod_usb_write(&s->regs, off, value);
    if (supported && (off == 0x900 || off == 0x910 || off == 0x914 ||
        (off == 0x10 && (value & 1)) || (off == 0x804 && (s->regs.dctl & 2)))) {
        /* Conservative internal transaction invalidation, not new HW events.
         * Accepted same-value IN0 reprogramming also ends the old generation.
         * IRQ/endpoint masks are deliberately excluded. */
        ipod_usb_in_invalidate(s);
    }
    if (!supported) {
        qemu_log_mask(LOG_UNIMP, "USBDEV unsupported write offset=0x%03x value=%08x\n",
                      (unsigned)off, (uint32_t)value);
    }
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
}
static void usb_vbus_input(void *opaque, int n, int level)
{
    IPodClassicUSBState *s = opaque;
    bool was_vbus = s->regs.vbus;
    ipod_usb_vbus(&s->regs, !!level);
    if (was_vbus && !s->regs.vbus) { ipod_usb_in_invalidate(s); }
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
static void usb_reset_set(Object *obj, bool value, Error **errp)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(obj);
    bool was_reset = s->regs.reset_line;
    bool event = ipod_usb_bus_reset(&s->regs, value);
    if (!was_reset && s->regs.reset_line) { ipod_usb_in_invalidate(s); }
    usb_update(s);
    if (getenv("USBTRACE")) {
        fprintf(stderr, "USBDEV bus-reset-line=%d accepted=%d vbus=%d pending=%08x irq=%d\n",
                value, event, s->regs.vbus, ipod_usb_status(&s->regs),
                ipod_usb_irq(&s->regs));
    }
}
static const MemoryRegionOps usb_ops = {
    .read = usb_read, .write = usb_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
};
static void usb_reset(DeviceState *dev)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(dev);
    ipod_usb_in_invalidate(s);
    ipod_usb_regs_reset(&s->regs);
    usb_update(s);
}
static int usb_pre_save(void *opaque)
{
    IPodClassicUSBState *s = opaque;
    return s->in_stage.active ? -EBUSY : 0;
}
static int usb_post_load(void *opaque, int version)
{
    IPodClassicUSBState *s = opaque;
    ipod_usb_in_invalidate(s);
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
    memory_region_init_io(&s->iomem, obj, &usb_ops, s, TYPE_IPOD_CLASSIC_USB, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
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
}
static void usb_unrealize(DeviceState *dev)
{
    IPodClassicUSBState *s = IPOD_CLASSIC_USB(dev);
    ipod_usb_in_invalidate(s);
    s->in_stage.valid = false;
    /* No backend exists now. A future backend must retain QOM refs and drain
     * its callbacks before releasing the final device reference/unrealizing. */
}
static void usb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
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
