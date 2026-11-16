/*
 * S5L8702 "SM1" audio clock/stream engine, 0x38500000.
 *
 * See include/hw/arm/ipod_classic/ipod_classic_sm1.h for what the block is and
 * wiki/19-classic6g-dsp-hle.md section 11 for how the map was found. This is the
 * real, stateful replacement for the two hand-faked reads (identity magic + a
 * hard-coded "clock ready" bit) that the previous session dropped into
 * ipod_classic.c. Nothing here is a constant pulled out of thin air: every read
 * that unblocks the guest is derived from a write the guest itself made, so the
 * two bring-up paths in the retailOS -- the codec bring-up 0x0808170c and the
 * start/resume 0x08017378 -- each complete on their own terms instead of racing
 * a timeout or spinning on a zero.
 *
 * WHAT THE GUEST DOES (all VERIFIED in the osos disassembly; roles marked):
 *
 *  - Identity: getter 0x08075d64 reads +0xc80 and the caller compares it to
 *    0x004e7309 (literal *0x080821cc); a mismatch is `b .` forever at 0x080819b8.
 *    VERIFIED role. We answer the magic. (Was the one thing the old stub got.)
 *
 *  - Per-channel sub-blocks: 0x080ba768 drives a small state machine on a
 *    sub-block whose base is base + {idx*0x20 for idx<4; 0x100/0x180/0x200/0x230
 *    for idx 4..7} (computed by 0x080bce04). To power a channel up it writes 4 to
 *    sub+0x10, then polls sub+0x14 (read by 0x080b8470, masked &7) up to ten
 *    times waiting for 7, then writes 16 (run). If the poll never reads 7 it
 *    STILL writes 16 but stamps an error (-236) that the caller can act on. So a
 *    model that leaves sub+0x14 at zero does not hang -- it makes the firmware
 *    believe every channel failed to power up. We make sub+0x14 read 7 once its
 *    sub+0x10 has been commanded, i.e. the channel reached its target state.
 *    VERIFIED offsets/flow; the 4/16/7 meanings are INFERIDO (strong).
 *
 *  - Clock-ready: after the channel and codec config, the bring-up spins on
 *    +0xa98 bit2 with NO timeout (0x08081af8 `tst r0,#4; beq`), and the
 *    start/resume path gates its whole body on the same bit (0x08017394). On
 *    silicon the bit rises when the gated audio clock locks. We raise it once the
 *    firmware has begun powering the engine (its first channel command, the codec
 *    enable at +0x824, the +0x400 start, or the +0xc48 commit). VERIFIED that
 *    bit2 is the awaited bit and that it is un-timed; "clock locked" is INFERIDO.
 *
 *  - Run: the start/resume path writes 1 to +0x400 (0x080ae97c), which itself
 *    re-reads +0x400 expecting exactly 1, and then 0x08017378 polls +0x400 (&3)
 *    until 1 with NO timeout. A plain write latch answers both: write 1, read 1.
 *    VERIFIED both accessors; "run bit" is INFERIDO (strong).
 *
 * Everything else in the low page is a latch (writes stored, reads return the
 * stored word, zero at reset) -- the config strobes at +0x8xx/+0x9xx, +0xa8c,
 * +0xc48, and the status words we have not decoded (+0x404, +0x860, +0xb18,
 * +0x1e0) which stay zero because nothing has been measured to want otherwise.
 * The retailOS never touches +0x1000 (Rockbox's conjectured SM1_DIV), so the
 * upper window is answered as zero exactly like the unimplemented device.
 *
 * This device only ever affects the retailOS audio-clock bring-up. Rockbox never
 * reads SM1, and boot/navigation never enter the bring-up, so both are byte-for-
 * byte unchanged (the block was a zero window for them and, outside +0xc80/
 * +0xa98/+0x400/sub-state, still reads zero).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"   /* cpu_physical_memory_write: el estado en IRAM1 */
#include "hw/irq.h"
#include "qemu/timer.h"
#include "hw/arm/ipod_classic/ipod_classic_sm1.h"
#include "hw/arm/ipod_classic/ipod_classic_hle_audio.h"
#include "hw/arm/ipod_classic/ipod_classic_i2s.h"

/*
 * Executor milestone-2's decode core (SM1ENGINE=1): the MPEG-1/2 Layer III
 * algorithm the silicon demonstrably implements, as a public-domain reference
 * implementation. Pure C (no SIMD) so it compiles anywhere QEMU does, and
 * MP3-only (this executor only ever sees subtype-1 frames).
 */
/* The decoder reports whether a complete Layer III frame retained its
 * reservoir but has no PCM yet. This host outcome state belongs to one kick;
 * minimp3's reservoir and decoded return value remain unchanged. */
