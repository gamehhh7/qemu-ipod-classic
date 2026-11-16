/*
 * S5L8720 / S5L8702 SHA-1 accelerator (raw-block compressor).
 *
 * SHA-REPAIR VERSION (isolated staging copy; the shared/qemu-display tree is
 * untouched). Two behaviours of the previous model were wrong, and both are
 * measurements, not preferences:
 *
 *  1. DOUBLE PADDING. The guest hands over data it has ALREADY padded itself
 *     (for one message it writes exactly 64 bytes: the 34-byte LCM/S-box key
 *     input plus 0x80, zeros and the bit length). The old model ran
 *     SHA1_Init/SHA1_Update(len)/SHA1_Final over what it had buffered, so it
 *     padded the guest's padding and produced SHA1(pad64(34 bytes)) =
 *     cef09b89a187538ca1376212c6fea7f4c3a1b649 instead of the raw compression
 *     state of that block, 7eb88d2a6ece73f0976c4454658455f7c757e874, which is
 *     the HMAC key the firmware actually uses. With the wrong key every digest
 *     downstream is wrong, so no iTunesDB signature can ever match and the
 *     library comes up empty.
 *
 *  2. NO CHAINING. SHA1_Final is destructive and the model cached the digest
 *     for the whole reset period. The firmware feeds the HMAC input in
 *     segments (measured: 64 bytes, then 4096 bytes, then the rest of the
 *     4788-byte database plus its own final padding block), reading the running
 *     state between segments. A one-shot hash per read cannot express that.
 *
 * The hardware is therefore modelled for what the guest actually drives: a
 * compression engine that consumes complete 64-byte blocks into a running
 * SHA-1 state and reports that state at the digest registers. It never pads;
 * the guest pads. It never finalises; the guest decides when the message ends.
 * SHA_RESET returns the state to the SHA-1 IV.
 *
 * SHA1TRACE=1 (unchanged name, richer output) prints the reset, every data feed
 * with its source/size/head, and every digest read, so the guest protocol can
 * be checked against the firmware's own driver rather than assumed.
 * SHA1_ZERO=1 still answers all-zero digests and SHA1_OFF=1 (in the Classic
 * machine) still leaves the block unimplemented; both are A/B diagnostics and
 * neither is set for any verification run of this repair.
 */
#include "hw/arm/ipod_touch_sha1.h"
#include "qemu/log.h"

static uint32_t bswap32(uint32_t x)
{
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}

static bool sha1_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SHA1TRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on == 1;
}

/* Fresh message: SHA-1 IV, empty partial block. */
static void sha1_state_init(IPodTouchSHA1State *s)
{
    SHA1_Init(&s->ctx);
    memset(s->block, 0, sizeof(s->block));
    s->block_len = 0;
    s->total_fed = 0;
    s->blocks_done = 0;
}

/* One compression function call on a complete 64-byte block. */
static void sha1_compress(IPodTouchSHA1State *s, const uint8_t *blk)
{
    SHA1_Transform(&s->ctx, blk);
    s->blocks_done++;
}

/*
 * Feed `len` bytes into the running state, compressing every complete block.
 * Bytes left over stay in s->block for the next segment -- this is what makes a
 * multi-segment HMAC input work.
 */
static void sha1_feed(IPodTouchSHA1State *s, const uint8_t *data, unsigned len)
{
    s->total_fed += len;
    while (len) {
        unsigned take = 64 - s->block_len;
        if (take > len) {
            take = len;
        }
        memcpy(s->block + s->block_len, data, take);
        s->block_len += take;
        data += take;
        len -= take;
        if (s->block_len == 64) {
            sha1_compress(s, s->block);
            s->block_len = 0;
        }
    }
}

