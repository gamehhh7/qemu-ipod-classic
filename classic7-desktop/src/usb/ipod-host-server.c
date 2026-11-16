/*
 * ipod-host-server.c - opt-in, loopback-only external host transport.
 *
 * See include/hw/usb/ipod-host-server.h for the layering statement. In one
 * sentence: this file moves bytes and enforces bounds; every USB-visible effect
 * goes through ipod_host_xfer_* into the real bridge/URA/controller path.
 *
 * Nothing here invents USB semantics. In particular:
 *   - no op writes a USBPacket field,
 *   - no op writes guest PC / IRQ / ready state,
 *   - the only guest memory this file touches is the two DECLARED synthetic
 *     source windows (the SETUP DMA target for read-back, and the IN staging
 *     source for arming), through the normal MemoryRegion dispatch path,
 *   - the only device MMIO this file performs is the ordinary model register
 *     programming a firmware stand-in would do to arm EP0 (see
 *     server_arm_setup_receiver / server_arm_synthetic_in).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "qemu/osdep.h"
#include "hw/arm/ipod_classic/ipod_native_control.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "exec/memattrs.h"
#include "exec/memop.h"
#include "io/channel.h"
#include "io/channel-socket.h"
#include "io/net-listener.h"
#include "qemu/sockets.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "hw/usb.h"
#include "hw/usb/ipod-host-protocol.h"
#include "hw/usb/ipod-host-server.h"
#include "hw/usb/ipod-host-xfer.h"
#include "hw/arm/ipod_classic/ipod_classic_usb.h"
#include "hw/arm/ipod_classic/ipod_classic.h"

/* One live client, one session, one listener. Bounded by construction. */
#define SERVER_MAX_CLIENTS 1

/* Model register offsets used to arm EP0 (ordinary MMIO, not packet forcing). */
#define REG_GAHBCFG   0x008u
#define REG_DCTL      0x804u
#define REG_DIEPCTL0  0x900u
#define REG_DIEPTSIZ0 0x910u
#define REG_DIEPDMA0  0x914u
#define REG_DOEPCTL0  0xb00u
#define REG_DOEPTSIZ0 0xb10u
#define REG_DOEPDMA0  0xb14u
#define REG_DOEPINT0  0xb08u

#define EPCTL_EPENA  (1u << 31)
#define EPCTL_CNAK   (1u << 26)
#define EPCTL_SNAK   (1u << 27)
#define EPCTL_EPDIS  (1u << 30)
#define DOEPCTL_USBACTEP (1u << 15)
#define DIEPCTL_USBACTEP (1u << 15)
#define DOEPTSIZ_PKTCNT1 (1u << 29)
#define DOEPTSIZ_XFRSIZ8 (1u << 19)
#define DIEPTSIZ_PKTCNT1 (1u << 29)
#define DIEPTSIZ_XFRSIZ   (1u << 19)
#define GAHBCFG_DMAEN (1u << 5)
#define DOEPINT_STUP  (1u << 3)

struct IPodHostServer {
    USBDevice *bridge;
    IPodClassicUSBState *controller;
    IPodHostXfer *xfer;

    /*
     * Transport mode. This is not a runtime flag that can drift: it is fixed by
     * which constructor ran, and the synthetic-only handlers test it before
     * anything else so a native server can never program model MMIO.
     */
    ipod_host_mode_t mode;

    /* Synthetic mode only. Always NULL/0 in native mode by construction. */
    uint8_t *ram;
    uint64_t ram_base;
    uint32_t ram_size;

    uint64_t identity;
    uint16_t port;
    uint32_t deadline_ms;

    QIONetListener *listener;
    QIOChannel *client;
    guint client_watch;
    guint client_out_watch;

    /*
     * Incremental, non-blocking framing state. QEMU's *_all QIO helpers would
     * spin up their own GMainContext/GMainLoop and block this thread, so this
     * file uses readv_full/writev_full and keeps the partial progress itself.
     */
    uint8_t rx_buf[IPOD_HOST_PROTO_HEADER_SIZE + IPOD_HOST_PROTO_MAX_PAYLOAD];
    uint32_t rx_have;
    uint32_t rx_need;
    uint8_t tx_buf[IPOD_HOST_PROTO_MAX_FRAME];
    uint32_t tx_len;
    uint32_t tx_off;

    /*
     * loop is the SYNTHETIC bounded loop and is NULL in native mode: there the
     * normal QEMU main loop dispatches these sources on the DEFAULT
     * GMainContext, with the BQL held.
     */
    GMainLoop *loop;
    gint64 deadline;
    bool stop_requested;
    bool client_gone;

    /* Native mode lifecycle. Exactly one close, whatever triggers it. */
    bool attached;
    bool closing;
    QEMUTimer *watchdog;
    uint32_t lifetime_ms;
    Notifier exit_notifier;

    /* Session state. A session exists only after a matching OPEN. */
    bool session_open;
    bool session_closed;
    bool desktop_service;
    uint64_t session_serial;
    uint64_t session_identity;
    uint64_t host_bus_token; /* acquired by native OPEN; terminal on close */

    /* The single control request this session may own. */
    bool have_control;
    uint64_t control_handle;

    IPodNativeControl peer;
    bool peer_mode;
    uint64_t next_peer_id;
    uint64_t bulk_out_sequence;
    QEMUTimer *pulse_timer;bool pulse_pending;uint64_t pulse_token,pulse_sequence;
    QEMUTimer *batch_timer, *batch_deadline;
    bool batch_pending;
    unsigned batch_ep, batch_size, batch_at, batch_naks;
    uint64_t batch_token, batch_serial;
    int64_t batch_started;
    uint8_t batch_data[16384];
    IPodHostServerStats stats;
};

/* ---------------------------------------------------------------- framing */

/*
 * Exactly IPOD_HOST_PROTO_HEADER_SIZE bytes, little endian:
 *   magic:4 version:2 op:2 status:2 flags:2 length:4
 * The size is asserted below; it is the only place the layout is defined.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t op;
    uint16_t status;
    uint16_t flags;
    uint32_t length;
} QEMU_PACKED HostFrameHeader;

QEMU_BUILD_BUG_ON(sizeof(HostFrameHeader) != IPOD_HOST_PROTO_HEADER_SIZE);

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v & 0xffffffffu));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static gboolean server_client_writable(QIOChannel *ioc, GIOCondition cond,
                                       gpointer opaque);

/*
 * Push whatever is buffered in tx_buf to the socket without blocking.
 *
 * Returns 1 when the buffer is fully drained, 0 when the socket is full (the
 * caller must wait for G_IO_OUT) and -1 on a hard error. The *_all QIO helper
 * is deliberately NOT used: with no coroutine it spins its own GMainLoop and
 * would stall this thread.
 */
static int server_tx_flush(IPodHostServer *srv)
{
    struct iovec iov;
    ssize_t rc;

    while (srv->tx_off < srv->tx_len) {
        iov.iov_base = srv->tx_buf + srv->tx_off;
        iov.iov_len = srv->tx_len - srv->tx_off;
        rc = qio_channel_writev_full(srv->client, &iov, 1, NULL, 0, 0, NULL);
        if (rc == QIO_CHANNEL_ERR_BLOCK) {
            return 0;
        }
        if (rc <= 0) {
            return -1;
        }
        srv->tx_off += (uint32_t)rc;
    }
    srv->tx_len = 0;
    srv->tx_off = 0;
    return 1;
}

/*
 * Queue one response frame and try to push it out.
 *
 * A response is always framed completely in tx_buf first, so a partial socket
 * write can never emit half a frame. While a frame is still pending no further
 * request is dispatched, which keeps request/response ordering strict and the
 * buffering bounded to one frame.
 */
static int server_send(IPodHostServer *srv, uint16_t op, uint16_t status,
                       const uint8_t *payload, uint32_t length)
{
    int flushed;

    if (length > IPOD_HOST_PROTO_MAX_PAYLOAD) {
        return -1;
    }
    if (!srv->client) {
        return -1;
    }
    if (srv->tx_len != 0) {
        /* Cannot happen: dispatch is gated on a drained tx buffer. */
        return -1;
    }
    memset(srv->tx_buf, 0, IPOD_HOST_PROTO_HEADER_SIZE);
    put_u32(srv->tx_buf + 0, IPOD_HOST_PROTO_MAGIC);
    put_u16(srv->tx_buf + 4, IPOD_HOST_PROTO_VERSION);
    put_u16(srv->tx_buf + 6, op);
    put_u16(srv->tx_buf + 8, status);
    put_u16(srv->tx_buf + 10, 0);
    put_u32(srv->tx_buf + 12, length);
    if (length) {
        memcpy(srv->tx_buf + IPOD_HOST_PROTO_HEADER_SIZE, payload, length);
    }
    srv->tx_len = IPOD_HOST_PROTO_HEADER_SIZE + length;
    srv->tx_off = 0;

    flushed = server_tx_flush(srv);
    if (flushed < 0) {
        return -1;
    }
    if (flushed == 0) {
        if (!srv->client_out_watch) {
            srv->client_out_watch =
                qio_channel_add_watch(srv->client, G_IO_OUT,
                                      server_client_writable, srv, NULL);
        }
    } else {
        srv->stats.frames_out++;
    }
    return 0;
}

static int server_reply_refused(IPodHostServer *srv, uint16_t op, uint16_t reason)
{
    uint8_t payload[2];
    put_u16(payload, reason);
    srv->stats.refusals++;
    return server_send(srv, op, IPOD_HOST_ST_REFUSED, payload, sizeof(payload));
}

static int server_reply_unsupported(IPodHostServer *srv, uint16_t op,
                                    uint16_t reason)
{
    uint8_t payload[2];
    put_u16(payload, reason);
    srv->stats.unsupported++;
    return server_send(srv, op, IPOD_HOST_ST_UNSUPPORTED, payload,
                       sizeof(payload));
}

static int server_reply_negative(IPodHostServer *srv, uint16_t op,
                                 int32_t backend_rc, uint16_t reason)
{
    uint8_t payload[6];
    put_u16(payload, reason);
    put_u32(payload + 2, (uint32_t)backend_rc);
    srv->stats.negatives++;
    return server_send(srv, op, IPOD_HOST_ST_NEGATIVE, payload, sizeof(payload));
}

static int server_reply_ok(IPodHostServer *srv, uint16_t op,
                           const uint8_t *payload, uint32_t length)
{
    return server_send(srv, op, IPOD_HOST_ST_OK, payload, length);
}

/*
 * The structural refusal for every synthetic-only operation. In native mode the
 * request is answered BEFORE any session, handle or range check runs, so it
 * cannot touch model state even as a side effect of validation.
 */
static int server_reply_synthetic_only(IPodHostServer *srv, uint16_t op)
{
    srv->stats.synthetic_refusals++;
    return server_reply_unsupported(srv, op, IPOD_HOST_REASON_SYNTHETIC_ONLY);
}