static bool sm1_mp3_reservoir_deficit;
static bool sm1_mp3_reservoir_saved;
static int sm1_mp3_need, sm1_mp3_pre, sm1_mp3_post;
static void sm1_mp3_reason(const char *reason, int a, int b, int c)
{
    static int enabled = -1;
    static unsigned count;
    if (!strcmp(reason, "reservoir_deficit")) {
        sm1_mp3_reservoir_deficit = true;
        sm1_mp3_need = a;
        sm1_mp3_pre = b;
    } else if (!strcmp(reason, "reservoir_saved")) {
        sm1_mp3_reservoir_saved = true;
        sm1_mp3_post = c;
    }
    if (enabled < 0) {
        const char *e = getenv("MINIMP3_REASONTRACE");
        enabled = e && *e && *e != '0';
    }
    if (enabled && count++ < 128) {
        fprintf(stderr, "SM1MP3DIAG reason=%s a=%d b=%d c=%d vns=%" PRId64 "\n",
                reason, a, b, c, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}
#define MINIMP3_DIAG(reason, a, b, c) sm1_mp3_reason(reason, a, b, c)
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "minimp3.h"
#pragma GCC diagnostic pop

/*
 * Region window pair (+0xa44/+0xa48, stride 0x14). Retail getter 080ab624
 * adds +a48 to its software IRAM base and returns +a44 as the second field.
 * In the algorithm model these are guest-write/readback registers, following
 * the device's existing reset state. They are not synthesized from I2S audio
 * samples: that made bootstrap silence relocate program/data copies. T49
 * caught a STRH at 0802e6b0 to 22040a16 (outside IRAM1) after +a48=5c00.
 * Silicon reset values and full window-control semantics remain unverified.
 * The legacy experiment's cursor behavior below is retained outside ENGINE.
 */
static bool sm1_decode_on(void);
static bool sm1_engine_on(void);

static bool sm1_pos_reg(IPodClassicSM1State *s, hwaddr offset, uint32_t *out)
{
    for (int idx = 0; idx < S5L8702_SM1_POS_CHANS; idx++) {
        hwaddr a48 = S5L8702_SM1_POS_A48 + idx * S5L8702_SM1_POS_STRIDE;
        hwaddr a44 = S5L8702_SM1_POS_A44 + idx * S5L8702_SM1_POS_STRIDE;
        if (offset != a48 && offset != a44) {
            continue;
        }
        if (sm1_engine_on()) {
            *out = s->reg[offset / 4];
            return true;
        }
        /* Position in stereo frames. When the data-path HLE owns playback, use
         * ITS cursor so pause (frozen), seek (jumped) and track-change (reset)
         * reflect in the position register; otherwise fall back to the I2S
         * transmit-sample count. */
        uint64_t pos;
        /*
         * Same rule as sm1_stream_pos: with SM1_DECODE the guest's own DMA is
         * what plays, so the position it reads back has to come from the I2S it
         * drives. Left on the host cursor the Now Playing screen renders but
         * its elapsed time sits at 0:00 for ever, because that cursor is not
         * the one moving.
         */
        if (!sm1_decode_on() && ipod_classic_hle_audio_have_pcm()) {
            pos = ipod_classic_hle_audio_position_frames();
        } else {
            pos = s->i2s ? s->i2s->tx_samples / 2 : 0;
        }
        /*
         * WRAP THE POSITION INTO THE RING. This is the fix for the wall that
         * fourteen assaults called a "structural deadlock"; see wiki/19 §11.26.
         *
         * MEASURED: the render's output pointer is computed by the firmware as
         *     dest = 0x22020000 + (this position * 4)
         * i.e. IRAM1's base plus the position in stereo frames. Proof: with the
         * position forced to 0 the copy's destination is exactly 0x22020000
         * (IRAM1 base); left free-running it was 0x222dc000 and then 0x222fbc00
         * on a later run -- 0x22020000 plus the audio bytes elapsed since play
         * started.
         *
         * So the engine's position register is a RING OFFSET, not a cumulative
         * total. Reporting the raw count walked the firmware's write pointer out
         * of IRAM1 after ~1.9 s of playback; the stores then hit unmapped memory
         * and took a data abort inside the tiny silence-copy at 0x08085ea0
         * (40 bytes, 10 words), so the copy never returned -- and neither did
         * the mixer 0x0809a74c, the render 0x0808170c, or the pump 0x22006118.
         * That is what looked like a rendezvous deadlock from every angle: the
         * render was not waiting for anything, it was faulting.
         *
         * The ring is IRAM1 (0x22020000, 128 KiB), so the position wraps every
         * 128 KiB / 4 bytes-per-frame = 32768 stereo frames, which keeps every
         * write the firmware issues inside that buffer.
         *
         * SM1_POS overrides the wrap for experiments (0 = pin the position at
         * the ring base).
         */
        {
            static int pos_mode = -1;
            static uint64_t pos_wrap;
            if (pos_mode < 0) {
                const char *e = getenv("SM1_POS");
                pos_wrap = (e && *e) ? strtoull(e, NULL, 0)
                                     : S5L8702_SM1_POS_RING_FRAMES;
                pos_mode = 1;
                if (e && *e) {
                    fprintf(stderr, "SM1_POS: ring wrap overridden to %llu frames\n",
                            (unsigned long long)pos_wrap);
                }
            }
            pos = pos_wrap ? (pos % pos_wrap) : 0;
        }
        /* +0xa48 is the position the copy uses; +0xa44 is its companion,
         * answered 0 until measurement says otherwise. */
        *out = (offset == a48) ? (uint32_t)pos : 0;
        return true;
    }
    return false;
}

void ipod_classic_sm1_set_i2s(IPodClassicSM1State *s,
                              struct IPodClassicI2SState *i2s)
{
    s->i2s = i2s;
}

/* --------------------------------------------------------- interrupt ------ */
/*
 * The engine's "playback advanced" interrupt, VIC0 line 18.
 *
 * On real silicon the audio engine tells the CPU when it has consumed audio;
 * Apple's driver hangs its whole render pump off that report (see the header,
 * and wiki/19 section 11.25). We had no line at all here, so the report could
 * never arrive. Raise it when the stream position has moved since the last
 * tick -- i.e. when audio really was clocked out -- which is the condition the
 * handler itself checks for (it reads the engine status and tests bit 2, the
 * same bit our `powered` state answers).
 *
 * Level-style pulse: raise, then drop on the next tick, so a handler that does
 * not clear anything still sees discrete events rather than a stuck line.
 * Gated by SM1_IRQ=1; with it unset nothing here runs and the machine behaves
 * exactly as the measured baseline.
 */
#define SM1_IRQ_TICK_MS 5

static uint64_t sm1_stream_pos(IPodClassicSM1State *s)
{
    /*
     * With SM1_DECODE the guest's own DMA is what plays the audio, so the
     * position has to come from the I2S it actually drives. Taking the host
     * shim's cursor instead stalls us: MEASURED, the render pump froze at ~44
     * activations even over a 150 s window, because that cursor stops
     * advancing and so the slot-freeing loop never crosses another boundary.
     * Without SM1_DECODE the old preference stands, untouched.
     */
    if (!sm1_decode_on() && ipod_classic_hle_audio_have_pcm()) {
        return ipod_classic_hle_audio_position_frames();
    }
    return s->i2s ? s->i2s->tx_samples / 2 : 0;
}

/* Drop the engine interrupt line; see the note where it is scheduled. */
static void sm1_irq_off(void *opaque)
{
    IPodClassicSM1State *s = opaque;

    if (s->irq_pending) {
        s->irq_pending = false;
        qemu_irq_lower(s->irq);
    }
}

/*
 * THE ENGINE'S MEMORY-SIDE STATE (SM1_SLOTS=1).
 *
 * The render pump stops after eight invocations because 0x080a3c00 -- the
 * "give me the next render buffer" call -- returns 0 where 0x22006358 requires
 * 2. MEASURED, it returns 0 at its FIRST guard:
 *
 *     80a3c2c: tst r2, r0    ; r2 = enable mask, halfword at [table+4]
 *     80a3c30: beq salir     ; mask = 0x0000 -> give up
 *
 * where `table` is 0x0896a684 = { _, 0x2203ff28, 0x2203ff00, 0x22038000,
 * 0x22038100, idx, ... }. With `memw=` over a whole run (~180 s, boot
 * included), NO PC of the CPU ever writes 0x2203ff28, and the slot array at
 * 0x22038000 is written only once at boot. Forcing the mask by hand reaches
 * the second guard -- the per-slot "ready" halfword -- which is zero for the
 * same reason. So this is not a missing bit: it is state the ENGINE publishes
 * and our register-only model never produced.
 *
 * THE GEOMETRY IS OURS TO CHOOSE, and that is what makes this implementable
 * rather than a guess. The firmware does not carry these numbers; it reads
 * them back from what the engine published and derives everything:
 *
 *     80a3c54: ldrh r1,[desc,#4]      ; frames per slot  <- we publish
 *     80a3c58: ldrh r0,[slot,#12]     ; samples in slot  <- we publish
 *     80a3c68: add r1, pcm, r1,lsl#1  ; buffer = PCM + idx*frames*2
 *     80a3c64: lsl r0, r0, #1         ; size   = samples*2
 *
 * So any self-consistent geometry that fits below the descriptors works. We
 * take 2048 frames/slot (the audio DMA's own burst: 2048 x 16-bit, measured
 * from the self-linking LLI) and 7 slots: 7 * 4096 = 28 KiB, which ends at
 * 0x2203f100, clear of 0x2203ff00.
 *
 * What stays UNDETERMINED and is NOT modelled here: the descriptor chain the
 * firmware hands the engine at sub-block 4 +0x00 (measured 0x08ad003d = chain
 * | go, chain entry { next, ctrl, buf }, buf holding real MP3 frames). Where
 * the engine would put ITS output is programmed in no register on the whole
 * 4 KiB page -- swept, only +0x100/+0x110/+0x9xx/+0xaxx are non-zero -- so it
 * cannot be derived from anything measured. This routine deliberately models
 * only the half that CAN be grounded: the buffer-return handshake, whose
 * cadence follows the I2S the same way the position register already does.
 *
 * Gated by SM1_SLOTS so the default build stays byte-for-byte as measured.
 */
#define SM1_SLOT_DESC        0x2203ff28   /* [table+4]  engine descriptor  */
#define SM1_SLOT_ARRAY       0x22038000   /* [table+12] per-slot state     */
#define SM1_SLOT_PCM         0x22038100   /* [table+16] the PCM ring       */
#define SM1_SLOT_FRAMES      2048         /* frames per slot (DMA burst)   */
#define SM1_SLOT_COUNT_MAX   7            /* 7 * 4096 = 28 KiB below 0x…ff00 */

/*
 * HOW MUCH OF IRAM1 IS ACTUALLY OURS is not something the firmware tells us --
 * we publish the geometry, so we also choose how much memory it implies. Seven
 * slots claims 28 KiB from 0x22038100, and MEASURED, filling all of it freezes
 * the user interface mid-transition into Now Playing while the audio keeps
 * playing: the screen renders fully with SM1_SLOTS alone, and with SM1_DECODE
 * added it stops half-drawn. Publishing the state is harmless; WRITING 28 KiB
 * of PCM through it is not, which says the ring is smaller than we claimed and
 * the tail lands on something else's memory.
 *
 * SM1_NSLOTS sweeps it so the real size can be found by bisection instead of
 * guessed.
 */
static unsigned sm1_slot_count(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("SM1_NSLOTS");
        n = (e && *e) ? atoi(e) : SM1_SLOT_COUNT_MAX;
        if (n < 1) {
            n = 1;
        }
        if (n > SM1_SLOT_COUNT_MAX) {
            n = SM1_SLOT_COUNT_MAX;
        }
    }
    return (unsigned)n;
}
#define SM1_SLOT_COUNT       sm1_slot_count()
#define SM1_SLOT_BYTES       (SM1_SLOT_FRAMES * 2)   /* = the firmware's own
                                          * derivation: size = samples * 2 */

static bool sm1_slots_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SM1_SLOTS");
        on = (e && *e && *e != '0') ? 1 : 0;
        if (on) {
            fprintf(stderr, "SM1_SLOTS: publicando el estado del motor en IRAM1 "
                    "(%d ranuras de %d marcos)\n",
                    SM1_SLOT_COUNT, SM1_SLOT_FRAMES);
        }
    }
    return on == 1;
}

/*
 * THE CODEC OUTPUT BUFFERS (SM1_DECODE=1).
 *
 * Where the guest actually reads its audio from is MEASURED, not assumed --
 * which is what the first attempt got wrong (it filled the IRAM1 slot ring,
 * which nothing transports). The pump asks 0x22003810 -> 0x081db4c8 for a
 * buffer, and that function is not a decoder at all: it is a two-entry
 * dispenser handing out one of two FIXED addresses carried as literals,
 *
 *     81db4f0: ldrne r1, =0x08a873c0
 *     81db4f4: ldreq r3, =0x08a8b3c0
 *     81db504: str r1,[r2]        ; hands it over
 *     81db510: mov r0,#0          ; and always reports success
 *
 * then copies 1024 stereo frames out of it (r5*4 = 4096 bytes, measured). So
 * something else is supposed to have FILLED those two buffers, and measured,
 * their contents are all zeros at request 3, 60 and 150 alike. That is the
 * whole silence: every stage below this one works.
 *
 * What should fill them is the MP3 decode the firmware delegates to this
 * engine -- it hands us a descriptor chain at sub-block 4 +0x00 whose buffer
 * carries real MP3 frames. Decoding those the way the undocumented on-chip
 * codec does is not something to reverse-engineer; it is something to EMULATE,
 * and ipod_classic_hle_audio.c already does it with verified output.
 *
 * Deliberately NOT touching hle_np_owns_feed this time: measured, switching
 * the host shim off removed the only audio that did come out (21% non-zero ->
 * 0.2%). If this path works the two should agree, not compete.
 */
#define SM1_CODEC_BUF_A      0x08a873c0
#define SM1_CODEC_BUF_B      0x08a8b3c0
#define SM1_CODEC_BUF_BYTES  4096      /* 1024 stereo frames, measured */

static bool sm1_decode_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SM1_DECODE");
        on = (e && *e && *e != '0') ? 1 : 0;
        if (on) {
            fprintf(stderr, "SM1_DECODE: el motor entrega PCM en los buferes del "
                    "codec (0x%08x / 0x%08x)\n",
                    SM1_CODEC_BUF_A, SM1_CODEC_BUF_B);
        }
    }
    return on == 1;
}

/*
 * THE DATA FLOWS THE OTHER WAY -- corrected, and it matters.
 *
 * The pump ACQUIRES a codec buffer and copies INTO it:
 *
 *     22006488: ldrne r0,[sp,#52]   ; dst = the codec buffer it just acquired
 *     2200648c: ldrne r1,[sp,#28]   ; src = the IRAM1 slot
 *     22006494: blne 0x22000020     ; slot -> codec buffer
 *     220064f0: bl  0x081db520      ; then hands the codec buffer on (lr=0x220064f4)
 *
 * So the codec buffers are the pump's OUTPUT, not its input, and the audio has
 * to already be in the IRAM1 SLOT -- which is what "ready" meant all along.
 * Filling the codec buffers (the previous cut) was writing to the far end of
 * the copy, where the pump promptly overwrote it with the empty slot; that is
 * why they still read as zeros with SM1_DECODE on.
 *
 * This is the same place the very first attempt wrote to, and it measured worse
 * then for three reasons since fixed: the slot-freeing rate was out by 2, the
 * position came from the host cursor instead of the I2S, and it switched the
 * host feed off. None of those is true now.
 */
