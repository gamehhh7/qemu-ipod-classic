/*
 * S5L8702 ATA controller — ASYNC EXPERIMENTAL FC1307 backend variant.
 *
 * SEPARATE source file for a SEPARATE QEMU build (async-ui).  The accepted
 * ui-sha tree and its binary are untouched.  Selected only when FC1307_BACKEND
 * is set; otherwise the stock IDE path is used (there is NO silent fallback:
 * with FC1307 selected there is no -drive path at all).
 *
 * WHY ASYNC
 * ---------
 * The previous (ui-sha) device performed the Python IPC *synchronously inside
 * the MMIO handler while holding the BQL*.  A slow/dropped backend therefore
 * blocked QEMU's main loop (HMP/SDL), and the independent GLM audit (D1) proved
 * that a recv timeout/disconnect left the byte stream desynchronised so the
 * NEXT command consumed the PREVIOUS command's late reply.
 *
 * This variant moves the IPC to a dedicated worker thread:
 *   MMIO command write  -> snapshot the (immutable) taskfile + payload, bump the
 *                          generation, mark BSY, wake the worker, RETURN.
 *   worker thread       -> bounded socket I/O only.  It NEVER touches guest
 *                          memory, IRQ lines or device registers.
 *   completion BH       -> runs on the main loop under the BQL; applies
 *                          status/error/data/IRQ/XFR_CNT and does any guest
 *                          DMA copy.
 *
 * Guest DMA writes are snapshotted on the MAIN LOOP (MMIO go bit) before the
 * worker is handed the payload, so the worker never reads guest memory.
 *
 * Rules:
 *   * one request in flight per device (serialized);
 *   * immutable request (taskfile + payload) handed to the worker;
 *   * a generation counter cancels stale completions (reset / new command);
 *   * the worker blocks on io_cond with the predicate checked under io_lock
 *     (GLM continuation: a sticky QemuEvent without qemu_event_reset() would
 *     have busy-spun the worker after the first command);
 *   * recv failure/timeout/disconnect/malformed closes the stream so the next
 *     command reconnects instead of consuming a desynchronised reply (D1);
 *   * odd payloads and status without DRDY/ERR are fail-closed (D4/D6);
 *   * BSY/DRQ/IRQ/XFR_CNT never indicate success before the backend answers;
 *   * teardown joins the worker; the socket is never reused after a fault.
 *
 * STILL BLOCKED BY DESIGN (never forced): SET FEATURES 0xEF is decided by the
 * REAL R3 handler (0x03/0x02 ACK, 0xAA ABRT), 0xB0/0xE0 blocked, FLUSH/SLEEP
 * conditional, late write-completion unmodelled, media synthesized in Python
 * (exact image bytes + isolated COW).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"
#include "hw/arm/ipod_classic/ipod_classic_ata.h"
#include "hw/arm/ipod_classic/ipod_classic_ata_fc1307.h"

#define FC1307_MAX_RESPONSE   (64u << 20)
#define FC1307_DEFAULT_TIMEOUT_MS 5000
#define ATA_BSY  0x80u
#define ATA_DRDY 0x40u
#define ATA_DRQ  0x08u
#define ATA_ERR  0x01u

typedef struct FC1307Taskfile {
    uint8_t  opcode;
    uint16_t feature;
    uint16_t count_raw;
    uint8_t  lba[6];
    uint8_t  device;
    bool     extended;
} FC1307Taskfile;

typedef struct FC1307Result {
    bool     valid;
    uint8_t  status;
    uint8_t  error;
    bool     awaiting_write;
    uint8_t *data;
    uint32_t data_len;
    char     outcome[32];
    char     classification[32];
    bool     transport_error;      /* link fault -> already fail-closed */
    uint64_t generation;           /* completion generation (stale-drop) */
} FC1307Result;

typedef enum {
    FC1307_REQ_NONE = 0,
    FC1307_REQ_TASKFILE,
    FC1307_REQ_RESET,
} FC1307ReqKind;

typedef struct FC1307Request {
    uint64_t       generation;
    FC1307ReqKind  kind;
    FC1307Taskfile tf;
    uint8_t       *payload;      /* owned by the queue until the worker frees it */
    uint32_t       payload_len;
} FC1307Request;

struct IPodClassicATAFC1307State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t reg[S5L8702_ATA_MEM_SIZE / 4];
    uint32_t rdata;
    bool     rdata_valid;

    /* taskfile accumulation (latest two writes per field) */
    uint8_t tf_feat_w[2];   int tf_feat_n;
    uint8_t tf_count_w[2];  int tf_count_n;
    uint8_t tf_lba_w[3][2]; int tf_lba_n[3];
    uint8_t tf_device;

    /* PIO read cursor */
    uint32_t pio_data_off;
    uint32_t pio_remaining;

    /* pending PIO write staging (main-loop owned) */
    bool           write_pending;
    FC1307Taskfile pending_tf;
    uint32_t       write_expect;
    uint8_t       *write_buf;
    uint32_t       write_len;

    /* last applied result (main-loop owned) */
    FC1307Result result;

    /* busy bookkeeping (main-loop owned) */
    bool           command_busy;
    FC1307Taskfile busy_tf;
    FC1307ReqKind  busy_kind;
    uint64_t       busy_xfr_bytes; /* DMA write payload size of the busy cmd */
    uint32_t       busy_payload_bytes; /* request phase, for BOTH PIO and DMA */

    /* deferred DMA go-bit (main-loop owned).  The real firmware fires the
     * COMMAND go bit WITHOUT waiting for the taskfile completion IRQ
     * (CSD -> program XFR_NUM/TBUF/SBUF -> START).  The synchronous device
     * hid this because its IPC blocked inside the CSD write; with the async
     * worker the go bit arrives while the command is still in flight, so it
     * is ARMED here and consumed by the completion BH (GLM fix G5). */
    bool           go_pending;
    uint32_t       go_xfr_num;
    uint32_t       go_tbuf;
    uint32_t       go_sbuf;

    /* async IPC */
    QemuMutex      io_lock;       /* guards generation / req / done / quit */
    QemuCond       io_cond;       /* worker waits for req or quit.  GLM G1: the
                                   * WIP draft used a sticky QemuEvent and never
                                   * called qemu_event_reset(), so after the
                                   * first command the worker busy-spun forever */
    QEMUBH        *io_bh;
    QemuThread     io_thread;
    bool           io_thread_started;
    bool           io_quit;
    FC1307Request  req;
    bool           req_pending;
    FC1307Result   done;
    bool           done_pending;
    uint64_t       generation;

    /* socket (worker owned once the thread starts) */
    int            fd;
    int            io_timeout_ms;
    uint8_t        rbuf[65536];
    size_t         rbuf_pos;
    size_t         rbuf_len;
};

