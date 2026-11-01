#ifndef IPOD_RAW_PACKET_BRIDGE_H
#define IPOD_RAW_PACKET_BRIDGE_H
#include "hw/usb.h"
#include "hw/arm/ipod_classic/ipod_classic_usb.h"
#include "hw/arm/ipod_classic/ura_adapter.h"
#define TYPE_IPOD_RAW_PACKET_BRIDGE "ipod-raw-packet-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(IPodRawPacketBridge,IPOD_RAW_PACKET_BRIDGE)
/* BQL only. Caller owns exclusive controller/export access and a strong bridge
 * reference; this API is not a global multi-exporter registry. Carrier speed is
 * explicit synthetic transport policy, NOT measured S5L PHY capability.
 * Configure once BEFORE realize. No descriptors/VID/PID are supplied. */
bool ipod_raw_bridge_configure(USBDevice *dev,IPodClassicUSBState *controller,
                               int carrier_speed,Error **errp);
int ipod_raw_bridge_bind_request(USBDevice *dev,const ura_wire_submit_t *submit,
                                 ura_final_t *out);
/* Mandatory ingress wrapper. Register transport association BEFORE the core can
 * queue a packet; raw callers must not bypass it with usb_handle_packet.
 * Caller owns packet storage until completed/cancelled AND callbacks drained.
 * Cookie is a non-wrapping transport identity, never URA seq or a device stamp. */
int ipod_raw_bridge_submit_packet(USBDevice *dev,uint32_t seq,USBPacket *packet,
                                  uint64_t *cookie);