static void sm1_fill_slot(unsigned i)
{
    static size_t cursor;
    uint8_t buf[SM1_SLOT_BYTES];
    size_t n;

    if (!sm1_decode_on() || !ipod_classic_hle_audio_have_pcm()) {
        return;
    }
    /*
     * SM1_FEED_OFF: switch the host shim's own DMA feed off, so the ONLY route
     * from decoded PCM to the speaker is this one -- engine buffer, guest render
     * pump, guest DMA, guest I2S. Purely an attribution control: if the music
     * survives with it set, the guest's own path is carrying it.
     */
    if (getenv("SM1_FEED_OFF")) {
        static bool told;
        if (!told) {
            told = true;
            ipod_classic_hle_audio_set_np_feed(true);
            fprintf(stderr, "SM1_FEED_OFF: el atajo del anfitrion queda apagado; "
                    "el audio solo puede salir por el camino del invitado\n");
        }
    }

    n = ipod_classic_hle_audio_copy_pcm(cursor, buf, sizeof(buf));
    if (n == 0) {
        return;                         /* nothing decoded yet, or track ended */
    }
    if (n < sizeof(buf)) {
        memset(buf + n, 0, sizeof(buf) - n);
    }
    /*
     * One slot per frame report, in the same ring order the firmware reads
     * them, so consecutive requests get consecutive audio.
     */
    cpu_physical_memory_write(SM1_SLOT_PCM + i * SM1_SLOT_BYTES,
                              buf, sizeof(buf));
    cursor += n;
}

static int sm1_publish_slots(IPodClassicSM1State *s)
{
    uint16_t h;
    int freed_now = 0;

    if (!sm1_slots_on()) {
        return -1;              /* not modelling: caller keeps its own cadence */
    }

    /*
     * The descriptor: enable mask bit 0 (guard 1 asks for bit 1 instead only
     * when the stream state byte is 6, so publish both), the halfword the
     * guard requires to be zero, and the slot geometry.
     */
    h = cpu_to_le16(0x0003);
    cpu_physical_memory_write(SM1_SLOT_DESC + 0, &h, 2);
    h = cpu_to_le16(0);
    cpu_physical_memory_write(SM1_SLOT_DESC + 2, &h, 2);
    h = cpu_to_le16(SM1_SLOT_FRAMES);
    cpu_physical_memory_write(SM1_SLOT_DESC + 4, &h, 2);

    /* The slot count the firmware wraps its index against. */
    h = cpu_to_le16(SM1_SLOT_COUNT);
    cpu_physical_memory_write(SM1_SLOT_ARRAY + 2, &h, 2);

    /*
     * FREEING SLOTS AT THE RATE PLAYBACK ACTUALLY CONSUMES THEM.
     *
     * The first cut re-armed all seven slots on every frame report. It broke
     * the wall -- rendret 8 -> 2505 -- but it also destroys flow control: the
     * firmware believes it has seven free buffers forever, never waits, and
     * MEASURED, its producer ran LESS (the submit gate 0x081fee60 went 8 -> 1,
     * "audio available" 7 -> 0). A ring the consumer never has to wait on is
     * not a ring.
     *
     * The right cadence is not invented either: the engine frees a slot when
     * it has PLAYED it, and how much has been played is something we already
     * know exactly -- it is the same I2S transmit count the position register
     * is derived from, just expressed in slots. So the credit ahead of the
     * play point is SM1_SLOT_COUNT slots, and each slot boundary the stream
     * crosses frees exactly one more, in ring order.
     */
    {
        static uint64_t freed;      /* one SM1 in the machine */
        static uint64_t base_pos;
        uint64_t pos = sm1_stream_pos(s);

        if (freed == 0) {
            /* Nothing played yet: the whole ring is the firmware's to fill. */
            base_pos = pos;
        }
        /*
         * A slot is SM1_SLOT_FRAMES *16-bit samples* = 4096 bytes, which at
         * stereo is 1024 FRAMES -- and `pos` counts frames, not samples. The
         * first cut divided by SM1_SLOT_FRAMES and so freed slots at half the
         * rate playback consumes them. Confirmed by the pump itself: it copies
         * r5*4 = 4096 bytes per request with r5 = 1024 (measured).
         */
        uint64_t played = (pos - base_pos) / (SM1_SLOT_FRAMES / 2);
        while (freed < played + SM1_SLOT_COUNT) {
            unsigned i = (unsigned)(freed % SM1_SLOT_COUNT);

            /*
             * TRIED AND BACKED OUT: filling the slot with host-decoded PCM.
             *
             * The reasoning was that a "ready" slot already CONTAINS decoded
             * audio (the guard hands its caller the address AND the size, and
             * 0x22006364 transports that buffer), which would make the engine
             * the codec -- consistent with the chain it is handed carrying raw
             * MP3 frames. So the slot was filled from ipod_classic_hle_audio's
             * verified PCM and hle_np_owns_feed was set so the host shim would
             * not double-feed the DMA.
             *
             * MEASURED, it makes things WORSE, and the control is unambiguous:
             *
             *   HLE_AUDIO_DATAPATH alone        21.06% non-zero, peak 32768
             *   + this fill (np_feed disabled)   0.20-0.47%,     peak 11444
             *
             * The 32768 peak that looked like the guest's own path carrying
             * real audio was the host shim's, still running before the first
             * slot publish switched it off. Writing PCM into the IRAM1 ring
             * does NOT get it to the I2S: the pump only ran 21-39 times in
             * those runs, so nothing transported it. Suppressing the shim
             * therefore just removed the audio that did work.
             *
             * Left as a comment rather than deleted because the reasoning is
             * still the most likely account of what the engine does -- what it
             * lacks is the transport, not the idea.
             */

            /* The engine's real output: PCM into the slot the pump reads from. */
            sm1_fill_slot(i);

            h = cpu_to_le16(1);
            cpu_physical_memory_write(SM1_SLOT_ARRAY + 10 + i * 4, &h, 2);
            h = cpu_to_le16(SM1_SLOT_FRAMES);
            cpu_physical_memory_write(SM1_SLOT_ARRAY + 12 + i * 4, &h, 2);
            freed++;
            freed_now++;
        }
    }
    return freed_now;
}

static void sm1_irq_tick(void *opaque)
{
    IPodClassicSM1State *s = opaque;
    uint64_t pos = sm1_stream_pos(s);

    /*
     * The trigger is the guest's own arming write to +0xa8c (see the write
     * handler). Earlier attempts gated on our derived `powered` flag (never set
     * on the play path) or on the stream position advancing (circular: the
     * position cannot move until the render this interrupt unblocks has run).
     * Both measured raised=0. Arming is the firmware's request, so honouring it
     * is what the hardware would do.
     *
     * Cadence: the audio DMA delivers ~43 bursts/s (2048 stereo frames each,
     * measured), so a frame-done report every ~25 ms is the right order. We tick
     * at 5 ms and fire every fifth tick.
     *
     * Shape: assert with the status bit set, then drop both on the next tick, so
     * the handler sees a discrete event and a missed one cannot wedge the line.
     */
    s->ticks++;
    /*
     * SM1_IRQ_EVERY overrides how many 5 ms ticks pass between reports.
     *
     * Needed to test a specific failure: the audio task's loop at 0x22006324
     * calls the event waiter, and when the waiter returns failure it sleeps and
     * retries -- but only TWICE (`cmp r7,#1 ; bgt exit`) before giving up. The
     * waiter's own wait carries a timeout of 500. So if a frame report does not
     * arrive inside that timeout the whole audio pump bails after two attempts,
     * which is exactly the eight invocations measured. Making the report more
     * frequent says whether the cadence is what misses the timeout.
     */
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("SM1_IRQ_EVERY");
        every = (e && *e) ? atoi(e) : 5;
        if (every < 1) {
            every = 1;
        }
    }
    if (!s->irq_pending && (s->irq_armed || s->irq_always) && (s->ticks % every == 0)) {
        /*
         * Publish the memory-side state BEFORE signalling: the frame report is
         * what makes the firmware come and read it, so it has to already be
         * there when the handler runs.
         *
         * And when we ARE modelling it, the report has to MEAN what its name
         * says. MEASURED: waking the pump on the bare 25 ms timer, whether or
         * not a slot had actually come free, let it run 16 times and then give
         * up -- it asks, misses, and 0x22006358 abandons after two retries
         * rather than waiting. So with SM1_SLOTS on, a frame report is raised
         * only when a frame really completed. Returns -1 when not modelling,
         * which keeps the default build's cadence byte-for-byte.
         */
        int freed = sm1_publish_slots(s);
        if (freed == 0) {
            timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)SM1_IRQ_TICK_MS * 1000000);
            s->irq_pending = false;
            return;
        }
        s->irq_pending = true;
        s->last_pos = pos;
        s->irqs++;
        qemu_irq_raise(s->irq);
        /*
         * The line is a LEVEL, but a short one.
         *
         * Holding it until the next 5 ms tick produced the storm in s11.41:
         * 4 raises, 789 182 handler entries even after any block read became an
         * acknowledge. That is ~197 000 re-entries inside one 5 ms window, which
         * is only possible because virtual time barely advances while the CPU
         * spins in the interrupt loop -- so a coarse timer is exactly the wrong
         * thing to be holding an interrupt line with.
         *
         * MEASURED, a bare pulse does not work: with SM1_IRQ_PULSE=1 (raise and
         * lower in the same instant) the handler count went to ZERO and so did
         * creditsig. The VIC wants a level; an edge is lost. So the line has to
         * stay up long enough to be dispatched and no longer.
         *
         * SM1_IRQ_ASSERT_US is that window, and it is a MODEL CHOICE, not a fact
         * about the part: what the real engine's line does is UNDETERMINED, and
         * what the firmware touches to acknowledge it has not been found -- the
         * handler's status getter at 0x080a7638 does not appear to reach this
         * block, or broadening the read acknowledge would have ended the storm
         * instead of trimming it by a third.
         */
        timer_mod(s->irq_off_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)S5L8702_SM1_IRQ_ASSERT_US * 1000);
        if (s->irqs <= 4 || s->irqs % 2000 == 0) {
            fprintf(stderr, "SM1_IRQ: line 18 raised #%" PRIu64
                    " (armed=%d powered=%d pos=%" PRIu64 ")\n",
                    s->irqs, (int)s->irq_armed, (int)s->powered, pos);
        }
    }
    timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)SM1_IRQ_TICK_MS * 1000000);
}

/*
 * SM1TRACE=1 prints every access with its value and the guest PC, the same
 * pattern as I2STRACE/ATATRACE/DISPTRACE. SM1 is touched only during the audio
 * bring-up (a few dozen accesses), so this is cheap and stays in as the way to
 * watch the handshake -- e.g. to confirm which bring-up path (0x0808170c codec
 * bring-up vs 0x08017378 start/resume) runs and in what order.
 */
