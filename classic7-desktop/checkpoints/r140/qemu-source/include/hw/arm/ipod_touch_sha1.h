#ifndef HW_ARM_IPOD_TOUCH_SHA1_H
#define HW_ARM_IPOD_TOUCH_SHA1_H

#include "qemu/osdep.h"
#include "hw/platform-bus.h"
#include "hw/hw.h"
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include <openssl/sha.h>

#define TYPE_IPOD_TOUCH_SHA1                "ipodtouch.sha1"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchSHA1State, IPOD_TOUCH_SHA1)

/*
 * Upper bound on SHA_INSIZE, kept at the accepted build's value so the clamp
 * semantics are unchanged. NOT a hardware fact (review L8): it exists only to
 * stop a wild guest value from driving an unbounded memory-mode read. The
 * 5 MB staging buffer it used to size is gone -- it was dead weight (written
 * only by a reset memset, never read), so the device instance is no longer
 * ~5 MB (safety-review change).
 */
#define SHA1_BUFFER_SIZE 1024 * 1024 * 5

#define SHA_CONFIG         0x0
#define SHA_RESET          0x4
#define SHA_MEMORY_MODE    0x80 // whether we read from the memory
#define SHA_MEMORY_START   0x84
#define SHA_INSIZE         0x8c

typedef struct IPodTouchSHA1State {
	SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t config;
    uint32_t memory_start;
    uint32_t memory_mode;
    uint32_t insize;
    uint32_t hw_buffer[0x10]; // hardware buffer
    uint8_t hashout[0x14];
    bool hw_buffer_dirty;
    SHA_CTX ctx;

    /*
     * Raw-block chaining state (SHA-repair).
     *
     * The hardware is a block compressor, not a one-shot hash: the guest hands
     * over complete, SELF-PADDED 64-byte blocks and reads the running chaining
     * state back. Feeding the buffered bytes through SHA1_Update/Final instead
     * pads an already-padded message (double padding) and, because Final is
     * destructive, cannot continue into the next segment of a multi-segment
     * HMAC. These fields carry the partial block and the counters across
     * segments; ctx.h0..h4 hold the digest-so-far.
     */
    uint8_t  block[64];     /* partial 64-byte block not yet compressed */
    unsigned block_len;     /* bytes currently in block[] */
    uint64_t total_fed;     /* bytes fed to the engine since reset */
    uint64_t blocks_done;   /* SHA-1 compression function calls since reset */
} IPodTouchSHA1State;

#endif