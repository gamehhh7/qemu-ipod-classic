/* ura_adapter.h — USB/IP request-lifecycle adapter prototype (QEMU-free core).
 *
 * Prototype scope (2026-10-23, ipod-zcode-usb-request-adapter):
 *   Maps decoded USB/IP SUBMIT/UNLINK requests onto the existing internal
 *   ipod_classic_usb IN-transaction API (begin/retry/cancel) and the SETUP
 *   ingress event. The core never fabricates completion: with no real host
 *   backend, a staged IN transaction stays pending; ipod_classic_usb_in_finish
 *   is NEVER called by this core (counted by the test fixture, must stay 0).
 *
 * Threading: every entry point must be called under the backend's BQL/lock,
 * exactly like the internal device API. No internal locking.
 *
 * Wire statuses below are Linux errno values as used by USB/IP RET_SUBMIT /
 * RET_UNLINK. Adapter error codes (URA_ERR_*) are internal and must never be
 * emitted on the wire; ura_final_t.status is the only wire-visible value.
 */
#ifndef URA_ADAPTER_H
#define URA_ADAPTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror of IPodUSBInResult (ipod_classic_usb_stage.h). Equality with the
 * real enum is static-asserted in ura_mirror_check.c and ura_qemu_binding.h.
 * Do not renumber. NO_ARM (appended by candidate patch 0002, value 13) means
 * no IN0 enable act happened after the currently accepted SETUP. */
enum {
    URA_IN_STAGED = 1, URA_IN_RETRIED, URA_IN_CANCELLED, URA_IN_UNSUPPORTED,
    URA_IN_BAD_ID, URA_IN_BAD_OWNER, URA_IN_BAD_GENERATION, URA_IN_BAD_MODE,
    URA_IN_BAD_SIZE, URA_IN_BAD_DMA, URA_IN_BUSY, URA_IN_STALE, URA_IN_NO_ARM
};

/* Separate pending-only ZLP result namespace. Never interpret as URA_IN_*.
 * There is deliberately no status finish/commit callback. */
enum {
    URA_ZLP_PENDING=1, URA_ZLP_RETRIED, URA_ZLP_CANCELLED,
    URA_ZLP_BAD_SESSION, URA_ZLP_BAD_SETUP, URA_ZLP_NO_ARM,
    URA_ZLP_BAD_MODE, URA_ZLP_BAD_SIZE, URA_ZLP_BUSY,
    URA_ZLP_STALE, URA_ZLP_EXHAUSTED
};

/* Mirror of IPodUSBSetupResult (ipod_classic_usb_core.h). */
enum {
    URA_SETUP_OK = 0, URA_SETUP_NO_VBUS, URA_SETUP_HOST_MODE,
    URA_SETUP_RESET_ACTIVE, URA_SETUP_DISCONNECTED, URA_SETUP_CLOCK_GATED,
    URA_SETUP_DMA_DISABLED, URA_SETUP_DESCRIPTOR_DMA, URA_SETUP_NOT_ARMED,
    URA_SETUP_HALTED, URA_SETUP_PENDING, URA_SETUP_SIZE, URA_SETUP_DMA_REJECTED
};

/* Mirror of IPOD_USB_IN_STAGE_MAX: single-transaction staging bound. */
#define URA_IN_STAGE_MAX 64u

/* URB-level final statuses (wire-visible, Linux errno). */
enum {
    URA_USB_ECONNRESET = -104, /* cancelled / superseded / aborted by bus event */
    URA_USB_ENODEV     = -19,  /* device detached */
    URA_USB_EPROTO     = -71,  /* structurally malformed request */
    URA_USB_EOPNOTSUPP = -95,  /* endpoint or phase with no internal path */
    URA_USB_EIO        = -5    /* adapter request table exhausted */
};

/* Adapter-internal error codes (never wire-visible). */
enum {
    URA_OK = 0,
    URA_ERR_ARG = -1000,
    URA_ERR_SESSION_GONE,
    URA_ERR_DEV_BUSY,
    URA_ERR_NO_SESSION_SLOT,
    URA_ERR_SEQ_DUP,
    URA_ERR_SEQ_UNKNOWN,
    URA_ERR_LATE,
    URA_ERR_STATE,
    URA_ERR_BACKEND,
    URA_ERR_TABLE_FULL
};

typedef struct { uint64_t owner, generation, id; } ura_stamp_t;

/* Backend vtable: one instance per attached exported device. Function shapes
 * mirror the existing C API exactly (see ADAPTER-CONTRACT.md §4). The QEMU
 * binding shim is ura_qemu_binding.h; the unit-test backend is
 * ura_backend_fixture.h and is a FIXTURE, not a real host. */