/* Running digest-so-far, big-endian, exactly as SHA1_Final would write it. */
static void sha1_digest_bytes(IPodTouchSHA1State *s, uint8_t out[20])
{
    uint32_t w[5] = { s->ctx.h0, s->ctx.h1, s->ctx.h2, s->ctx.h3, s->ctx.h4 };
    int i;

    for (i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(w[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(w[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(w[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(w[i]);
    }
}

static void sha1_reset(IPodTouchSHA1State *s)
{
	s->config = 0;
	s->memory_start = 0;
	s->memory_mode = 0;
	s->insize = 0;
	memset(&s->hw_buffer, 0, 0x10 * sizeof(uint32_t));
	memset(&s->hashout, 0, 0x14);
	s->hw_buffer_dirty = false;
    sha1_state_init(s);

    if (sha1_trace_on()) {
        fprintf(stderr, "SHA1TRACE reset: state <- SHA-1 IV, partial block cleared\n");
    }
}

static uint64_t ipod_touch_sha1_read(void *opaque, hwaddr offset, unsigned size)
{
	IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

	switch(offset) {
		case SHA_CONFIG:
			return s->config;
		case SHA_RESET:
			return 0;
		case SHA_MEMORY_START:
			return s->memory_start;
		case SHA_MEMORY_MODE:
			return s->memory_mode;
		case SHA_INSIZE:
			return s->insize;
		/* Hash result: the RUNNING chaining state, never a padded digest. */
		case 0x20 ... 0x34:
            if (getenv("SHA1_ZERO")) {
                return 0;
            }
            sha1_digest_bytes(s, s->hashout);
            if (sha1_trace_on()) {
                static int nr;
                if (nr++ < 60) {
                    fprintf(stderr, "SHA1TRACE digest read +0x%02x size=%u: fed=%" PRIu64
                            " bytes, %" PRIu64 " blocks, partial=%u; digest=",
                            (unsigned)offset, size, s->total_fed, s->blocks_done,
                            s->block_len);
                    for (int i = 0; i < 20; i++) {
                        fprintf(stderr, "%02x", s->hashout[i]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            /*
             * EXPLICIT SAFE ACCESS (safety-review change).
             *
             * The digest window is 20 bytes (0x20..0x33) of 32-bit registers.
             * QEMU's dispatch accepts ANY access size here (valid.max_access_size
             * is unset, i.e. 0 = "all valid", system/memory.c:1398-1401) and
             * rejects only addresses unaligned for their size, so a size-1 read
             * at 0x33 is legal and DOES reach this function. The previous
             * `*(uint32_t *)&s->hashout[offset - 0x20]` then read 1-3 bytes past
             * the 20-byte array (host out-of-bounds read) and was an unaligned /
             * aliasing cast in general.
             *
             * Assemble the bytes explicitly instead: bounded to the window and
             * to 32 bits, no cast. For the observed 4-byte aligned reads
             * (0x20..0x30) the result is bit-identical to the old cast on a
             * little-endian host. Anything outside the window, or wider than a
             * register, answers 0 (unobserved; not invented).
             */
            if (size > 4 || offset + size > 0x34) {
                return 0;
            }
            {
                uint32_t v = 0;
                unsigned i;
                for (i = 0; i < size; i++) {
                    v |= (uint32_t)s->hashout[offset - 0x20 + i] << (8 * i);
                }
                return v;
            }
	}

    return 0;
}

static void ipod_touch_sha1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

	switch(offset) {
		case SHA_CONFIG:
            if (sha1_trace_on()) {
                fprintf(stderr, "SHA1TRACE config write = 0x%" PRIx64
                        " (memory_mode=%u start=0x%08x insize=%u hw_dirty=%d)\n",
                        value, s->memory_mode, s->memory_start, s->insize,
                        s->hw_buffer_dirty);
            }
			if(value == 0x2 || value == 0xa)
			{
                /*
                 * Register path: the guest filled the 64-byte hardware buffer
                 * (0x40..0x7c) and kicks it here. Feed those bytes as a block.
                 */
                if(s->hw_buffer_dirty) {
                    sha1_feed(s, (const uint8_t *)s->hw_buffer, 0x40);
                    memset(s->hw_buffer, 0, 0x40);
                    s->hw_buffer_dirty = false;
                    if (sha1_trace_on()) {
                        uint8_t d[20];
                        sha1_digest_bytes(s, d);
                        fprintf(stderr, "SHA1TRACE register block fed, digest now=");
                        for (int i = 0; i < 20; i++) {
                            fprintf(stderr, "%02x", d[i]);
                        }
                        fprintf(stderr, "\n");
                    }
                }

				if(s->memory_mode)
				{
                    /*
                     * Memory mode: the guest named the buffer and its size, and
                     * the bytes there are ALREADY padded as far as it is
                     * concerned. Feed exactly `insize` bytes (not insize/64
                     * blocks) so a segment that is not a multiple of 64 is not
                     * silently truncated; sha1_feed buffers the remainder.
                     */
                    uint8_t chunk[0x400];
                    uint32_t left = s->insize;
                    hwaddr addr = s->memory_start;

                    if (left > SHA1_BUFFER_SIZE) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "s5l8720.sha1: insize 0x%x clamped to 0x%x\n",
                                      left, SHA1_BUFFER_SIZE);
                        left = SHA1_BUFFER_SIZE;
                    }
                    if (sha1_trace_on()) {
                        uint8_t head[16] = { 0 };
                        unsigned hn = left < sizeof(head) ? left : sizeof(head);
                        if (hn) {
                            cpu_physical_memory_read(addr, head, hn);
                        }
                        fprintf(stderr, "SHA1TRACE memory feed: start=0x%08x insize=%u"
                                " head=", (uint32_t)addr, left);
                        for (unsigned i = 0; i < hn; i++) {
                            fprintf(stderr, "%02x", head[i]);
                        }
                        fprintf(stderr, "\n");
                    }
                    while (left) {
                        uint32_t n = left > sizeof(chunk) ? sizeof(chunk) : left;
                        cpu_physical_memory_read(addr, chunk, n);
                        sha1_feed(s, chunk, n);
                        addr += n;
                        left -= n;
                    }
					/* memory-mode feed completed (state is in the running ctx) */
				}
			} else {
				s->config = value;
			}
			break;
		case SHA_RESET:
			sha1_reset(s);
			break;
		case SHA_MEMORY_START:
			s->memory_start = value;
            if (sha1_trace_on()) {
                fprintf(stderr, "SHA1TRACE memory_start write = 0x%08x (size=%u)\n",
                        s->memory_start, size);
            }
			break;
		case SHA_MEMORY_MODE:
			s->memory_mode = value;
            if (sha1_trace_on()) {
                fprintf(stderr, "SHA1TRACE memory_mode write = %u (size=%u)\n",
                        s->memory_mode, size);
            }
			break;
		case SHA_INSIZE:
            if (value > SHA1_BUFFER_SIZE) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "s5l8720.sha1: insize 0x%" PRIx64 " clamped\n", value);
                value = SHA1_BUFFER_SIZE;
            }
			s->insize = value;
            if (sha1_trace_on()) {
                fprintf(stderr, "SHA1TRACE insize write = %u (size=%u)\n",
                        s->insize, size);
            }
			break;
		case 0x40 ... 0x7c:
            // write to the hardware buffer
            s->hw_buffer[(offset - 0x40) / 4] |= value;
            s->hw_buffer_dirty = true;
			break;
        /*
         * Seeding the running state through the digest registers. The guest has
         * not been observed doing this (SHA1TRACE shows no such write), but
         * some SHA engines allow loading an intermediate state to continue a
         * chain. Implemented symmetrically with the read (the read hands out
         * the big-endian digest bytes as a native word, so a write takes a
         * native word and stores the same way) and traced, so it is visible if
         * the firmware ever uses it.
         */
        case 0x20 ... 0x30:
            {
                uint32_t v = bswap32((uint32_t)value);
                switch (offset) {
                case 0x20: s->ctx.h0 = v; break;
                case 0x24: s->ctx.h1 = v; break;
                case 0x28: s->ctx.h2 = v; break;
                case 0x2c: s->ctx.h3 = v; break;
                case 0x30: s->ctx.h4 = v; break;
                }
                s->ctx.num = 0;
                s->ctx.Nl = 0;
                s->ctx.Nh = 0;
                memset(s->ctx.data, 0, sizeof(s->ctx.data));
                s->block_len = 0;
                if (sha1_trace_on()) {
                    fprintf(stderr, "SHA1TRACE state seed +0x%02x = 0x%08x\n",
                            (unsigned)offset, (uint32_t)value);
                }
            }
            break;
	}
}

static const MemoryRegionOps sha1_ops = {
    .read = ipod_touch_sha1_read,
    .write = ipod_touch_sha1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_sha1_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchSHA1State *s = IPOD_TOUCH_SHA1(dev);

    memory_region_init_io(&s->iomem, obj, &sha1_ops, s, "sha1", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);

    sha1_reset(s);
}

static void ipod_touch_sha1_class_init(ObjectClass *klass, void *data)
{

}

static const TypeInfo ipod_touch_sha1_info = {
    .name          = TYPE_IPOD_TOUCH_SHA1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSHA1State),
    .instance_init = ipod_touch_sha1_init,
    .class_init    = ipod_touch_sha1_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_sha1_info);
}

type_init(ipod_touch_machine_types)