int ipod_raw_bridge_pump_packet(USBDevice *dev,uint64_t cookie);
void ipod_raw_bridge_cancel_request(USBDevice *dev);
int ipod_raw_bridge_poll_final(USBDevice *dev,ura_final_t *out);
/* Packet-owner cancellation/drain/rebind contract (BQL only).
 *
 * 1. The DEVICE side may cancel submitted packets at any lifecycle event
 *    (controller lost, bus reset, vbus fall, error close, cancel_request,
 *    unrealize) by calling usb_cancel_packet(). For raw-packet devices that
 *    invocation reaches the optional cancel hook (usb_raw_packet_cancel)
 *    exactly once per packet, after core queue removal, with the packet in
 *    USB_PACKET_CANCELED state.
 * 2. The packet OWNER must therefore treat its own usb_cancel_packet() as
 *    forbidden for device-drained packets: a second usb_cancel_packet() on a
 *    CANCELED packet asserts (usb_packet_is_inflight). After a cancel hook or
 *    a cancel_request/drain/unrealize call, the owner drops its packet record
 *    without re-cancelling.
 * 3. Negative URA finals produced by bridge-side cancellation are consumed
 *    inline by the bridge and are NOT re-emitted by poll_final; the owner
 *    learns of them through the cancel hook plus this contract. poll_final
 *    therefore normally returns an empty result while the session lives; it
 *    still exists for lifecycle finals emitted with no packet attached.
 *    After unrealize the session is destroyed and poll_final returns
 *    URA_ERR_SESSION_GONE. A seq the owner still considers open at unrealize
 *    time is finalized ENODEV by detach; the owner closes it locally.
 * 4. URA seq numbers are tombstoned forever after any terminal (including
 *    cancellation): rebinding a previously used seq returns URA_ERR_SEQ_DUP.
 *    Each new control transfer binds a fresh, never-reused seq.
 * 5. One bound request at a time: bind_request refuses while a request is
 *    bound; between control transfers the owner calls cancel_request (full
 *    drain, binding cleared) before the next bind_request. An IN packet
 *    parked ASYNC never completes on its own (no status/data success
 *    producer exists) and must be retired by cancel/drain.
 * 6. Cookies: submit_packet returns a fresh non-wrapping cookie. pump_packet
 *    with a stale/freed/garbage cookie returns URA_ERR_LATE with no state
 *    change. Packet pointers are keyed by (pointer,id) at submit; a completed
 *    or cancelled packet's storage may be re-set-up and re-submitted.
 *
 * Async transport owner notification (task 10). BQL only; synchronous.
 *
 * 7. One exclusive owner may register a notification callback BEFORE its first
 *    bind_request. Every packet retirement and every drain of a still-bound
 *    request then fires exactly ONE synchronous callback under BQL:
 *      IPOD_RAW_OWNER_CANCEL       device-side cancel/drain reached the packet
 *                                  (cancel hook after usb_cancel_packet, or a
 *                                  bridge_drain sweep entry);
 *      IPOD_RAW_OWNER_TERMINAL     packet completed (sync complete inside
 *                                  submit, or the usb_packet_complete path);
 *      IPOD_RAW_OWNER_NAK          NAK retirement inside submit_packet: the
 *                                  packet attempt is dead but the URA request
 *                                  stays OPEN for a fresh same-seq submission;
 *      IPOD_RAW_OWNER_BIND_CLOSED  a still-bound request was retired with no
 *                                  live packet (drain cleared the binding);
 *                                  notify->packet is NULL and cookie is 0.
 *    The notification carries the exact submitted packet pointer, the
 *    transport cookie, the submit-time packet id, the URA seq (USBPacket.id is
 *    never used as seq), the registration owner_generation, the realize-time
 *    session_serial, the controller's live setup_epoch (0 if none/gone), the
 *    real terminal packet_status, and for CANCEL whether the packet was ASYNC
 *    or QUEUED. A notification is proof of retirement ONLY: it is never a
 *    URB success, ACK, XFRC or status/data completion.
 * 8. Ordering per retirement: the negative URA final is consumed inline
 *    (ledger closed) FIRST, then the transport slot/ref is retired, THEN the
 *    owner notification fires. On the dormant usb_packet_complete path the
 *    port->ops->complete callback runs after the notification; everywhere
 *    else nothing touches the packet after the notification returns.
 * 9. Lifetime and re-entrancy: the bridge holds a strong reference across the
 *    callback. The callback may re-enter cancel_request, pump_packet and
 *    unrealize, and may call ipod_raw_bridge_set_owner_notify itself. For
 *    CANCEL, NAK and BIND_CLOSED the owner may free the packet storage inside
 *    the callback; for TERMINAL it may do so when in_owner_submit is true
 *    (delivered within the owner's own submit_packet call), otherwise the port
 *    complete callback may still reference the packet. The bridge never
 *    dereferences a packet after its notification returns. The owner must NOT
 *    call usb_cancel_packet for a retired packet (second-cancel assert).
 * 10. Registration lifecycle with a provable unregister fence (revision 2):
 *     generations start at 1 and increment on every real transition (clear
 *     or set); every notification carries the generation in effect when it
 *     fired. UNREGISTERING A REGISTRATION ALWAYS ENDS WITH EXACTLY ONE
 *     IPOD_RAW_OWNER_UNREGISTER_DONE fence notification to that owner: the
 *     fence is the LAST callback the owner ever receives for that
 *     registration, at that point every bridge-side retirement of its
 *     packets/binding has already been notified, and after the fence returns
 *     the bridge never invokes the callback again. The owner may destroy
 *     itself and release every remaining external resource after the fence
 *     returns; it must stay alive and keep handling notifications until then.
 *     Outside a drain, set_owner_notify(NULL or a new cb) completes inline:
 *     still-live packets and the binding are drained first (retire
 *     notifications under the OLD generation), then the fence fires, the
 *     registration clears and the generation bumps - nothing fires after the
 *     call returns. INSIDE a drain (called from a notification callback) the
 *     request cannot complete inline: clearing returns
 *     IPOD_RAW_OWNER_DEFERRED with the generation unchanged and the callback
 *     KEPT; the remaining retirements of that drain are still notified to
 *     this owner (never silently dropped), and the fence is delivered at the
 *     end of that same drain, completing the unregister there. Re-registering
 *     a different non-NULL callback from inside a drain is refused
 *     URA_ERR_STATE. After requesting an unregister (deferred or not) the
 *     owner must not submit, bind or pump again. Identical re-registration
 *     and NULL->NULL are idempotent (no fence, generation unchanged).
 *     Registering on an unrealized bridge fails URA_ERR_SESSION_GONE.
 *
 *     Fence-completion window (revision 3, disambiguated in revision 4):
 *     while the UNREGISTER_DONE fence callback itself is running - on
 *     EITHER exit, the drain epilogue or the inline non-drain path - the
 *     registration is in the "completing" state. A redundant
 *     set_owner_notify(NULL) from inside that fence callback is an
 *     IDEMPOTENT URA_OK with the generation unchanged: the in-flight fence
 *     IS the completion, no new pending request is recorded, no second
 *     fence ever fires, and the registration still closes when the fence
 *     returns. EVERY non-NULL set_owner_notify call from inside that fence
 *     callback is refused URA_ERR_STATE with no state change - INCLUDING a
 *     call whose callback and opaque are IDENTICAL to the registration
 *     being completed. The API has no parameter distinguishing an
 *     idempotent re-registration from a query of the current one, and the
 *     epilogue clears the registration the moment the fence returns, so an
 *     URA_OK for the identical pair inside the window would falsely
 *     promise continuation; the only safe reading is that every non-NULL
 *     call inside the window requests a registration and is refused until
 *     the fence returns. A new generation may therefore only be
 *     established by an explicit set_owner_notify AFTER the fence returns
 *     (that call then behaves normally: fresh registration, generation
 *     bump, notifications flow). Outside the fence-completion window the
 *     identical re-registration and NULL->NULL idempotency of section 10
 *     apply unchanged. A pending unregister request can never outlive its
 *     own registration: no fence callback re-entry can leave state behind
 *     for a LATER registration, and a new registration additionally starts
 *     with any stale pending bit cleared, so a new owner can never inherit
 *     an unsolicited fence. */
