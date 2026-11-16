/* S5L8702 SYSALV: only the measured ALVCON control register is modelled. */
#ifndef HW_ARM_IPOD_CLASSIC_SYSALV_H
#define HW_ARM_IPOD_CLASSIC_SYSALV_H
#include "hw/sysbus.h"
#include "qom/object.h"
#define TYPE_IPOD_CLASSIC_SYSALV "s5l8702.sysalv"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicSysalvState, IPOD_CLASSIC_SYSALV)
struct IPodClassicSysalvState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[7];
    uint32_t level[7], type[7], enable[7], edge[7], input[7], valid[7];
    uint32_t alvcon;
    bool trace_read_pending;
};
#endif
