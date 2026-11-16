/* Task 16: the EXTERNAL host-side transport owner caller that task 12 left
 * missing (see 12-OWNER-FENCE-CONTRACT/HANDOFF.md: "controller-side usb-caller
 * glue not converted to this callback", "real external transport owner caller
 * absent").
 *
 * LAYER SEPARATION - these three things must never be collapsed:
 *
 *   SYNTHETIC      this module and the fixture harness. No USB descriptors, no
 *                  USB/IP or socket server, no HCD, no guest IRQ/ACK/XFRC
 *                  producer, no Windows enumeration, no descriptor constants.
 *   STAGED BYTES   ipod_host_xfer_observe() returns ONLY bytes the controller
 *                  itself staged from the real DMA source through
 *                  ipod_usb_in_stage_prepare(); every observation is tagged
 *                  observation_only and carries controller stamp provenance.
 *   REAL COMPLETION does not exist in this slice. A TERMINAL owner notification
 *                  for a SETUP packet means "the copied controller receiver
 *                  accepted the 8 SETUP bytes and performed the single DMA" -
 *                  it is NOT an ACK, XFRC, status completion or URB success.
 *
 * The owner obeys 12-OWNER-FENCE-CONTRACT bridge.h section 1-10:
 *   - registration happens BEFORE the first bind/submit (create());
 *   - exactly one URA request is reserved per control transfer, in the SAME
 *     ledger as the bridge (bind_request), and the original 8 SETUP bytes are
 *     submitted exactly once through the bridge raw-packet wrapper;
 *   - request storage is retired only from the terminal/cancel/NAK
 *     notification, never earlier;
 *   - the owner object is destroyed only after the UNREGISTER_DONE fence.
 */
#ifndef IPOD_HOST_XFER_H
#define IPOD_HOST_XFER_H
#include "qapi/error.h"
#include "hw/usb.h"
#include "hw/arm/ipod_classic/ura_adapter.h"

struct IPodClassicUSBState;
struct IPodRawPacketBridge;

/* Bounded transport resources. Not endpoint or device limits. */
#define IPOD_HOST_XFER_MAX_PACKETS 8
#define IPOD_HOST_XFER_MAX_OBSERVE URA_IN_STAGE_MAX

/* Returned by submit_control() when the ledger reserved the request and
 * produced a NEGATIVE final before anything reached the receiver (structural
 * reject: control-OUT data stage, endpoint != 0, over-cap IN, flags). The guest
 * side is provably untouched: the accepted-SETUP epoch does not move. */
enum { IPOD_HOST_XFER_REJECTED = -2200 };

typedef enum {
    IPOD_HOST_XFER_STATE_FREE = 0,  /* record not in use */
    IPOD_HOST_XFER_STATE_SUBMITTED, /* packet submitted, no retirement yet */
    IPOD_HOST_XFER_STATE_RETIRED    /* terminal/cancel/NAK seen: storage retired */
} ipod_host_xfer_state_t;

/* Bytes the CONTROLLER staged from its own DMA source. observation_only is
 * always true: this is data visibility, never a commit or a completion. */
typedef struct {
    uint8_t bytes[IPOD_HOST_XFER_MAX_OBSERVE];
    uint32_t length;           /* bytes actually staged by the controller */
    uint32_t declared_length;  /* wLength of the accepted SETUP */
    uint8_t direction;         /* 1 = IN (device -> host) */
    uint32_t seq;              /* URA request this observation belongs to */
    uint64_t setup_epoch;      /* controller accepted-SETUP epoch */
    uint64_t stamp_owner;      /* controller in-stage stamp provenance */
    uint64_t stamp_generation;
    uint64_t stamp_id;
    bool staged;               /* true only when the controller staged bytes */
    bool observation_only;     /* always true in this slice */
} ipod_host_observation_t;

/* Snapshot of one control request's owner-side state. packet_cookie is the
 * transport cookie of the most recent SETUP packet attempt (0 before the first
 * submit); it is what ipod_host_xfer_pump() takes. */
typedef struct {
    uint64_t req_handle;    /* stable control handle (survives a NAK retry) */
    uint64_t packet_cookie; /* most recent SETUP packet transport cookie */
    uint32_t seq;           /* URA request seq inside the bridge ledger */
    uint8_t direction;
    uint32_t declared_length;
    bool open;              /* binding still owns the URA request */
    bool setup_retired;     /* a retirement was observed for the SETUP packet */
    int setup_status;       /* p->status seen at that retirement */
    uint64_t setup_epoch;   /* controller accepted-SETUP epoch at retirement */
} ipod_host_control_info_t;

typedef struct {
    uint64_t cookie;      /* transport cookie assigned by the bridge */
    uint32_t seq;         /* URA request the packet belongs to */
    bool retired;         /* a terminal/cancel/NAK notification arrived */
    bool live;            /* still in flight, no retirement yet */
    int reason;           /* IPOD_RAW_OWNER_* of the retirement */
    int status;           /* p->status observed at retirement */
    bool was_async;
    bool in_submit;       /* retirement happened inside the submit call */
    bool attached;        /* URA session attached at notification time */
    uint64_t epoch;       /* controller accepted-SETUP epoch at notification */
    uint64_t order;       /* monotonic notification order */
} ipod_host_packet_info_t;

