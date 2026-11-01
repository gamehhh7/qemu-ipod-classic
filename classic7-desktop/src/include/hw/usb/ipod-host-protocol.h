/*
 * ipod-host-protocol.h - framed wire protocol for the opt-in external host
 * transport of the iPod Classic USB device core.
 *
 * TWO TRANSPORT MODES, STRUCTURALLY SEPARATED
 *
 *   IPOD_HOST_MODE_SYNTHETIC (task 17 fixture)
 *     Runs inside a bounded fixture process that owns a 4 KiB synthetic RAM
 *     window and a firmware stand-in. The stand-in is allowed to program the
 *     model's EP0 registers and to declare synthetic IN source bytes. That is
 *     test scaffolding and exists only there.
 *
 *   IPOD_HOST_MODE_NATIVE (task 18 candidate)
 *     Runs inside the NORMAL qemu-system-arm main loop, attached to the
 *     controller the iPod-Classic machine already created. There is no
 *     synthetic RAM, no firmware stand-in, and no register programming.
 *     Version2 accepts qualified external reset edges through the controller;
 *     reset IRQ/pending is derived by its existing partial model. The ARM_SETUP_RECEIVER and ARM_SYNTHETIC_IN
 *     operations are refused structurally (UNSUPPORTED +
 *     IPOD_HOST_REASON_SYNTHETIC_ONLY) before anything can be touched, and
 *     READ_SETUP_DMA is replaced by READ_NATIVE_SETUP, which only reads back
 *     the 8-byte window the FIRMWARE itself selected in DOEPDMA0.
 *
 * THREE-LAYER SEPARATION (unchanged in meaning)
 *
 *   SYNTHETIC      Fixture process, synthetic RAM, firmware stand-in MMIO.
 *                  Never present in native mode.
 *
 *   STAGED BYTES   OBSERVE returns only bytes the controller staged through
 *                  its own in-stage DMA read. Always observation_only = true,
 *                  with the stamp provenance captured when the stage was made.
 *                  Data visibility, never a commit and never a completion.
 *
 *   REAL COMPLETION (packet/status completion, distinct from reset input)
 *                  Does not exist in either mode. There is no status/commit
 *                  producer, no ACK, no XFRC or packet-completion IRQ, and no
 *                  packet URB completion/enumeration. Reset input can derive
 *                  USBRST/IRQ in the controller; no packet op reports completion:
 *                  REQUEST_STATUS_COMMIT is answered UNSUPPORTED by design.
 *
 * WHAT A SUCCESSFUL RESPONSE MEANS
 *   A status of OK means "this QEMU-side host command was accepted and executed
 *   against the real owner/bridge/controller objects". It NEVER means that a USB
 *   token was ACKed, that a URB completed, or that a device enumerated.
 *
 * WIRE FORMAT
 *   Every message is a 16-byte little-endian header followed by `length`
 *   payload bytes. `length` is capped by IPOD_HOST_PROTO_MAX_PAYLOAD. Flags
 *   must be zero. Version must match. Anything else is a protocol error and
 *   closes the connection without touching the guest side.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef IPOD_HOST_PROTOCOL_H
#define IPOD_HOST_PROTOCOL_H

/*
 * Protocol identity. A mismatch closes the client before dispatch.
 * Version 2 extends native OPEN to32 bytes (serial, identity, reset token, min ns); the
 * framing stays 16 bytes; v2 intentionally rejects v1 clients. Mode uses the
 * HELLO capability word plus NATIVE_INFO; v2 isolates reset-event clients.
 */
#define IPOD_HOST_PROTO_MAGIC   0x58445049u /* "IPDX" little endian */
#define IPOD_HOST_PROTO_VERSION 2u

#define IPOD_HOST_PROTO_HEADER_SIZE   16u
#define IPOD_HOST_PROTO_MAX_PAYLOAD   (16384u + 32u)
#define IPOD_HOST_PROTO_MAX_FRAME \
    (IPOD_HOST_PROTO_HEADER_SIZE + IPOD_HOST_PROTO_MAX_PAYLOAD)

/* Which transport mode this server instance runs in. */
typedef enum {
    /* Task 17 fixture: synthetic RAM + firmware stand-in, own GMainLoop. */
    IPOD_HOST_MODE_SYNTHETIC = 0,
    /* Task 18 candidate: normal QEMU main loop, real machine controller. */
    IPOD_HOST_MODE_NATIVE = 1,
} ipod_host_mode_t;

