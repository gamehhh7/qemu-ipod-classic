/* Fixed private-AddressSpace fixture; exposed only by exact TEST_HOST=1.
 * No guest system RAM writes, endpoint updates, packet or ACK completion. */
#ifndef IPOD_CLASSIC_USB_DMA_SELFTEST_H
#define IPOD_CLASSIC_USB_DMA_SELFTEST_H
#include "sysemu/runstate.h"
#include "hw/arm/ipod_classic/ipod_classic_usb_dma.h"
typedef struct {
    AddressSpace as;
    MemoryRegion root, ram, adjacent, alias, rom, romd, mmio, ramdev;
    MemoryRegion readonly_alias, readonly_container, container_alias;
    MemoryRegion *regions[12];
    unsigned initialized, mapped, reads, writes, passed, failed;
    bool as_live, container_alias_live;
    uint8_t device_bytes[256];
    GString *cases;
} IPodUSBDMAFixture;
static char *ipod_usb_dma_selftest_result;
static uint64_t ipod_usb_fixture_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodUSBDMAFixture *f = opaque;
    f->reads++;
    return 0;
}
static void ipod_usb_fixture_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    IPodUSBDMAFixture *f = opaque;
    f->writes++;
}
static const MemoryRegionOps ipod_usb_fixture_ops = {
    .read = ipod_usb_fixture_read, .write = ipod_usb_fixture_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 8,
    .impl.min_access_size = 1, .impl.max_access_size = 8,
};
static void ipod_usb_fixture_add(IPodUSBDMAFixture *f, MemoryRegion *mr,
                                 hwaddr at)
{
    f->regions[f->initialized++] = mr;
    memory_region_add_subregion(&f->root, at, mr);
    f->mapped++;
}
static void ipod_usb_fixture_case(IPodUSBDMAFixture *f, const char *name,
                                  uint32_t addr, size_t len, bool expected,
                                  const uint8_t *bytes)
{
    uint8_t output[80], before[80];
    bool actual, untouched, exact = true, pass;
    memset(output, 0x5a, sizeof(output));
    memcpy(before, output, sizeof(before));
    actual = ipod_usb_dma_read_plain(&f->as, addr, output + 8, len);
    untouched = memcmp(output, before, sizeof(output)) == 0;
    if (actual) {
        exact = bytes && len <= 64 && memcmp(output + 8, bytes, len) == 0 &&
                memcmp(output, before, 8) == 0 &&
                memcmp(output + 8 + len, before + 8 + len,
                       sizeof(output) - 8 - len) == 0;
    }
    pass = actual == expected && (actual ? exact : untouched) &&
           f->reads == 0 && f->writes == 0;
    f->passed += pass;
    f->failed += !pass;
    g_string_append_printf(f->cases,
        "%s{\"name\":\"%s\",\"expected\":%s,\"actual\":%s,"
        "\"dst_unchanged\":%s,\"pass\":%s}",
        f->cases->len ? "," : "", name, expected ? "true" : "false",
        actual ? "true" : "false", untouched ? "true" : "false",
        pass ? "true" : "false");
}
typedef struct {
    unsigned passed, failed;
    GString *cases;
} IPodUSBInFixtureChecks;
static void ipod_usb_in_fixture_check(IPodUSBInFixtureChecks *c,
                                       const char *name, bool pass)
{
    c->passed += pass; c->failed += !pass;
    g_string_append_printf(c->cases, "%s{\"name\":\"%s\",\"pass\":%s}",
        c->cases->len ? "," : "", name, pass ? "true" : "false");
}
static void ipod_usb_in_fixture_arm(IPodClassicUSBState *s, uint32_t addr)
{
    /* Fixture-authored MMIO callback inputs on an empty stopped board. */
    usb_reset_set(OBJECT(s), false, NULL);
    usb_vbus_input(s, 0, 1);
    usb_write(s, 0x804, 0, 4);
    usb_write(s, 8, 0x20, 4);
    usb_write(s, 0x914, addr, 4);
    usb_write(s, 0x910, 0x80040, 4);
    usb_write(s, 0x900, 0x80008000, 4);
}
typedef struct { AddressSpace *as; uint32_t addr; unsigned calls; } IPodUSBSetupFixture;
static bool ipod_usb_in_fixture_setup_dma(void *opaque, uint32_t addr, const uint8_t bytes[8])
{
    IPodUSBSetupFixture *f = opaque;
    if (addr != f->addr) { return false; }
    f->calls++;
    return dma_memory_write(f->as, addr, bytes, 8, MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}
static char *ipod_usb_in_fixture_lifecycle(IPodClassicUSBState *s,
                                           AddressSpace *as, uint8_t *ram,
                                           const uint8_t saved[256], bool *passed)
{
    IPodUSBInFixtureChecks c = {0, 0, g_string_new("")};
    IPodUSBRegisters original = s->regs, before;
    IPodUSBInStamp stamp = {0}, old, newer, bad;
    uint8_t output[64];
    const uint8_t setup[8] = {0x40, 0x7d, 0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a};
    IPodUSBSetupFixture setup_dma = {as, SDRAM_MEM_BASE + 0x1080, 0};
    uint64_t owner = s->in_stage.owner, generation, id_before;
    uint64_t private_counter = 0, reserved_owner;
    const uint32_t offsets[] = {0x900, 0x910, 0x914};
    Error *err = NULL;
    char *result;
    unsigned i;
#define ICHECK(name, value) ipod_usb_in_fixture_check(&c, name, (value))
    usb_reset(DEVICE(s));
    ipod_usb_in_fixture_arm(s, SDRAM_MEM_BASE + 0x1000);
    before = s->regs;
    ICHECK("actual-dma-payload-begin", ipod_usb_in_begin_as(s, as, output, 64, &stamp) == IPOD_USB_IN_STAGED && memcmp(output, saved, 64) == 0);
    memset(ram, 0x3c, 64);
    ICHECK("stable-retry-no-reread", ipod_classic_usb_in_retry(s, &stamp, output, 64) == IPOD_USB_IN_RETRIED && memcmp(output, saved, 64) == 0);
    memcpy(ram, saved, 256);
    ICHECK("finish-unsupported-no-register-commit", ipod_classic_usb_in_finish(s, &stamp) == IPOD_USB_IN_UNSUPPORTED && memcmp(&before, &s->regs, sizeof(before)) == 0);
    ICHECK("active-migration-rejected", usb_pre_save(s) == -EBUSY);
    bad = stamp; bad.id++;
    ICHECK("retry-must-correlate-not-create", ipod_classic_usb_in_retry(s, &bad, output, 64) == IPOD_USB_IN_STALE && s->in_stage.id == stamp.id);
    generation = s->in_stage.generation;
    usb_write(s, 0x18, 0, 4); usb_write(s, 0x810, 0, 4); usb_write(s, 0x81c, 0, 4);
    ICHECK("IRQ-masks-not-rearm", s->in_stage.generation == generation && s->in_stage.active);
    before = s->regs;
    usb_realize(DEVICE(s), &err);
    ICHECK("realize-constructor-only", err && s->in_stage.owner == owner && s->in_stage.active && memcmp(&before, &s->regs, sizeof(before)) == 0);
    error_free(err); err = NULL;
    ICHECK("valid-cancel", ipod_classic_usb_in_cancel(s, &stamp) == IPOD_USB_IN_CANCELLED && !s->in_stage.active);
    ICHECK("cancelled-retry-not-create", ipod_classic_usb_in_retry(s, &stamp, output, 64) == IPOD_USB_IN_STALE && !s->in_stage.active);
    ICHECK("inactive-migration-allowed", usb_pre_save(s) == 0);
    generation = s->in_stage.generation; id_before = s->next_in_id;
    ICHECK("postload-invalidates-no-id-reset", usb_post_load(s, 3) == 0 && s->in_stage.generation > generation && s->next_in_id == id_before && s->in_stage.owner == owner);
    for (i = 0; i < 3; i++) {
        uint32_t value;
        ipod_usb_in_fixture_arm(s, SDRAM_MEM_BASE + 0x1000);
        if (ipod_usb_in_begin_as(s, as, output, 64, &old) != IPOD_USB_IN_STAGED) { c.failed++; break; }
        generation = s->in_stage.generation;
        value = offsets[i] == 0x900 ? s->regs.in_ctl[0] : offsets[i] == 0x910 ? s->regs.in_tsiz[0] : s->regs.in_dma[0];
        usb_write(s, offsets[i], value, 4);
        ICHECK(i == 0 ? "same-value-CTL-invalidate" : i == 1 ? "same-value-TSIZ-invalidate" : "same-value-DMA-invalidate",
            s->in_stage.generation > generation && !s->in_stage.active);
        ipod_usb_in_fixture_arm(s, SDRAM_MEM_BASE + 0x1000);
        ICHECK("fresh-ID-after-rearm", ipod_usb_in_begin_as(s, as, output, 64, &newer) == IPOD_USB_IN_STAGED && newer.id > old.id);
        ICHECK("old-cancel-cannot-touch-new", ipod_classic_usb_in_cancel(s, &old) == IPOD_USB_IN_BAD_GENERATION && s->in_stage.active && s->in_stage.id == newer.id);
        ipod_classic_usb_in_cancel(s, &newer);
    }
#define ARM_STAGE() do { ipod_usb_in_fixture_arm(s, SDRAM_MEM_BASE + 0x1000); \
    if (ipod_usb_in_begin_as(s, as, output, 64, &stamp) != IPOD_USB_IN_STAGED) { c.failed++; } } while (0)
    ARM_STAGE(); generation = s->in_stage.generation; usb_write(s, 0x10, 1, 4);
    ICHECK("CSRST-invalidates", !s->in_stage.active && s->in_stage.generation > generation);
    ARM_STAGE(); generation = s->in_stage.generation; usb_reset_set(OBJECT(s), true, NULL);
    ICHECK("reset-rising-invalidates", !s->in_stage.active && s->in_stage.generation > generation && s->regs.reset_line);
    generation = s->in_stage.generation; usb_reset_set(OBJECT(s), true, NULL);
    ICHECK("same-high-not-new-reset", s->in_stage.generation == generation);
    usb_reset_set(OBJECT(s), false, NULL);
    ICHECK("reset-deassert-level-not-event", !s->regs.reset_line && s->in_stage.generation == generation);
    ARM_STAGE(); generation = s->in_stage.generation; usb_reset(DEVICE(s));
    ICHECK("device-reset-invalidates", !s->in_stage.active && s->in_stage.generation > generation && s->in_stage.owner == owner);
    ARM_STAGE(); generation = s->in_stage.generation; usb_vbus_input(s, 0, 0);
    ICHECK("VBUS-down-invalidates", !s->in_stage.active && s->in_stage.generation > generation);
    ARM_STAGE(); generation = s->in_stage.generation; usb_write(s, 0x804, 2, 4);
    ICHECK("soft-disconnect-invalidates", !s->in_stage.active && s->in_stage.generation > generation);
    ARM_STAGE(); old = stamp;
    usb_write(s, 0xb14, setup_dma.addr, 4); usb_write(s, 0xb10, 0x60080040, 4); usb_write(s, 0xb00, 0x88000000, 4);
    ICHECK("accepted-SETUP-supersedes-IN", ipod_classic_usb_setup_event(s, setup, ipod_usb_in_fixture_setup_dma, &setup_dma) == IPOD_USB_SETUP_OK &&
        setup_dma.calls == 1 && memcmp(ram + 128, setup, 8) == 0 && !s->in_stage.active && ipod_classic_usb_in_cancel(s, &old) == IPOD_USB_IN_BAD_GENERATION);
    memcpy(ram, saved, 256);
    ARM_STAGE(); generation = s->in_stage.generation;
    usb_write(s, 0xb00, 0x40000000, 4);
    ICHECK("rejected-SETUP-preserves-IN", ipod_classic_usb_setup_event(s, setup, ipod_usb_in_fixture_setup_dma, &setup_dma) != IPOD_USB_SETUP_OK &&
        setup_dma.calls == 1 && s->in_stage.generation == generation && s->in_stage.active &&
        ipod_classic_usb_in_retry(s, &stamp, output, 64) == IPOD_USB_IN_RETRIED);
    ipod_classic_usb_in_cancel(s, &stamp);
    ARM_STAGE(); reserved_owner = ipod_usb_in_take_counter(&ipod_usb_in_next_owner);
    bad = stamp; bad.owner = reserved_owner;
    ICHECK("owner-namespace-not-reused", reserved_owner > owner && ipod_classic_usb_in_cancel(s, &bad) == IPOD_USB_IN_BAD_OWNER && s->in_stage.active);
    ICHECK("zero-counter-failclosed", ipod_usb_in_take_counter(&private_counter) == 0 && private_counter == 0);
    private_counter = UINT64_MAX - 1;
    ICHECK("counter-no-wrap", ipod_usb_in_take_counter(&private_counter) == UINT64_MAX - 1 && private_counter == UINT64_MAX && ipod_usb_in_take_counter(&private_counter) == 0);
    ipod_usb_in_invalidate(s);
    s->regs = original; usb_update(s);
    ICHECK("teardown-owner-stable-id-monotonic", s->in_stage.owner == owner && !s->in_stage.active && s->next_in_id > id_before);
    *passed = c.failed == 0;
    result = g_strdup_printf("{\"passed\":%u,\"failed\":%u,\"unrealize_backend_drain\":\"not implemented: no backend\",\"cases\":[%s]}", c.passed, c.failed, c.cases->str);
    g_string_free(c.cases, true);
#undef ICHECK
#undef ARM_STAGE
    return result;
}
static bool ipod_usb_dma_run_private_fixture(IPodClassicUSBState *s, char **result, Error **errp)
{
    IPodUSBDMAFixture *f = g_new0(IPodUSBDMAFixture, 1);
    Error *local_err = NULL;
    uint8_t saved[256];
    uint8_t *ram;
    bool ok = false, root_live = false;
    const hwaddr base = SDRAM_MEM_BASE + 0x1000;
    unsigned i;
    hwaddr saved_addrs[12];
    ram_addr_t backing_length;
    char *staging_result = NULL;
    bool staging_pass = false;

    f->cases = g_string_new("");
    memory_region_init(&f->root, NULL, "ipod-dma-fixture-root", UINT64_MAX);
    root_live = true;
    memory_region_init_ram_nomigrate(&f->ram, NULL, "ipod-dma-fixture-ram", 256, &local_err);
    f->regions[f->initialized++] = &f->ram;
    if (local_err) { goto cleanup; }
    memory_region_add_subregion(&f->root, base, &f->ram); f->mapped++;
    memory_region_init_ram_nomigrate(&f->adjacent, NULL, "ipod-dma-fixture-adjacent", 256, &local_err);
    f->regions[f->initialized++] = &f->adjacent;
    if (local_err) { goto cleanup; }
    memory_region_add_subregion(&f->root, base + 256, &f->adjacent); f->mapped++;
    memory_region_init_alias(&f->alias, NULL, "ipod-dma-fixture-alias", &f->ram, 0, 256);
    ipod_usb_fixture_add(f, &f->alias, SDRAM_UNCACHED_MEM_BASE + 0x1000);
    memory_region_init_ram_nomigrate(&f->rom, NULL, "ipod-dma-fixture-rom", 256, &local_err);
    f->regions[f->initialized++] = &f->rom;
    if (local_err) { goto cleanup; }
    memory_region_set_readonly(&f->rom, true);
    memory_region_add_subregion(&f->root, base + 0x10000, &f->rom); f->mapped++;
    memory_region_init_rom_device_nomigrate(&f->romd, NULL, &ipod_usb_fixture_ops,
        f, "ipod-dma-fixture-romd", 256, &local_err);
    f->regions[f->initialized++] = &f->romd;
    if (local_err) { goto cleanup; }
    memory_region_add_subregion(&f->root, base + 0x20000, &f->romd); f->mapped++;
    memory_region_init_io(&f->mmio, NULL, &ipod_usb_fixture_ops, f, "ipod-dma-fixture-mmio", 256);
    ipod_usb_fixture_add(f, &f->mmio, base + 0x30000);
    memory_region_init_ram_device_ptr(&f->ramdev, NULL, "ipod-dma-fixture-ramdev", 256, f->device_bytes);
    ipod_usb_fixture_add(f, &f->ramdev, base + 0x40000);
    memory_region_init_alias(&f->readonly_alias, NULL, "ipod-dma-fixture-readonly-alias", &f->ram, 0, 256);
    memory_region_set_readonly(&f->readonly_alias, true);
    ipod_usb_fixture_add(f, &f->readonly_alias, base + 0x60000);
    memory_region_init(&f->readonly_container, NULL, "ipod-dma-fixture-readonly-container", 256);
    memory_region_set_readonly(&f->readonly_container, true);
    memory_region_init_alias(&f->container_alias, NULL, "ipod-dma-fixture-container-alias", &f->ram, 0, 256);
    f->container_alias_live = true;
    memory_region_add_subregion(&f->readonly_container, 0, &f->container_alias);
    ipod_usb_fixture_add(f, &f->readonly_container, base + 0x70000);
    address_space_init(&f->as, &f->root, "ipod-dma-private-as"); f->as_live = true;
    ram = memory_region_get_ram_ptr(&f->ram);
    for (i = 0; i < sizeof(saved); i++) { ram[i] = saved[i] = i ^ 0xa5; }
    ipod_usb_fixture_case(f, "plain-ram64", base, 64, true, saved);
    ipod_usb_fixture_case(f, "plain-ram1", base, 1, true, saved);
    ipod_usb_fixture_case(f, "resolved-alias", SDRAM_UNCACHED_MEM_BASE + 0x1000, 64, true, saved);
    ipod_usb_fixture_case(f, "exact-region-end", base + 252, 4, true, saved + 252);
    ipod_usb_fixture_case(f, "distinct-adjacent-ram-cross", base + 252, 8, false, NULL);
    ipod_usb_fixture_case(f, "readonly-ram-rom", base + 0x10000, 8, false, NULL);
    ipod_usb_fixture_case(f, "romd", base + 0x20000, 8, false, NULL);
    ipod_usb_fixture_case(f, "mmio", base + 0x30000, 8, false, NULL);
    ipod_usb_fixture_case(f, "ram-device", base + 0x40000, 8, false, NULL);
    ipod_usb_fixture_case(f, "readonly-alias-to-plain-ram", base + 0x60000, 8, false, NULL);
    ipod_usb_fixture_case(f, "readonly-container-to-plain-ram", base + 0x70000, 8, false, NULL);
    ipod_usb_fixture_case(f, "unassigned", base + 0x50000, 8, false, NULL);
    ipod_usb_fixture_case(f, "address-zero", 0, 8, false, NULL);
    ipod_usb_fixture_case(f, "unaligned", base + 1, 8, false, NULL);
    ipod_usb_fixture_case(f, "outside-window", 0x3f000000, 8, false, NULL);
    ipod_usb_fixture_case(f, "window-cross", 0x0bfffffc, 8, false, NULL);
    ipod_usb_fixture_case(f, "address-overflow", 0xfffffffc, 8, false, NULL);
    ipod_usb_fixture_case(f, "len-zero", base, 0, false, NULL);
    ipod_usb_fixture_case(f, "len65", base, 65, false, NULL);
    /* Synthetic malformed MR sizing: mapping span is large but backing is not.
     * No access is made before the helper rejects xlat >= actual used_length. */
    /* Remove sibling mappings while temporarily enlarging this private MR. */
    for (i = 1; i < f->mapped; i++) {
        saved_addrs[i] = f->regions[i]->addr;
        memory_region_del_subregion(&f->root, f->regions[i]);
    }
    backing_length = qemu_ram_get_used_length(f->ram.ram_block);
    memory_region_set_size(&f->ram, backing_length + 4);
    ipod_usb_fixture_case(f, "backing-used-length", base + backing_length, 4, false, NULL);
    memory_region_set_size(&f->ram, 256);
    for (i = 1; i < f->mapped; i++) {
        memory_region_add_subregion(&f->root, saved_addrs[i], f->regions[i]);
    }
    /* Real RAM->MMIO boundary, distinct from the adjacent-RAM case. */
    memory_region_del_subregion(&f->root, &f->adjacent);
    memory_region_del_subregion(&f->root, &f->mmio);
    memory_region_add_subregion(&f->root, base + 256, &f->mmio);
    memory_region_add_subregion(&f->root, base + 0x30000, &f->adjacent);
    ipod_usb_fixture_case(f, "ram-mmio-cross", base + 252, 8, false, NULL);
    if (memcmp(ram, saved, sizeof(saved)) != 0) { f->failed++; }
    staging_result = ipod_usb_in_fixture_lifecycle(s, &f->as, ram, saved, &staging_pass);
    ok = f->failed == 0 && f->reads == 0 && f->writes == 0 && staging_pass;
    *result = g_strdup_printf("{\"status\":\"%s\",\"passed\":%u,\"failed\":%u,"
        "\"mmio_reads\":%u,\"mmio_writes\":%u,\"source_ram_unchanged\":%s,"
        "\"private_as\":true,\"native_acceptance\":false,\"staging\":%s,\"cases\":[%s]}",
        ok ? "PASS" : "FAIL", f->passed, f->failed, f->reads, f->writes,
        memcmp(ram, saved, sizeof(saved)) == 0 ? "true" : "false", staging_result, f->cases->str);
cleanup:
    /* AddressSpace destruction is deferred. Keep heap AS/regions/opaque alive
     * until RCU completion; unparent alias first, and free fixture last. */
    for (i = 0; i < f->mapped; i++) { memory_region_del_subregion(&f->root, f->regions[i]); }
    if (f->container_alias_live) {
        memory_region_del_subregion(&f->readonly_container, &f->container_alias);
    }
    if (f->as_live) { address_space_destroy(&f->as); drain_call_rcu(); }
    if (f->container_alias_live) {
        object_unparent(OBJECT(&f->container_alias));
        object_unparent(OBJECT(&f->readonly_alias));
    }
    if (f->initialized > 2) { object_unparent(OBJECT(&f->alias)); }
    for (i = 0; i < f->initialized; i++) {
        if (f->regions[i] != &f->alias && f->regions[i] != &f->readonly_alias) {
            object_unparent(OBJECT(f->regions[i]));
        }
    }
    if (root_live) { object_unparent(OBJECT(&f->root)); }
    drain_call_rcu();
    g_free(staging_result);
    g_string_free(f->cases, true); g_free(f);
    if (local_err) { error_propagate(errp, local_err); return false; }
    if (!ok) { error_setg(errp, "private AddressSpace DMA selftest failed"); }
    return ok;
}
static void ipod_usb_dma_selftest_set(Object *obj, bool value, Error **errp)
{
    char *result = NULL;
    bool ok;
    if (!value || runstate_is_running()) {
        error_setg(errp, "fixed DMA selftest requires true and a stopped CPU");
        return;
    }
    ok = ipod_usb_dma_run_private_fixture(IPOD_CLASSIC_USB(obj), &result, errp);
    g_free(ipod_usb_dma_selftest_result);
    ipod_usb_dma_selftest_result = result ? result : g_strdup("{\"status\":\"INIT_ERROR\"}");
    fprintf(stderr, "USB DMA PRIVATE-AS SELFTEST %s\n", ipod_usb_dma_selftest_result);
    (void)ok;
}
static char *ipod_usb_dma_selftest_get(Object *obj, Error **errp)
{
    return g_strdup(ipod_usb_dma_selftest_result ? ipod_usb_dma_selftest_result : "{\"status\":\"NOT_RUN\"}");
}
#endif