static int sm1trace = -1;

static bool sm1_trace_on(void)
{
    if (sm1trace < 0) {
        const char *e = getenv("SM1TRACE");
        sm1trace = (e && *e && *e != '0') ? 1 : 0;
    }
    return sm1trace == 1;
}

/*
 * SM1HI=1: report writes ABOVE the modelled 4 KB register page.
 *
 * Its own variable on purpose. The question -- does the firmware put anything
 * in the other 1020 KB of this block's window? -- needs a handful of lines,
 * while SM1TRACE prints one line per register access and produced a log big
 * enough to take the whole WSL filesystem down with it. Cheap when unset.
 */
static int sm1hi = -1;

static bool sm1_hi_on(void)
{
    if (sm1hi < 0) {
        const char *e = getenv("SM1HI");
        sm1hi = (e && *e && *e != '0') ? 1 : 0;
    }
    return sm1hi == 1;
}

static uint32_t sm1_trace_pc(void)
{
    CPUState *cs = current_cpu;
    return cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;
}

/* Read-only bounded register observer for the current payload. */
static void sm1_count_hit(hwaddr offset, bool write, uint32_t value)
{
    static int enabled = -1;
    static int flow_enabled = -1;
    static uint64_t rd[1029], wr[1029];
    unsigned slot = offset < S5L8702_SM1_REG_BYTES ? offset / 4 :
                    offset < S5L8702_SM1_REG_BYTES + 16 ?
                    1024 + (offset - S5L8702_SM1_REG_BYTES) / 4 : 1028;
    uint64_t n;

    if (enabled < 0) {
        const char *e = getenv("SM1COUNT");
        enabled = e && *e && *e != '0';
    }
    if (flow_enabled < 0) {
        const char *e = getenv("SM1FLOWTRACE");
        flow_enabled = e && *e && *e != '0';
    }
    if (!enabled && !flow_enabled) {
        return;
    }
    n = ++(write ? wr : rd)[slot];
    if (flow_enabled && n <= 256 &&
        (offset == 0x100 || offset == 0x110 || offset == 0x114 ||
         offset == 0xa90 || offset == 0xa98 || offset == 0xc48 ||
         offset == 0x400 || offset == 0x1008 || offset == 0x100c)) {
        /* RAM-only observer: do not read MMIO and accidentally acknowledge it.
         * This payload's output ring and task state are already independently
         * located. Retain every initial access, not just power-of-two counts. */
        uint8_t idx[4], desc[2], ready0[2], ready1[2], task[4];
        cpu_physical_memory_read(0x08a0ec70ull, idx, sizeof(idx));
        cpu_physical_memory_read(0x2203ff2aull, desc, sizeof(desc));
        cpu_physical_memory_read(0x2203800aull, ready0, sizeof(ready0));
        cpu_physical_memory_read(0x2203800eull, ready1, sizeof(ready1));
        cpu_physical_memory_read(0x22010728ull, task, sizeof(task));
        fprintf(stderr, "SM1FLOW %s +0x%03" HWADDR_PRIx
                " n=%" PRIu64 " value=0x%08x vns=%" PRId64
                " pc=0x%08x idx=%u desc=%u ready=%u,%u task=%u\n",
                write ? "wr" : "rd", offset, n, value,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), sm1_trace_pc(),
                ldl_le_p(idx), lduw_le_p(desc), lduw_le_p(ready0),
                lduw_le_p(ready1), ldl_le_p(task));
    }
    if (enabled && ((n & (n - 1)) == 0 || n % 1000000 == 0)) {
        fprintf(stderr, "SM1COUNT %s +0x%03" HWADDR_PRIx
                " n=%" PRIu64 " value=0x%08x vns=%" PRId64 " pc=0x%08x\n",
                write ? "wr" : "rd", offset, n, value,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), sm1_trace_pc());
    }
}

/*
 * EXECUTOR MILESTONE-1 (SM1EXECTRACE=1): the submit-kick diagnostic.
 *
 * The firmware's submit chain ends in ONE MMIO write: the 12-byte input-record
 * chain head pointer|1 into sub-block 4's base register at +0x100 (runtime-
 * confirmed twice in T40's stderr trace: 0x38500100 <- 0x08b211fd, pc
 * 0808ec7c). Today that write lands in the latch and nothing happens -- the
 * engine never consumes its input and the firmware retries the whole submit
 * every ~15.8 virtual seconds. Before the engine can COMPUTE anything (the
 * bank-program semantics are still being reverse-engineered, ISA-RE-BATCH-11),
 * this diagnostic exercises everything around the computation so the runtime
 * half of the contract is pinned down:
 *
 *   1. the kick itself (value, head pointer, guest PC, virtual time);
 *   2. the record-chain walk {next|0, (nbytes<<16)|ring_off, tagged_ptr}
 *      exactly as the firmware composed them at 0x080d4590/d9bd4;
 *   3. the INPUT COPY: nbytes from the (bit31-untagged) data pointer into the
 *      engine's input ring -- the submit record's ring_off 0xf80 = engine word
 *      3968, and engine word w lives at 0x22030c00 + 4*w (type1 window; the
 *      addresses of the batch-06 engine-obligation words match this mapping);
 *   4. the BANK PROGRAM TRACE: the four 1514-word banks at
 *      0x22020c00 + q*0x4000 (T38 byte-verified loads) decoded per the
 *      ISA-RE-BATCH-11 grammar -- strict (even, odd) two-word instructions of
 *      four 16-bit units plane3|word9|sub4 -- one line each with the CURRENT
 *      guest value at the source and destination words. Decode+log only.
 *
 * Deliberately NOT done here: no PCM, no +0xa98 bit12, no VIC-18 raise, no
 * completion of any kind. The guest sees the same silent engine as the
 * measured baseline; this block only READS guest memory (plus the one input
 * copy INTO the engine's own ring area, which nothing on the ARM path reads).
 * All of it is inert unless SM1EXECTRACE is set, so the default build stays
 * byte-for-byte identical in behavior.
 */
#define SM1_EXEC_KICK_OFF    0x100
#define SM1_EXEC_G           0x22020c00ull      /* bank q at G + q*0x4000 (T38) */
#define SM1_EXEC_DATA(w)     (0x22030c00ull + 4ull * (w))
#define SM1_EXEC_BANK_WORDS  1514
#define SM1_EXEC_MAX_RECS    16

static bool sm1_exec_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SM1EXECTRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
        if (on) {
            fprintf(stderr, "SM1EXEC: executor milestone-1 kick trace armed "
                    "(kick +0x100, records, input copy, bank reference trace)\n");
        }
    }
    return on == 1;
}

static uint32_t sm1_exec_ld(uint64_t addr)
{
    uint32_t v = 0;
    cpu_physical_memory_read(addr, &v, 4);
    return v;
}

static uint32_t sm1_exec_fnv1a(uint64_t addr, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        uint8_t b;
        cpu_physical_memory_read(addr + i, &b, 1);
        h ^= b;
        h *= 16777619u;
    }
    return h;
}

static void sm1_exec_kick(IPodClassicSM1State *s, uint32_t value)
{
    static unsigned kicks;
    unsigned k = ++kicks;
    uint64_t head = value & ~1ull;

    fprintf(stderr, "SM1EXEC === kick #%u value=0x%08x head=0x%08llx "
            "vns=%lld pc=0x%08x ===\n",
            k, value, (unsigned long long)head,
            (long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), sm1_trace_pc());

    /* 1+2. the record chain, exactly as the firmware linked it. */
    for (uint64_t rec = head; rec && rec >> 32 == 0; ) {
        static unsigned recs;
        if (++recs > SM1_EXEC_MAX_RECS) {
            fprintf(stderr, "SM1EXEC rec walk cap hit\n");
            break;
        }
        uint32_t w0 = sm1_exec_ld(rec);
        uint32_t w1 = sm1_exec_ld(rec + 4);
        uint32_t w2 = sm1_exec_ld(rec + 8);
        uint32_t nbytes = w1 >> 16;
        uint32_t ring_off = w1 & 0xffff;
        uint64_t data = w2 & 0x7fffffffull;
        fprintf(stderr, "SM1EXEC rec @0x%08llx {next=0x%08x w1=0x%08x "
                "nbytes=%u ring_off=%u ptr=0x%08llx}\n",
                (unsigned long long)rec, w0, w1, nbytes, ring_off,
                (unsigned long long)data);
        if (nbytes && nbytes <= 4096 && data) {
            static uint8_t buf[4096];
            cpu_physical_memory_read(data, buf, nbytes);
            uint64_t dst = SM1_EXEC_DATA(ring_off);
            cpu_physical_memory_write(dst, buf, nbytes);
            unsigned nz = 0;
            for (uint32_t i = 0; i < nbytes; i++) {
                nz += buf[i] != 0;
            }
            fprintf(stderr, "SM1EXEC input copy %u B 0x%08llx -> 0x%08llx "
                    "fnv=%08x nonzero=%u first=%02x %02x %02x %02x\n",
                    nbytes, (unsigned long long)data, (unsigned long long)dst,
                    sm1_exec_fnv1a(data, nbytes), nz,
                    buf[0], buf[1], buf[2], buf[3]);
            /* read back the first ring words through the guest bus */
            fprintf(stderr, "SM1EXEC ring readback:");
            for (unsigned i = 0; i < 4; i++) {
                fprintf(stderr, " %08x", sm1_exec_ld(dst + 4 * i));
            }
            fprintf(stderr, "\n");
        }
        if (!(w0 & ~1ull)) {
            break;
        }
        rec = w0 & ~1ull;
    }

    /* 3. the configuration rows the guest packed into +0x938..+0x97c. */
    fprintf(stderr, "SM1EXEC cfg +0x938..0x97c:");
    for (unsigned off = 0x938; off <= 0x97c; off += 4) {
        fprintf(stderr, " %08x", s->reg[off / 4]);
    }
    fprintf(stderr, "\n");

    /* 4. the bank program reference trace (ISA-RE-BATCH-11 grammar). */
    for (unsigned q = 0; q < 4; q++) {
        uint64_t base = SM1_EXEC_G + q * 0x4000ull;
        uint32_t first = sm1_exec_ld(base);
        if (!first) {
            fprintf(stderr, "SM1EXEC b%u @0x%08llx NOT LOADED (first word 0)\n",
                    q, (unsigned long long)base);
            continue;
        }
        fprintf(stderr, "SM1EXEC b%u @0x%08llx first=%08x fnv=%08x\n", q,
                (unsigned long long)base, first,
                sm1_exec_fnv1a(base, SM1_EXEC_BANK_WORDS * 4));
        for (unsigned i = 0; i < SM1_EXEC_BANK_WORDS / 2; i++) {
            uint32_t we = sm1_exec_ld(base + 8ull * i);
            uint32_t wo = sm1_exec_ld(base + 8ull * i + 4);
            uint16_t a = we >> 16, sb = we & 0xffff;
            uint16_t c = wo >> 16, d = wo & 0xffff;
            unsigned tw_s = ((sb >> 13) << 9) | ((sb >> 4) & 0x1ff);
            unsigned tw_d = ((d >> 13) << 9) | ((d >> 4) & 0x1ff);
            uint32_t v_s = tw_s < 4608 ? sm1_exec_ld(SM1_EXEC_DATA(tw_s)) : 0;
            uint32_t v_d = tw_d < 4608 ? sm1_exec_ld(SM1_EXEC_DATA(tw_d)) : 0;
        fprintf(stderr, "SM1EXEC b%u w%04u A=%04x(P%u.w%u.s%u) "
                "S=%04x(w%u.s%u,v=%08x) C=%04x(P%u.w%u.s%u) "
                "D=%04x(w%u.s%u,v=%08x)\n",
                q, 2 * i, a, a >> 13, (a >> 4) & 0x1ff, a & 0xf,
                sb, tw_s, sb & 0xf, v_s,
                c, c >> 13, (c >> 4) & 0x1ff, c & 0xf,
                d, tw_d, d & 0xf, v_d);
        }
    }
}

