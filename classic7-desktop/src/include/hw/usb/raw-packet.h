/* Optional per-token interface. Existing USBDevice/Class layouts stay intact. */
#ifndef USB_RAW_PACKET_H
#define USB_RAW_PACKET_H
#include "qom/object.h"
#include "hw/usb.h"
#define TYPE_USB_RAW_PACKET "usb-raw-packet"
typedef struct USBRawPacketClass USBRawPacketClass;
typedef struct USBRawPacket USBRawPacket;
DECLARE_CLASS_CHECKERS(USBRawPacketClass, USB_RAW_PACKET, TYPE_USB_RAW_PACKET)
#define USB_RAW_PACKET(obj) INTERFACE_CHECK(USBRawPacket,(obj),TYPE_USB_RAW_PACKET)
struct USBRawPacketClass {
    InterfaceClass parent;
    void (*handle_packet)(USBDevice *dev,USBPacket *packet);
    void (*cancel_packet)(USBDevice *dev,USBPacket *packet,bool was_async);
    void (*terminal_packet)(USBDevice *dev,USBPacket *packet);
};
/* True means this interface owns dispatch, including unsupported tokens;
 * it must never fall through to the generic EP0 engine a second time. */
bool usb_raw_packet_dispatch(USBDevice *dev,USBPacket *packet);
/* After core queue removal, called for QUEUED as well as ASYNC. True suppresses
 * the legacy class cancel callback, so a raw transport is notified only once. */
bool usb_raw_packet_cancel(USBDevice *dev,USBPacket *packet,bool was_async);
/* After COMPLETE/removal, BEFORE a port callback may free the packet. */
void usb_raw_packet_terminal(USBDevice *dev,USBPacket *packet);
#endif
