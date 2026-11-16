/* PCF50635 hardware USB cable edge semantics, independent of guest software. */
#ifndef HW_IPOD_CLASSIC_PMU_USB_H
#define HW_IPOD_CLASSIC_PMU_USB_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PCF5063X_INT1_USBINS 0x04
#define PCF5063X_INT1_USBREM 0x08

/* Rockbox ipod6g board: EXTON2 is the physical USB VBUS input. */
#define PCF5063X_OOCSTAT_EXTON2 0x04
#define PCF5063X_INT2_EXTON2R 0x10
#define PCF5063X_INT2_EXTON2F 0x20

static inline bool pmu_usb_exton2_enabled(const char *flag)
{
    return flag && !strcmp(flag, "1");
}

static inline uint8_t pmu_usb_oocstat(uint8_t status, bool connected,
                                     bool exton2_enabled)
{
    if (!exton2_enabled) {
        return status;
    }
    return (status & ~PCF5063X_OOCSTAT_EXTON2) |
           (connected ? PCF5063X_OOCSTAT_EXTON2 : 0);
}

static inline bool pmu_usb_legacy_present(const char *charger)
{
    return charger && *charger && strcmp(charger, "adapter");
}

static inline uint8_t pmu_int_read_clear(uint8_t reg[256], unsigned addr)
{
    uint8_t value = reg[addr];
    reg[addr] = 0;
    return value;
}

static inline bool pmu_usb_transition(bool *connected, bool next, uint8_t *int1)
{
    if (*connected == next) {
        return false;
    }
    *connected = next;
    *int1 |= next ? PCF5063X_INT1_USBINS : PCF5063X_INT1_USBREM;
    return true;
}

static inline bool pmu_usb_board_transition(bool *connected, bool next,
                                           uint8_t *int1, uint8_t *int2,
                                           bool exton2_enabled)
{
    if (!pmu_usb_transition(connected, next, int1)) {
        return false;
    }
    if (exton2_enabled) {
        *int2 |= next ? PCF5063X_INT2_EXTON2R : PCF5063X_INT2_EXTON2F;
    }
    return true;
}

static inline uint8_t pmu_usb_present_status(uint8_t status, bool connected)
{
    return (status & ~0x03) | (connected ? 0x03 : 0);
}

/* INT1..5 at 02..06, masks at 07..0b; INT6/INT6M at85/86. */
static inline uint8_t pmu_irq_pending(const uint8_t reg[256])
{
    uint8_t pending = reg[0x85] & ~reg[0x86];
    for (unsigned i = 0; i < 5; i++) {
        pending |= reg[0x02 + i] & ~reg[0x07 + i];
    }
    return pending;
}
#endif
