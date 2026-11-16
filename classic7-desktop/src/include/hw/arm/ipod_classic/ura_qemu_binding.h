/* ura_qemu_binding.h — production binding shim: ura backend vtable to the
 * EXISTING QEMU device C API. STATIC INFERENCE ONLY in this prototype: this
 * header is NOT compiled by the offline unit tests (it needs hw/sysbus.h and
 * the rest of the QEMU tree). Review at integration time together with
 * patches/0001-*.patch.
 *
 * Placement: hw/arm/ipod_classic/ (same include graph as ipod_classic_usb.c).
 * The unit-test build already static-asserts the mirror enums/stamp against
 * the real headers (src/ura_mirror_check.c); the asserts below repeat inside
 * the QEMU tree so renumbering on either side breaks the build there too.
 *
 * Binding rules (see ADAPTER-CONTRACT.md §4):
 *  - All ops run under BQL (qemu_mutex_iothread_locked), same as the device.
 *  - dev is the IPodClassicUSBState QOM object; the backend holder keeps a
 *    QOM reference (object_ref) for the whole attach lifetime and drains all
 *    stamps (in_cancel) before object_unref / before unrealize completes.
 *  - The adapter NEVER calls in_finish (UNSUPPORTED today); completion stays
 *    pending until a real DWC success-commit producer exists.
 *  - setup_event REQUIRES patches/0001-*.patch (ipod_classic_usb_setup_ingress):
 *    the internal usb_setup_dma callback is static to ipod_classic_usb.c and
 *    cannot be re-implemented outside without duplicating audited DMA logic.
 *  - setup_epoch/in_arm_epoch REQUIRES candidate patch 0002 (arming
 *    provenance accessors + NO_ARM begin gate). The binding table is not
 *    complete without both patches; integration review must apply 0001 then
 *    0002 (0002 depends on 0001 context).
 */
#ifndef URA_QEMU_BINDING_H
#define URA_QEMU_BINDING_H

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/arm/ipod_classic/ipod_classic_usb.h"
#include "hw/arm/ipod_classic/ura_adapter.h"

#define CT_ASSERT(name, cond) typedef char name[(cond) ? 1 : -1]

CT_ASSERT(ura_q_staged,         (int)URA_IN_STAGED == (int)IPOD_USB_IN_STAGED);
CT_ASSERT(ura_q_retried,        (int)URA_IN_RETRIED == (int)IPOD_USB_IN_RETRIED);
CT_ASSERT(ura_q_cancelled,      (int)URA_IN_CANCELLED == (int)IPOD_USB_IN_CANCELLED);
CT_ASSERT(ura_q_unsupported,    (int)URA_IN_UNSUPPORTED == (int)IPOD_USB_IN_UNSUPPORTED);
CT_ASSERT(ura_q_bad_id,         (int)URA_IN_BAD_ID == (int)IPOD_USB_IN_BAD_ID);
CT_ASSERT(ura_q_bad_owner,      (int)URA_IN_BAD_OWNER == (int)IPOD_USB_IN_BAD_OWNER);
CT_ASSERT(ura_q_bad_generation, (int)URA_IN_BAD_GENERATION == (int)IPOD_USB_IN_BAD_GENERATION);
CT_ASSERT(ura_q_bad_mode,       (int)URA_IN_BAD_MODE == (int)IPOD_USB_IN_BAD_MODE);
CT_ASSERT(ura_q_bad_size,       (int)URA_IN_BAD_SIZE == (int)IPOD_USB_IN_BAD_SIZE);
CT_ASSERT(ura_q_bad_dma,        (int)URA_IN_BAD_DMA == (int)IPOD_USB_IN_BAD_DMA);
CT_ASSERT(ura_q_busy,           (int)URA_IN_BUSY == (int)IPOD_USB_IN_BUSY);
CT_ASSERT(ura_q_stale,          (int)URA_IN_STALE == (int)IPOD_USB_IN_STALE);
CT_ASSERT(ura_q_no_arm,         (int)URA_IN_NO_ARM == (int)IPOD_USB_IN_NO_ARM);
CT_ASSERT(ura_q_stamp, sizeof(ura_stamp_t) == sizeof(IPodUSBInStamp));