typedef struct ura_backend_ops {
    /* Mirror of ipod_classic_usb_in_begin(s, dst, cap, stamp). */
    int (*in_begin)(void *dev, uint8_t *dst, size_t cap, ura_stamp_t *stamp);
    /* Mirror of ipod_classic_usb_in_retry(s, stamp, dst, cap). */
    int (*in_retry)(void *dev, const ura_stamp_t *stamp, uint8_t *dst, size_t cap);
    /* Mirror of ipod_classic_usb_in_cancel(s, stamp). */
    int (*in_cancel)(void *dev, const ura_stamp_t *stamp);
    /* Binding completeness only. The adapter never calls it while no
     * completion producer exists; a counting fixture asserts finish_calls==0. */
    int (*in_finish)(void *dev, const ura_stamp_t *stamp);
    /* Mirror of ipod_classic_usb_setup_event. The offline fixture omits IRQ
     * delivery. Production binding requires the setup-ingress export patch:
     * the internal SETUP DMA callback is static to ipod_classic_usb.c and
     * unreachable from outside (contract gap G1). */
    int (*setup_event)(void *dev, const uint8_t setup[8]);
    /* Length staged by the active transaction, 0 if none. Proposed binding
     * uses the staged-length accessor in the unapplied patch. */
    uint32_t (*in_staged_len)(void *dev);
    /* Upper bound of one IN staging (binding: IPOD_USB_IN_STAGE_MAX). */
    uint32_t (*in_stage_cap)(void *dev);
    /* Arming provenance epochs (candidate patch 0002 accessors; fixture:
     * replica hooks driving the real ipod_classic_usb_arm.h rules).
     * setup_epoch: seq of the device's currently accepted SETUP, 0 = none.
     * in_arm_epoch: seq of the last valid IN0 enable act for that SETUP,
     * 0 = not armed. The adapter authorizes a staged IN read only when the
     * device's SETUP is still this request's SETUP and an enable act
     * strictly after that SETUP exists. Both required non-NULL. */
    uint64_t (*setup_epoch)(void *dev);
    uint64_t (*in_arm_epoch)(void *dev);
    /* Attach refs once; detach cancels stamps before unref. The external
     * backend MUST stop and drain callbacks before detach/destroy; this core
     * owns no worker/callback registration and cannot drain one. */
    void (*ref)(void *dev);
    void (*unref)(void *dev);
    /* New candidate requires all four typed pending-only status callbacks.
     * No payload/DMA/finish argument; return URA_ZLP_* only. */
    int (*status_in_begin)(void *dev, ura_stamp_t *stamp);
    int (*status_in_retry)(void *dev, const ura_stamp_t *stamp);
    int (*status_in_cancel)(void *dev, const ura_stamp_t *stamp);
    /* Abort metadata only if expected SETUP still owns current context.
     * Used even before a stamp exists; does not drive a physical line. */
    bool (*ep0_abort_epoch)(void *dev, uint64_t expected);

} ura_backend_ops_t;

/* Decoded USB/IP SUBMIT header (payload intentionally excluded: control-OUT
 * data stages are rejected before any guest side effect, and no other
 * direction carries data in this prototype). */
typedef struct {
    uint32_t seq, devid;
    uint8_t direction, endpoint;   /* direction: 0=OUT(host->dev), 1=IN */
    uint32_t flags, length;
    uint8_t setup[8];
} ura_wire_submit_t;

/* One wire-visible final result, emitted at most once per request. */
typedef struct {
    uint8_t has_final;
    uint32_t seq;
    int32_t status;
} ura_final_t;

typedef struct {
    uint64_t submits, rejects_devid, rejects_dup, rejects_ep, rejects_dir,
             rejects_out_data, rejects_len_cap, rejects_len_match;
    uint64_t setup_attempts, setup_ok;
    uint64_t begin_attempts, begin_staged, staged_len_mismatch;
    uint64_t retries_ok, probes_superseded;
    uint64_t cancels_ok, cancels_stale;
    uint64_t completes_ok, completes_refused, completes_late;
    uint64_t finalize_reset, finalize_disconnect, finalize_detach;
    uint64_t arm_holds;       /* begin held: no post-SETUP enable act yet */
    uint64_t epoch_mismatch;  /* begin refused: device SETUP is not the request's */
    uint64_t finish_calls; /* must remain 0 */
    uint64_t status_begin_attempts, status_pending, status_retries_ok;
    uint64_t status_cancels_ok, status_cancels_stale;
} ura_stats_t;

typedef struct ura_session ura_session_t;
typedef struct ura_adapter ura_adapter_t;

ura_adapter_t *ura_adapter_create(void);
void ura_adapter_destroy(ura_adapter_t *a);

/* Attach one exported device to one session. USB/IP IMPORT is exclusive:
 * attaching the same dev pointer twice without detach is refused. Takes one
 * ops->ref on success. */