/*
 * EXECUTOR MILESTONE-2 (SM1ENGINE=1): the engine's frame decode, functionally
 * reconstructed, delivered through the measured guest contracts.
 *
 * What closed in batch-13 (2026-10-06):
 *   - the engine's FUNCTION is established numerically. The static type1
 *     image carries the MPEG-1 Layer III antialias table -- cs[0..7] EXACT
 *     ISO values in Q23 at words 1416/1418/../1430, interleaved with
 *     2+ca[0..7] at the odd words, ca[0]/cs[5] recomputed as the exact
 *     rotation sqrt(1-cs^2) -- plus the quarter-octave scalefactor ladders
 *     2^(-k/4) (Q23, words 1448..1529) and 24-bit signed coefficient data in
 *     the type2 window. The input per kick is one raw MP3 frame (batch-13
 *     probe4: kick #1 FNV-matches frame 0 of the actual file on the disk
 *     image, 320 kbps 44.1 kHz stereo). No public bank-ISA candidate ever
 *     matched the bank bytes (ARM, CalmRISC16+MAC2424, Exynos SRP: all
 *     calibrated negatives), so the per-INSTRUCTION encoding stays open --
 *     ISA-RE-BATCH-11's grammar and the batch-13 dataflow tables stand, and
 *     the interpreter built on them remains the long-term route. What runs
 *     here is the decoder FUNCTION the silicon demonstrably performs,
 *     executed at the kick point, on the real input frame, in the engine's
 *     own place in the machine. Not a host shortcut: the guest firmware,
 *     its submit chain, the completion handshake, the render pump, the
 *     codec-buffer dispenser, the DMA and the I2S all stay real.
 *
 *   - the decode core is minimp3 (public domain; bit-reservoir aware via
 *     L3_restore_reservoir), included above with no SIMD. The decoder state
 *     persists across kicks, exactly like the silicon's main-data window.
 *
 * Delivery -- every address below is MEASURED (t31/t33/t34 watchpoints and
 * the pump decode behind sm1_publish_slots' comment block):
 *   - descriptor 0x2203ff28: +0 enable mask halfword (bit0, and bit1 when the
 *     stream state byte is 6 -> publish 0x0003), +2 must read zero,
 *     +4 = samples per slot (the firmware derives buffer = pcm + idx*fps*2);
 *   - slot array 0x22038000: +2 = slot count, +10+i*4 = slot ready halfword,
 *     +12+i*4 = samples in slot;
 *   - PCM ring 0x22038100: slot i at +i*4096 (4096 B = 2048 samples = 1024
 *     stereo frames -- the pump's measured per-request copy r5*4);
 *   - the geometry is the ENGINE's to publish (0x080a3c54/58 read it back),
 *     so it is self-consistent by construction. Slot count defaults to 2:
 *     the only full slots that fit between the PCM base and the firmware's
 *     save/restore region at 0x2203a500 (CACHE-CONSUMER-CONTRACT.md);
 *     SM1ENGINE_NSLOTS overrides for the bisection experiment.
 *   - completion per decoded frame: +0xa98 bit12 (the render pump 0x080a4768
 *     tests it) until the next kick, and the VIC line 18 pulse when the
 *     guest armed +0xa8c -- the handler then self-serves (+0x400=1,
 *     +0x1008=1) and signals the render event 0x22008ce8
 *     (COMPLETION-CONTRACT.md). The pulse shape reuses the measured
 *     SM1_IRQ machinery (assert, bounded by sm1_irq_off).
 *
 * Deliberately NOT touched: the legacy SM1_SLOTS/SM1_DECODE experiments
 * (they must stay unset; this path is independent of them), the HLE audio
 * shim (no HLE_* variable is involved), and the +0xa48 position model
 * (still derived from the I2S transmit count).
 */
#define SM1_ENGINE_DESC        0x2203ff28ull
#define SM1_ENGINE_ARRAY       0x22038000ull
#define SM1_ENGINE_PCM         0x22038100ull
#define SM1_ENGINE_SLOT_SAMPLES 2304         /* 1152 stereo frames = one MP3
                                              * frame; 2 slots span exactly
                                              * 0x22038100..0x2203a500 */
#define SM1_ENGINE_SLOT_BYTES  (SM1_ENGINE_SLOT_SAMPLES * 2)   /* 4608 B */
#define SM1_ENGINE_MAX_SLOTS   2
#define SM1_ENGINE_REC_MAX     16

static bool sm1_engine_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SM1ENGINE");
        on = (e && *e && *e != '0') ? 1 : 0;
        if (on) {
            fprintf(stderr, "SM1ENGINE: executor milestone-2 armed -- kick decodes "
                    "the input frame (minimp3 core) and delivers it through the "
                    "slot contract at 0x22038100 with VIC-18 completion\n");
        }
    }
    return on == 1;
}

static unsigned sm1_engine_nslots(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("SM1ENGINE_NSLOTS");
        n = (e && *e) ? atoi(e) : SM1_ENGINE_MAX_SLOTS;
        if (n < 1) {
            n = 1;
        }
        if (n > SM1_ENGINE_MAX_SLOTS) {
            n = SM1_ENGINE_MAX_SLOTS;
        }
    }
    return (unsigned)n;
}

static void sm1_engine_w16(uint64_t addr, uint16_t v)
{
    uint16_t le = cpu_to_le16(v);
    cpu_physical_memory_write(addr, &le, 2);
}

static uint16_t sm1_engine_r16(uint64_t addr)
{
    uint16_t le = 0;
    cpu_physical_memory_read(addr, &le, 2);
    return le16_to_cpu(le);
}

/*
 * desc+2 (0x2203ff2a) is the acquire's RETURN CODE (0x080bdd04 tail) and the
 * task needs exactly 2 to keep the submit chain alive, while a 0 lets the
 * acquire check slot readiness and hand the buffer out. So it is a live STATE
 * the engine owns: 0 while the slot the guest's own index points at is still
 * ready for pickup, 2 once it has been consumed. The acquire's control block
 * IS the output object (its literal is 0x08a0ec5c): +0xc = slot array, +0x14 =
 * the guest's ring index -- both readable right out of guest RAM.
 */
static bool sm1_engine_verbose_frame(uint64_t frame);

static void sm1_engine_desc_update(IPodClassicSM1State *s)
{
    static int last_code = -1;
    static uint64_t desc_changes;
    uint32_t idx = sm1_exec_ld(0x08a0ec5c + 0x14);
    uint32_t array = sm1_exec_ld(0x08a0ec5c + 0xc);
    int code = 2;
    if (array == 0x22038000u && idx < 16u) {
        uint16_t ready = sm1_engine_r16(0x22038000ull + 10 + idx * 4);
        code = (ready == 1) ? 0 : 2;
    }
    if (code != last_code) {
        if (sm1_engine_verbose_frame(++desc_changes)) {
            fprintf(stderr, "SM1ENGINE desc+2 -> %d (idx=%u)\n", code, idx);
        }
        last_code = code;
    }
    sm1_engine_w16(SM1_ENGINE_DESC + 2, (uint16_t)code);
}

/*
 * FLOW OBSERVER (same SM1ENGINE gate; observational only, no guest writes).
 *
 * The batch-15 static decode closed the frame-source architecture: the audio
 * task (IRAM work fn 0x22006180) per scheduler tick pulls a frame from the
 * reader object via vtable+0x1c (0x082110e4), submits it through the
 * processing function, then pumps/acquires/copies and releases the frame via
 * vtable+0x20. The reader (0x08ae4978, globals 0x08a09d28/0x08a09d68, vtable
 * 0x08993f40) records what its getter last yielded at +0x1c4 (get path) and
 * +0x1c0 (release path), and gates on the int16 pair at +0x1c8/+0x1ca. The
 * run-5 stall -- the same frame resubmitted five times -- is a frozen cursor
 * in this object or in the per-handle stream node its iterator resolves
 * through. Sampling those fields plus the 2-deep message-queue counters
 * (singleton 0x08a09d2c, the filled-buffer exchange between the audio task
 * and the sink side) makes a run log alone attribute the stall. Prints on
 * change of the sampled fingerprint, plus a 2 s heartbeat while quiescent.
 */
static bool sm1_engine_verbose_frame(uint64_t frame)
{
    static int full = -1;
    if (full < 0) {
        const char *flow = getenv("SM1FLOWTRACE");
        const char *play = getenv("IPOD_PLAYTRACE");
        full = (flow && *flow && *flow != '0') ||
               (play && *play && *play != '0');
    }
    return full || (frame && (frame <= 4 || frame % 500 == 0));
}