enum {
    IPOD_RAW_OWNER_CANCEL=1, IPOD_RAW_OWNER_TERMINAL, IPOD_RAW_OWNER_NAK,
    IPOD_RAW_OWNER_BIND_CLOSED, IPOD_RAW_OWNER_UNREGISTER_DONE
};
/* Bridge-local status (outside the URA_ERR_* namespace): the unregister
 * request was accepted but is deferred to the end of the in-progress drain;
 * the fence notification will complete it. The owner must stay alive and
 * keep handling notifications until the fence. */
enum { IPOD_RAW_OWNER_DEFERRED=-2100 };
typedef struct {
    USBPacket *packet;          /* exact submitted packet; NULL for BIND_CLOSED
                                 * and UNREGISTER_DONE (no packet involved) */
    uint64_t cookie;            /* transport cookie from submit; 0 for BIND_CLOSED
                                 * and UNREGISTER_DONE */
    uint64_t packet_id;         /* submit-time packet->id; never the URA seq */
    uint32_t seq;               /* URA seq bound to this packet / closed bind;
                                 * 0 for UNREGISTER_DONE */
    uint64_t owner_generation;  /* registration generation in effect at notify */
    uint64_t session_serial;    /* bridge session identity assigned at realize */
    uint64_t setup_epoch;       /* controller accepted-SETUP epoch; 0 if none/gone */
    int reason;                 /* IPOD_RAW_OWNER_* */
    int packet_status;          /* p->status at retirement; 0 for BIND_CLOSED
                                 * and UNREGISTER_DONE */
    bool was_async;             /* CANCEL: packet was ASYNC (true) or QUEUED */
    bool in_owner_submit;       /* delivered within the owner's submit_packet */
    bool session_attached;      /* URA session still attached at notify */
} ipod_raw_owner_notify_t;
typedef void (*ipod_raw_owner_notify_fn)(USBDevice *bridge,
                                         const ipod_raw_owner_notify_t *notify,
                                         void *owner);
int ipod_raw_bridge_set_owner_notify(USBDevice *dev,ipod_raw_owner_notify_fn cb,
                                     void *owner,uint64_t *generation);
#endif
