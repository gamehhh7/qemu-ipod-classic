/*
 * ipod-host-server.h - opt-in, loopback-only external host transport for the
 * iPod Classic USB device core.
 *
 * TWO CONSTRUCTORS, TWO STRUCTURALLY DIFFERENT MODES
 *
 *   ipod_host_server_new()        IPOD_HOST_MODE_SYNTHETIC  (task 17 fixture)
 *     Requires a synthetic RAM window and runs its own bounded GMainLoop. The
 *     fixture's firmware stand-in may program model EP0 registers and declare
 *     synthetic IN source bytes. This is the only mode that can.
 *
 *   ipod_host_server_new_native() IPOD_HOST_MODE_NATIVE     (task 18 candidate)
 *     Takes NO synthetic RAM, NO deadline loop and NO firmware stand-in. It
 *     attaches its QIO sources to the DEFAULT GMainContext, which the NORMAL
 *     qemu-system-arm main loop dispatches, so there is no nested loop and no
 *     change to guest execution. It drives the SAME controller object the
 *     iPod-Classic machine created; it never creates a second one.
 *
 *   In native mode every synthetic-only operation (ARM_SETUP_RECEIVER,
 *   ARM_SYNTHETIC_IN, READ_SETUP_DMA) is answered UNSUPPORTED +
 *   IPOD_HOST_REASON_SYNTHETIC_ONLY and touches nothing. There is no MMIO
 *   write path and no arbitrary memory read/write op in either mode.
 *
 * WHAT THIS IS
 *       external client
 *         -> ipod-host-server.c  (framing, session, identity, bounds)
 *           -> ipod_host_xfer_*  (task 16 owner: one URA request per control
 *                                 transfer, stable req handle, transport cookie)
 *             -> ipod-raw-packet-bridge (mandatory ingress wrapper)
 *               -> ura_adapter        (the ONE ledger)
 *                 -> ipod_classic_usb_setup_event / in_begin
 *                   -> IPodClassicUSBState (the real controller receiver)
 *
 *   There is no bypass: the server never calls setup_event() or in_begin()
 *   directly and never writes USBPacket internals, guest state, PC, IRQ or RAM.
 *
 * WHAT THIS IS NOT
 *   Not a USB/IP server. Not an HCD. Not a descriptor or enumeration provider.
 *   There is no status/commit producer, so there is no ACK, no XFRC, no IRQ, no
 *   URB completion and no Windows/iTunes support anywhere in this path. The
 *   protocol has no op that can report one.
 *
 * THREADING
 *   Everything runs on the QEMU main thread with the BQL held. The listener
 *   never spawns a thread. In synthetic mode the fixture runs the loop; in
 *   native mode QEMU's own main loop dispatches the sources.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef IPOD_HOST_SERVER_H
#define IPOD_HOST_SERVER_H

#include "qapi/error.h"
#include "qemu/typedefs.h"
#include "hw/usb.h"
#include "hw/usb/ipod-host-protocol.h"

struct IPodClassicUSBState;
typedef struct IPodHostServer IPodHostServer;

/* Server-side counters. These describe the TRANSPORT, never USB semantics. */
typedef struct {
    uint64_t client_connections;   /* bounded: at most 1 live client */
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t protocol_errors;      /* framing/version violations */
    uint64_t refusals;
    uint64_t unsupported;
    uint64_t negatives;
    uint64_t session_opens;
    uint64_t session_closes;
    uint64_t arm_setup_calls;      /* synthetic mode only, always 0 in native */
    uint64_t arm_in_calls;         /* synthetic mode only, always 0 in native */
    uint64_t setup_dma_reads;      /* synthetic mode only, always 0 in native */
    uint64_t setup_submits;
    uint64_t stage_in_submits;
    uint64_t observations;
    uint64_t oversize_frames;
    /* Native mode accounting. */
    uint64_t synthetic_refusals;   /* synthetic-only op asked of native mode */
    uint64_t native_setup_reads;   /* READ_NATIVE_SETUP that found a window */
    uint64_t native_setup_absent;  /* READ_NATIVE_SETUP with no armed window */
    uint64_t ep0_queries;
    uint64_t mmio_writes;          /* must stay 0 in native mode */
    uint64_t unregister_rc;        /* owner fence result, recorded at teardown */
    bool deadline_hit;
    bool loop_exited;
    bool native_watchdog_fired;
    bool lifecycle_closed;
} IPodHostServerStats;