/* ------------------------------------------------------------ stop/teardown */

/*
 * Ask the server to stop. In synthetic mode that means leaving the bounded
 * fixture loop; in native mode there is no loop to leave, so the transport is
 * torn down in place and normal QEMU execution continues untouched.
 */
static void server_request_stop(IPodHostServer *srv);
static void server_end_session(IPodHostServer *srv);
static void server_pulse_cancel(IPodHostServer *srv);
static void server_batch_cancel(IPodHostServer *srv);
static void server_close_client(IPodHostServer *srv);

/* -------------------------------------------------------- guest-side access */

/*
 * Program the real controller's EP0 OUT registers so that ONE 8-byte SETUP is
 * accepted into the declared synthetic RAM window. This is the same ordinary
 * MMIO sequence a firmware stand-in performs; it is NOT a packet or state
 * injection. It writes model registers through the device's own MemoryRegion.
 */
static bool server_ctrl_write(IPodHostServer *srv, uint32_t off, uint32_t value)
{
    MemTxResult res;

    /*
     * Native mode must never write model MMIO: the real firmware owns EP0. This
     * guard is belt-and-braces on top of the handler-level refusals, and the
     * counter it feeds is asserted to stay zero by the native harness.
     */
    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        return false;
    }
    res = memory_region_dispatch_write(&srv->controller->iomem, off, value,
                                       MO_32 | MO_LE, MEMTXATTRS_UNSPECIFIED);
    if (res == MEMTX_OK) {
        srv->stats.mmio_writes++;
        return true;
    }
    return false;
}

static bool server_arm_setup_receiver(IPodHostServer *srv, uint32_t dma_addr,
                                      uint32_t length)
{
    if (length == 0 || length > 64 || (dma_addr & 3)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_GAHBCFG, GAHBCFG_DMAEN)) {
        return false;
    }
    /* Clear a stale SETUP-done interrupt, then arm the OUT endpoint. */
    if (!server_ctrl_write(srv, REG_DOEPINT0, DOEPINT_STUP)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_DOEPCTL0,
                           EPCTL_EPENA | DOEPCTL_USBACTEP)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_DOEPTSIZ0,
                           DOEPTSIZ_PKTCNT1 | DOEPTSIZ_XFRSIZ8 | length)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_DOEPDMA0, dma_addr)) {
        return false;
    }
    srv->stats.arm_setup_calls++;
    return true;
}

/*
 * Program the real controller's EP0 IN registers so a subsequent IN token can
 * stage up to `length` bytes from the declared synthetic RAM source window.
 * Again: model register writes only, no packet or completion fabrication.
 */
static bool server_arm_synthetic_in(IPodHostServer *srv, uint32_t dma_addr,
                                    uint32_t length)
{
    if (length == 0 || length > IPOD_HOST_RAM_IN_MAX || (dma_addr & 3)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_GAHBCFG, GAHBCFG_DMAEN)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_DIEPDMA0, dma_addr)) {
        return false;
    }
    if (!server_ctrl_write(srv, REG_DIEPTSIZ0,
                           DIEPTSIZ_PKTCNT1 | DIEPTSIZ_XFRSIZ | length)) {
        return false;
    }
    /* EPENA|CNAK last: the controller arm sequence requires the enable last. */
    if (!server_ctrl_write(srv, REG_DIEPCTL0,
                           EPCTL_EPENA | EPCTL_CNAK | DIEPCTL_USBACTEP)) {
        return false;
    }
    srv->stats.arm_in_calls++;
    return true;
}

/* Read back the declared synthetic SETUP window through the address space. */
static bool server_read_setup_window(IPodHostServer *srv, uint8_t *out,
                                     uint32_t *actual)
{
    if (address_space_read(&address_space_memory, srv->ram_base,
                           MEMTXATTRS_UNSPECIFIED, out,
                           IPOD_HOST_RAM_SETUP_LEN) != MEMTX_OK) {
        return false;
    }
    *actual = IPOD_HOST_RAM_SETUP_LEN;
    srv->stats.setup_dma_reads++;
    return true;
}

/* ---------------------------------------------------------------- handlers */

static int server_op_hello(IPodHostServer *srv, const uint8_t *req,
                           uint32_t req_len)
{
    uint8_t out[32];
    uint32_t caps;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_HELLO,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    /*
     * A closed session is terminal for this bounded server: report it instead
     * of answering the handshake as if the session were still usable.
     */
    if (srv->session_closed && !srv->desktop_service) {
        return server_reply_refused(srv, IPOD_HOST_OP_HELLO,
                                    IPOD_HOST_REASON_SESSION_CLOSED);
    }
    /*
     * The capability word IS the mode statement, and it is derived from the
     * mode field, not from what happens to be configured. SYNTHETIC_RAM is
     * never set in native mode; NATIVE_CONTROLLER is never set in synthetic
     * mode. A client cannot ask for the other one.
     */
    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        caps = IPOD_HOST_CAP_OBSERVATION_ONLY | IPOD_HOST_CAP_NATIVE_CONTROLLER |
               IPOD_HOST_CAP_BUS_RESET_EVENTS | (1u << 8) | IPOD_HOST_CAP_BULK_OUT_BATCH;
    } else {
        caps = IPOD_HOST_CAP_OBSERVATION_ONLY | IPOD_HOST_CAP_SYNTHETIC_RAM;
    }
    /*
     * Registration proof is the owner object itself: ipod_host_xfer_create()
     * only returns non-NULL after the bridge accepted the owner notification
     * registration, so a live xfer IS a registered owner. binding_open is a
     * per-control-transfer state and is false before the first bind, so it must
     * not be used as the registration test here.
     */
    if (srv->xfer) {
        caps |= IPOD_HOST_CAP_OWNER_REGISTERED;
    }
    /* STATUS_COMMIT / HCD / ENUMERATION are deliberately absent. */
    memset(out, 0, sizeof(out));
    put_u32(out + 0, IPOD_HOST_PROTO_VERSION);
    put_u32(out + 4, caps);
    put_u64(out + 8, srv->identity);
    /*
     * The synthetic RAM description is reported only in synthetic mode. Native
     * mode has no synthetic window, so it reports zeros rather than inventing a
     * layout a client could then try to use.
     */
    put_u32(out + 16, srv->mode == IPOD_HOST_MODE_NATIVE ? 0 : srv->ram_size);
    put_u32(out + 20, srv->mode == IPOD_HOST_MODE_NATIVE
                          ? 0 : IPOD_HOST_RAM_SETUP_OFFSET);
    put_u32(out + 24, srv->mode == IPOD_HOST_MODE_NATIVE
                          ? 0 : IPOD_HOST_RAM_IN_OFFSET);
    put_u32(out + 28, srv->mode == IPOD_HOST_MODE_NATIVE
                          ? 0 : IPOD_HOST_RAM_IN_MAX);
    return server_reply_ok(srv, IPOD_HOST_OP_HELLO, out, sizeof(out));
}

static int server_op_open(IPodHostServer *srv, const uint8_t *req,
                          uint32_t req_len)
{
    uint8_t out[32];
    uint64_t identity;

    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_OPEN,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    identity = get_u64(req);
    if (srv->desktop_service && srv->session_closed && identity == srv->identity) { srv->session_closed=false;srv->bulk_out_sequence=0;memset(&srv->peer,0,sizeof(srv->peer));srv->peer_mode=false; }
    if (srv->session_closed) {
        return server_reply_refused(srv, IPOD_HOST_OP_OPEN,
                                    IPOD_HOST_REASON_SESSION_CLOSED);
    }
    if (srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_OPEN,
                                    IPOD_HOST_REASON_SESSION_BUSY);
    }
    if (identity != srv->identity) {
        return server_reply_refused(srv, IPOD_HOST_OP_OPEN,
                                    IPOD_HOST_REASON_IDENTITY);
    }
    if (!srv->xfer) {
        /* The real owner must exist (registered on the bridge) before a session. */
        return server_reply_refused(srv, IPOD_HOST_OP_OPEN,
                                    IPOD_HOST_REASON_NO_SESSION);
    }

    if (srv->mode == IPOD_HOST_MODE_NATIVE &&
        !ipod_classic_usb_host_bus_acquire(srv->controller, srv,
                                         &srv->host_bus_token)) {
        return server_reply_refused(srv, IPOD_HOST_OP_OPEN,
                                    IPOD_HOST_REASON_BUS_EVENT);
    }
    srv->session_open = true;
    srv->session_serial = (uint64_t)(srv->stats.session_opens + 1);
    srv->session_identity = identity;
    srv->stats.session_opens++;

    memset(out, 0, sizeof(out));
    put_u64(out + 0, srv->session_serial);
    put_u64(out + 8, identity);
    put_u64(out + 16, srv->host_bus_token);
    put_u64(out + 24, IPOD_HOST_BUS_MIN_RESET_NS);
    return server_reply_ok(srv, IPOD_HOST_OP_OPEN, out,
                          srv->mode == IPOD_HOST_MODE_NATIVE ? 32 : 16);
}

/*
 * Turn the owner's packet retirement record into the protocol's explicit
 * outcome. "ACCEPTED" only ever means the copied controller receiver accepted
 * the 8 SETUP bytes and performed its single DMA; it is NOT an ACK, an XFRC, a
 * status completion or a URB success.
 */
static uint16_t server_setup_outcome(IPodHostXfer *xfer, uint64_t cookie,
                                     ipod_host_packet_info_t *pi_out)
{
    ipod_host_packet_info_t pi;

    memset(&pi, 0, sizeof(pi));
    if (!cookie || !ipod_host_xfer_packet_info(xfer, cookie, &pi)) {
        return IPOD_HOST_SETUP_PENDING;
    }
    if (pi_out) {
        *pi_out = pi;
    }
    if (!pi.retired) {
        /* Still parked in the transport: the honest answer is "pending". */
        return IPOD_HOST_SETUP_PENDING;
    }
    if (pi.status == USB_RET_SUCCESS) {
        return IPOD_HOST_SETUP_ACCEPTED;
    }
    if (pi.status == USB_RET_NAK) {
        return IPOD_HOST_SETUP_NAK;
    }
    return IPOD_HOST_SETUP_NEGATIVE;
}

static int server_reply_setup(IPodHostServer *srv, uint16_t op, uint64_t handle,
                              uint64_t cookie, const ipod_host_control_info_t *ci)
{
    uint8_t out[40];
    ipod_host_packet_info_t pi;

    memset(&pi, 0, sizeof(pi));
    memset(out, 0, sizeof(out));
    put_u64(out + 0, handle);
    put_u64(out + 8, cookie);
    put_u32(out + 16, ci->seq);
    put_u16(out + 20, server_setup_outcome(srv->xfer, cookie, &pi));
    put_u16(out + 22, 0);
    put_u32(out + 24, (uint32_t)ci->setup_status);
    put_u64(out + 28, ci->setup_epoch);
    return server_reply_ok(srv, op, out, 36);
}


