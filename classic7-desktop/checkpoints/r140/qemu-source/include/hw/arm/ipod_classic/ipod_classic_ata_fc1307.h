/*
 * S5L8702 ATA controller — EXPERIMENTAL FC1307 backend variant.
 *
 * Declares the separate FC1307 ATA device used only when the machine is built
 * with the FC1307 patch and FC1307_BACKEND=1 is set at runtime.  The baseline
 * ipod_classic_ata.h / ipod_classic_ata.c are untouched.
 */

#ifndef HW_ARM_IPOD_CLASSIC_ATA_FC1307_H
#define HW_ARM_IPOD_CLASSIC_ATA_FC1307_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_IPOD_CLASSIC_ATA_FC1307 "s5l8702.ata-fc1307"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicATAFC1307State, IPOD_CLASSIC_ATA_FC1307)

#endif