static void sm1_engine_observe(void)
{
    static unsigned ticks;
    static uint32_t last[23];
    static bool have_last;
    uint32_t k[23];
    bool changed;
    uint32_t rp;
    int i;

    /* This RAM walk is diagnostic only; keep it out of ordinary playback. */
    if (!sm1_engine_verbose_frame(0)) {
        return;
    }
    rp = sm1_exec_ld(0x08a09d28);

    k[0] = rp;
    k[1] = k[2] = k[3] = k[4] = k[5] = 0;
    if (rp >= 0x08000000u && rp < 0x09000000u) {
        k[1] = sm1_exec_ld(rp + 0x1c0);
        k[2] = sm1_exec_ld(rp + 0x1c4);
        k[3] = sm1_exec_ld(rp + 0x1c8);
        k[4] = sm1_exec_ld(rp + 0x1bc);
        k[5] = sm1_exec_ld(rp + 0x1d0);
    }
    for (i = 0; i < 5; i++) {
        k[6 + i] = sm1_exec_ld(0x08a09d2c + i * 4);
    }
    /* batch-16 wake chain: sink state obj 0x22008ca0, kernel events
     * 0x22008cb0/0x22008cb4, DMAC0 per-channel TC handler table 0x2200ae7c
     * (ch0/ch1/ch2; the ISR at 0x220027cc dispatches through it) */
    k[11] = sm1_exec_ld(0x22008ca0);
    k[12] = sm1_exec_ld(0x22008cac);
    k[13] = sm1_exec_ld(0x22008cb0);
    /* the audio task object (runtime-confirmed 0x220106dc): +0x4c = kernel
     * task state, +0x44.. flag bytes */
    k[14] = sm1_exec_ld(0x220106dc + 0x4c);
    k[15] = sm1_exec_ld(0x220106dc + 0x44);
    k[16] = sm1_exec_ld(0x2200ae7c);
    k[17] = sm1_exec_ld(0x2200ae80);
    /* batch-17: the wake event is a SEMAPHORE whose live count sits INSIDE
     * the inner object ([0x22008cb0]) -- deref it, this is the measurement
     * batches 15/16 missed */
    k[18] = k[19] = k[20] = 0;
    if (k[13] >= 0x08000000u && k[13] < 0x10000000u) {
        k[18] = sm1_exec_ld(k[13]);
        k[19] = sm1_exec_ld(k[13] + 4);
        k[20] = sm1_exec_ld(k[13] + 8);
    }
    /* batch-18: the pump's completion event 0x22008ce8 -- same treatment.
     * Its inner count tells whether the completion handler's signal lands
     * while the pump is blocked (0x804b8f8 is the V/signal op: the handler
     * 0x0802eb20 signals it via 0x080bba50 when the object flags bit2 is
     * set; 0x804baa4 is the blocking P, NOT the signal). */
    k[21] = k[22] = 0;
    uint32_t ev = sm1_exec_ld(0x22008ce8);
    if (ev >= 0x08000000u && ev < 0x10000000u) {
        k[21] = sm1_exec_ld(ev);
        k[22] = sm1_exec_ld(ev + 4);
    }

    changed = !have_last;
    for (i = 0; i < 23; i++) {
        changed = changed || (k[i] != last[i]);
    }
    ticks++;
    if (!changed && (ticks % 400) != 0) {
        return;
    }
    memcpy(last, k, sizeof(last));
    have_last = true;
    fprintf(stderr,
            "SM1OBS t=%u reader=0x%08x pos1c0=0x%08x pos1c4=0x%08x "
            "gates1c8=0x%08x flg1bc=0x%08x flg1d0=0x%08x "
            "msgq=%08x %08x %08x %08x %08x "
            "sink=%08x/%08x task=%08x/%08x sem=%08x,%08x,%08x "
            "pev=%08x,%08x tcH=%08x,%08x\n",
            ticks, k[0], k[1], k[2], k[3], k[4], k[5],
            k[6], k[7], k[8], k[9], k[10],
            k[11], k[12], k[14], k[15], k[18], k[19], k[20],
            k[21], k[22], k[16], k[17]);
}

static QEMUTimer *engine_tick_timer;

static void sm1_engine_tick(void *opaque)
{
    IPodClassicSM1State *s = opaque;

    sm1_engine_desc_update(s);
    sm1_engine_observe();
    timer_mod(engine_tick_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              5 * 1000000ll);
}

/* A compressed frame's playback duration is not its decode latency.
 * Decode is already completed before this report is queued. Keep delivery
 * asynchronous through the existing timer and level/ack path, but do not
 * add a whole playback frame of idle time after every executed command.
 * I2S/DMA remains the sample-clock authority; slots still belong to guest. */
#define SM1_ENGINE_FRAME_NS 0ll
static volatile uint64_t c7_sm1_pcm_bytes;
static volatile uint64_t c7_sm1_frames;
static volatile uint64_t c7_sm1_completions;

static QEMUTimer *engine_done_timer;

static void sm1_engine_frame(IPodClassicSM1State *s, uint32_t value)
{
    static mp3dec_t dec;
    static bool dec_init;
    static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    static uint8_t framebuf[4096];
    static unsigned kicks, filled;
    uint64_t head = value & ~1ull;
    uint32_t nbytes = 0;
    unsigned nslots = sm1_engine_nslots();
    unsigned walk = 0;
    int64_t kick_vns;
    static int playtrace = -1;
    unsigned pcm_nonzero = 0;

    if (playtrace < 0) {
        const char *e = getenv("IPOD_PLAYTRACE");
        playtrace = (e && *e && *e != '0');
    }
    kick_vns = playtrace == 1 ? qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) : 0;

    /* a new kick: the previous frame's bit12 report is consumed */
    s->frame_pending = false;

    /* walk the record chain; keep the last input record's bytes (the frame) */
    for (uint64_t rec = head; rec && rec >> 32 == 0; ) {
        uint32_t w0 = sm1_exec_ld(rec);
        uint32_t w1 = sm1_exec_ld(rec + 4);
        uint32_t w2 = sm1_exec_ld(rec + 8);
        uint32_t rn = w1 >> 16;
        uint64_t data = w2 & 0x7fffffffull;
        if (rn && rn <= sizeof(framebuf) && data) {
            cpu_physical_memory_read(data, framebuf, rn);
            nbytes = rn;
        }
        if (!(w0 & ~1ull) || ++walk >= SM1_ENGINE_REC_MAX) {
            break;
        }
        rec = w0 & ~1ull;
    }
    if (!nbytes) {
        if (playtrace == 1) {
            fprintf(stderr, "SM1ENGINE kick #%u: no input record payload vns=%"
                    PRId64 "\n", ++kicks, kick_vns);
        } else {
            fprintf(stderr, "SM1ENGINE kick #%u: no input record payload\n", ++kicks);
        }
        return;
    }

    if (!dec_init) {
        mp3dec_init(&dec);
        dec_init = true;
    }
    mp3dec_frame_info_t info;
    sm1_mp3_reservoir_deficit = false;
    sm1_mp3_reservoir_saved = false;
    int nsamples = mp3dec_decode_frame(&dec, framebuf, nbytes, pcm, &info);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < nbytes; i++) {
        h = (h ^ framebuf[i]) * 16777619u;
    }
    if (playtrace == 1 && nsamples > 0 && info.channels > 0 &&
        nsamples * info.channels <= MINIMP3_MAX_SAMPLES_PER_FRAME) {
        for (int j = 0; j < nsamples * info.channels; j++) {
            pcm_nonzero += pcm[j] != 0;
        }
    }
    ++kicks;
    if (playtrace == 1) {
        fprintf(stderr, "SM1ENGINE kick #%u frame %u B fnv=%08x -> samples=%d "
                "ch=%d frame_bytes=%d vns=%" PRId64 " pcm_nz_trace=%u\n",
                kicks, nbytes, h, nsamples, info.channels, info.frame_bytes,
                kick_vns, pcm_nonzero);
    } else if (sm1_engine_verbose_frame(kicks)) {
        fprintf(stderr, "SM1ENGINE kick #%u frame %u B fnv=%08x -> samples=%d "
                "ch=%d frame_bytes=%d\n", kicks, nbytes, h, nsamples,
                info.channels, info.frame_bytes);
    }
    if (nsamples <= 0 || !info.channels) {
        /* A complete, valid Layer III input was consumed and its reservoir
         * saved. Report this executed operation using the same asynchronous
         * completion as a PCM-producing frame. Publish no PCM slot, ready
         * word or sample length: the guest's existing no-output/state-2 path
         * decides how to continue. Invalid/truncated inputs retain the
         * previous error behavior and do not qualify for this completion. */
        if (nsamples == 0 && sm1_mp3_reservoir_deficit &&
            sm1_mp3_reservoir_saved && info.layer == 3 && info.channels > 0 &&
            info.frame_offset == 0 && info.frame_bytes == (int)nbytes &&
            engine_done_timer) {
            timer_del(engine_done_timer);
            timer_mod(engine_done_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      SM1_ENGINE_FRAME_NS);
            fprintf(stderr, "SM1ENGINE consumed noPCM frame: bytes=%u need=%d "
                    "reserv_pre=%d reserv_post=%d async_ns=%" PRId64 "\n",
                    nbytes, sm1_mp3_need, sm1_mp3_pre, sm1_mp3_post,
                    (int64_t)SM1_ENGINE_FRAME_NS);
        }
        return;
    }

    /* publish the geometry (idempotent; the firmware reads it back). desc+2
     * is NOT published here: it is a live state (see sm1_engine_desc_update)
     * -- 0 while the guest's next slot is ready for pickup, 2 once consumed,
     * which is what makes the audio task alternate pickup/submit (measured:
     * constant 0 = pickups without submits, constant 2 = submits without
     * pickups, both stall after 2-5 frames). */
    sm1_engine_w16(SM1_ENGINE_DESC + 0, 0x0003);
    sm1_engine_w16(SM1_ENGINE_DESC + 4, SM1_ENGINE_SLOT_SAMPLES);
    sm1_engine_w16(SM1_ENGINE_ARRAY + 2, nslots);

    /* one decoded frame = one slot, in ring order. The slot index is NOT
     * ours to choose: the processing function writes object+0x14 before each
     * submit (0x0809b2c4: str sl,[r5,#0x14]) and the acquire checks exactly
     * that slot (r4+0x14) before handing the buffer out -- filling any other
     * slot makes the ready word it polls stay 0 forever (measured, T42
     * runs 2-4: no pickup at all). */
    size_t out = (size_t)nsamples * info.channels * sizeof(int16_t);
    c7_sm1_pcm_bytes += out;
    c7_sm1_frames++;
    uint32_t want = sm1_exec_ld(0x08a0ec5c + 0x14);
    unsigned i = (want < nslots) ? (unsigned)want : (unsigned)(want % nslots);
    if (want >= nslots) {
        fprintf(stderr, "SM1ENGINE: guest slot index %u >= nslots %u (wrap)\n",
                want, nslots);
    }
    uint64_t dst = SM1_ENGINE_PCM + (uint64_t)i * SM1_ENGINE_SLOT_BYTES;
    cpu_physical_memory_write(dst, pcm, out);
    sm1_engine_w16(SM1_ENGINE_ARRAY + 10 + i * 4, 1);
    sm1_engine_w16(SM1_ENGINE_ARRAY + 12 + i * 4, (uint16_t)(out / 2));
    if (sm1_engine_verbose_frame(filled + 1)) {
        fprintf(stderr, "SM1ENGINE slot %u (guest idx %u) <- %zu B (%u stereo frames), fill #%u\n",
                i, want, out, (unsigned)(out / 4), filled + 1);
    }
    filled++;
    sm1_engine_desc_update(s);

    /* Report the completed command on the next timer dispatch, never inline
     * in the MMIO write. The existing level IRQ stays asserted until the
     * guest's real callback acknowledges it. */
    if (engine_done_timer) {
        timer_del(engine_done_timer);
        timer_mod(engine_done_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SM1_ENGINE_FRAME_NS);
    }
}