/* Real line consumer is the existing machine controller, never the bridge.
 * OPEN is not attach; bridge reset below only drains metadata after an actual
 * accepted ASSERT input. The partial core derives USBRST at that rising edge. */
static int server_op_bus_reset(IPodHostServer *srv, uint16_t op,
    const uint8_t *req, uint32_t req_len)
{
    IPodHostBusResult result;
    uint8_t out[32] = {0};
    bool asserted = op == IPOD_HOST_OP_BUS_RESET_ASSERT;
    if(srv->pulse_pending) return server_reply_refused(srv,op,IPOD_HOST_REASON_SESSION_BUSY);
    if (srv->mode != IPOD_HOST_MODE_NATIVE) {
        return server_reply_unsupported(srv, op, IPOD_HOST_REASON_WRONG_MODE);
    }
    if (req_len != 24) {
        return server_reply_refused(srv, op, IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open || srv->session_closed || srv->closing || !srv->xfer) {
        return server_reply_refused(srv, op, IPOD_HOST_REASON_NO_SESSION);
    }
    if (get_u64(req) != srv->session_serial ||
        get_u64(req + 8) != srv->host_bus_token) {
        return server_reply_refused(srv, op, IPOD_HOST_REASON_IDENTITY);
    }
    result = ipod_classic_usb_host_bus_event(srv->controller, srv,
        srv->host_bus_token, get_u64(req + 16), asserted);
    if (asserted && result == IPOD_HOST_BUS_OK) {
        usb_device_reset(srv->bridge); /* metadata, not a second electrical edge */
        (void)ipod_host_xfer_cancel(srv->xfer);
        srv->have_control = false;
        srv->control_handle = 0;
        memset(&srv->peer,0,sizeof(srv->peer));
    }
    put_u32(out, result);
    put_u32(out + 4, asserted);
    put_u64(out + 8, get_u64(req + 16));
    put_u64(out + 16, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    put_u64(out + 24, IPOD_HOST_BUS_RECOVERY_NS);
    /* SHORT is negative even though its physical line withdrawal took place.
     * Other rejections do not consume seq. No response claims enumeration. */
    return server_send(srv, op, result == IPOD_HOST_BUS_OK ?
                       IPOD_HOST_ST_OK : IPOD_HOST_ST_NEGATIVE, out, sizeof(out));
}

static bool server_bus_tokens_ready(IPodHostServer *srv)
{
    return srv->mode != IPOD_HOST_MODE_NATIVE ||
        ipod_classic_usb_host_bus_ready(srv->controller, srv, srv->host_bus_token);
}

static int server_op_submit_setup(IPodHostServer *srv, const uint8_t *req,
                                  uint32_t req_len)
{
    uint64_t handle = 0;
    int final_status = 0;
    int rc;
    ipod_host_control_info_t ci;

    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_SUBMIT_SETUP,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_SUBMIT_SETUP,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    if (!server_bus_tokens_ready(srv)) {
        return server_reply_refused(srv, IPOD_HOST_OP_SUBMIT_SETUP,
                                    IPOD_HOST_REASON_RESET_RECOVERY);
    }
    if (srv->have_control) {
        /* One control request per session keeps the ledger bound explicit. */
        return server_reply_refused(srv, IPOD_HOST_OP_SUBMIT_SETUP,
                                    IPOD_HOST_REASON_SESSION_BUSY);
    }

    rc = ipod_host_xfer_submit_control(srv->xfer, req, &handle, &final_status);
    srv->stats.setup_submits++;
    if (rc == IPOD_HOST_XFER_REJECTED) {
        return server_reply_negative(srv, IPOD_HOST_OP_SUBMIT_SETUP,
                                     final_status, IPOD_HOST_REASON_BACKEND);
    }
    if (rc != 0) {
        return server_reply_negative(srv, IPOD_HOST_OP_SUBMIT_SETUP, rc,
                                     IPOD_HOST_REASON_BACKEND);
    }

    srv->have_control = true;
    srv->control_handle = handle;

    memset(&ci, 0, sizeof(ci));
    (void)ipod_host_xfer_control_info(srv->xfer, handle, &ci);
    return server_reply_setup(srv, IPOD_HOST_OP_SUBMIT_SETUP, handle,
                              ci.packet_cookie, &ci);
}

static int server_op_retry_setup(IPodHostServer *srv, const uint8_t *req,
                                 uint32_t req_len)
{
    uint64_t handle;
    uint64_t cookie = 0;
    int rc;
    ipod_host_control_info_t ci;

    /* handle(8) */
    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_RETRY_SETUP,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_RETRY_SETUP,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    if (!server_bus_tokens_ready(srv)) {
        return server_reply_refused(srv, IPOD_HOST_OP_RETRY_SETUP,
                                    IPOD_HOST_REASON_RESET_RECOVERY);
    }
    handle = get_u64(req);
    if (!srv->have_control || handle != srv->control_handle) {
        return server_reply_refused(srv, IPOD_HOST_OP_RETRY_SETUP,
                                    IPOD_HOST_REASON_UNKNOWN_HANDLE);
    }
    rc = ipod_host_xfer_retry_setup(srv->xfer, handle, &cookie);
    if (rc != 0) {
        /* A retry is only legal after a NAK retirement; anything else is a
         * negative answer, never a fabricated acceptance. */
        return server_reply_negative(srv, IPOD_HOST_OP_RETRY_SETUP, rc,
                                     IPOD_HOST_REASON_BAD_SHAPE);
    }
    memset(&ci, 0, sizeof(ci));
    (void)ipod_host_xfer_control_info(srv->xfer, handle, &ci);
    return server_reply_setup(srv, IPOD_HOST_OP_RETRY_SETUP, handle,
                              ci.packet_cookie, &ci);
}

static int server_op_stage_in(IPodHostServer *srv, const uint8_t *req,
                              uint32_t req_len)
{
    uint8_t out[24];
    uint8_t buf[IPOD_HOST_RAM_IN_MAX];
    uint64_t handle;
    uint64_t cookie = 0;
    uint32_t length;
    int rc;

    /* handle(8) + length(4) = 12 */
    if (req_len != 12) {
        return server_reply_refused(srv, IPOD_HOST_OP_STAGE_IN,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_STAGE_IN,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    if (!server_bus_tokens_ready(srv)) {
        return server_reply_refused(srv, IPOD_HOST_OP_STAGE_IN,
                                    IPOD_HOST_REASON_RESET_RECOVERY);
    }
    handle = get_u64(req);
    length = get_u32(req + 8);
    if (!srv->have_control || handle != srv->control_handle) {
        return server_reply_refused(srv, IPOD_HOST_OP_STAGE_IN,
                                    IPOD_HOST_REASON_UNKNOWN_HANDLE);
    }
    if (length > IPOD_HOST_RAM_IN_MAX) {
        return server_reply_refused(srv, IPOD_HOST_OP_STAGE_IN,
                                    IPOD_HOST_REASON_OVERSIZE);
    }

    if (length == 0) {
        /* Status stage of a no-data request: zero-length IN, no caller buffer. */
        rc = ipod_host_xfer_submit_stage_in(srv->xfer, handle, NULL, 0, &cookie);
    } else {
        rc = ipod_host_xfer_submit_stage_in(srv->xfer, handle, buf, length,
                                            &cookie);
    }
    srv->stats.stage_in_submits++;
    if (rc != 0) {
        return server_reply_negative(srv, IPOD_HOST_OP_STAGE_IN, rc,
                                     IPOD_HOST_REASON_BAD_SHAPE);
    }

    memset(out, 0, sizeof(out));
    put_u64(out + 0, cookie);
    put_u32(out + 8, length);
    put_u32(out + 12, 0);
    put_u64(out + 16, handle);
    return server_reply_ok(srv, IPOD_HOST_OP_STAGE_IN, out, sizeof(out));
}

static int server_op_observe(IPodHostServer *srv, const uint8_t *req,
                             uint32_t req_len)
{
    uint8_t out[8 + 4 + 4 + 1 + 3 + 4 + 8 * 4 + IPOD_HOST_PROTO_MAX_PAYLOAD];
    ipod_host_observation_t obs;
    uint64_t handle;
    uint32_t off = 0;
    int rc;

    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_OBSERVE,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_OBSERVE,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    handle = get_u64(req);
    if (!srv->have_control || handle != srv->control_handle) {
        return server_reply_refused(srv, IPOD_HOST_OP_OBSERVE,
                                    IPOD_HOST_REASON_UNKNOWN_HANDLE);
    }

    memset(&obs, 0, sizeof(obs));
    rc = ipod_host_xfer_observe(srv->xfer, handle, &obs);
    if (rc == URA_ERR_STATE) {
        return server_reply_refused(srv, IPOD_HOST_OP_OBSERVE,
                                    IPOD_HOST_REASON_NOTHING_STAGED);
    }
    if (rc != 0) {
        return server_reply_negative(srv, IPOD_HOST_OP_OBSERVE, rc,
                                     IPOD_HOST_REASON_BACKEND);
    }
    srv->stats.observations++;

    memset(out, 0, sizeof(out));
    put_u32(out + off, obs.length); off += 4;
    put_u32(out + off, obs.declared_length); off += 4;
    out[off++] = obs.direction;
    /*
     * Provenance is a statement about where the bytes could have come from, so
     * it is derived from the mode: a native observation can only ever be guest
     * RAM the real firmware staged from, never the fixture's synthetic window.
     */
    out[off++] = (srv->mode == IPOD_HOST_MODE_NATIVE)
                     ? (uint8_t)IPOD_HOST_SOURCE_GUEST_RAM
                     : (uint8_t)IPOD_HOST_SOURCE_SYNTHETIC_RAM;
    out[off++] = 1; /* observation_only: always true in this slice */
    out[off++] = obs.staged ? 1 : 0;
    put_u32(out + off, obs.seq); off += 4;
    put_u64(out + off, obs.setup_epoch); off += 8;
    put_u64(out + off, obs.stamp_owner); off += 8;
    put_u64(out + off, obs.stamp_generation); off += 8;
    put_u64(out + off, obs.stamp_id); off += 8;
    memcpy(out + off, obs.bytes, obs.length); off += obs.length;
    return server_reply_ok(srv, IPOD_HOST_OP_OBSERVE, out, off);
}

static int server_op_query_control(IPodHostServer *srv, const uint8_t *req,
                                   uint32_t req_len)
{
    uint8_t out[40];
    ipod_host_control_info_t ci;
    uint64_t handle;

    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_CONTROL,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_CONTROL,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    handle = get_u64(req);
    memset(&ci, 0, sizeof(ci));
    if (!ipod_host_xfer_control_info(srv->xfer, handle, &ci)) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_CONTROL,
                                    IPOD_HOST_REASON_UNKNOWN_HANDLE);
    }
    memset(out, 0, sizeof(out));
    put_u64(out + 0, ci.req_handle);
    put_u64(out + 8, ci.packet_cookie);
    put_u32(out + 16, ci.seq);
    out[20] = ci.direction;
    out[21] = ci.open ? 1 : 0;
    out[22] = ci.setup_retired ? 1 : 0;
    out[23] = 0;
    put_u32(out + 24, ci.declared_length);
    put_u32(out + 28, (uint32_t)ci.setup_status);
    put_u64(out + 32, ci.setup_epoch);
    return server_reply_ok(srv, IPOD_HOST_OP_QUERY_CONTROL, out, sizeof(out));
}

static int server_op_query_packet(IPodHostServer *srv, const uint8_t *req,
                                  uint32_t req_len)
{
    uint8_t out[48];
    ipod_host_packet_info_t pi;
    uint64_t cookie;

    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_PACKET,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_PACKET,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    cookie = get_u64(req);
    memset(&pi, 0, sizeof(pi));
    if (!ipod_host_xfer_packet_info(srv->xfer, cookie, &pi)) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_PACKET,
                                    IPOD_HOST_REASON_UNKNOWN_COOKIE);
    }
    memset(out, 0, sizeof(out));
    put_u64(out + 0, pi.cookie);
    put_u32(out + 8, pi.seq);
    put_u32(out + 12, (uint32_t)pi.reason);
    put_u32(out + 16, (uint32_t)pi.status);
    out[20] = pi.retired ? 1 : 0;
    out[21] = pi.live ? 1 : 0;
    out[22] = pi.was_async ? 1 : 0;
    out[23] = pi.in_submit ? 1 : 0;
    out[24] = pi.attached ? 1 : 0;
    out[25] = 0;
    out[26] = 0;
    out[27] = 0;
    put_u64(out + 28, pi.epoch);
    put_u64(out + 36, pi.order);
    return server_reply_ok(srv, IPOD_HOST_OP_QUERY_PACKET, out, 44);
}

