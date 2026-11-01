/* Internal read-only DMA helper. No packet/ACK/XFRC/IRQ semantics. */
#ifndef IPOD_CLASSIC_USB_DMA_H
#define IPOD_CLASSIC_USB_DMA_H
#include "exec/memory.h"
#include "exec/cpu-common.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "sysemu/dma.h"
#include "hw/arm/ipod_classic/ipod_classic.h"
static inline bool ipod_usb_dma_read_plain(AddressSpace *as, uint32_t addr,
                                            uint8_t *dst, size_t len)
{
    uint8_t scratch[64];
    uint64_t end = (uint64_t)addr + len;
    hwaddr xlat, span = len;
    MemoryRegion *mr;
    MemoryRegionSection section;
    bool ok = false;
    bool window =
        (addr >= SDRAM_MEM_BASE && end <= (uint64_t)SDRAM_MEM_BASE + SDRAM_MEM_SIZE) ||
        (addr >= SDRAM_UNCACHED_MEM_BASE && end <= (uint64_t)SDRAM_UNCACHED_MEM_BASE + SDRAM_MEM_SIZE) ||
        (addr >= IRAM0_MEM_BASE && end <= (uint64_t)IRAM0_MEM_BASE + IRAM0_MEM_SIZE) ||
        (addr >= IRAM1_MEM_BASE && end <= (uint64_t)IRAM1_MEM_BASE + IRAM1_MEM_SIZE);

    assert(qemu_mutex_iothread_locked());
    if (!as || !as->root || !dst || !len || len > sizeof(scratch) || !addr ||
        (addr & 3) || end > (1ULL << 32) || !window) {
        return false;
    }
    rcu_read_lock();
    section = memory_region_find(as->root, addr, len);
    mr = address_space_translate(as, addr, &xlat, &span, false,
                                 MEMTXATTRS_UNSPECIFIED);
    /* FlatView readonly can be inherited from aliases/containers; the resolved
     * leaf MR alone loses that attribute. Match the exact section and offsets.
     * Read-direct alone includes readonly RAM and ROMD. Exclude both explicitly.
     * Translation must resolve aliases and cover the entire single plain region.
     * BQL/RCU keep mappings and backing alive through the checked single read. */
    if (section.mr == mr && !section.readonly &&
        section.offset_within_address_space == addr &&
        section.offset_within_region == xlat &&
        int128_eq(section.size, int128_make64(len)) &&
        mr && !mr->alias && memory_region_is_ram(mr) && !mr->readonly &&
        !mr->rom_device && !memory_region_is_ram_device(mr) && mr->ram_block &&
        memory_access_is_direct(mr, false) && span >= len &&
        xlat <= memory_region_size(mr) && memory_region_size(mr) - xlat >= len &&
        xlat <= qemu_ram_get_used_length(mr->ram_block) &&
        qemu_ram_get_used_length(mr->ram_block) - xlat >= len) {
        ok = dma_memory_read(as, addr, scratch, len,
                             MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
    }
    if (section.mr) {
        memory_region_unref(section.mr);
    }
    rcu_read_unlock();
    /* Even a late failed read cannot partially modify the caller's destination. */
    if (ok) {
        memcpy(dst, scratch, len);
    }
    return ok;
}
#endif
