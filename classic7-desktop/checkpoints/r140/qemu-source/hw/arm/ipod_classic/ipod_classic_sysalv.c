/* S5L8702 SYSALV/EIC. Register and GPIO mapping:
 * Rockbox firmware/export/s5l87xx.h (EIC section), gpio-s5l8702.c.
 * No input is invented: only connected device pins produce status/IRQs.
 * GPIO4 is deliberately not connected to keyboard/wheel events.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "hw/arm/ipod_classic/ipod_classic_sysalv.h"

static uint32_t supported(unsigned g)
{
    if (g == 0) { return 5; } /* ALIVE and USB internal sources */
    if (g == 1 || g == 2) { return 0; }
    if (g == 3) { return 0xffffff0f; } /* GPIO124..127 unsupported */
    return UINT32_MAX;
}
static uint32_t status(IPodClassicSysalvState *s, unsigned g)
{
    uint32_t active = ~(s->input[g] ^ s->level[g]) & s->valid[g];
    return ((s->edge[g] & ~s->type[g]) | (active & s->type[g])) & supported(g);
}
static void update_irq(IPodClassicSysalvState *s, unsigned g)
{
    qemu_set_irq(s->irq[g], (status(s, g) & s->enable[g]) != 0);
}
static void input_pin(void *opaque, int n, int value)
{
    IPodClassicSysalvState *s = opaque;
    unsigned g = 6 - (n >> 5);
    uint32_t bit = 1u << ((0x18 - (n & 0x18)) | (n & 7));
    bool old = (s->input[g] & bit) != 0;
    value = !!value;
    if ((s->valid[g] & bit) && old != value &&
        !(s->type[g] & bit) && value == !!(s->level[g] & bit)) {
        s->edge[g] |= bit;
    }
    s->valid[g] |= bit;
    if (value) { s->input[g] |= bit; } else { s->input[g] &= ~bit; }
    update_irq(s, g);
}
static uint64_t sysalv_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodClassicSysalvState *s = opaque;
    if (offset == 0) {
        if (getenv("ALVTRACE") && s->trace_read_pending) {
            fprintf(stderr, "SYSALV read ALVCON=0x%08x vns=%" PRId64 "\n",
                    s->alvcon, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            s->trace_read_pending = false;
        }
        return s->alvcon;
    }
    unsigned g = (offset & 0x1f) / 4;
    if (g >= 7) { return 0; }
    switch (offset & ~0x1f) {
    case 0x80: return s->level[g];
    case 0xa0: return status(s, g);
    case 0xc0: return s->enable[g];
    case 0xe0: return s->type[g];
    default: return 0; /* timers and other ALIVE registers unmodelled */
    }
}
static void sysalv_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodClassicSysalvState *s = opaque;
    if (offset == 0) {
        uint32_t old = s->alvcon;
        s->alvcon = value;
        s->trace_read_pending = true;
        if (getenv("ALVTRACE")) {
            fprintf(stderr, "SYSALV write ALVCON old=0x%08x new=0x%08x vns=%"
                    PRId64 "\n", old, s->alvcon, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        return;
    }
    unsigned g = (offset & 0x1f) / 4;
    if (g >= 7) { return; }
    uint32_t v = value & supported(g);
    switch (offset & ~0x1f) {
    case 0x80: s->level[g] = v; break;
    case 0xa0: s->edge[g] &= ~v; break; /* level sources must deassert */
    case 0xc0: s->enable[g] = v; break;
    case 0xe0: s->type[g] = v; break;
    default: return;
    }
    update_irq(s, g);
    if (getenv("EICTRACE")) {
        CPUState *cs = current_cpu;
        /* Observed PC is the TB head, not the precise MMIO instruction. */
        uint32_t tbpc = cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;
        uint32_t r0 = cs ? ARM_CPU(cs)->env.regs[0] : 0;
        uint32_t r1 = cs ? ARM_CPU(cs)->env.regs[1] : 0;
        uint32_t lr = cs ? ARM_CPU(cs)->env.regs[14] : 0;
        fprintf(stderr, "EIC wr offset=%03x value=%08x group=%u status=%08x enable=%08x"
                " cpu_context=%u observedTBpc~=%08x lr=%08x r0=%08x r1=%08x\n",
                (unsigned)offset, (uint32_t)value, g, status(s, g), s->enable[g],
                cs != NULL, tbpc, lr, r0, r1);
    }
}
static const MemoryRegionOps sysalv_ops = {
    .read = sysalv_read, .write = sysalv_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
};
static void sysalv_reset(DeviceState *dev)
{
    IPodClassicSysalvState *s = IPOD_CLASSIC_SYSALV(dev);
    s->alvcon = 0;
    s->trace_read_pending = false;
    memset(s->level, 0, sizeof(s->level));
    memset(s->type, 0, sizeof(s->type));
    memset(s->enable, 0, sizeof(s->enable));
    memset(s->edge, 0, sizeof(s->edge));
    for (unsigned g = 0; g < 7; g++) { update_irq(s, g); }
}
static int sysalv_post_load(void *opaque, int version_id)
{
    IPodClassicSysalvState *s = opaque;
    for (unsigned g = 0; g < 7; g++) { update_irq(s, g); }
    return 0;
}
static void sysalv_init(Object *obj)
{
    IPodClassicSysalvState *s = IPOD_CLASSIC_SYSALV(obj);
    memory_region_init_io(&s->iomem, obj, &sysalv_ops, s, TYPE_IPOD_CLASSIC_SYSALV, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned g = 0; g < 7; g++) { sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[g]); }
    qdev_init_gpio_in_named(DEVICE(obj), input_pin, "gpio", 224);
}
static const VMStateDescription vmstate_sysalv = {
    .name = TYPE_IPOD_CLASSIC_SYSALV, .version_id = 2, .minimum_version_id = 1,
    .post_load = sysalv_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(alvcon, IPodClassicSysalvState),
        VMSTATE_UINT32_ARRAY_V(level, IPodClassicSysalvState, 7, 2),
        VMSTATE_UINT32_ARRAY_V(type, IPodClassicSysalvState, 7, 2),
        VMSTATE_UINT32_ARRAY_V(enable, IPodClassicSysalvState, 7, 2),
        VMSTATE_UINT32_ARRAY_V(edge, IPodClassicSysalvState, 7, 2),
        VMSTATE_UINT32_ARRAY_V(input, IPodClassicSysalvState, 7, 2),
        VMSTATE_UINT32_ARRAY_V(valid, IPodClassicSysalvState, 7, 2),
        VMSTATE_END_OF_LIST()
    },
};
static void sysalv_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "S5L8702 SYSALV ALVCON and external interrupt controller";
    dc->reset = sysalv_reset;
    dc->vmsd = &vmstate_sysalv;
}
static const TypeInfo sysalv_info = {
    .name = TYPE_IPOD_CLASSIC_SYSALV, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicSysalvState), .instance_init = sysalv_init,
    .class_init = sysalv_class_init,
};
static void sysalv_register_types(void) { type_register_static(&sysalv_info); }
type_init(sysalv_register_types)