static int server_op_pump(IPodHostServer *srv, const uint8_t *req,
                          uint32_t req_len)
{
    uint8_t out[8];
    uint64_t cookie;
    int rc;

    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_PUMP,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_PUMP,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    cookie = get_u64(req);
    rc = ipod_host_xfer_pump(srv->xfer, cookie);
    if (rc == URA_ERR_LATE) {
        return server_reply_refused(srv, IPOD_HOST_OP_PUMP,
                                    IPOD_HOST_REASON_UNKNOWN_COOKIE);
    }
    if (rc != 0) {
        return server_reply_negative(srv, IPOD_HOST_OP_PUMP, rc,
                                     IPOD_HOST_REASON_BACKEND);
    }
    memset(out, 0, sizeof(out));
    put_u64(out + 0, cookie);
    return server_reply_ok(srv, IPOD_HOST_OP_PUMP, out, sizeof(out));
}

static int server_op_cancel(IPodHostServer *srv, const uint8_t *req,
                            uint32_t req_len)
{
    uint8_t out[8];
    int rc;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_CANCEL,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_CANCEL,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    rc = ipod_host_xfer_cancel(srv->xfer);
    /* The drain closes every control record; the handle is now stale. */
    srv->have_control = false;
    srv->control_handle = 0;
    if (rc != 0) {
        return server_reply_negative(srv, IPOD_HOST_OP_CANCEL, rc,
                                     IPOD_HOST_REASON_BACKEND);
    }
    memset(out, 0, sizeof(out));
    put_u64(out + 0, srv->session_serial);
    return server_reply_ok(srv, IPOD_HOST_OP_CANCEL, out, sizeof(out));
}

static int server_op_arm_synthetic_in(IPodHostServer *srv, const uint8_t *req,
                                      uint32_t req_len)
{
    uint8_t out[16];
    uint32_t offset;
    uint32_t length;

    /*
     * Native mode has no synthetic source window and must not program model
     * MMIO. Refuse before reading the request so nothing can be misread as a
     * range or session error.
     */
    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        return server_reply_synthetic_only(srv, IPOD_HOST_OP_ARM_SYNTHETIC_IN);
    }
    /* offset(4) + length(4) */
    if (req_len != 8) {
        return server_reply_refused(srv, IPOD_HOST_OP_ARM_SYNTHETIC_IN,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_ARM_SYNTHETIC_IN,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    offset = get_u32(req);
    length = get_u32(req + 4);
    if (length == 0 || length > IPOD_HOST_RAM_IN_MAX ||
        offset != IPOD_HOST_RAM_IN_OFFSET ||
        offset + length > srv->ram_size) {
        return server_reply_refused(srv, IPOD_HOST_OP_ARM_SYNTHETIC_IN,
                                    IPOD_HOST_REASON_RANGE);
    }
    if (!server_arm_synthetic_in(srv, (uint32_t)(srv->ram_base + offset),
                                 length)) {
        return server_reply_negative(srv, IPOD_HOST_OP_ARM_SYNTHETIC_IN,
                                     -1, IPOD_HOST_REASON_BACKEND);
    }
    memset(out, 0, sizeof(out));
    put_u32(out + 0, offset);
    put_u32(out + 4, length);
    put_u64(out + 8, srv->ram_base + offset);
    return server_reply_ok(srv, IPOD_HOST_OP_ARM_SYNTHETIC_IN, out, sizeof(out));
}

static int server_op_arm_setup_receiver(IPodHostServer *srv, const uint8_t *req,
                                        uint32_t req_len)
{
    uint8_t out[8];

    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        return server_reply_synthetic_only(srv,
                                           IPOD_HOST_OP_ARM_SETUP_RECEIVER);
    }
    if (req_len != 4) {
        return server_reply_refused(srv, IPOD_HOST_OP_ARM_SETUP_RECEIVER,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_ARM_SETUP_RECEIVER,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    if (!server_arm_setup_receiver(srv,
                                   (uint32_t)(srv->ram_base +
                                              IPOD_HOST_RAM_SETUP_OFFSET),
                                   get_u32(req))) {
        return server_reply_negative(srv, IPOD_HOST_OP_ARM_SETUP_RECEIVER,
                                     -1, IPOD_HOST_REASON_BACKEND);
    }
    memset(out, 0, sizeof(out));
    put_u32(out + 0, IPOD_HOST_RAM_SETUP_OFFSET);
    put_u32(out + 4, IPOD_HOST_RAM_SETUP_LEN);
    return server_reply_ok(srv, IPOD_HOST_OP_ARM_SETUP_RECEIVER, out,
                           sizeof(out));
}

static int server_op_read_setup_dma(IPodHostServer *srv, const uint8_t *req,
                                    uint32_t req_len)
{
    uint8_t out[4 + IPOD_HOST_RAM_SETUP_LEN];
    uint8_t bytes[IPOD_HOST_RAM_SETUP_LEN];
    uint32_t actual = 0;

    /*
     * The synthetic read-back reads the fixture's declared window. Native mode
     * has no such window, and must never fall back to a fixed address: the only
     * legal native read-back is READ_NATIVE_SETUP, which is bound to whatever
     * address the firmware itself programmed into DOEPDMA0.
     */
    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        return server_reply_synthetic_only(srv, IPOD_HOST_OP_READ_SETUP_DMA);
    }
    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_READ_SETUP_DMA,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_READ_SETUP_DMA,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    if (!server_read_setup_window(srv, bytes, &actual)) {
        return server_reply_negative(srv, IPOD_HOST_OP_READ_SETUP_DMA,
                                     -1, IPOD_HOST_REASON_BACKEND);
    }
    memset(out, 0, sizeof(out));
    put_u32(out + 0, actual);
    memcpy(out + 4, bytes, actual);
    return server_reply_ok(srv, IPOD_HOST_OP_READ_SETUP_DMA, out, 4 + actual);
}

/*
 * Native-only observation ops. These read; they never write. There is no op
 * anywhere in this protocol that can write guest RAM, a model register, a
 * USBPacket field, guest PC, or an IRQ line.
 */

/* Real machine controller identity + the honest statement of what is absent. */
static int server_op_native_info(IPodHostServer *srv, const uint8_t *req,
                                 uint32_t req_len)
{
    uint8_t out[48];
    const IPodUSBRegisters *r;
    uint32_t caps;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_NATIVE_INFO,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (srv->mode != IPOD_HOST_MODE_NATIVE) {
        /* The synthetic server has no machine controller to describe. */
        srv->stats.refusals++;
        return server_reply_refused(srv, IPOD_HOST_OP_NATIVE_INFO,
                                    IPOD_HOST_REASON_WRONG_MODE);
    }
    if (!srv->controller) {
        return server_reply_refused(srv, IPOD_HOST_OP_NATIVE_INFO,
                                    IPOD_HOST_REASON_BACKEND);
    }

    r = &srv->controller->regs;
    caps = IPOD_HOST_CAP_OBSERVATION_ONLY | IPOD_HOST_CAP_NATIVE_CONTROLLER |
               IPOD_HOST_CAP_BUS_RESET_EVENTS | (1u << 8) | IPOD_HOST_CAP_BULK_OUT_BATCH;
    if (srv->xfer) {
        caps |= IPOD_HOST_CAP_OWNER_REGISTERED;
    }
    memset(out, 0, sizeof(out));
    put_u32(out + 0, IPOD_HOST_PROTO_VERSION);
    put_u32(out + 4, caps);
    put_u64(out + 8, srv->identity);
    /* Live controller line state: read-only facts, not inferred ones. */
    out[16] = r->vbus ? 1 : 0;
    out[17] = r->reset_line ? 1 : 0;
    out[18] = (r->gusbcfg & (1u << 29)) ? 1 : 0; /* host mode */
    out[19] = (r->gahbcfg & (1u << 5)) ? 1 : 0;  /* DMA enable */
    /* Controller identity the machine instantiated (the USBOTG window base). */
    put_u64(out + 24, (uint64_t)USBOTG_MEM_BASE);
    put_u32(out + 32, (uint32_t)ipod_classic_usb_setup_epoch(srv->controller));
    put_u32(out + 36, (uint32_t)ipod_classic_usb_in_arm_epoch(srv->controller));
    put_u32(out + 40, (uint32_t)ipod_classic_usb_in_staged_len(srv->controller));
    put_u32(out + 44, 0); /* reserved, always zero */
    return server_reply_ok(srv, IPOD_HOST_OP_NATIVE_INFO, out, sizeof(out));
}

/*
 * Read back the 8 SETUP bytes the firmware's own receiver wrote, from the
 * address the firmware itself programmed into DOEPDMA0.
 *
 * The window is not a constant: it is whatever the guest chose. It is accepted
 * only if it is 4-byte aligned, inside a real RAM window of this machine, and
 * fully readable for 8 bytes. Anything else is an explicit "unsupported" --
 * there is no fallback address and no synthetic substitute.
 */
static int server_op_read_native_setup(IPodHostServer *srv, const uint8_t *req,
                                       uint32_t req_len)
{
    uint8_t out[4 + IPOD_HOST_RAM_SETUP_LEN];
    uint8_t bytes[IPOD_HOST_RAM_SETUP_LEN];
    uint32_t addr;
    uint32_t end;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (srv->mode != IPOD_HOST_MODE_NATIVE) {
        srv->stats.refusals++;
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_WRONG_MODE);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    if (!srv->controller) {
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_BACKEND);
    }

    /*
     * The firmware-selected SETUP DMA target. If the guest has not armed EP0
     * OUT with a DMA address, there is nothing to read and saying so is the
     * only honest answer.
     */
    addr = srv->controller->regs.out_dma[0];
    if (!addr || (addr & 3)) {
        srv->stats.native_setup_absent++;
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_NOT_ARMED);
    }
    end = addr + (uint32_t)IPOD_HOST_RAM_SETUP_LEN;
    if (end < addr) { /* 32-bit wrap */
        srv->stats.native_setup_absent++;
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_RANGE);
    }
    if (!((addr >= SDRAM_MEM_BASE &&
           end <= SDRAM_MEM_BASE + SDRAM_MEM_SIZE) ||
          (addr >= SDRAM_UNCACHED_MEM_BASE &&
           end <= SDRAM_UNCACHED_MEM_BASE + SDRAM_MEM_SIZE) ||
          (addr >= IRAM0_MEM_BASE && end <= IRAM0_MEM_BASE + 0x20000u) ||
          (addr >= IRAM1_MEM_BASE && end <= IRAM1_MEM_BASE + 0x20000u))) {
        /*
         * The guest programmed a DMA target outside every RAM window this
         * machine maps. Refuse rather than read a device or an unmapped hole.
         */
        srv->stats.native_setup_absent++;
        return server_reply_refused(srv, IPOD_HOST_OP_READ_NATIVE_SETUP,
                                    IPOD_HOST_REASON_RANGE);
    }
    if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                           bytes, IPOD_HOST_RAM_SETUP_LEN) != MEMTX_OK) {
        srv->stats.native_setup_absent++;
        return server_reply_negative(srv, IPOD_HOST_OP_READ_NATIVE_SETUP, -1,
                                     IPOD_HOST_REASON_BACKEND);
    }

    srv->stats.native_setup_reads++;
    memset(out, 0, sizeof(out));
    put_u32(out + 0, IPOD_HOST_RAM_SETUP_LEN);
    memcpy(out + 4, bytes, IPOD_HOST_RAM_SETUP_LEN);
    return server_reply_ok(srv, IPOD_HOST_OP_READ_NATIVE_SETUP, out,
                           sizeof(out));
}