/*
 * Create the SYNTHETIC server (task 17 fixture). Nothing is listening until
 * ipod_host_server_start().
 *
 * bridge      : the realized ipod-raw-packet-bridge (the owner registers on it)
 * controller  : the SAME controller the bridge was configured with; passed
 *               explicitly so every reused task-16 source stays byte-identical
 * ram         : host pointer of the fixture's declared synthetic RAM window
 * ram_base    : guest-physical base of that window
 * ram_size    : its size in bytes (must be >= IPOD_HOST_RAM_SIZE)
 * identity    : controller identity the client must present to OPEN
 * port        : loopback TCP port, 1..65535 (0 is refused: an ephemeral port
 *               could not be reported to the client ahead of time)
 * deadline_ms : hard wall-clock bound for the whole listen/accept/serve phase
 */
IPodHostServer *ipod_host_server_new(USBDevice *bridge,
                                     struct IPodClassicUSBState *controller,
                                     uint8_t *ram, uint64_t ram_base,
                                     uint32_t ram_size, uint64_t identity,
                                     uint16_t port, uint32_t deadline_ms,
                                     Error **errp);

/*
 * Create the NATIVE server (task 18 candidate).
 *
 * There is deliberately no `ram` and no `ram_base` argument: native mode has no
 * synthetic RAM by construction, so the API cannot be handed one by mistake.
 * There is no deadline loop either; `lifetime_ms` arms a one-shot QEMUTimer
 * watchdog that closes the listener and tears the owner down exactly once.
 *
 * The caller must already own a realized bridge configured against the real
 * machine controller. Nothing here creates, replaces or re-parents a controller.
 */
IPodHostServer *ipod_host_server_new_native(USBDevice *bridge,
                                            struct IPodClassicUSBState *controller,
                                            uint64_t identity, uint16_t port,
                                            uint32_t lifetime_ms,
                                            Error **errp);

/* Bind 127.0.0.1:port and start listening. Fails if the port is taken. */
int ipod_host_server_start(IPodHostServer *srv, Error **errp);

/*
 * Attach the NATIVE server to the DEFAULT GMainContext and arm the one-shot
 * lifetime watchdog. It does NOT run a loop: the normal QEMU main loop
 * dispatches these sources, with the BQL held. Refused in synthetic mode.
 */
int ipod_host_server_attach(IPodHostServer *srv, Error **errp);

/* Run the bounded fixture loop. Synthetic mode only; refused in native mode. */
int ipod_host_server_run(IPodHostServer *srv);

/*
 * Tear down the listener, close any client, destroy the owner (drain +
 * unregister + fence) and free the server. Returns the owner unregister rc.
 * Idempotent in the sense that a second call on the same pointer is invalid,
 * so the native lifecycle calls it from exactly one place.
 */
int ipod_host_server_destroy(IPodHostServer *srv,
                             IPodHostServerStats *out_stats);

/*
 * Native lifecycle: tear the transport down now (close client, disconnect the
 * listener, destroy the owner). Safe to call more than once; the second call is
 * a no-op. Called by the watchdog and by the QEMU exit notifier.
 */
void ipod_host_server_close(IPodHostServer *srv);

/* Diagnostics for the harness/log. Safe to call at any time. */
void ipod_host_server_get_stats(const IPodHostServer *srv,
                                IPodHostServerStats *out);
bool ipod_host_server_session_open(const IPodHostServer *srv);
bool ipod_host_server_session_closed(const IPodHostServer *srv);
ipod_host_mode_t ipod_host_server_mode(const IPodHostServer *srv);

#endif /* IPOD_HOST_SERVER_H */
