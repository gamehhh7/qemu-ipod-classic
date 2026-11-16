/*
 * ipod-native-host.h - default-off native observation transport for the
 * iPod-Classic machine's real USB device core.
 *
 * This is the task 18 candidate: the NORMAL qemu-system-arm main() runs, the
 * NORMAL iPod-Classic machine instantiates the NORMAL controller at
 * USBOTG_MEM_BASE with the existing IRQ and guest RAM, and this module hangs an
 * opt-in loopback observation entry point off THAT controller.
 *
 * It creates no second controller, no synthetic RAM, no firmware stand-in and
 * no nested main loop. It programs no model register and writes no guest
 * memory. Every USB-visible effect still travels
 *
 *     external client -> ipod-host-server -> ipod_host_xfer_*
 *         -> ipod-raw-packet-bridge -> ura_adapter -> the real controller
 *
 * exactly as it does in the accepted task 16/17 path.
 *
 * ENABLING
 *   IPOD_USB_CORE=1                (existing switch: instantiate the device core)
 *   IPOD_USB_NATIVE_HOST=1         (this module; default OFF)
 *   IPOD_USB_NATIVE_PORT=<port>    (loopback port, default 45117)
 *   IPOD_USB_NATIVE_LIFETIME_MS=<ms>  (watchdog, default 360000)
 *
 * With IPOD_USB_NATIVE_HOST unset nothing here runs and the machine behaves
 * exactly as before.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef IPOD_NATIVE_HOST_H
#define IPOD_NATIVE_HOST_H

#include "qapi/error.h"

struct IPodClassicUSBState;

/* Identity the external client must present to OPEN. Fixed, not random. */
#define IPOD_NATIVE_HOST_IDENTITY 0x1C3300C0DEULL

/*
 * Attach the opt-in native observation transport to an ALREADY REALIZED
 * machine controller. Called from the machine's own USB block.
 *
 * Returns true when the transport is listening, false when it is not (disabled,
 * misconfigured, or the port was taken). A false return is never fatal: the
 * machine must keep booting exactly as it would without this module.
 */
bool ipod_native_host_maybe_attach(struct IPodClassicUSBState *controller,
                                   Error **errp);

/* True once the transport is attached; used by the harness for its verdict. */
bool ipod_native_host_is_attached(void);

#endif /* IPOD_NATIVE_HOST_H */