/* Read-only EP0 arm snapshot: what the firmware has programmed, verbatim. */
static int server_op_query_ep0(IPodHostServer *srv, const uint8_t *req,
                               uint32_t req_len)
{
    uint8_t out[64];
    const IPodUSBRegisters *r;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_EP0,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (srv->mode != IPOD_HOST_MODE_NATIVE) {
        srv->stats.refusals++;
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_EP0,
                                    IPOD_HOST_REASON_WRONG_MODE);
    }
    if (!srv->controller) {
        return server_reply_refused(srv, IPOD_HOST_OP_QUERY_EP0,
                                    IPOD_HOST_REASON_BACKEND);
    }
    r = &srv->controller->regs;
    memset(out, 0, sizeof(out));
    put_u32(out + 0, r->out_ctl[0]);
    put_u32(out + 4, r->out_tsiz[0]);
    put_u32(out + 8, r->out_dma[0]);
    put_u32(out + 12, r->in_ctl[0]);
    put_u32(out + 16, r->in_tsiz[0]);
    put_u32(out + 20, r->in_dma[0]);
    put_u32(out + 24, r->out_int[0]);
    put_u32(out + 28, r->in_int[0]);
    put_u32(out + 32, r->gahbcfg);
    put_u32(out + 36, r->dcfg);
    out[40] = r->vbus ? 1 : 0;
    out[41] = r->reset_line ? 1 : 0;
    out[42] = r->in_nak_requested ? 1 : 0;
    out[43] = r->out_nak_requested ? 1 : 0;
    put_u64(out + 48, ipod_classic_usb_setup_epoch(srv->controller));
    put_u64(out + 56, ipod_classic_usb_in_arm_epoch(srv->controller));
    srv->stats.ep0_queries++;
    return server_reply_ok(srv, IPOD_HOST_OP_QUERY_EP0, out, sizeof(out));
}

static int server_op_stats(IPodHostServer *srv, const uint8_t *req,
                           uint32_t req_len)
{
    uint8_t out[8 * 16];
    ipod_host_xfer_stats_t xs;
    uint32_t off = 0;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_STATS,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    memset(&xs, 0, sizeof(xs));
    ipod_host_xfer_get_stats(srv->xfer, &xs);
    memset(out, 0, sizeof(out));
    put_u64(out + off, xs.submits); off += 8;
    put_u64(out + off, xs.stage_in_submits); off += 8;
    put_u64(out + off, xs.notifications); off += 8;
    put_u64(out + off, xs.terminals); off += 8;
    put_u64(out + off, xs.cancels); off += 8;
    put_u64(out + off, xs.naks); off += 8;
    put_u64(out + off, xs.observations); off += 8;
    put_u64(out + off, xs.observation_refusals); off += 8;
    put_u64(out + off, xs.rejects); off += 8;
    put_u64(out + off, xs.bind_closes); off += 8;
    put_u64(out + off, xs.fences); off += 8;
    put_u64(out + off, xs.packets_freed); off += 8;
    put_u64(out + off, xs.post_fence_notifications); off += 8;
    put_u64(out + off, xs.control_closes); off += 8;
    put_u64(out + off, xs.destroy_refused_no_fence); off += 8;
    put_u64(out + off, xs.unregister_rc); off += 8;
    return server_reply_ok(srv, IPOD_HOST_OP_STATS, out, off);
}

/*
 * Server-level statistics, separate from the owner's own counters above. A
 * client needs both to tell "the transport saw nothing" from "the owner had
 * nothing": the second one is the honest no-observation answer.
 */
static int server_op_transport_stats(IPodHostServer *srv, const uint8_t *req,
                                     uint32_t req_len)
{
    uint8_t out[8 * 12];
    uint32_t off = 0;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_TRANSPORT_STATS,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    memset(out, 0, sizeof(out));
    put_u64(out + off, srv->stats.client_connections); off += 8;
    put_u64(out + off, srv->stats.frames_in); off += 8;
    put_u64(out + off, srv->stats.frames_out); off += 8;
    put_u64(out + off, srv->stats.protocol_errors); off += 8;
    put_u64(out + off, srv->stats.synthetic_refusals); off += 8;
    put_u64(out + off, srv->stats.native_setup_reads); off += 8;
    put_u64(out + off, srv->stats.native_setup_absent); off += 8;
    put_u64(out + off, srv->stats.ep0_queries); off += 8;
    put_u64(out + off, srv->stats.mmio_writes); off += 8;
    put_u64(out + off, (uint64_t)srv->mode); off += 8;
    put_u64(out + off, srv->stats.native_watchdog_fired ? 1 : 0); off += 8;
    put_u64(out + off, srv->stats.lifecycle_closed ? 1 : 0); off += 8;
    return server_reply_ok(srv, IPOD_HOST_OP_TRANSPORT_STATS, out, off);
}

static int server_op_close(IPodHostServer *srv, const uint8_t *req,
                           uint32_t req_len)
{
    uint8_t out[8];
    int rc;

    if (req_len != 0) {
        return server_reply_refused(srv, IPOD_HOST_OP_CLOSE,
                                    IPOD_HOST_REASON_MALFORMED);
    }
    if (!srv->session_open) {
        return server_reply_refused(srv, IPOD_HOST_OP_CLOSE,
                                    IPOD_HOST_REASON_NO_SESSION);
    }
    server_pulse_cancel(srv);
    if (srv->host_bus_token) {
        ipod_classic_usb_host_bus_release(srv->controller, srv,
            srv->host_bus_token, "protocol-close-abort");
        srv->host_bus_token = 0;
    }
    /* Drain the real owner first so no packet outlives the session. */
    rc = ipod_host_xfer_cancel(srv->xfer);
    srv->have_control = false;
    srv->control_handle = 0;
    srv->session_open = false;
    srv->session_closed = true;
    srv->stats.session_closes++;
    memset(out, 0, sizeof(out));
    put_u64(out + 0, srv->session_serial);
    if (rc != 0) {
        return server_reply_negative(srv, IPOD_HOST_OP_CLOSE, rc,
                                     IPOD_HOST_REASON_BACKEND);
    }
    /*
     * The session is over but the CONNECTION is not: the client may still ask
     * questions that must be refused honestly (a reopen, a submit after close).
     * The bounded run therefore ends on client EOF or on the deadline, never
     * because a CLOSE was merely accepted.
     */
    return server_reply_ok(srv, IPOD_HOST_OP_CLOSE, out, sizeof(out));
}

static int server_op_link_snapshot(IPodHostServer *srv,uint32_t length)
{
    uint32_t facts[8];uint64_t now;uint8_t reply[64]={0};unsigned i;
    if (srv->mode!=IPOD_HOST_MODE_NATIVE) return server_reply_unsupported(srv,0x37,IPOD_HOST_REASON_WRONG_MODE);
    if (length) return server_reply_refused(srv,0x37,IPOD_HOST_REASON_MALFORMED);
    ipod_classic_usb_link_snapshot(srv->controller,facts,&now);
    for(i=0;i<8;i++) { put_u32(reply+i*4,facts[i]); }
    put_u64(reply+32,now);put_u64(reply+40,srv->controller->host_bus_policy.asserted_ns);
    put_u64(reply+48,srv->controller->host_bus_policy.released_ns);put_u64(reply+56,srv->controller->host_bus_policy.last_ns);
    return server_reply_ok(srv,0x37,reply,sizeof(reply));
}

/* Dispatch one already-framed request. Returns -1 to end the connection. */

void ipod_classic_usb_rx_journal(IPodClassicUSBState *,const char *,uint32_t,uint32_t,const uint8_t *);
/* 0x30 SETUP(serial,token,setup8); 0x31 POLL_IN(serial,token,id,epoch);
 * 0x32 ACK_IN(serial,token,id,epoch,arm_nonce); 0x33 STATUS_OUT(same32).
 * Replies: result:i32,length:u32,id:u64,epoch:u64,nonce:u64,done:u32,bytes.
 * A completed native control does NOT complete an old bridge USB/IP URB.
 */