static inline int ura_q_in_begin(void *dev, uint8_t *dst, size_t cap,
                                 ura_stamp_t *stamp)
{
    IPodUSBInStamp local = {0};
    int rc;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_IN_BAD_ID; }
    rc = (int)ipod_classic_usb_in_begin((IPodClassicUSBState *)dev, dst,
                                      cap, &local);
    if (rc == URA_IN_STAGED) {
        stamp->owner = local.owner;
        stamp->generation = local.generation;
        stamp->id = local.id;
    }
    return rc;
}

static inline int ura_q_in_retry(void *dev, const ura_stamp_t *stamp,
                                 uint8_t *dst, size_t cap)
{
    IPodUSBInStamp local;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_IN_BAD_ID; }
    local = (IPodUSBInStamp){stamp->owner, stamp->generation, stamp->id};
    return (int)ipod_classic_usb_in_retry((IPodClassicUSBState *)dev,
                                          &local, dst,
                                          cap);
}

static inline int ura_q_in_cancel(void *dev, const ura_stamp_t *stamp)
{
    IPodUSBInStamp local;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_IN_BAD_ID; }
    local = (IPodUSBInStamp){stamp->owner, stamp->generation, stamp->id};
    return (int)ipod_classic_usb_in_cancel((IPodClassicUSBState *)dev,
                                           &local);
}

/* Binding completeness only; never called by the adapter prototype. */
static inline int ura_q_in_finish(void *dev, const ura_stamp_t *stamp)
{
    IPodUSBInStamp local;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_IN_BAD_ID; }
    local = (IPodUSBInStamp){stamp->owner, stamp->generation, stamp->id};
    return (int)ipod_classic_usb_in_finish((IPodClassicUSBState *)dev,
                                           &local);
}

/* Requires patches/0001-*.patch (ipod_classic_usb_setup_ingress export).
 * Without the patch this shim does not compile: the gap stays explicit. */
static inline int ura_q_setup_event(void *dev, const uint8_t setup[8])
{
    assert(qemu_mutex_iothread_locked());
    return (int)ipod_classic_usb_setup_ingress((IPodClassicUSBState *)dev,
                                               setup);
}

static inline uint32_t ura_q_in_staged_len(void *dev)
{
    IPodClassicUSBState *s = (IPodClassicUSBState *)dev;
    assert(qemu_mutex_iothread_locked());
    return (uint32_t)ipod_classic_usb_in_staged_len(s);
}

static inline uint32_t ura_q_in_stage_cap(void *dev)
{
    (void)dev;
    return IPOD_USB_IN_STAGE_MAX;
}

/* Requires candidate patch 0002 (arming provenance accessors). */
static inline uint64_t ura_q_setup_epoch(void *dev)
{
    assert(qemu_mutex_iothread_locked());
    return ipod_classic_usb_setup_epoch((IPodClassicUSBState *)dev);
}
static inline uint64_t ura_q_in_arm_epoch(void *dev)
{
    assert(qemu_mutex_iothread_locked());
    return ipod_classic_usb_in_arm_epoch((IPodClassicUSBState *)dev);
}