ura_session_t *ura_attach(ura_adapter_t *a, uint64_t session_id, uint64_t devid,
                          const ura_backend_ops_t *ops, void *dev);

/* Detach: cancels staged stamps via ops->in_cancel, finalizes every
 * non-terminal request URA_USB_ENODEV exactly once, then ops->unref once.
 * Afterwards the handle is stale; all further calls fail URA_ERR_SESSION_GONE.
 * Returns the number of finalized requests (>=0) or an URA_ERR_*. */
int ura_detach(ura_session_t *sess);

/* Consume one queued final result exactly once. Immediate out results are
 * already consumed; reset/disconnect/pump/detach results are retrieved here.
 * Detached handles may be polled until destroy; nothing may use a handle
 * after destroy. Four lifetime session handles per adapter, no slot reuse;
 * 128 lifetime seq tombstones per session, no seq reuse. Exhaustion fails
 * closed. A production backend needs generation-bearing recyclable handles. */
int ura_poll_final(ura_session_t *sess, ura_final_t *out);

/* Wire-driven ops: call once per decoded SUBMIT/UNLINK, after the framing
 * layer has accepted the frame (framing owns dup-seq at protocol level; the
 * adapter defends independently). A structural reject produces an immediate
 * final result; otherwise the request stays pending. */
int ura_submit(ura_session_t *sess, const ura_wire_submit_t *sub,
               ura_final_t *out);
/* Raw-token ingress: reserves in this SAME request table, never delivers SETUP
 * from submit/pump. USBPacket.id is unrelated to seq. No second URB ledger. */
int ura_submit_deferred(ura_session_t *sess,const ura_wire_submit_t *sub,
                        ura_final_t *out);
enum { URA_TOKEN_SETUP_ACCEPTED=1, URA_TOKEN_SETUP_DEFERRED=2 };
int ura_deliver_setup_token(ura_session_t *sess,uint32_t seq,
                            const uint8_t setup[8]);
typedef enum { URA_PACKET_WAIT_SETUP=0, URA_PACKET_WAIT_ARM,
               URA_PACKET_DATA_STAGED, URA_PACKET_STATUS_PENDING,
               URA_PACKET_TERMINAL } ura_packet_phase_t;
typedef struct {
    uint32_t seq,length;uint8_t direction,endpoint,setup[8];
    uint64_t setup_epoch;ura_stamp_t stamp;bool stamp_live;
    ura_packet_phase_t phase;
} ura_packet_context_t;
/* Read-only exact identity view; no pump, acceptance, allocator or completion. */
int ura_request_packet_context(ura_session_t *sess,uint32_t seq,
                                ura_packet_context_t *out);

/* target = seqnum of the URB to cancel. The UNLINK frame's own seqnum is
 * framing-layer state (RET_UNLINK reply), not adapter state. */
int ura_unlink(ura_session_t *sess, uint32_t target,
               ura_final_t *out, int *was_pending);

/* Backend-observed guest lifecycle inputs. The USB/IP URB channel itself
 * carries no reset opcode (readonly/USBIP-TRANSPORT-BOUNDARY.md): these are
 * only called from the backend when it observes the guest-side line/level.
 * They finalize every non-terminal request with URA_USB_ECONNRESET once. */
int ura_notify_bus_reset(ura_session_t *sess, int *finalized);
int ura_notify_disconnect(ura_session_t *sess, int *finalized);

/* Guest-progress pump: one pass over pending requests. Re-attempts SETUP
 * delivery for transiently rejected requests, and for SETUP-accepted
 * control-IN requests verifies the arming provenance (device SETUP still
 * this request's SETUP + enable act strictly after it) before one in_begin
 * attempt, and probes staged transactions for supersession. Bounded
 * attempts per request; production should trigger the pump from the
 * in-armed notify hook (patches/) instead of polling. */
int ura_pump(ura_session_t *sess);

/* Error producer only: status<0, no payload, after SETUP accepted.
 * status>=0 is always refused: no genuine DWC commit / STATUS completion
 * producer exists. Terminal requests refuse late callbacks. */
int ura_request_complete(ura_session_t *sess, uint32_t seq, int32_t status,
                         const uint8_t *data, uint32_t actual,
                         ura_final_t *out);

/* Backend-driven re-read of the staged payload of one request's still-active
 * internal transaction (RETRIED). If the transaction was invalidated or its
 * registers changed, the request is finalized URA_USB_ECONNRESET exactly once
 * and URA_ERR_STATE is returned. */
int ura_request_retry_staged(ura_session_t *sess, uint32_t seq,
                             uint8_t *dst, size_t cap, uint32_t *actual);

void ura_get_stats(const ura_session_t *sess, ura_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* URA_ADAPTER_H */