/* Response status classes. */
typedef enum {
    /* Command executed against the real objects. NOT a USB ACK or completion. */
    IPOD_HOST_ST_OK = 0,
    /* Malformed / oversize / unknown / wrong-state request. No guest mutation. */
    IPOD_HOST_ST_REFUSED = 1,
    /* Understood, but this mode/slice has no producer for it. */
    IPOD_HOST_ST_UNSUPPORTED = 2,
    /* The owner/controller returned a negative status for a real operation. */
    IPOD_HOST_ST_NEGATIVE = 3,
    /* Framing/version violation. The connection is closed. */
    IPOD_HOST_ST_PROTOCOL = 4,
} ipod_host_status_t;

/* Operations. */
typedef enum {
    /* 0x01 */ IPOD_HOST_OP_HELLO = 0x01,
    /* 0x02 */ IPOD_HOST_OP_OPEN = 0x02,
    /* 0x03 */ IPOD_HOST_OP_SUBMIT_SETUP = 0x03,
    /* 0x04 */ IPOD_HOST_OP_STAGE_IN = 0x04,
    /* 0x05 */ IPOD_HOST_OP_OBSERVE = 0x05,
    /* 0x06 */ IPOD_HOST_OP_QUERY_CONTROL = 0x06,
    /* 0x07 */ IPOD_HOST_OP_QUERY_PACKET = 0x07,
    /* 0x08 */ IPOD_HOST_OP_PUMP = 0x08,
    /* 0x09 */ IPOD_HOST_OP_CANCEL = 0x09,
    /*
     * 0x0a / 0x0b are SYNTHETIC-ONLY. In native mode they are answered
     * UNSUPPORTED + IPOD_HOST_REASON_SYNTHETIC_ONLY and touch nothing.
     */
    /* 0x0a */ IPOD_HOST_OP_ARM_SYNTHETIC_IN = 0x0a,
    /* 0x0b */ IPOD_HOST_OP_ARM_SETUP_RECEIVER = 0x0b,
    /*
     * 0x0c reads the DECLARED synthetic SETUP window (synthetic mode only).
     * Native mode refuses it the same way as 0x0a/0x0b and the client must use
     * READ_NATIVE_SETUP instead.
     */
    /* 0x0c */ IPOD_HOST_OP_READ_SETUP_DMA = 0x0c,
    /* 0x0d */ IPOD_HOST_OP_STATS = 0x0d,
    /* 0x0e */ IPOD_HOST_OP_CLOSE = 0x0e,
    /*
     * Re-submit the SAME 8 SETUP bytes for a request whose previous submission
     * retired as a NAK. Same URA seq (no rebind), new transport cookie. This is
     * the only retry shape the bridge contract allows; it is not a completion.
     */
    /* 0x0f */ IPOD_HOST_OP_RETRY_SETUP = 0x0f,
    /* 0x10 */ IPOD_HOST_OP_NATIVE_INFO = 0x10,
    /* 0x11 */ IPOD_HOST_OP_READ_NATIVE_SETUP = 0x11,
    /* 0x12 */ IPOD_HOST_OP_QUERY_EP0 = 0x12,
    /*
     * Transport-level counters (frames, refusals, synthetic refusals, native
     * read-backs, watchdog/teardown flags). Separate from 0x0d, which reports
     * the owner's own ledger, so "the transport saw nothing" and "the owner had
     * nothing" stay distinguishable.
     */
    /* 0x13 */ IPOD_HOST_OP_TRANSPORT_STATS = 0x13,
    /* Native-only v2, request = session_serial:u64, reset_token:u64, seq:u64.
     * Explicit emulated downstream-hub input edges; never a pulse generator. */
    IPOD_HOST_OP_BUS_RESET_ASSERT = 0x14,
    IPOD_HOST_OP_BUS_RESET_DEASSERT = 0x15,
    /* 0x20 */ IPOD_HOST_OP_SUBMIT_BULK_OUT = 0x20,      /* UNSUPPORTED */
    /* 0x21 */ IPOD_HOST_OP_REQUEST_STATUS_COMMIT = 0x21, /* UNSUPPORTED */
} ipod_host_op_t;

/* Capability / feature bits reported by HELLO. */
enum {
    /* Real status/commit producer present. Always 0 in both modes. */
    IPOD_HOST_CAP_STATUS_COMMIT = 1u << 0,
    /* Real HCD / host controller in the path. Always 0 in both modes. */
    IPOD_HOST_CAP_HCD = 1u << 1,
    /* Observations are observation-only and always labelled as such. */
    IPOD_HOST_CAP_OBSERVATION_ONLY = 1u << 2,
    /* Staged bytes come from a synthetic RAM window (synthetic mode only). */
    IPOD_HOST_CAP_SYNTHETIC_RAM = 1u << 3,
    /* A real owner is registered with the bridge before any packet. */
    IPOD_HOST_CAP_OWNER_REGISTERED = 1u << 4,
    /* Windows enumeration / iTunes support. Always 0 in both modes. */
    IPOD_HOST_CAP_ENUMERATION = 1u << 5,
    /*
     * The transport is attached to the controller the real iPod-Classic
     * machine created, inside the normal QEMU main loop. Set in native mode
     * only; it is the structural opposite of CAP_SYNTHETIC_RAM.
     */
    IPOD_HOST_CAP_NATIVE_CONTROLLER = 1u << 6,
    /* Native v2 reset edge ingress only; no HCD/chirp/ACK/enumeration claim. */
    IPOD_HOST_CAP_BUS_RESET_EVENTS = 1u << 7,
    /* Native OUT transport batching; every token still uses real DMA gates. */
    IPOD_HOST_CAP_BULK_OUT_BATCH = 1u << 9,
};