/* ------------------------------------------------------------------ trace */
static bool fc1307_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("FC1307TRACE") != NULL;
    }
    return on;
}

/* ------------------------------------------------------------- opcode sets */
static bool fc1307_ext_opcode(uint8_t op)
{
    return op == 0x24 || op == 0x25 || op == 0x29 ||
           op == 0x34 || op == 0x35 || op == 0x39;
}

/* Must match the Python controller's WRITE_OPCODES exactly. */
static bool fc1307_write_opcode(uint8_t op)
{
    return op == 0x30 || op == 0x31 || op == 0x34 || op == 0x35 ||
           op == 0x39 || op == 0xC5 || op == 0xCA;
}

static bool fc1307_dma_opcode(uint8_t op)
{
    return op == 0x25 || op == 0x35 || op == 0xC8 || op == 0xC9 || op == 0xCA;
}

/* ------------------------------------------------------------- result util */
static void fc1307_result_clear(FC1307Result *r)
{
    r->valid = false;
    r->status = 0;
    r->error = 0;
    r->awaiting_write = false;
    r->outcome[0] = '\0';
    r->classification[0] = '\0';
    r->transport_error = false;
    r->generation = 0;
    g_free(r->data);
    r->data = NULL;
    r->data_len = 0;
}

static void fc1307_fail_closed(FC1307Result *r, uint8_t err, const char *why)
{
    fc1307_result_clear(r);
    r->valid = true;
    r->transport_error = true;
    r->status = ATA_DRDY | ATA_ERR;
    r->error = err;
    g_strlcpy(r->outcome, "fail_closed", sizeof(r->outcome));
    g_strlcpy(r->classification, "fail_closed", sizeof(r->classification));
    if (fc1307_trace()) {
        fprintf(stderr, "FC1307 FAIL_CLOSED err=0x%02x (%s)\n", err, why);
    }
}

/* --------------------------------------------------- worker-side socket I/O */
static void fc1307_io_link_close(IPodClassicATAFC1307State *s)
{
    if (s->fd >= 0) {
        closesocket(s->fd);
        s->fd = -1;
    }
    s->rbuf_pos = s->rbuf_len = 0;
}