static int server_op_peer(IPodHostServer *srv,uint16_t op,const uint8_t *req,
                           uint32_t length)
{
    uint8_t out[36+64]={0};
    uint32_t expected=op==0x30?24:op==0x32?40:32;
    int result;
    uint32_t bytes=0;
    if (srv->mode!=IPOD_HOST_MODE_NATIVE) return server_reply_unsupported(srv,op,IPOD_HOST_REASON_WRONG_MODE);
    if (length!=expected) return server_reply_refused(srv,op,IPOD_HOST_REASON_MALFORMED);
    if (!srv->session_open || srv->session_closed || !srv->host_bus_token)
        return server_reply_refused(srv,op,IPOD_HOST_REASON_NO_SESSION);
    if (get_u64(req)!=srv->session_serial || get_u64(req+8)!=srv->host_bus_token)
        return server_reply_refused(srv,op,IPOD_HOST_REASON_IDENTITY);
    if (!server_bus_tokens_ready(srv)) return server_reply_refused(srv,op,IPOD_HOST_REASON_RESET_RECOVERY);
    if (srv->have_control) return server_reply_refused(srv,op,IPOD_HOST_REASON_SESSION_BUSY);
    if (op==0x30) {
        if (srv->peer.open || srv->next_peer_id==UINT64_MAX)
            return server_reply_refused(srv,op,IPOD_HOST_REASON_SESSION_BUSY);
        ipod_classic_usb_rx_journal(srv->controller,"before_SETUP",op,0,req+16);
        result=ipod_classic_usb_peer_begin(srv->controller,&srv->peer,req+16,srv->next_peer_id+1);
        ipod_classic_usb_rx_journal(srv->controller,"after_SETUP",op,result,req+16);
        if (result==IPOD_PEER_OK) {srv->next_peer_id++;srv->peer_mode=true;}
    } else {
        if (!srv->peer_mode || !srv->peer.open || get_u64(req+16)!=srv->peer.id ||
            get_u64(req+24)!=srv->peer.epoch)
            return server_reply_refused(srv,op,IPOD_HOST_REASON_UNKNOWN_HANDLE);
        if (op==0x31) result=ipod_classic_usb_peer_poll(srv->controller,&srv->peer);
        else if (op==0x32) result=ipod_classic_usb_peer_ack(srv->controller,&srv->peer,get_u64(req+32));
        else result=ipod_classic_usb_peer_status_out(srv->controller,&srv->peer);
        if (op==0x31 && result==IPOD_PEER_OK) bytes=srv->peer.actual;
    }
    if(op==0x32 || op==0x33)ipod_classic_usb_rx_journal(srv->controller,"after_ACK_or_STATUS",op,result,srv->peer.setup);
    put_u32(out,(uint32_t)result);put_u32(out+4,bytes);
    put_u64(out+8,srv->peer.id);put_u64(out+16,srv->peer.epoch);
    put_u64(out+24,srv->peer.packet_nonce);put_u32(out+32,srv->peer.done);
    if (bytes) memcpy(out+36,srv->peer.data,bytes);
    fprintf(stderr,"USBPEER op=%02x result=%d id=%"PRIu64" epoch=%"PRIu64" arm=%"PRIu64" bytes=%u done=%d vns=%"PRId64"\n",
        op,result,srv->peer.id,srv->peer.epoch,srv->peer.packet_nonce,bytes,
        srv->peer.done,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return server_send(srv,op,result<0?IPOD_HOST_ST_NEGATIVE:IPOD_HOST_ST_OK,out,36+bytes);
}


/* op34 OUT: serial/token/ep/length/sequence/bytes; op35 IN: serial/token/ep/0;
 * op36 ACK: serial/token/ep/0/nonce. Replies result/length/nonce/bytes (LE).
 * OUT accepted sequences are consumed before reply; lost replies are terminal,
 * not permission to replay a write. IN polling pins bytes; ACK alone consumes.
 */
#include "native_pulse.inc"
#include "native_batch.inc"
static int server_op_bulk(IPodHostServer *srv,uint16_t op,const uint8_t *req,uint32_t length)
{
    uint8_t reply[16+512]={0};unsigned ep,n=0;uint64_t nonce=0,seq;
    int rc;IPodBulkPacket *packet=NULL;
    if (srv->mode!=IPOD_HOST_MODE_NATIVE) return server_reply_unsupported(srv,op,IPOD_HOST_REASON_WRONG_MODE);
    if (length<(op==0x35?24:32)) return server_reply_refused(srv,op,IPOD_HOST_REASON_MALFORMED);
    ep=get_u32(req+16);
    if (!ep || ep>=IPOD_DWC_EP_SLOTS || (op!=0x34 && get_u32(req+20)))
        return server_reply_refused(srv,op,IPOD_HOST_REASON_MALFORMED);
    if (op==0x34) {
        n=get_u32(req+20);
        if (n>512 || length!=32+n) return server_reply_refused(srv,op,IPOD_HOST_REASON_MALFORMED);
    } else if (length!=(op==0x35?24:32)) return server_reply_refused(srv,op,IPOD_HOST_REASON_MALFORMED);
    if (!srv->session_open || srv->session_closed || !srv->host_bus_token)
        return server_reply_refused(srv,op,IPOD_HOST_REASON_NO_SESSION);
    if (get_u64(req)!=srv->session_serial || get_u64(req+8)!=srv->host_bus_token)
        return server_reply_refused(srv,op,IPOD_HOST_REASON_IDENTITY);
    if (!server_bus_tokens_ready(srv)) return server_reply_refused(srv,op,IPOD_HOST_REASON_RESET_RECOVERY);
    if (srv->have_control || srv->peer.open) return server_reply_refused(srv,op,IPOD_HOST_REASON_SESSION_BUSY);
    if (op==0x34) {
        seq=get_u64(req+24);
        if (srv->bulk_out_sequence==UINT64_MAX || seq!=srv->bulk_out_sequence+1)
            return server_reply_refused(srv,op,IPOD_HOST_REASON_UNKNOWN_HANDLE);
        rc=ipod_classic_usb_bulk_out(srv->controller,ep,req+32,n);
        if (rc==IPOD_BULK_OK) {srv->bulk_out_sequence=seq;nonce=seq;}
        else n=0;
    } else if (op==0x35) {
        rc=ipod_classic_usb_bulk_in(srv->controller,ep,&packet);
        if (rc==IPOD_BULK_OK) {n=packet->length;nonce=packet->nonce;memcpy(reply+16,packet->bytes,n);}
    } else {
        nonce=get_u64(req+24);rc=ipod_classic_usb_bulk_ack(srv->controller,ep,nonce);
    }
    put_u32(reply,rc);put_u32(reply+4,n);put_u64(reply+8,nonce);
    if (rc!=IPOD_BULK_WAIT && (rc!=IPOD_BULK_OK || getenv("CLASSIC7_DIAGNOSTICS"))) fprintf(stderr,"USBBULK op=%02x ep=%u result=%d bytes=%u nonce=%"PRIu64" vns=%"PRId64"\n",op,ep,rc,n,nonce,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return server_send(srv,op,rc<0?IPOD_HOST_ST_NEGATIVE:IPOD_HOST_ST_OK,reply,16+(op==0x35?n:0));
}

static int server_op_endpoint_snapshot(IPodHostServer *srv,const uint8_t *req,uint32_t length)
{
    unsigned ep,in;uint8_t out[48]={0};IPodUSBRegisters *r;
    if(srv->mode!=IPOD_HOST_MODE_NATIVE) return server_reply_unsupported(srv,0x3a,IPOD_HOST_REASON_WRONG_MODE);
    if(length!=24 || !(ep=get_u32(req+16)) || ep>=IPOD_DWC_EP_SLOTS || (in=get_u32(req+20))>1)
        return server_reply_refused(srv,0x3a,IPOD_HOST_REASON_MALFORMED);
    if(!srv->session_open || srv->session_closed || !srv->host_bus_token)
        return server_reply_refused(srv,0x3a,IPOD_HOST_REASON_NO_SESSION);
    if(get_u64(req)!=srv->session_serial || get_u64(req+8)!=srv->host_bus_token)
        return server_reply_refused(srv,0x3a,IPOD_HOST_REASON_IDENTITY);
    r=&srv->controller->regs;
    put_u32(out,1);put_u32(out+4,ep);put_u32(out+8,in);
    put_u32(out+12,in?r->in_ctl[ep]:r->out_ctl[ep]);
    put_u32(out+16,in?r->in_tsiz[ep]:r->out_tsiz[ep]);
    put_u32(out+20,in?r->in_dma[ep]:r->out_dma[ep]);
    put_u32(out+24,in?r->in_int[ep]:r->out_int[ep]);put_u32(out+28,ipod_usb_status(r));
    put_u64(out+32,srv->controller->bulk.generation[in][ep]);put_u64(out+40,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return server_reply_ok(srv,0x3a,out,sizeof(out));
}

static int server_dispatch(IPodHostServer *srv, uint16_t op,
                           const uint8_t *req, uint32_t req_len)
{
    /* The native wire permits exactly one outstanding RPC. */
    if (srv->batch_pending) { return -1; }
    if (op==0x3b) return server_op_batch_out(srv,req,req_len);
    if (op==0x3a) return server_op_endpoint_snapshot(srv,req,req_len);
    if (op==0x38) return server_op_pulse(srv,req,req_len);
    if (op>=0x34 && op<=0x36) return server_op_bulk(srv,op,req,req_len);
    if (op==0x37) return server_op_link_snapshot(srv,req_len);
    if (op>=0x30 && op<=0x33) return server_op_peer(srv,op,req,req_len);
    if (srv->peer_mode && ((op>=3 && op<=9) || op==15))
        return server_reply_refused(srv,op,IPOD_HOST_REASON_WRONG_MODE);
    switch (op) {
    case IPOD_HOST_OP_BUS_RESET_ASSERT:
    case IPOD_HOST_OP_BUS_RESET_DEASSERT:
        return server_op_bus_reset(srv, op, req, req_len);
    case IPOD_HOST_OP_HELLO:
        return server_op_hello(srv, req, req_len);
    case IPOD_HOST_OP_OPEN:
        return server_op_open(srv, req, req_len);
    case IPOD_HOST_OP_SUBMIT_SETUP:
        return server_op_submit_setup(srv, req, req_len);
    case IPOD_HOST_OP_STAGE_IN:
        return server_op_stage_in(srv, req, req_len);
    case IPOD_HOST_OP_OBSERVE:
        return server_op_observe(srv, req, req_len);
    case IPOD_HOST_OP_QUERY_CONTROL:
        return server_op_query_control(srv, req, req_len);
    case IPOD_HOST_OP_QUERY_PACKET:
        return server_op_query_packet(srv, req, req_len);
    case IPOD_HOST_OP_PUMP:
        return server_op_pump(srv, req, req_len);
    case IPOD_HOST_OP_CANCEL:
        return server_op_cancel(srv, req, req_len);
    case IPOD_HOST_OP_ARM_SYNTHETIC_IN:
        return server_op_arm_synthetic_in(srv, req, req_len);
    case IPOD_HOST_OP_ARM_SETUP_RECEIVER:
        return server_op_arm_setup_receiver(srv, req, req_len);
    case IPOD_HOST_OP_READ_SETUP_DMA:
        return server_op_read_setup_dma(srv, req, req_len);
    case IPOD_HOST_OP_STATS:
        return server_op_stats(srv, req, req_len);
    case IPOD_HOST_OP_CLOSE:
        return server_op_close(srv, req, req_len);
    case IPOD_HOST_OP_RETRY_SETUP:
        return server_op_retry_setup(srv, req, req_len);
    /*
     * Native observation ops. Each one refuses WRONG_MODE in synthetic mode, so
     * the two transports cannot be driven through each other's interface.
     */
    case IPOD_HOST_OP_NATIVE_INFO:
        return server_op_native_info(srv, req, req_len);
    case IPOD_HOST_OP_READ_NATIVE_SETUP:
        return server_op_read_native_setup(srv, req, req_len);
    case IPOD_HOST_OP_QUERY_EP0:
        return server_op_query_ep0(srv, req, req_len);
    case IPOD_HOST_OP_TRANSPORT_STATS:
        return server_op_transport_stats(srv, req, req_len);
    case IPOD_HOST_OP_SUBMIT_BULK_OUT:
        /* No bulk data path exists in this slice: honest refusal, no guess. */
        return server_reply_unsupported(srv, op, IPOD_HOST_REASON_UNKNOWN_OP);
    case IPOD_HOST_OP_REQUEST_STATUS_COMMIT:
        /* There is no status/commit producer. Never fabricate one. */
        return server_reply_unsupported(srv, op, IPOD_HOST_REASON_UNKNOWN_OP);
    default:
        srv->stats.refusals++;
        return server_send(srv, op, IPOD_HOST_ST_REFUSED, NULL, 0);
    }
}

/* ------------------------------------------------------------ client loop */

static void server_close_client(IPodHostServer *srv)
{
    server_batch_cancel(srv);
    if (srv->client_out_watch) {
        g_source_remove(srv->client_out_watch);
        srv->client_out_watch = 0;
    }
    if (srv->client_watch) {
        g_source_remove(srv->client_watch);
        srv->client_watch = 0;
    }
    srv->rx_have = 0;
    srv->rx_need = 0;
    srv->tx_len = 0;
    srv->tx_off = 0;
    if (srv->client) {
        qio_channel_shutdown(srv->client, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
        qio_channel_close(srv->client, NULL);
        object_unref(OBJECT(srv->client));
        srv->client = NULL;
    }
}

static gboolean server_client_writable(QIOChannel *ioc, GIOCondition cond,
                                       gpointer opaque)
{
    IPodHostServer *srv = opaque;
    int flushed;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        server_close_client(srv);
        server_request_stop(srv);

        return G_SOURCE_REMOVE;
    }
    flushed = server_tx_flush(srv);
    if (flushed < 0) {
        server_close_client(srv);
        server_request_stop(srv);

        return G_SOURCE_REMOVE;
    }
    if (flushed == 1) {
        srv->stats.frames_out++;
        if (srv->client_out_watch) {
            g_source_remove(srv->client_out_watch);
            srv->client_out_watch = 0;
        }
        if (srv->stop_requested) {
            /* The stop reply is fully on the wire: the bounded run is done. */
            server_request_stop(srv);
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/*
 * Read whatever is available into rx_buf and dispatch every complete frame.
 *
 * Framing is driven by rx_have/rx_need so a short read never loses bytes and
 * never blocks: rx_need is 16 until the header is complete, then 16 + length.
 */
static bool server_header_valid(const uint8_t *header)
{
    return get_u32(header) == IPOD_HOST_PROTO_MAGIC &&
           get_u16(header + 4) == IPOD_HOST_PROTO_VERSION &&
           get_u16(header + 8) == 0 && get_u16(header + 10) == 0;
}

static gboolean server_client_readable(QIOChannel *ioc, GIOCondition cond,
                                       gpointer opaque)
{
    IPodHostServer *srv = opaque;
    uint16_t op;
    uint32_t length;
    struct iovec iov;
    ssize_t rc;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        server_close_client(srv);
        server_request_stop(srv);

        return G_SOURCE_REMOVE;
    }

    for (;;) {
        if (srv->rx_need == 0) {
            srv->rx_need = IPOD_HOST_PROTO_HEADER_SIZE;
        }
        if (srv->rx_have < srv->rx_need) {
            iov.iov_base = srv->rx_buf + srv->rx_have;
            iov.iov_len = srv->rx_need - srv->rx_have;
            rc = qio_channel_readv_full(ioc, &iov, 1, NULL, NULL, 0, NULL);
            if (rc == QIO_CHANNEL_ERR_BLOCK) {
                return G_SOURCE_CONTINUE;
            }
            if (getenv("CLASSIC7_SERVICE_TRACE")) fprintf(stderr,"DESKTOP-RX rc=%zd have=%u need=%u\n",rc,srv->rx_have,srv->rx_need);
            if (rc <= 0) {
                /*
                 * The external client is gone. That is the normal end of a
                 * bounded run, not an error, so close the session honestly and
                 * stop instead of idling until the deadline.
                 */
                srv->client_gone = true;
                server_close_client(srv);
                server_request_stop(srv);

                return G_SOURCE_REMOVE;
            }
            srv->rx_have += (uint32_t)rc;
            if (srv->rx_have < srv->rx_need) {
                return G_SOURCE_CONTINUE;
            }
        }

        if (srv->rx_need == IPOD_HOST_PROTO_HEADER_SIZE) {
            if (!server_header_valid(srv->rx_buf)) {
                srv->stats.protocol_errors++;
                server_close_client(srv);
                server_request_stop(srv);

                return G_SOURCE_REMOVE;
            }
            length = get_u32(srv->rx_buf + 12);
            if (length > IPOD_HOST_PROTO_MAX_PAYLOAD) {
                srv->stats.oversize_frames++;
                srv->stats.protocol_errors++;
                server_close_client(srv);
                server_request_stop(srv);

                return G_SOURCE_REMOVE;
            }
            srv->rx_need = IPOD_HOST_PROTO_HEADER_SIZE + length;
            if (srv->rx_have < srv->rx_need) {
                continue;
            }
        }

        op = get_u16(srv->rx_buf + 6);
        length = get_u32(srv->rx_buf + 12);
        srv->stats.frames_in++;
        /*
         * Consume the frame BEFORE dispatching. A response that does not fit in
         * the socket leaves tx_len != 0 and we return early; if rx_have/rx_need
         * were still pointing at the consumed frame the next readable event
         * would dispatch that same frame a second time.
         */
        srv->rx_have = 0;
        srv->rx_need = IPOD_HOST_PROTO_HEADER_SIZE;
        if (server_dispatch(srv, op, srv->rx_buf + IPOD_HOST_PROTO_HEADER_SIZE,
                            length) < 0) {
            server_close_client(srv);
            server_request_stop(srv);

            return G_SOURCE_REMOVE;
        }

        /* A partially written response keeps the rest for the G_IO_OUT watch. */
        if (srv->tx_len != 0) {
            return G_SOURCE_CONTINUE;
        }
        if (srv->stop_requested) {
            /* In native mode the transport is already closed at this point. */
            return G_SOURCE_REMOVE;
        }
    }
}

static void server_accept(QIONetListener *listener, QIOChannelSocket *sioc,
                          gpointer opaque)
{
    IPodHostServer *srv = opaque;
    Error *err = NULL;

    if (srv->client) {
        /* Bounded: exactly one live client. Refuse extras by dropping them. */
        return;
    }
    srv->client = QIO_CHANNEL(sioc);
    object_ref(OBJECT(sioc));
    /* Small synchronous native transactions must not wait for TCP coalescing. */
    qio_channel_set_delay(srv->client, false);
    if (qio_channel_set_blocking(srv->client, false, &err) < 0) {
        error_free(err);
        server_close_client(srv);
        return;
    }
    if (getenv("CLASSIC7_SERVICE_TRACE")) fprintf(stderr,"DESKTOP-ACCEPT have=%u need=%u\n",srv->rx_have,srv->rx_need);
    srv->stats.client_connections++;
    srv->client_watch = qio_channel_add_watch(srv->client, G_IO_IN,
                                              server_client_readable, srv,
                                              NULL);
}

/* ------------------------------------------------------------------- API */

/*
 * End the current session without touching the listener. Used when a client
 * disappears: the transport stays usable, the session does not. In native mode
 * this is the honest answer to "the client went away" -- the firmware keeps
 * running, the owner is drained, and nothing pretends a transfer completed.
 */
static void server_end_session(IPodHostServer *srv)
{
    server_batch_cancel(srv);
    server_pulse_cancel(srv);
    if (srv->host_bus_token) {
        ipod_classic_usb_host_bus_release(srv->controller, srv,
            srv->host_bus_token, "transport-close-abort");
        srv->host_bus_token = 0;
    }
    if (srv->xfer) {
        (void)ipod_host_xfer_cancel(srv->xfer);
    }
    srv->have_control = false;
    srv->control_handle = 0;
    if (srv->session_open) {
        srv->session_open = false;
        srv->session_closed = true;
        srv->stats.session_closes++;
    }
}

/*
 * Native lifecycle teardown of the TRANSPORT. Exactly once, whatever the
 * trigger: client EOF, a protocol error, the lifetime watchdog, or the QEMU
 * exit notifier. Idempotent.
 *
 * The lifetime watchdog is deliberately NOT cancelled here. The declared total
 * bound belongs to the run, not to the transport: a client that connects and
 * disconnects early must not be able to leave the process with no remaining
 * deadline. Cancelling it here would do exactly that and the run would hang
 * until an outer wall clock killed it.
 */
void ipod_host_server_close(IPodHostServer *srv)
{
    if (!srv || srv->closing) {
        return;
    }
    srv->closing = true;
    srv->stop_requested = true;

    if (srv->attached) {
        qemu_remove_exit_notifier(&srv->exit_notifier);
        srv->attached = false;
    }

    server_end_session(srv);
    server_close_client(srv);

    if (srv->listener) {
        qio_net_listener_disconnect(srv->listener);
        object_unref(OBJECT(srv->listener));
        srv->listener = NULL;
    }
    if (srv->xfer) {
        /* Drain, unregister, fence, release remaining packet storage. */
        srv->stats.unregister_rc =
            (uint64_t)(int64_t)ipod_host_xfer_destroy(srv->xfer, NULL);
        srv->xfer = NULL;
    }
    srv->stats.lifecycle_closed = true;
}

static void server_request_stop(IPodHostServer *srv)
{
    if (srv->desktop_service && !srv->closing) {
        server_end_session(srv);
        memset(&srv->peer,0,sizeof(srv->peer));srv->peer_mode=false;
        srv->client_gone=false;srv->stop_requested=false;
        return; /* listener/owner remain; every new session gets a fresh token */
    }
    srv->stop_requested = true;
    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        ipod_host_server_close(srv);
    } else if (srv->loop) {
        g_main_loop_quit(srv->loop);
    }
}

static void server_watchdog_cb(void *opaque)
{
    IPodHostServer *srv = opaque;

    srv->stats.native_watchdog_fired = true;
    /*
     * Tear the transport down first, then ask QEMU to leave its own main loop.
     *
     * Closing the transport is not enough to end the run: the normal main loop
     * only exits from main_loop_should_exit(), which needs a shutdown request.
     * Without this call a no-guest smoke would sit in the loop until the outer
     * wall clock killed it, and a bounded native run would never terminate on
     * its own lifetime. This is the single place where the declared total
     * bound becomes an actual process exit.
     */
    ipod_host_server_close(srv);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

static void server_exit_notify(Notifier *notifier, void *data)
{
    IPodHostServer *srv = container_of(notifier, IPodHostServer, exit_notifier);

    ipod_host_server_close(srv);
}

IPodHostServer *ipod_host_server_new(USBDevice *bridge,
                                     struct IPodClassicUSBState *controller,
                                     uint8_t *ram, uint64_t ram_base,
                                     uint32_t ram_size, uint64_t identity,
                                     uint16_t port, uint32_t deadline_ms,
                                     Error **errp)
{
    IPodHostServer *srv;
    IPodHostXfer *xfer;

    if (!bridge || !controller || !ram || !ram_size) {
        error_setg(errp, "host server requires bridge, controller and RAM");
        return NULL;
    }
    if (ram_size < IPOD_HOST_RAM_SIZE) {
        error_setg(errp, "synthetic RAM window must be at least %u bytes",
                   (unsigned)IPOD_HOST_RAM_SIZE);
        return NULL;
    }
    if (!port) {
        error_setg(errp, "host server requires an explicit loopback port");
        return NULL;
    }
    if (!deadline_ms) {
        error_setg(errp, "host server requires a bounded deadline");
        return NULL;
    }

    xfer = ipod_host_xfer_create(bridge, controller, errp);
    if (!xfer) {
        return NULL;
    }

    srv = g_new0(IPodHostServer, 1);
    srv->mode = IPOD_HOST_MODE_SYNTHETIC;
    srv->bridge = bridge;
    srv->controller = controller;
    srv->xfer = xfer;
    srv->ram = ram;
    srv->ram_base = ram_base;
    srv->ram_size = ram_size;
    srv->identity = identity;
    srv->port = port;
    srv->deadline_ms = deadline_ms;
    return srv;
}

/*
 * Native constructor. There is deliberately no RAM argument: native mode has no
 * synthetic window, so it cannot be handed one by mistake. There is no loop
 * either; lifetime_ms arms a one-shot watchdog.
 */
IPodHostServer *ipod_host_server_new_native(USBDevice *bridge,
                                            struct IPodClassicUSBState *controller,
                                            uint64_t identity, uint16_t port,
                                            uint32_t lifetime_ms,
                                            Error **errp)
{
    IPodHostServer *srv;
    IPodHostXfer *xfer;

    if (!bridge || !controller) {
        error_setg(errp, "native host server requires the machine bridge and "
                          "the machine controller");
        return NULL;
    }
    if (!port) {
        error_setg(errp, "native host server requires an explicit loopback port");
        return NULL;
    }
    if (!lifetime_ms) {
        error_setg(errp, "native host server requires a bounded lifetime");
        return NULL;
    }

    xfer = ipod_host_xfer_create(bridge, controller, errp);
    if (!xfer) {
        return NULL;
    }

    srv = g_new0(IPodHostServer, 1);
    srv->mode = IPOD_HOST_MODE_NATIVE;
    srv->bridge = bridge;
    srv->controller = controller;
    srv->xfer = xfer;
    srv->identity = identity;
    srv->port = port;
    srv->lifetime_ms = lifetime_ms;
    srv->desktop_service = getenv("IPOD_USB_DESKTOP_SERVICE") && !strcmp(getenv("IPOD_USB_DESKTOP_SERVICE"),"1");
    srv->exit_notifier.notify = server_exit_notify;
    return srv;
}

int ipod_host_server_attach(IPodHostServer *srv, Error **errp)
{
    if (!srv) {
        return URA_ERR_ARG;
    }
    if (srv->mode != IPOD_HOST_MODE_NATIVE) {
        error_setg(errp, "attach is native-only: the synthetic server owns its "
                          "own bounded loop");
        return URA_ERR_STATE;
    }
    if (srv->attached) {
        error_setg(errp, "native host server is already attached");
        return URA_ERR_STATE;
    }
    /*
     * One-shot watchdog, not a poll: it fires once and closes the transport.
     * The normal QEMU main loop keeps running either way.
     */
    if (!srv->desktop_service) {
    srv->watchdog = timer_new_ms(QEMU_CLOCK_REALTIME, server_watchdog_cb, srv);
    timer_mod(srv->watchdog,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + srv->lifetime_ms);
    }
    qemu_add_exit_notifier(&srv->exit_notifier);
    srv->attached = true;
    return URA_OK;
}

int ipod_host_server_start(IPodHostServer *srv, Error **errp)
{
    /*
     * Heap-allocated on purpose: qapi_free_SocketAddress() runs the dealloc
     * visitor, whose end_struct() g_free()s the struct itself, so handing it the
     * address of a stack object would free stack memory and corrupt the heap.
     */
    SocketAddress *addr = g_new0(SocketAddress, 1);
    int rc;

    addr->type = SOCKET_ADDRESS_TYPE_INET;
    addr->u.inet.host = g_strdup("127.0.0.1");
    addr->u.inet.port = g_strdup_printf("%u", (unsigned)srv->port);
    addr->u.inet.has_numeric = true;
    addr->u.inet.numeric = true;

    srv->listener = qio_net_listener_new();
    qio_net_listener_set_name(srv->listener, "ipod-host-external");
    rc = qio_net_listener_open_sync(srv->listener, addr, SERVER_MAX_CLIENTS,
                                    errp);
    qapi_free_SocketAddress(addr);
    if (rc < 0) {
        return -1;
    }
    qio_net_listener_set_client_func(srv->listener, server_accept, srv, NULL);
    return 0;
}

int ipod_host_server_run(IPodHostServer *srv)
{
    guint timeout_id;

    if (!srv || !srv->listener) {
        return URA_ERR_ARG;
    }
    if (srv->mode != IPOD_HOST_MODE_SYNTHETIC) {
        /*
         * Native mode must never own a nested loop: the normal QEMU main loop
         * dispatches the same sources. Refuse instead of quietly blocking the
         * machine thread for the whole lifetime window.
         */
        return URA_ERR_STATE;
    }

    /*
     * Run on the DEFAULT GMainContext: qio_net_listener_set_client_func() and
     * qio_channel_add_watch() both attach their sources there, so a private
     * context would silently never dispatch them.
     */
    srv->loop = g_main_loop_new(NULL, FALSE);
    srv->deadline = g_get_monotonic_time() + (int64_t)srv->deadline_ms * 1000;

    /* Hard wall-clock bound for the whole listen/accept/serve phase. */
    timeout_id = g_timeout_add((guint)srv->deadline_ms,
                               (GSourceFunc)g_main_loop_quit, srv->loop);

    g_main_loop_run(srv->loop);

    if (g_main_loop_is_running(srv->loop)) {
        g_main_loop_quit(srv->loop);
    }
    g_source_remove(timeout_id);

    if (!srv->stop_requested && !srv->client_gone) {
        srv->stats.deadline_hit = true;
    }
    srv->stats.loop_exited = true;

    g_main_loop_unref(srv->loop);
    srv->loop = NULL;
    return 0;
}

int ipod_host_server_destroy(IPodHostServer *srv,
                             IPodHostServerStats *out_stats)
{
    int rc = URA_OK;

    if (!srv) {
        return URA_ERR_ARG;
    }
    if (srv->mode == IPOD_HOST_MODE_NATIVE) {
        /* One teardown path: close() already did the drain + fence + unref. */
        ipod_host_server_close(srv);
        /*
         * The watchdog deliberately outlives close(): the declared bound
         * belongs to the run, not to the transport. It is released here, at
         * the single place where the server itself is freed.
         */
        if (srv->watchdog) {
            timer_del(srv->watchdog);
            timer_free(srv->watchdog);
            srv->watchdog = NULL;
        }
        rc = (int)(int64_t)srv->stats.unregister_rc;
        if (out_stats) {
            *out_stats = srv->stats;
        }
        if(srv->pulse_timer) timer_free(srv->pulse_timer);
        if(srv->batch_timer) timer_free(srv->batch_timer);
        if(srv->batch_deadline) timer_free(srv->batch_deadline);
        g_free(srv);
        return rc;
    }
    server_close_client(srv);
    if (srv->listener) {
        qio_net_listener_disconnect(srv->listener);
        object_unref(OBJECT(srv->listener));
        srv->listener = NULL;
    }
    if (srv->loop) {
        g_main_loop_unref(srv->loop);
        srv->loop = NULL;
    }
    if (srv->xfer) {
        /* Drain, unregister, fence, release remaining storage. */
        rc = ipod_host_xfer_destroy(srv->xfer, NULL);
        srv->xfer = NULL;
    }
    if (out_stats) {
        *out_stats = srv->stats;
    }
    if(srv->pulse_timer) timer_free(srv->pulse_timer);
    if(srv->batch_timer) timer_free(srv->batch_timer);
    if(srv->batch_deadline) timer_free(srv->batch_deadline);
    g_free(srv);
    return rc;
}

void ipod_host_server_get_stats(const IPodHostServer *srv,
                                IPodHostServerStats *out)
{
    if (srv && out) {
        *out = srv->stats;
    }
}

bool ipod_host_server_session_open(const IPodHostServer *srv)
{
    return srv && srv->session_open;
}

bool ipod_host_server_session_closed(const IPodHostServer *srv)
{
    return srv && srv->session_closed;
}

ipod_host_mode_t ipod_host_server_mode(const IPodHostServer *srv)
{
    return srv ? srv->mode : IPOD_HOST_MODE_SYNTHETIC;
}