/* Outcome of SUBMIT_SETUP, derived from the owner/bridge/controller result. */
typedef enum {
    /* The real receiver accepted the 8 SETUP bytes and performed the single
     * DMA. This is an accepted SETUP only - not an ACK, not a URB completion. */
    IPOD_HOST_SETUP_ACCEPTED = 0,
    /* Submitted, no retirement yet: the packet is still in flight. */
    IPOD_HOST_SETUP_PENDING = 1,
    /* Retired with USB_RET_NAK: the receiver refused it; retry is allowed. */
    IPOD_HOST_SETUP_NAK = 2,
    /* Structurally rejected before anything reached the receiver. */
    IPOD_HOST_SETUP_REJECTED = 3,
    /* Retired with another negative status. */
    IPOD_HOST_SETUP_NEGATIVE = 4,
} ipod_host_setup_outcome_t;

/* Where observed bytes came from. */
typedef enum {
    IPOD_HOST_SOURCE_UNKNOWN = 0,
    /* The controller's in-stage DMA read of the declared synthetic RAM window. */
    IPOD_HOST_SOURCE_SYNTHETIC_RAM = 1,
    /*
     * The controller's in-stage DMA read of a guest-RAM address the real
     * firmware programmed. Native mode only.
     */
    IPOD_HOST_SOURCE_GUEST_RAM = 2,
} ipod_host_observe_source_t;

/* Refusal reason codes carried in a REFUSED payload as u16. */
typedef enum {
    IPOD_HOST_REASON_NONE = 0,
    IPOD_HOST_REASON_MALFORMED = 1,      /* payload length wrong for the op */
    IPOD_HOST_REASON_OVERSIZE = 2,       /* payload over the declared cap */
    IPOD_HOST_REASON_UNKNOWN_OP = 3,
    IPOD_HOST_REASON_NO_SESSION = 4,     /* OPEN was not done yet */
    IPOD_HOST_REASON_SESSION_BUSY = 5,   /* a session already exists */
    IPOD_HOST_REASON_SESSION_CLOSED = 6, /* CLOSE already ran */
    IPOD_HOST_REASON_IDENTITY = 7,       /* controller identity mismatch */
    IPOD_HOST_REASON_UNKNOWN_HANDLE = 8,
    IPOD_HOST_REASON_UNKNOWN_COOKIE = 9,
    IPOD_HOST_REASON_NOTHING_STAGED = 10,
    IPOD_HOST_REASON_BAD_SHAPE = 11,     /* refused token shape for this request */
    IPOD_HOST_REASON_RANGE = 12,         /* offset/length outside the RAM window */
    IPOD_HOST_REASON_BACKEND = 13,       /* owner returned a negative code */
    /* Synthetic-only operation asked of a native server. Nothing was touched. */
    IPOD_HOST_REASON_SYNTHETIC_ONLY = 14,
    /* The firmware has not armed the receiver, so there is nothing to read. */
    IPOD_HOST_REASON_NOT_ARMED = 15,
    /* A synthetic-mode-only question asked of a native server, or vice versa. */
    IPOD_HOST_REASON_WRONG_MODE = 16,
    IPOD_HOST_REASON_BUS_EVENT = 17,
    IPOD_HOST_REASON_RESET_RECOVERY = 18,
} ipod_host_reason_t;

/*
 * Fixed synthetic RAM layout. Used by IPOD_HOST_MODE_SYNTHETIC only; in native
 * mode HELLO reports ram_size = 0 and these offsets are never used.
 */
#define IPOD_HOST_RAM_SIZE          0x1000u
/* Where the controller writes the accepted 8 SETUP bytes (DMA target). */
#define IPOD_HOST_RAM_SETUP_OFFSET  0x000u
#define IPOD_HOST_RAM_SETUP_LEN     8u
/* Declared synthetic source window for IN staging. */
#define IPOD_HOST_RAM_IN_OFFSET     0x100u
#define IPOD_HOST_RAM_IN_MAX        64u

#endif /* IPOD_HOST_PROTOCOL_H */