static void sm1_engine_done(void *opaque)
{
    IPodClassicSM1State *s = opaque;

    s->frame_pending = true;
    c7_sm1_completions++;
    if (s->irq_armed && !s->irq_pending) {
        s->irq_pending = true;
        s->irqs++;
        qemu_irq_raise(s->irq);
        /*
         * NO bounded pulse here. The line is held LEVEL until the guest
         * acknowledges the engine through its callback-tail clear write
         * (+0xc48 = 4, pc 0x080c17d4). The
         * pulse variant (the SM1_IRQ experiment's backstop) loses the
         * interrupt whenever the CPU sits in a masked window when the
         * 26 ms-late completion arrives: measured T42 run14 -- the model
         * raised VIC-18 twice, the guest's line-18 handler 0x0802eb20 never
         * executed once, the pump's blocking waits all timed out, and the
         * per-frame advance (+0x1008=1) never happened. This model holds the
         * line until the guest callback clears it; hardware timing is not
         * established by this algorithm-level implementation.
         */
        if (sm1_engine_verbose_frame(s->irqs)) {
            fprintf(stderr, "SM1ENGINE frame done: VIC-18 raised #%" PRIu64 "\n",
                    s->irqs);
        }
    }
}

/*
 * The eight sub-block "state" registers (sub+0x14). Their absolute offsets are
 * the sub-block bases {0,0x20,0x40,0x60,0x100,0x180,0x200,0x230} plus 0x14. Each
 * one's command register (sub+0x10) sits exactly 4 bytes below it. We enumerate
 * them rather than test "offset ends in 4" so we never mistake an unrelated
 * register (e.g. +0x404, whose 4-below neighbour is the +0x400 run register) for
 * a sub-block state.
 */
static const hwaddr sm1_sub_state_off[] = {
    0x0014, 0x0034, 0x0054, 0x0074,   /* idx 0..3: base + idx*0x20 + 0x14 */
    0x0114, 0x0194, 0x0214, 0x0244,   /* idx 4..7                          */
};

static bool sm1_is_sub_state(hwaddr offset)
{
    for (size_t i = 0; i < ARRAY_SIZE(sm1_sub_state_off); i++) {
        if (offset == sm1_sub_state_off[i]) {
            return true;
        }
    }
    return false;
}

static bool sm1_is_sub_cmd(hwaddr offset)
{
    /* A command register is the state register minus 4. */
    for (size_t i = 0; i < ARRAY_SIZE(sm1_sub_state_off); i++) {
        if (offset == sm1_sub_state_off[i] - 4) {
            return true;
        }
    }
    return false;
}

static uint64_t ipod_classic_sm1_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IPodClassicSM1State *s = opaque;
    uint32_t val;
    bool was_pending = s->irq_pending;

    /*
     * ANY read of this block acknowledges the engine interrupt.
     *
     * It used to be only +0xa98, on the reasoning that the line-18 handler's
     * status getter reads exactly that. MEASURED, that is not enough: in one run
     * the model raised the line FOUR times and the handler at 0x08017378 was
     * entered 1,196,485 times -- and QEMU's own -d int log counted ~1.05 million
     * IRQ exceptions. The line stays asserted for the whole 5 ms until the next
     * tick drops it, and the guest re-enters the handler for that entire window,
     * which starves every audio thread. That is why the decode side crawled:
     * dpump ran 8 times and creditsig 493 while the handler ran 1.2 million.
     *
     * Whatever the getter at 0x080a7638 actually touches, the handler does reach
     * this block, so taking any read as the acknowledge cannot leave the level
     * wedged. The STAT value below still reports bit 2 from the pre-clear state,
     * so the handler that triggered on this interrupt still sees it set and
     * signals its event.
     */
    /* Algorithm engine: status reads observe completion, not acknowledge it.
     * The current payload's callback tail writes +c48 bit2 (080baba8 ->
     * 080c17bc), after signalling the pump. Keep the separate legacy pulse
     * experiment's read-clear behavior unchanged. */
    if (was_pending && !sm1_engine_on()) {
        s->irq_pending = false;
        qemu_irq_lower(s->irq);
    }

    /* Above the modelled low page: a plain zero window, as before. */
    if (offset >= S5L8702_SM1_REG_BYTES) {
        sm1_count_hit(offset, false, 0);
        return 0;
    }

    switch (offset) {
    case S5L8702_SM1_ID:
        /* Block identity. A mismatch here is an eternal `b .`. */
        val = S5L8702_SM1_ID_MAGIC;
        break;

    case S5L8702_SM1_STAT:
        /*
         * Clock/engine status, read by getter 0x080a7638 (r1=0 -> [base+0xa98],
         * top five bits masked off by the firmware itself).
         *
         * Bit 2 serves two readers:
         *  - the bring-up spin at 0x08081af8 ("clock ready"), answered from
         *    `powered` as before; and
         *  - the VIC line-18 handler 0x08017378, which does `tst r0,#4` and only
         *    signals the render-credit event 0x22008c58 when the bit is set. So
         *    while we are asserting the engine interrupt the bit must read set,
         *    or the handler would take its early exit and signal nothing.
         */
        val = (s->powered || was_pending) ? S5L8702_SM1_STAT_READY : 0;
        /*
         * SM1_FRAME_BIT=1 also reports bit 12 while a frame report is pending.
         *
         * The waiter on the render event (0x0808af58) reads this same word and
         * does `tst r0,#0x1000` before doing its per-frame work, so with only
         * bit 2 answered that branch is never taken -- which looked like the
         * missing piece. MEASURED, it is not: with the bit set, dpump went 16 ->
         * 14 and aacmath stayed at 1. So bit 12 means something else, and it is
         * left OFF by default rather than inventing a status the part may never
         * report. Kept as a switch because the finding -- that this waiter gates
         * on bit 12 while the interrupt handler gates on bit 2 -- is real and
         * worth being able to re-test.
         */
        if (was_pending && getenv("SM1_FRAME_BIT")) {
            val |= S5L8702_SM1_STAT_FRAME;
        }
        /*
         * Executor milestone-2 (SM1ENGINE): a decoded frame is waiting. The
         * render pump 0x080a4768 reads this same word and only does its
         * per-frame work when bit 12 is set (COMPLETION-CONTRACT.md), so the
         * frame report has to be visible to THAT reader -- which runs after
         * the interrupt handler's own read already acknowledged the line.
         * The engine clears the flag at its next kick.
         */
        if (s->frame_pending) {
            val |= S5L8702_SM1_STAT_FRAME;
        }
        /* Legacy pulse experiments clear on read; the algorithm engine clears
         * on the callback's +c48 write. Both observe the incoming pending state. */
        break;

    case S5L8702_SM1_RUN:
        /*
         * Run/run-state. The start path writes 1 here and both it and the
         * writer's own retry loop read the value straight back, so return the
         * latched word (write 1 -> read 1). The &3 the poll applies is the
         * firmware's, not ours.
         */
        val = s->reg[S5L8702_SM1_RUN / 4];
        break;

    default:
        if (sm1_is_sub_state(offset)) {
            /*
             * Sub-block state (sub+0x14): a STATE MACHINE, not a sticky latch.
             *
             * It reports "reached target state" (7) while the channel is
             * commanded up or running, and returns to 0 (idle) once it has been
             * commanded to stop. Two firmware sites read it and they want
             * OPPOSITE values, which is what forces the distinction:
             *
             *  - The power-up 0x080ba768 writes cmd 4, polls for 7 up to ten
             *    times, writes cmd 16 (run) -- and finally writes cmd 96 (stop).
             *  - The render 0x0808170c then reads sub-block 4's state at
             *    0x080820f0 and REQUIRES 0:
             *        80820f0: bl   0x80b8470     ; read sub+0x14, & 7
             *        80820f4: cmp  r0, #0
             *        80820f8: bne  0x80821a8     ; non-zero -> SKIP the build
             *    The block it skips, 0x08082100-0x080821a0, is the engine
             *    CONTEXT CONSTRUCTION: the buffer copy, the descriptor
             *    allocation (0x080b67a8), the chain append (0x080bfa58) and the
             *    address translation (0x08075d80).
             *
             * Answering a sticky 7 satisfied the power-up poll but made the
             * render skip its own context build forever -- which is exactly the
             * "missing render-context construction step" wiki/19 s11.25 was
             * looking for, and why every gate forced downstream revealed the
             * layer below it was also unbuilt.
             *
             * So: 7 while the last command is up (4) or run (16); 0 once the
             * channel has been stopped (96) or never commanded.
             */
            uint32_t cmd = s->reg[(offset - 4) / 4];
            switch (cmd) {
            case S5L8702_SM1_SUB_CMD_UP:
            case S5L8702_SM1_SUB_CMD_RUN:
                val = S5L8702_SM1_SUB_STATE_READY;
                break;
            default:            /* 0 = never commanded, 96 = stopped */
                val = 0;
                break;
            }
        } else if (sm1_pos_reg(s, offset, &val)) {
            /* ENGINE region window readback; legacy experimental cursor. */
        } else {
            /* Latch: config strobes and undecoded status words read back as
             * stored (zero at reset). */
            val = s->reg[offset / 4];
        }
        break;
    }

    sm1_count_hit(offset, false, val);

    if (sm1_trace_on()) {
        fprintf(stderr, "SM1TRACE rd +0x%03" HWADDR_PRIx " -> 0x%08x  "
                "pc~0x%08x%s\n", offset, val, sm1_trace_pc(),
                offset == S5L8702_SM1_STAT ? "  [STAT bit2=ready]" :
                offset == S5L8702_SM1_ID ? "  [ID]" :
                offset == S5L8702_SM1_RUN ? "  [RUN]" :
                sm1_is_sub_state(offset) ? "  [SUBSTATE]" : "");
    }
    return val;
}