static bool fc1307_io_link_open(IPodClassicATAFC1307State *s)
{
    const char *spec = getenv("FC1307_IPC_ADDR");
    const char *tmo = getenv("FC1307_IPC_TIMEOUT_MS");
    Error *err = NULL;
    int fd;

    if (s->fd >= 0) {
        return true;
    }
    if (!spec) {
        spec = "127.0.0.1:4777";
    }
    s->io_timeout_ms = tmo ? atoi(tmo) : FC1307_DEFAULT_TIMEOUT_MS;
    if (s->io_timeout_ms <= 0) {
        s->io_timeout_ms = FC1307_DEFAULT_TIMEOUT_MS;
    }
    fd = inet_connect(spec, &err);
    if (fd < 0) {
        error_free(err);
        return false;
    }
    qemu_socket_set_block(fd);
#ifdef _WIN32
    {
        DWORD tv = (DWORD)s->io_timeout_ms;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = s->io_timeout_ms / 1000;
        tv.tv_usec = (s->io_timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif
    s->fd = fd;
    s->rbuf_pos = s->rbuf_len = 0;
    return true;
}

/*
 * Read one LF-terminated line with the socket deadline.  Any recv failure
 * (timeout / disconnect / partial) CLOSES the stream, so the next command
 * reconnects instead of consuming a stale reply (audit D1).
 */
static bool fc1307_io_read_line(IPodClassicATAFC1307State *s, GString *line,
                                bool *overflow)
{
    *overflow = false;
    g_string_truncate(line, 0);
    for (;;) {
        if (s->rbuf_pos < s->rbuf_len) {
            uint8_t *nl = memchr(s->rbuf + s->rbuf_pos, '\n',
                                 s->rbuf_len - s->rbuf_pos);
            if (nl) {
                size_t n = (size_t)(nl - (s->rbuf + s->rbuf_pos));
                if (line->len + n > FC1307_MAX_RESPONSE) {
                    *overflow = true;
                    fc1307_io_link_close(s);
                    return false;
                }
                g_string_append_len(line, (const char *)s->rbuf + s->rbuf_pos, n);
                s->rbuf_pos += n + 1;
                return true;
            }
            if (line->len + (s->rbuf_len - s->rbuf_pos) > FC1307_MAX_RESPONSE) {
                *overflow = true;
                fc1307_io_link_close(s);
                return false;
            }
            g_string_append_len(line, (const char *)s->rbuf + s->rbuf_pos,
                                s->rbuf_len - s->rbuf_pos);
            s->rbuf_pos = s->rbuf_len = 0;
        }
        {
            ssize_t n = recv(s->fd, s->rbuf, sizeof(s->rbuf), 0);
            if (n <= 0) {
                fc1307_io_link_close(s);   /* D1 */
                return false;
            }
            s->rbuf_len = (size_t)n;
            s->rbuf_pos = 0;
        }
    }
}

static bool fc1307_io_send_line(IPodClassicATAFC1307State *s, const char *fmt, ...)
{
    va_list ap;
    char *line;
    char *nl;
    size_t len;
    ssize_t wrote;

    va_start(ap, fmt);
    line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    nl = g_strconcat(line, "\n", NULL);
    g_free(line);
    line = nl;
    len = strlen(line);
    wrote = qemu_send_full(s->fd, line, len);
    g_free(line);
    if (wrote != (ssize_t)len) {
        fc1307_io_link_close(s);
        return false;
    }
    return true;
}

/* Strict unsigned parse (D6): no silent garbage, no truncation. */
static bool fc1307_parse_u32(const char *tok, uint32_t *out)
{
    unsigned long long v;
    char *end = NULL;

    if (!tok || !*tok || tok[0] == '-') {
        return false;
    }
    errno = 0;
    v = strtoull(tok, &end, 0);
    if (errno != 0 || end == tok || *end != '\0' || v > 0xFFFFFFFFull) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* ------------------------------------------------------- worker transact */
static void fc1307_io_transact(IPodClassicATAFC1307State *s,
                               const FC1307Request *req, FC1307Result *res)
{
    gchar *data_b64 = NULL;
    GString *resp;
    gchar **tok;
    int ntok;
    bool overflow = false;
    uint64_t lba64 = 0;
    uint32_t status = 0, error = 0, irq = 0, steps = 0, declared = 0;

    fc1307_result_clear(res);

    if (!fc1307_io_link_open(s)) {
        fc1307_fail_closed(res, 0x04, "no ipc link");
        return;
    }

    if (req->kind == FC1307_REQ_RESET) {
        if (fc1307_io_send_line(s, "RESET")) {
            resp = g_string_new(NULL);
            (void)fc1307_io_read_line(s, resp, &overflow);
            g_string_free(resp, TRUE);
        }
        return;
    }

    for (int i = 0; i < 6; i++) {
        lba64 |= (uint64_t)req->tf.lba[i] << (8 * i);
    }
    if (req->payload && req->payload_len) {
        data_b64 = g_base64_encode(req->payload, req->payload_len);
    }
    if (fc1307_trace()) {
        fprintf(stderr,
                "FC1307 TX op=0x%02x feature=0x%04x count=%u lba=0x%" PRIx64
                " dev=0x%02x ext=%d datalen=%u\n",
                req->tf.opcode, req->tf.feature, req->tf.count_raw, lba64,
                req->tf.device, req->tf.extended ? 1 : 0, req->payload_len);
    }

    if (!fc1307_io_send_line(s, "TASKFILE %u %u %u %u %u %u %u %u %u %u %u %s",
                             req->tf.opcode, req->tf.feature, req->tf.count_raw,
                             req->tf.lba[0], req->tf.lba[1], req->tf.lba[2],
                             req->tf.lba[3], req->tf.lba[4], req->tf.lba[5],
                             req->tf.device, req->payload_len,
                             data_b64 ? data_b64 : "-")) {
        g_free(data_b64);
        fc1307_fail_closed(res, 0x04, "send failed/timeout");
        return;
    }
    g_free(data_b64);

    resp = g_string_new(NULL);
    if (!fc1307_io_read_line(s, resp, &overflow)) {
        g_string_free(resp, TRUE);
        fc1307_fail_closed(res, 0x04,
                           overflow ? "response over FC1307_MAX_RESPONSE"
                                    : "recv failed/timeout/no-LF");
        return;
    }

    tok = g_strsplit(resp->str, " ", 9);
    g_string_free(resp, TRUE);
    ntok = 0;
    while (tok && tok[ntok]) {
        ntok++;
    }
    if (ntok < 8 || g_strcmp0(tok[0], "RESULT") != 0 ||
        !fc1307_parse_u32(tok[1], &status) ||
        !fc1307_parse_u32(tok[2], &error) ||
        !fc1307_parse_u32(tok[3], &irq) ||
        !fc1307_parse_u32(tok[6], &steps) ||
        !fc1307_parse_u32(tok[7], &declared)) {
        g_strfreev(tok);
        /* D1 rule: every post-send protocol fault closes the stream so the
         * next command reconnects instead of risking a desynchronised reply */
        fc1307_io_link_close(s);
        fc1307_fail_closed(res, 0x04, "malformed response");
        return;
    }
    /* D6: a valid ATA status must show DRDY or ERR and must not show BSY. */
    if (status > 0xFF || error > 0xFF || irq > 1 ||
        !(status & (ATA_DRDY | ATA_ERR)) || (status & ATA_BSY)) {
        g_strfreev(tok);
        fc1307_io_link_close(s);
        fc1307_fail_closed(res, 0x04, "invalid status/error in response");
        return;
    }

    res->status = (uint8_t)status;
    res->error = (uint8_t)error;
    g_strlcpy(res->outcome, tok[4] ? tok[4] : "?", sizeof(res->outcome));
    g_strlcpy(res->classification, tok[5] ? tok[5] : "?",
              sizeof(res->classification));
    res->awaiting_write = (g_strcmp0(tok[4], "awaiting_pio_write") == 0);

    if (ntok >= 9 && g_strcmp0(tok[8], "-") != 0) {
        gsize dl = 0;
        guchar *dec = g_base64_decode(tok[8], &dl);
        if ((uint64_t)dl != (uint64_t)declared || (dl & 1)) {   /* D4 */
            g_free(dec);
            g_strfreev(tok);
            fc1307_io_link_close(s);
            fc1307_fail_closed(res, 0x04,
                               (dl & 1) ? "payload not word-aligned"
                                        : "payload length mismatch");
            return;
        }
        res->data = dec;
        res->data_len = (uint32_t)dl;
    }
    res->valid = true;
    if (fc1307_trace()) {
        fprintf(stderr, "FC1307 RX status=0x%02x err=0x%02x outcome=%s "
                        "class=%s datalen=%u\n",
                res->status, res->error, res->outcome, res->classification,
                res->data_len);
    }
    g_strfreev(tok);
}

/* --------------------------------------------------------------- worker */
static void *fc1307_io_worker(void *opaque)
{
    IPodClassicATAFC1307State *s = opaque;

    for (;;) {
        FC1307Request req;
        FC1307Result res;
        bool have = false;
        bool quit = false;

        /* predcheck: req_pending/io_quit are examined under io_lock before
         * every wait, so a submit racing the worker cannot be lost (G1) */
        qemu_mutex_lock(&s->io_lock);
        while (!s->req_pending && !s->io_quit) {
            qemu_cond_wait(&s->io_cond, &s->io_lock);
        }
        if (s->io_quit) {
            quit = true;
        } else if (s->req_pending) {
            req = s->req;
            memset(&s->req, 0, sizeof(s->req));
            s->req_pending = false;
            have = true;
        }
        qemu_mutex_unlock(&s->io_lock);
        if (quit) {
            break;
        }
        if (!have) {
            continue;
        }

        fc1307_io_transact(s, &req, &res);
        res.generation = req.generation;
        g_free(req.payload);
        req.payload = NULL;

        qemu_mutex_lock(&s->io_lock);
        if (req.generation == s->generation) {
            fc1307_result_clear(&s->done);
            s->done = res;
            memset(&res, 0, sizeof(res));
            s->done_pending = true;
        }
        qemu_mutex_unlock(&s->io_lock);
        fc1307_result_clear(&res);       /* stale or moved: drop */
        qemu_bh_schedule(s->io_bh);
    }
    return NULL;
}

/* ------------------------------------------------------------- IRQ / status */
static void fc1307_update_irq(IPodClassicATAFC1307State *s)
{
    uint32_t active = s->reg[S5L8702_ATA_IRQSTAT / 4] &
                      s->reg[S5L8702_ATA_IRQMASK / 4];
    qemu_set_irq(s->irq, active != 0);
}

static void fc1307_complete_event(IPodClassicATAFC1307State *s)
{
    if (!(s->reg[S5L8702_ATA_PIO_DAD / 4] & 0x02)) {   /* nIEN */
        /* The measured FC standby (E0) wait uses IRQMASK=4, including
         * real error completions. Keeping that event on bit0 stranded it
         * until a later reset unmasked an unrelated transaction. */
        s->reg[S5L8702_ATA_IRQSTAT / 4] |=
            s->busy_tf.opcode == 0xe0 ? S5L8702_ATA_PIO_INTRQ_BIT :
            s5l8702_ata_irq_source(s->busy_tf.opcode);
    }
    fc1307_update_irq(s);
    if (fc1307_trace()) {
        uint32_t st = s->reg[S5L8702_ATA_IRQSTAT / 4];
        uint32_t mk = s->reg[S5L8702_ATA_IRQMASK / 4];
        fprintf(stderr, "FC1307 IRQ irqstat=0x%02x irqmask=0x%02x irqline=%d nIEN=%d\n",
                st & 0xFF, mk & 0xFF, (st & mk) != 0,
                (s->reg[S5L8702_ATA_PIO_DAD / 4] & 0x02) != 0);
    }
}

static uint8_t fc1307_status(IPodClassicATAFC1307State *s)
{
    uint8_t st;

    if (s->command_busy) {
        return ATA_BSY;                  /* never a success before the BH */
    }
    st = s->result.valid ? s->result.status : 0;
    st &= (uint8_t)~ATA_DRQ;
    if (s->pio_remaining > 0) {
        st |= ATA_DRQ;
    }
    if (s->write_pending && !fc1307_dma_opcode(s->pending_tf.opcode)) {
        st |= ATA_DRQ;
    }
    return st;
}

static void fc1307_cancel_write(IPodClassicATAFC1307State *s)
{
    s->write_pending = false;
    s->write_len = 0;
    s->write_expect = 0;
    memset(&s->pending_tf, 0, sizeof(s->pending_tf));
}

static void fc1307_reset_tf(IPodClassicATAFC1307State *s)
{
    memset(s->tf_feat_w, 0, sizeof(s->tf_feat_w));
    memset(s->tf_count_w, 0, sizeof(s->tf_count_w));
    memset(s->tf_lba_w, 0, sizeof(s->tf_lba_w));
    s->tf_feat_n = 0;
    s->tf_count_n = 0;
    memset(s->tf_lba_n, 0, sizeof(s->tf_lba_n));
    s->tf_device = 0xA0;
}

static void fc1307_decode_tf(IPodClassicATAFC1307State *s, uint8_t op, bool ext,
                             FC1307Taskfile *tf)
{
    int i;
    tf->opcode = op;
    tf->extended = ext;
    tf->device = s->tf_device;
    memset(tf->lba, 0, sizeof(tf->lba));
    if (ext) {
        tf->feature = (uint16_t)((s->tf_feat_w[0] << 8) | s->tf_feat_w[1]);
        tf->count_raw = (uint16_t)((s->tf_count_w[0] << 8) | s->tf_count_w[1]);
        for (i = 0; i < 3; i++) {
            tf->lba[i] = s->tf_lba_w[i][s->tf_lba_n[i] == 2 ? 1 : 0];
            tf->lba[i + 3] = s->tf_lba_w[i][0];
        }
    } else {
        tf->feature = s->tf_feat_w[s->tf_feat_n == 2 ? 1 : 0];
        tf->count_raw = s->tf_count_w[s->tf_count_n == 2 ? 1 : 0];
        for (i = 0; i < 3; i++) {
            tf->lba[i] = s->tf_lba_w[i][s->tf_lba_n[i] == 2 ? 1 : 0];
        }
    }
}

static uint32_t fc1307_effective_count(uint16_t raw, bool ext)
{
    if (ext) {
        return raw == 0 ? 65536 : raw;
    }
    return raw == 0 ? 256 : raw;
}

/* -------------------------------------------------------------- submit */
static void fc1307_submit(IPodClassicATAFC1307State *s, FC1307ReqKind kind,
                          const FC1307Taskfile *tf, uint8_t *payload,
                          uint32_t payload_len)
{
    if (!s->io_thread_started) {
        g_free(payload);
        return;
    }
    /* Main-loop request metadata.  A command-only write response must never
     * be mistaken for the acknowledgement of a submitted data phase. */
    s->busy_payload_bytes = payload_len;
    qemu_mutex_lock(&s->io_lock);
    s->generation++;
    if (s->req_pending) {
        g_free(s->req.payload);          /* never taken by the worker */
    }
    s->req.generation = s->generation;
    s->req.kind = kind;
    s->req.tf = *tf;
    s->req.payload = payload;
    s->req.payload_len = payload_len;
    s->req_pending = true;
    qemu_mutex_unlock(&s->io_lock);
    qemu_cond_signal(&s->io_cond);
}

/* ------------------------------------------- DMA data-phase helpers (BQL) */
/*
 * Apply a DMA read transfer: copy the cached payload into the guest TBUF and
 * count the bytes.  Runs on the main loop (go-bit MMIO write or completion
 * BH).  On failure *why is set and the caller fail-closes the result.
 */
static bool fc1307_dma_read_apply(IPodClassicATAFC1307State *s, uint32_t tbuf,
                                  uint64_t n, const char **why)
{
    MemTxResult mr;

    if (!s->result.valid || !s->result.data) {
        *why = "dma read without payload";
        return false;
    }
    if (n > (uint64_t)s->result.data_len) {
        *why = "dma read asks for more than produced";
        return false;
    }
    mr = address_space_write(&address_space_memory, tbuf,
                             MEMTXATTRS_UNSPECIFIED, s->result.data,
                             (size_t)n);
    if (mr != MEMTX_OK) {
        *why = "dma read guest TBUF write failed";
        return false;
    }
    /* FIXTURE ASSUMPTION (audit D2): XFR_CNT counts BYTES transferred, the
     * same unit the read path has always used.  This is NOT a measured
     * hardware fact. */
    s->reg[S5L8702_ATA_XFR_CNT / 4] =
        (uint32_t)((uint64_t)s->reg[S5L8702_ATA_XFR_CNT / 4] + n);
    return true;
}

/*
 * DMA write payload phase: length-check the go snapshot, snapshot the guest
 * SBUF on the MAIN LOOP (never in the worker) and submit the payload to the
 * backend.  Reached either directly from the go-bit handler (the normal
 * late-go case) or from the completion BH when the go bit was armed while
 * the taskfile was still in flight (G5).
 */
static void fc1307_dma_write_payload_phase(IPodClassicATAFC1307State *s,
                                           uint32_t sbuf, uint64_t n)
{
    FC1307Taskfile tf = s->pending_tf;
    uint8_t *payload;
    MemTxResult mr;

    if (n != (uint64_t)s->write_expect || n == 0 || n > 0x7FFFFFFF) {
        fc1307_cancel_write(s);
        fc1307_fail_closed(&s->result, 0x04,
                           "dma write length != command byte count");
        s->command_busy = false;
        fc1307_complete_event(s);
        return;
    }
    payload = g_malloc((size_t)n);
    mr = address_space_read(&address_space_memory, sbuf,
                            MEMTXATTRS_UNSPECIFIED, payload, (size_t)n);
    fc1307_cancel_write(s);
    if (mr != MEMTX_OK) {
        g_free(payload);
        fc1307_fail_closed(&s->result, 0x04,
                           "dma write guest SBUF read failed");
        s->command_busy = false;
        fc1307_complete_event(s);
        return;
    }
    s->busy_tf = tf;
    s->busy_kind = FC1307_REQ_TASKFILE;
    s->busy_xfr_bytes = n;      /* D2: consumed at completion */
    s->command_busy = true;
    fc1307_submit(s, FC1307_REQ_TASKFILE, &tf, payload, (uint32_t)n);
}

/* --------------------------------------------------- completion BH (BQL) */
static void fc1307_apply_completion(IPodClassicATAFC1307State *s,
                                    const FC1307Taskfile *tf, FC1307Result *res)
{
    fc1307_result_clear(&s->result);
    s->result = *res;
    memset(res, 0, sizeof(*res));        /* ownership moved */

    if (fc1307_write_opcode(tf->opcode)) {
        bool success = s->result.valid && s->result.error == 0 &&
                       !(s->result.status & ATA_ERR);
        uint32_t expected =
            fc1307_effective_count(tf->count_raw, tf->extended) * 512;

        /* The IPC protocol is two-phase for every write opcode.  Only an
         * initial request with no data may ask for its payload.  Conversely,
         * success is impossible until the entire PIO/DMA payload was actually
         * submitted.  This is a transport contract, not proof that a physical
         * device persisted data; media remains the declared synthetic COW. */
        if (success &&
            ((s->result.awaiting_write && s->busy_payload_bytes != 0) ||
             (!s->result.awaiting_write && s->busy_payload_bytes != expected))) {
            fc1307_fail_closed(&s->result, 0x04,
                s->result.awaiting_write ?
                    "write payload requested again after data submission" :
                    "write success without full payload submission");
            s->busy_xfr_bytes = 0;
            s->go_pending = false;
            fc1307_cancel_write(s);
            fc1307_complete_event(s);
            return;
        }
        if (s->result.valid && s->result.awaiting_write &&
            s->result.error == 0 && !(s->result.status & ATA_ERR)) {
            s->write_pending = true;
            s->pending_tf = *tf;
            s->write_expect =
                fc1307_effective_count(tf->count_raw, tf->extended) * 512;
            s->write_len = 0;
            g_free(s->write_buf);
            s->write_buf = g_malloc0(s->write_expect ? s->write_expect : 512);
            if (fc1307_dma_opcode(tf->opcode)) {
                if (s->go_pending) {
                    /* G5: the go bit already fired while this taskfile was in
                     * flight - enter the payload phase from the snapshot. */
                    s->go_pending = false;
                    fc1307_dma_write_payload_phase(s, s->go_sbuf,
                                                   (uint64_t)s->go_xfr_num + 1);
                }
                return;                      /* no IRQ until the commit lands */
            }
            fc1307_complete_event(s);        /* PIO write service event */
            return;
        }
        if (fc1307_dma_opcode(tf->opcode) && s->result.valid &&
            s->result.error == 0 && !(s->result.status & ATA_ERR) &&
            s->busy_xfr_bytes) {
            /* FIXTURE ASSUMPTION (audit D2, GLM fix G3): XFR_CNT counts BYTES
             * transferred on the DMA WRITE path, mirroring the read path.
             * NOT a measured hardware fact; the current real firmware also
             * succeeds without this update. */
            uint64_t cnt = (uint64_t)s->reg[S5L8702_ATA_XFR_CNT / 4] +
                           s->busy_xfr_bytes;
            s->reg[S5L8702_ATA_XFR_CNT / 4] = (uint32_t)cnt;
        }
        s->busy_xfr_bytes = 0;
        s->go_pending = false;               /* nothing left to consume */
        fc1307_cancel_write(s);
        fc1307_complete_event(s);
        return;
    }

    if (fc1307_dma_opcode(tf->opcode)) {
        if (s->go_pending) {
            /* G5: the firmware fired the go bit before the completion; the
             * armed snapshot is the transfer it asked for. */
            const char *why = NULL;
            bool ok = fc1307_dma_read_apply(s, s->go_tbuf,
                                            (uint64_t)s->go_xfr_num + 1,
                                            &why);
            s->go_pending = false;
            if (!ok) {
                fc1307_fail_closed(&s->result, 0x04, why);
            }
            fc1307_complete_event(s);
        }
        return;                              /* no armed go: guest fires later */
    }
    s->pio_remaining = s->result.data_len;
    fc1307_complete_event(s);
}

static void fc1307_io_bh(void *opaque)
{
    IPodClassicATAFC1307State *s = opaque;
    FC1307Result comp;
    FC1307Taskfile tf;
    FC1307ReqKind kind;
    uint64_t gen;
    bool have = false;

    qemu_mutex_lock(&s->io_lock);
    if (s->done_pending) {
        comp = s->done;
        memset(&s->done, 0, sizeof(s->done));
        s->done_pending = false;
        have = true;
    }
    gen = s->generation;
    qemu_mutex_unlock(&s->io_lock);
    if (!have) {
        return;
    }

    tf = s->busy_tf;
    kind = s->busy_kind;
    if (comp.generation != gen) {
        if (fc1307_trace()) {
            fprintf(stderr, "FC1307 BH stale drop gen=%" PRIu64 " cur=%" PRIu64
                            "\n", comp.generation, gen);
        }
        fc1307_result_clear(&comp);      /* stale: never applied */
        return;
    }
    if (kind == FC1307_REQ_RESET) {
        /* advisory server reset; nothing to apply, no BSY was raised */
        fc1307_result_clear(&comp);
        return;
    }
    s->command_busy = false;
    fc1307_apply_completion(s, &tf, &comp);
    fc1307_result_clear(&comp);
}

/* --------------------------------------------------------------- MMIO read */
static uint64_t fc1307_ata_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodClassicATAFC1307State *s = opaque;

    switch (offset) {
    case S5L8702_ATA_PIO_READY:
        return S5L8702_ATA_READY_CYCLE |
               (s->rdata_valid ? S5L8702_ATA_READY_RDATA : 0);
    case S5L8702_ATA_PIO_RDATA:
        s->rdata_valid = false;
        return s->rdata;
    case S5L8702_ATA_PIO_DTR:
        if (s->pio_remaining >= 2 && s->result.data &&
            s->pio_data_off + 1 < s->result.data_len) {
            s->rdata = s->result.data[s->pio_data_off] |
                       (s->result.data[s->pio_data_off + 1] << 8);
            s->pio_data_off += 2;
            s->pio_remaining -= 2;
            if (s->pio_remaining == 0) {
                fc1307_complete_event(s);
            }
        } else {
            s->rdata = 0;
        }
        s->rdata_valid = true;
        return s->rdata;
    case S5L8702_ATA_PIO_DAD:
    case S5L8702_ATA_PIO_CSD:
        s->rdata = fc1307_status(s);
        s->rdata_valid = true;
        return s->rdata;
    case S5L8702_ATA_PIO_FED:
        s->rdata = s->result.valid ? s->result.error : 0;
        s->rdata_valid = true;
        return s->rdata;
    default:
        if (offset < S5L8702_ATA_PIO_DTR) {
            return s->reg[offset / 4];
        }
        qemu_log_mask(LOG_UNIMP, "fc1307-ata: read 0x%" HWADDR_PRIx "\n", offset);
        return 0;
    }
}

/* -------------------------------------------------------------- MMIO write */
static void fc1307_ata_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IPodClassicATAFC1307State *s = opaque;
    uint8_t v8 = (uint8_t)value;

    switch (offset) {
    case S5L8702_ATA_SWRST:
        s->reg[offset / 4] = value;
        if (value & 1) {
            fc1307_result_clear(&s->result);
            s->rdata_valid = false;
            s->pio_remaining = 0;
            s->command_busy = false;
            s->busy_xfr_bytes = 0;
            s->go_pending = false;
            fc1307_cancel_write(s);
            fc1307_reset_tf(s);
            fc1307_update_irq(s);        /* D3: drive the line low too */
            if (s->io_thread_started) {
                qemu_mutex_lock(&s->io_lock);
                s->generation++;
                if (s->req_pending) {
                    g_free(s->req.payload);
                    s->req.payload = NULL;
                    s->req.payload_len = 0;
                    s->req_pending = false;
                }
                qemu_mutex_unlock(&s->io_lock);
            }
        }
        break;
    case S5L8702_ATA_IRQSTAT:
        s->reg[offset / 4] &= ~(uint32_t)value;
        fc1307_update_irq(s);
        break;
    case S5L8702_ATA_IRQMASK:
        s->reg[offset / 4] = value;
        fc1307_update_irq(s);
        break;

    case S5L8702_ATA_PIO_FED:
        if (s->tf_feat_n == 2) { s->tf_feat_w[0] = s->tf_feat_w[1]; s->tf_feat_w[1] = v8; }
        else { s->tf_feat_w[s->tf_feat_n++] = v8; }
        break;
    case S5L8702_ATA_PIO_SCR:
        if (s->tf_count_n == 2) { s->tf_count_w[0] = s->tf_count_w[1]; s->tf_count_w[1] = v8; }
        else { s->tf_count_w[s->tf_count_n++] = v8; }
        break;
    case S5L8702_ATA_PIO_LLR:
        if (s->tf_lba_n[0] == 2) { s->tf_lba_w[0][0] = s->tf_lba_w[0][1]; s->tf_lba_w[0][1] = v8; }
        else { s->tf_lba_w[0][s->tf_lba_n[0]++] = v8; }
        break;
    case S5L8702_ATA_PIO_LMR:
        if (s->tf_lba_n[1] == 2) { s->tf_lba_w[1][0] = s->tf_lba_w[1][1]; s->tf_lba_w[1][1] = v8; }
        else { s->tf_lba_w[1][s->tf_lba_n[1]++] = v8; }
        break;
    case S5L8702_ATA_PIO_LHR:
        if (s->tf_lba_n[2] == 2) { s->tf_lba_w[2][0] = s->tf_lba_w[2][1]; s->tf_lba_w[2][1] = v8; }
        else { s->tf_lba_w[2][s->tf_lba_n[2]++] = v8; }
        break;
    case S5L8702_ATA_PIO_DVR:
        s->tf_device = v8;
        break;

    case S5L8702_ATA_PIO_DTR:            /* PIO write data word (main loop) */
        if (s->write_pending && !fc1307_dma_opcode(s->pending_tf.opcode)) {
            if (s->write_len + 2 <= s->write_expect) {
                s->write_buf[s->write_len++] = v8;
                s->write_buf[s->write_len++] = (uint8_t)(value >> 8);
            }
            if (s->write_len >= s->write_expect) {
                FC1307Taskfile tf = s->pending_tf;
                uint8_t *payload = g_memdup2(s->write_buf, s->write_len);
                uint32_t n = s->write_len;
                fc1307_cancel_write(s);
                s->busy_tf = tf;
                s->busy_kind = FC1307_REQ_TASKFILE;
                s->busy_xfr_bytes = 0;      /* PIO write: no DMA counter */
                s->command_busy = true;
                fc1307_submit(s, FC1307_REQ_TASKFILE, &tf, payload, n);
            }
        }
        break;

    case S5L8702_ATA_PIO_CSD:            /* command register */
    {
        uint8_t op = v8;
        bool ext = fc1307_ext_opcode(op);
        FC1307Taskfile tf;

        fc1307_cancel_write(s);
        s->go_pending = false;               /* stale arm belongs to the old cmd */
        s->pio_remaining = 0;
        fc1307_decode_tf(s, op, ext, &tf);
        s->busy_tf = tf;
        s->busy_kind = FC1307_REQ_TASKFILE;
        s->busy_xfr_bytes = 0;              /* not a DMA write payload yet */
        s->command_busy = true;
        fc1307_submit(s, FC1307_REQ_TASKFILE, &tf, NULL, 0);
        break;
    }

    case S5L8702_ATA_COMMAND:            /* DMA go bit */
        s->reg[offset / 4] = value;
        if (value & S5L8702_ATA_COMMAND_START) {
            if (s->write_pending && fc1307_dma_opcode(s->pending_tf.opcode)) {
                fc1307_dma_write_payload_phase(s,
                    s->reg[S5L8702_ATA_SBUF_START / 4],
                    (uint64_t)s->reg[S5L8702_ATA_XFR_NUM / 4] + 1);
            } else if (s->command_busy &&
                       fc1307_dma_opcode(s->busy_tf.opcode)) {
                /* G5: the real firmware fires the go bit without waiting for
                 * the taskfile completion (CSD -> program regs -> START).
                 * Arm the transfer; the completion BH consumes the snapshot. */
                s->go_pending = true;
                s->go_xfr_num = s->reg[S5L8702_ATA_XFR_NUM / 4];
                s->go_tbuf = s->reg[S5L8702_ATA_TBUF_START / 4];
                s->go_sbuf = s->reg[S5L8702_ATA_SBUF_START / 4];
            } else {
                /* no command in flight: apply from the cached result now */
                const char *why = NULL;
                if (!fc1307_dma_read_apply(s,
                        s->reg[S5L8702_ATA_TBUF_START / 4],
                        (uint64_t)s->reg[S5L8702_ATA_XFR_NUM / 4] + 1,
                        &why)) {
                    fc1307_fail_closed(&s->result, 0x04, why);
                }
                fc1307_complete_event(s);
            }
        }
        if (value & S5L8702_ATA_COMMAND_STOP) {
            fc1307_cancel_write(s);
            s->go_pending = false;           /* transfer aborted */
        }
        break;

    case S5L8702_ATA_PIO_DAD:
        s->reg[offset / 4] = value;
        break;

    default:
        if (offset < S5L8702_ATA_PIO_DTR) {
            s->reg[offset / 4] = value;
        } else {
            qemu_log_mask(LOG_UNIMP, "fc1307-ata: write 0x%" HWADDR_PRIx
                          " = 0x%" PRIx64 "\n", offset, value);
        }
        break;
    }
}

static const MemoryRegionOps fc1307_ata_ops = {
    .read = fc1307_ata_read,
    .write = fc1307_ata_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------- lifecycle */
static void fc1307_ata_reset(DeviceState *dev)
{
    IPodClassicATAFC1307State *s = IPOD_CLASSIC_ATA_FC1307(dev);

    memset(s->reg, 0, sizeof(s->reg));
    fc1307_result_clear(&s->result);
    s->rdata = 0;
    s->rdata_valid = false;
    s->pio_data_off = 0;
    s->pio_remaining = 0;
    s->command_busy = false;
    s->busy_xfr_bytes = 0;
    s->go_pending = false;
    fc1307_cancel_write(s);
    g_free(s->write_buf);
    s->write_buf = NULL;
    fc1307_reset_tf(s);
    fc1307_update_irq(s);                /* D3: line low after the wipe */

    if (s->io_thread_started) {
        FC1307Taskfile z;
        qemu_mutex_lock(&s->io_lock);
        s->generation++;                 /* every in-flight result is stale */
        if (s->req_pending) {
            g_free(s->req.payload);
            s->req.payload = NULL;
            s->req.payload_len = 0;
            s->req_pending = false;
        }
        if (s->done_pending) {
            fc1307_result_clear(&s->done);
            s->done_pending = false;
        }
        qemu_mutex_unlock(&s->io_lock);
        memset(&z, 0, sizeof(z));
        s->busy_kind = FC1307_REQ_RESET;
        if (fc1307_trace()) {
            fprintf(stderr, "FC1307 RESET submit (device reset; COW preserved "
                            "server-side, in-flight result dropped)\n");
        }
        fc1307_submit(s, FC1307_REQ_RESET, &z, NULL, 0);
    }
}

static void fc1307_ata_realize(DeviceState *dev, Error **errp)
{
    IPodClassicATAFC1307State *s = IPOD_CLASSIC_ATA_FC1307(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &fc1307_ata_ops, s,
                          TYPE_IPOD_CLASSIC_ATA_FC1307, S5L8702_ATA_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    s->io_bh = qemu_bh_new(fc1307_io_bh, s);
    qemu_thread_create(&s->io_thread, "fc1307-io", fc1307_io_worker, s,
                       QEMU_THREAD_JOINABLE);
    s->io_thread_started = true;
}

static void fc1307_ata_init(Object *obj)
{
    IPodClassicATAFC1307State *s = IPOD_CLASSIC_ATA_FC1307(obj);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->fd = -1;
    s->io_timeout_ms = FC1307_DEFAULT_TIMEOUT_MS;
    qemu_mutex_init(&s->io_lock);
    qemu_cond_init(&s->io_cond);
    fc1307_reset_tf(s);
}

static void fc1307_ata_finalize(Object *obj)
{
    IPodClassicATAFC1307State *s = IPOD_CLASSIC_ATA_FC1307(obj);

    if (s->io_thread_started) {
        qemu_mutex_lock(&s->io_lock);
        s->io_quit = true;
        qemu_mutex_unlock(&s->io_lock);
        qemu_cond_signal(&s->io_cond);
        qemu_thread_join(&s->io_thread);
        s->io_thread_started = false;
    }
    qemu_mutex_lock(&s->io_lock);
    g_free(s->req.payload);
    s->req.payload = NULL;
    fc1307_result_clear(&s->done);
    qemu_mutex_unlock(&s->io_lock);
    if (s->io_bh) {
        qemu_bh_delete(s->io_bh);
        s->io_bh = NULL;
    }
    qemu_cond_destroy(&s->io_cond);
    fc1307_io_link_close(s);
    fc1307_result_clear(&s->result);
    g_free(s->write_buf);
}

static void fc1307_ata_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "S5L8702 ATA controller (ASYNC EXPERIMENTAL FC1307 backend)";
    dc->realize = fc1307_ata_realize;
    dc->reset = fc1307_ata_reset;
    dc->user_creatable = false;
}

static const TypeInfo fc1307_ata_info = {
    .name = TYPE_IPOD_CLASSIC_ATA_FC1307,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicATAFC1307State),
    .instance_init = fc1307_ata_init,
    .instance_finalize = fc1307_ata_finalize,
    .class_init = fc1307_ata_class_init,
};

static void fc1307_ata_register_types(void)
{
    type_register_static(&fc1307_ata_info);
}

type_init(fc1307_ata_register_types)