CT_ASSERT(ura_q_zlp_pending, (int)URA_ZLP_PENDING==(int)IPOD_ZLP_PENDING);
CT_ASSERT(ura_q_zlp_retried, (int)URA_ZLP_RETRIED==(int)IPOD_ZLP_RETRIED);
CT_ASSERT(ura_q_zlp_cancelled, (int)URA_ZLP_CANCELLED==(int)IPOD_ZLP_CANCELLED);
CT_ASSERT(ura_q_zlp_bad_session, (int)URA_ZLP_BAD_SESSION==(int)IPOD_ZLP_BAD_SESSION);
CT_ASSERT(ura_q_zlp_bad_setup, (int)URA_ZLP_BAD_SETUP==(int)IPOD_ZLP_BAD_SETUP);
CT_ASSERT(ura_q_zlp_no_arm, (int)URA_ZLP_NO_ARM==(int)IPOD_ZLP_NO_ARM);
CT_ASSERT(ura_q_zlp_bad_mode, (int)URA_ZLP_BAD_MODE==(int)IPOD_ZLP_BAD_MODE);
CT_ASSERT(ura_q_zlp_bad_size, (int)URA_ZLP_BAD_SIZE==(int)IPOD_ZLP_BAD_SIZE);
CT_ASSERT(ura_q_zlp_busy, (int)URA_ZLP_BUSY==(int)IPOD_ZLP_BUSY);
CT_ASSERT(ura_q_zlp_stale, (int)URA_ZLP_STALE==(int)IPOD_ZLP_STALE);
CT_ASSERT(ura_q_zlp_exhausted, (int)URA_ZLP_EXHAUSTED==(int)IPOD_ZLP_EXHAUSTED);
static inline int ura_q_status_in_begin(void *dev, ura_stamp_t *stamp)
{
    IPodUSBInStamp local = {0}; IPodZLPResult result;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_ZLP_BAD_SESSION; }
    result=ipod_classic_usb_status_in_begin(dev,&local);
    if (result==IPOD_ZLP_PENDING) {
        stamp->owner=local.owner;stamp->generation=local.generation;stamp->id=local.id;
    }
    return (int)result; /* ZLP namespace only, CT_ASSERTed above. */
}
static inline int ura_q_status_in_retry(void *dev, const ura_stamp_t *stamp)
{
    IPodUSBInStamp local;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_ZLP_STALE; }
    local=(IPodUSBInStamp){stamp->owner,stamp->generation,stamp->id};
    return (int)ipod_classic_usb_status_in_retry(dev,&local);
}
static inline int ura_q_status_in_cancel(void *dev, const ura_stamp_t *stamp)
{
    IPodUSBInStamp local;
    assert(qemu_mutex_iothread_locked());
    if (!stamp) { return URA_ZLP_STALE; }
    local=(IPodUSBInStamp){stamp->owner,stamp->generation,stamp->id};
    return (int)ipod_classic_usb_status_in_cancel(dev,&local);
}
static inline bool ura_q_ep0_abort_epoch(void *dev,uint64_t expected)
{
    assert(qemu_mutex_iothread_locked());
    return ipod_classic_usb_ep0_abort_epoch(dev,expected);
}

/* QOM reference discipline: ura_attach refuses NULL ref/unref, so the backend
 * holder copies this table and fills ref/unref before use, e.g.:
 *   static void ura_q_ref(void *dev)   { object_ref((Object *)dev); }
 *   static void ura_q_unref(void *dev) { object_unref((Object *)dev); }
 * ref runs at attach; unref only after ura_detach has cancelled every staged
 * stamp and the session is gone (drain-then-unref; contract §5). */

/* Template table: ref/unref intentionally NULL — the holder must fill them
 * in before ura_attach. */
static const ura_backend_ops_t ura_qemu_backend_ops = {
    ura_q_in_begin,
    ura_q_in_retry,
    ura_q_in_cancel,
    ura_q_in_finish,
    ura_q_setup_event,
    ura_q_in_staged_len,
    ura_q_in_stage_cap,
    ura_q_setup_epoch,
    ura_q_in_arm_epoch,
    NULL, /* ref: fill with object_ref wrapper before ura_attach */
    NULL, /* unref: fill with object_unref wrapper before ura_attach */
    ura_q_status_in_begin,
    ura_q_status_in_retry,
    ura_q_status_in_cancel,
    ura_q_ep0_abort_epoch
};

#endif /* URA_QEMU_BINDING_H */
