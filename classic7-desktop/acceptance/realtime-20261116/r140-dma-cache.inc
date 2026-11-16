/* Candidate-only transport optimization. A synchronous paced I2S block is
 * already moved before returning to the guest. Cache its RAM source and exact
 * FIFO mapping for that one block, preserving each 16-bit device transaction,
 * channel cursor/count ordering, LLI completion and interrupt path. */
static volatile uint64_t c7_dma_cached_blocks, c7_dma_cached_bytes;
/* Optional bounded paired measurement, then permanently use the fast path.
 * Both paths execute the same real device transactions. No wall/guest clock,
 * FIFO data, interrupts, or sample rate is adjusted by this comparison. */
static uint64_t c7_dma_compare_seen;
static unsigned c7_dma_compare_path, c7_dma_compare_size;
extern volatile uint64_t c7_sm1_frames; /* Observer only, never drives guest state. */
static unsigned pl080_cached_i2s_transfer(PL080State *s, pl080_channel *ch,
                                         unsigned count, unsigned flow,
                                         unsigned swidth, unsigned dwidth)
{
    static int enabled = -1;
    static bool compare;
    MemoryRegionCache source = { 0 }, sink = { 0 };
    unsigned copied = 0;
    uint64_t bytes = (uint64_t)count * 2;

    if (enabled < 0) {
        const char *e = getenv("CLASSIC7_DMA_CACHE");
        enabled = !e || strcmp(e, "0") != 0;
        e = getenv("CLASSIC7_DMA_COMPARE");
        compare = e && strcmp(e, "1") == 0;
    }
    if (!enabled || flow != 1 || swidth != 2 || dwidth != 2 || count < 2 ||
        (ch->ctrl & (PL080_CCTRL_SI | PL080_CCTRL_DI)) != PL080_CCTRL_SI ||
        ((ch->src | ch->dest) & 1) || bytes > UINT32_MAX - (uint64_t)ch->src ||
        !(s->paced_lines & (1u << ((ch->conf >> 6) & 31)))) {
        return 0;
    }
    if (compare && c7_sm1_frames >= 100 && c7_dma_compare_seen < 1024) {
        c7_dma_compare_size = bytes;
        c7_dma_compare_path = (++c7_dma_compare_seen & 1) ? 1 : 2;
        if (c7_dma_compare_path == 1) {
            return 0;
        }
    }
    RCU_READ_LOCK_GUARD();
    int64_t srclen = address_space_cache_init(&source, &s->downstream_as,
                                             ch->src, bytes, false);
    int64_t dstlen = address_space_cache_init(&sink, &s->downstream_as,
                                             ch->dest, 2, true);
    /* No IOMMU, MMIO source, RAM destination, partial mapping, or other device
     * is accelerated. Those all retain the exact original element loop. */
    if (!source.ptr || srclen < (int64_t)bytes || sink.ptr || dstlen < 2 ||
        sink.xlat != 0x10 ||
        strcmp(memory_region_name(sink.mrs.mr), "s5l8702.i2s") != 0) {
        c7_dma_compare_path = 0;
        goto out;
    }
    for (unsigned i = 0; i < count; i++) {
        uint16_t value = address_space_lduw_le_cached(&source, i * 2,
                                                     MEMTXATTRS_UNSPECIFIED, NULL);
        ch->src += 2;
        address_space_stw_le_cached(&sink, 0, value,
                                     MEMTXATTRS_UNSPECIFIED, NULL);
        ch->ctrl = (ch->ctrl & 0xfffff000) | (count - i - 1);
    }
    copied = count;
    c7_dma_cached_blocks++;
    c7_dma_cached_bytes += bytes;
out:
    address_space_cache_destroy(&sink);
    address_space_cache_destroy(&source);
    return copied;
}