typedef struct {
    uint64_t submits, stage_in_submits, notifications;
    uint64_t terminals, cancels, naks, bind_closes, fences;
    uint64_t packets_freed, deferred_releases;
    uint64_t observations, observation_refusals, rejects;
    uint64_t control_closes;           /* control records that stopped owning a request */
    uint64_t observation_uncorrelated; /* stages refused: not the stage this handle made */
    uint64_t destroy_refused_no_fence; /* destroy() saw no UNREGISTER_DONE fence */
    uint64_t post_fence_notifications; /* must stay 0 */
    uint64_t unregister_rc;
    bool fence_seen, registered;
} ipod_host_xfer_stats_t;

typedef struct IPodHostXfer IPodHostXfer;

/* Register the owner callback BEFORE any bind. Fails if the bridge has no live
 * session, or if another owner is already registered. */
/* Create the external transport owner. Registration with the bridge happens
 * here, i.e. BEFORE the owner can bind or submit anything (contract section 10).
 *
 * controller is the same IPodClassicUSBState the bridge was configured with. It
 * is passed explicitly rather than fetched back out of the bridge so that every
 * reused task-12 source stays byte-identical: the owner needs the controller
 * only to read stage provenance for observation. */
IPodHostXfer *ipod_host_xfer_create(USBDevice *bridge,
                                    struct IPodClassicUSBState *controller,
                                    Error **errp);
/* Drain, unregister, wait for the fence, release remaining storage. Returns the
 * unregister result (URA_OK on a normal close) and reports the final counters.
 * out_stats may be NULL. */
int ipod_host_xfer_destroy(IPodHostXfer *xfer, ipod_host_xfer_stats_t *out_stats);

/* Reserve ONE control request in the bridge's URA ledger and submit the
 * original 8 SETUP bytes exactly once. *req_handle_out receives the stable
 * owner handle for this control request (0 when the request was structurally
 * rejected); it survives a NAK re-submission, unlike the per-packet bridge
 * transport cookie, which ipod_host_xfer_control_info() reports.
 * *final_status_out receives the negative URA status of a structural reject. */
int ipod_host_xfer_submit_control(IPodHostXfer *xfer, const uint8_t setup[8],
                                  uint64_t *req_handle_out,
                                  int *final_status_out);
/* Submit one IN token for an OPEN control request.
 *
 * Two legal shapes, following the real no-data rule of USB 2.0 8.5.3:
 *   - len > 0, buf != NULL: the data stage of a control-IN request
 *     (bmRequestType direction bit 1, wLength > 0).
 *   - len == 0, buf == NULL: the STATUS stage of a request that has no data
 *     stage at all (wLength == 0). The status stage of a no-data control
 *     transfer is always an IN transaction, whatever the request direction bit
 *     says, so SET_CONFIGURATION (direction bit 0, wLength 0) is legal here.
 * A zero-length token for a request that declared wLength > 0, or a data token
 * for a direction-bit-0 request, is refused with URA_ERR_ARG.
 *
 * Parking is the expected outcome: this slice has no status/commit producer, so
 * the packet stays ASYNC and the request stays pending. URA_OK means "the token
 * was accepted by the transport", never "the status stage succeeded". */
int ipod_host_xfer_submit_stage_in(IPodHostXfer *xfer, uint64_t req_handle,
                                   uint8_t *buf, size_t len,
                                   uint64_t *cookie_out);
/* Re-submit the SAME control request's original 8 SETUP bytes after a NAK
 * retirement (the URA request stays open and keeps its seq). Refused unless the
 * previous attempt is retired and the binding is still open. */
int ipod_host_xfer_retry_setup(IPodHostXfer *xfer, uint64_t req_handle,
                               uint64_t *cookie_out);
/* Observe controller-staged bytes for a control request. URA_ERR_STATE means
 * "nothing staged right now" - never a fabricated buffer. */
int ipod_host_xfer_observe(IPodHostXfer *xfer, uint64_t req_handle,
                           ipod_host_observation_t *out);
/* Give the bridge a chance to re-attempt a live parked packet. */
int ipod_host_xfer_pump(IPodHostXfer *xfer, uint64_t cookie);
/* Read back the owner record for a submitted packet (diagnostics/assertions).
 * Returns false when the cookie is unknown to this owner. */
bool ipod_host_xfer_packet_info(IPodHostXfer *xfer, uint64_t cookie,
                                ipod_host_packet_info_t *out);
/* Read back the owner record for a control request handle. */
bool ipod_host_xfer_control_info(IPodHostXfer *xfer, uint64_t req_handle,
                                 ipod_host_control_info_t *out);
/* Device-side style cancellation of the whole binding (drain). */
int ipod_host_xfer_cancel(IPodHostXfer *xfer);
bool ipod_host_xfer_binding_open(const IPodHostXfer *xfer);
void ipod_host_xfer_get_stats(const IPodHostXfer *xfer,
                              ipod_host_xfer_stats_t *out);
const char *ipod_host_xfer_state_name(ipod_host_xfer_state_t state);
#endif