static void ipod_classic_sm1_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IPodClassicSM1State *s = opaque;

    if (offset >= S5L8702_SM1_REG_BYTES) {
        /*
         * A BLIND SPOT worth naming. The block's window is 1 MB and only its
         * first 4 KB is modelled; everything above was dropped here silently,
         * BEFORE the trace below, so SM1TRACE could never show it either. If
         * the firmware puts descriptors -- or DSP code -- up there, the whole
         * lot has been going into a hole and no measurement in this
         * investigation would have noticed.
         *
         * Counting is enough to answer that, and costs nothing when the trace
         * is off. Doubling schedule so a bulk upload is loud and a stray write
         * stays quiet.
         */
        if (sm1_hi_on()) {
            static uint64_t hi_writes;
            static hwaddr hi_lo = (hwaddr)-1, hi_hi;
            uint64_t n = ++hi_writes;

            if (offset < hi_lo) {
                hi_lo = offset;
            }
            if (offset > hi_hi) {
                hi_hi = offset;
            }
            if ((n & (n - 1)) == 0 || n % 100000 == 0) {
                fprintf(stderr, "SM1TRACE HI-WRITE #%" PRIu64 " +0x%05"
                        HWADDR_PRIx " = 0x%08x  (rango visto +0x%05" HWADDR_PRIx
                        "..+0x%05" HWADDR_PRIx ")  pc~0x%08x\n",
                        n, offset, (uint32_t)value, hi_lo, hi_hi,
                        sm1_trace_pc());
            }
        }
        sm1_count_hit(offset, true, (uint32_t)value);
        return;
    }

    /*
     * The engine is "powered" (clock will lock, run bit will follow) once the
     * firmware issues any of its bring-up writes. This is the single derived
     * signal behind +0xa98 bit2; deriving it from the guest's own first move
     * keeps the model coherent instead of hard-wiring the bit on. VERIFIED that
     * each of these writes is part of a bring-up; that they are what "powers"
     * the clock is INFERIDO.
     */
    switch (offset) {
    case S5L8702_SM1_COMMIT:     /* +c48: current callback clears bit2 */
        s->powered = true;
        if (sm1_engine_on() && (value & S5L8702_SM1_STAT_READY) &&
            s->irq_pending) {
            s->irq_pending = false;
            qemu_irq_lower(s->irq);
            /* The pump consumes the bit12 report AFTER this callback.
             * Clearing the IRQ must not discard that frame report. */
            if (sm1_engine_verbose_frame(s->irqs)) {
                fprintf(stderr, "SM1ENGINE guest IRQ clear: +c48=0x%08x"
                    " vns=%" PRId64 " pc=0x%08x frame=%u\n",
                    (uint32_t)value, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                        sm1_trace_pc(), s->frame_pending);
            }
        }
        break;
    case 0xa90:
        /* Preserve the existing initialization behavior. Current payload
         * writes 0x07ffffff here before its first kick; the per-frame callback
         * instead ends with +c48=4. Further +a90 semantics remain open. */
        if (s->irq_pending) {
            s->irq_pending = false;
            qemu_irq_lower(s->irq);
        }
        break;
    case S5L8702_SM1_CODEC_EN:   /* +0x824 codec enable   */
        s->powered = true;
        break;
    case S5L8702_SM1_RUN:        /* +0x400 start (nonzero) */
        if (value & S5L8702_SM1_RUN_MASK) {
            s->powered = true;
        }
        break;
    case S5L8702_SM1_INTEN:      /* +0xa8c interrupt enable */
        /*
         * The engine's interrupt enable. The firmware arms it from a four-
         * instruction leaf that writes the bare constant:
         *
         *   08017364: ldr r1, =0x38500000 ; mov r0, #4 ; str r0, [r1, #0xa8c]
         *
         * called at 0x080820e4 from the render's own path. Bit 2 here pairs with
         * bit 2 of the status at +0xa98, which the VIC line-18 handler
         * (0x08017378) tests before signalling the render-credit event
         * 0x22008c58. In other words the firmware is asking the engine to
         * interrupt it when a frame is done -- and then waits for exactly that.
         *
         * We used to keep +0xa8c as a plain latch and had no interrupt line at
         * all, so the request could never be answered: the render pump's 500 ms
         * wait (0x0808af58) always timed out and tore the codec down. Arming
         * here makes the trigger the guest's own, rather than a timer we chose.
         */
        if (value & S5L8702_SM1_INTEN_FRAME) {
            if (!s->irq_armed) {
                s->irq_armed = true;
                fprintf(stderr, "SM1_IRQ: guest armed the engine interrupt "
                        "(+0xa8c = 0x%02x); VIC line 18 live\n",
                        (uint32_t)value);
            }
        } else {
            s->irq_armed = false;
        }
        break;
    default:
        if (sm1_is_sub_cmd(offset) && value != 0) {
            s->powered = true;   /* a channel was commanded up */
        }
        break;
    }

    s->reg[offset / 4] = (uint32_t)value;

    /*
     * The submit kick: the firmware writing the record-chain head|1 into
     * sub-block 4's base (+0x100). Under SM1EXECTRACE this walks the chain,
     * copies the input into the engine ring and traces the bank programs --
     * diagnostics only, see the milestone-1 note above; the latch below stays
     * the only effect the guest otherwise sees.
     */
    if (offset == SM1_EXEC_KICK_OFF && sm1_exec_on()) {
        sm1_exec_kick(s, (uint32_t)value);
    }

    /*
     * Executor milestone-2 (SM1ENGINE=1): the same kick, taken as work. This
     * decodes the input frame the record chain points at and delivers it
     * through the measured slot contract, then completes the frame (bit12 +
     * the guest's own VIC-18). Independent of SM1EXECTRACE; inert when unset.
     */
    if (offset == SM1_EXEC_KICK_OFF && sm1_engine_on()) {
        sm1_engine_frame(s, (uint32_t)value);
    }

    sm1_count_hit(offset, true, (uint32_t)value);

    if (sm1_trace_on()) {
        fprintf(stderr, "SM1TRACE wr +0x%03" HWADDR_PRIx " = 0x%08x  "
                "pc~0x%08x%s%s\n", offset, (uint32_t)value, sm1_trace_pc(),
                sm1_is_sub_cmd(offset) ? "  [SUBCMD]" : "",
                s->powered ? "  powered" : "");
    }
}

static const MemoryRegionOps ipod_classic_sm1_ops = {
    .read = ipod_classic_sm1_read,
    .write = ipod_classic_sm1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_classic_sm1_reset(DeviceState *dev)
{
    IPodClassicSM1State *s = IPOD_CLASSIC_SM1(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->powered = false;
    s->frame_pending = false;
    if (sm1_engine_on()) {
        /* Do not deliver a completion scheduled before a device reset. */
        if (engine_done_timer) {
            timer_del(engine_done_timer);
        }
        s->irq_pending = false;
        s->irq_armed = false;
        qemu_irq_lower(s->irq);
    }
}

static void ipod_classic_sm1_init(Object *obj)
{
    IPodClassicSM1State *s = IPOD_CLASSIC_SM1(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_classic_sm1_ops, s,
                          TYPE_IPOD_CLASSIC_SM1, S5L8702_SM1_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    /*
     * The engine's interrupt line. Always created so the machine can wire it to
     * VIC0 line 18; only ever RAISED when SM1_IRQ=1, so the default build is
     * byte-identical to the baseline every prior session measured.
     */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    const char *e = getenv("SM1_IRQ");
    s->irq_enabled = e && *e && *e != '0';
    s->irq_always = e && *e == '2';
    if (s->irq_enabled) {
        s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_irq_tick, s);
        s->irq_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_irq_off, s);
        timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)SM1_IRQ_TICK_MS * 1000000);
        fprintf(stderr, "SM1_IRQ: engine interrupt armed on VIC0 line 18\n");
    }
    /*
     * Executor milestone-2 raises the same line from REAL frame completion
     * (the kick), so it needs the bounded-pulse backstop even without the
     * SM1_IRQ timer experiment running. Created only under SM1ENGINE, so the
     * default build still constructs nothing.
     */
    if (sm1_engine_on() && !s->irq_off_timer) {
        s->irq_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_irq_off, s);
    }
    /*
     * desc+2 is a live state (0 = slot pending pickup / 2 = consumed): the
     * consume happens inside the guest (the acquire clears the ready
     * halfword), so the engine re-derives the code from guest RAM on a 5 ms
     * tick and after every fill.
     */
    if (sm1_engine_on() && !engine_tick_timer) {
        engine_tick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_engine_tick, s);
        timer_mod(engine_tick_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  5 * 1000000ll);
    }
    /* asynchronous frame completion (see sm1_engine_frame): the report is
     * delivered one audio frame after the kick, as the silicon does */
    if (sm1_engine_on() && !engine_done_timer) {
        engine_done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sm1_engine_done, s);
    }
}

static const VMStateDescription vmstate_ipod_classic_sm1 = {
    .name = TYPE_IPOD_CLASSIC_SM1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, IPodClassicSM1State, S5L8702_SM1_REG_WORDS),
        VMSTATE_BOOL(powered, IPodClassicSM1State),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_sm1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 SM1 audio clock/stream engine";
    dc->reset = ipod_classic_sm1_reset;
    dc->vmsd = &vmstate_ipod_classic_sm1;
    dc->user_creatable = false;
}

static const TypeInfo ipod_classic_sm1_info = {
    .name          = TYPE_IPOD_CLASSIC_SM1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicSM1State),
    .instance_init = ipod_classic_sm1_init,
    .class_init    = ipod_classic_sm1_class_init,
};

static void ipod_classic_sm1_register_types(void)
{
    type_register_static(&ipod_classic_sm1_info);
}

type_init(ipod_classic_sm1_register_types)
