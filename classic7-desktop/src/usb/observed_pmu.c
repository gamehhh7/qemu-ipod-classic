/*
 * NXP PCF50635 power management unit, at I2C address 0xe6 (7-bit 0x73).
 *
 * The first chip Apple's retailOS talks to. 0x08325a20 starts the transfer and
 * then blocks on RTXC event 61 with nothing but a retry loop behind it, so an
 * iPod classic with nothing on I2C0 boots as far as its idle task and stays
 * there for ever -- which is exactly where this machine stopped before this
 * file existed.
 *
 * The bus protocol is a register-pointer device: a write starts with the
 * register number and the pointer advances after every byte; a read carries on
 * from wherever the pointer was left. That the pointer survives the end of a
 * transaction is not a guess -- Rockbox's i2c_rd() sends the register number in
 * one transaction and reads in a second one (i2c-s5l8702.c, i2c_wr_internal
 * with len 0 followed by i2c_rd_internal), and Apple's driver does the same.
 *
 * Register numbers are from Rockbox's firmware/export/pcf5063x.h, with the
 * three undocumented ones (INT6 0x85, INT6M 0x86, GPIOSTAT 0x87) from
 * firmware/target/arm/s5l8702/ipod6g/pmu-target.h.
 *
 * WHAT IS MODELLED
 *
 *   - The register file, which is what most of this chip is to a driver:
 *     regulator voltages, enables, interrupt masks and standby control get
 *     written and, when read back, have to read back.
 *   - The A/D converter. ADCC1 bit 0 starts a conversion; ADCS1/ADCS2/ADCS3
 *     return a 10-bit result with ADCRDY set. See the note on the value below.
 *   - The interrupt status registers INT1..INT5 and INT6, which are
 *     read-to-clear on the real chip and therefore must not be a plain
 *     register file: a driver that reads them expects the bits to go away.
 *   - RTC BCD calendar writes and its SECOND/ALARM interrupt sources use
 *     the configured RTC clock (see pmu_rtc.inc).
 *
 * WHAT IS NOT
 *
 *   - Charging, the ON/OFF controller and the regulators. Writing them changes
 *     nothing, because there is no voltage in here for a voltage to mean.
 *   - Other physical PMU interrupt sources besides HOLD remain unmodelled.
 *
 * ASSUMED, and marked as such because nothing derives them:
 *
 *   - VERSION (0x00) and VARIANT (0x01). No dump of a real PCF50635 exists in
 *     any of our sources, and neither Rockbox nor iPodLinux reads them. They
 *     answer zero. If the firmware turns out to compare them against something
 *     the disassembly will say what, and then they get a real value.
 *   - The battery reading, 844 out of 1023. Through Rockbox's own conversion
 *     (pmu-6g.c: raw * 2000 / 1023 + 2250) that is 3900 mV, an ordinary
 *     charged-but-not-full lithium cell. Nothing in our sources says what a
 *     real one reads; the number is chosen to be unremarkable.
 */

#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "sysemu/rtc.h"
#include "sysemu/sysemu.h"
#include "hw/arm/ipod_classic/ipod_classic_pmu.h"
#include "hw/arm/ipod_classic/ipod_classic_pmu_usb.h"

/*
 * PMUBATT=N overrides the raw battery reading, 0..1023.
 *
 * Why this needs to be reachable: on the way into a song retailOS puts up its
 * "low battery -- connect to power" screen and refuses to play, which is a
 * plausible reason for playback to start and immediately stop. The default 844
 * is 3900 mV only through ROCKBOX's conversion; what Apple's power manager makes
 * of the same number is UNDETERMINED, so the honest thing is to be able to vary
 * it and see, rather than to assume the two firmwares scale it alike.
 */
void ipod_i2c_journal_arm(bool connected);
void ipod_i2c_journal_pmu(const char *,uint8_t,uint8_t,bool,bool);
static int64_t wake_trace_until;
static unsigned wake_trace_rows;
static bool wake_trace_on(void)
{
    const char *e = getenv("CLASSIC7_WAKE_TRACE");
    return !e || strcmp(e, "0");
}
static void pmu_wake_trace(IPodClassicPMUState *s, const char *event,
                           uint8_t addr, uint8_t value)
{
    if (!wake_trace_on() || wake_trace_rows >= 4096) { return; }
    fprintf(stderr, "WAKEPMU n=%u event=%s reg=%02x value=%02x hold=%d"
            " int6=%02x mask6=%02x pending=%02x vns=%" PRId64 "\n",
            ++wake_trace_rows, event, addr, value, s->hold,
            s->reg[PCF50635_REG_INT6], s->reg[PCF50635_REG_INT6M],
            pmu_irq_pending(s->reg), qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
/* Opt-in bounded observation, including the byte actually returned to I2C.
 * This does not change ADC, register-pointer, charging or reset behaviour. */
static void pmu_power_trace(IPodClassicPMUState *s, const char *event,
                            uint8_t addr, uint8_t value)
{
    static uint32_t sequence;
    const char *enabled = getenv("CLASSIC7_POWER_TRACE");
    if (!enabled || strcmp(enabled, "1") || sequence >= 32768) {
        return;
    }
    fprintf(stderr, "POWERTRACE n=%u event=%s addr=%02x value=%02x"
            " pointer=%02x saved=%02x pending=%d wrote=%d usb=%d"
            " adc1=%02x adc2=%02x adc3=%02x result1=%02x result2=%02x result3=%02x"
            " vns=%" PRId64 " pc_approx=%08x lr=%08x\n",
            ++sequence, event, addr, value, s->addr, s->addr_saved,
            s->addr_pending, s->wrote_data, s->usb_connected,
            s->reg[PCF5063X_REG_ADCC1], s->reg[PCF5063X_REG_ADCC2],
            s->reg[PCF5063X_REG_ADCC3], s->reg[PCF5063X_REG_ADCS1],
            s->reg[PCF5063X_REG_ADCS2], s->reg[PCF5063X_REG_ADCS3],
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0,
            current_cpu ? ARM_CPU(current_cpu)->env.regs[14] : 0);
}
static uint32_t pmu_battery_raw(void)
{
    const char *e = getenv("PMUBATT");
    if (e && *e) {
        unsigned long v = strtoul(e, NULL, 0);
        if (v > 1023) {
            v = 1023;
        }
        return (uint32_t)v;
    }
    return S5L8702_PMU_ADC_BATTERY_RAW;
}

static void pmu_start_adc(IPodClassicPMUState *s)
{
    uint32_t raw = pmu_battery_raw();

    s->reg[PCF5063X_REG_ADCS1] = (raw >> 2) & 0xff;
    s->reg[PCF5063X_REG_ADCS2] = 0;
    s->reg[PCF5063X_REG_ADCS3] = PCF5063X_ADCS3_ADCRDY | (raw & 0x03);
}

/* PCF50635 nIRQ is active low; mask bits suppress the output, not status. */
static void pmu_update_irq(IPodClassicPMUState *s)
{
    uint8_t pending = pmu_irq_pending(s->reg);
    qemu_set_irq(s->nirq, !pending);
}

static void pmu_leave_warm_standby(IPodClassicPMUState *s)
{
    if (s->warm_standby) {
        s->warm_standby=false;
        qemu_set_irq(s->warm_standby_out,0);
    }
}

#include "pmu_rtc.inc"

static bool pmu_usb_initial_state(void)
{
    return pmu_usb_legacy_present(getenv("PMUCHARGER"));
}

static bool pmu_usb_get(Object *obj, Error **errp)
{
    return IPOD_CLASSIC_PMU(obj)->usb_connected;
}

static void pmu_usb_set(Object *obj, bool connected, Error **errp)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(obj);
    if (!pmu_usb_board_transition(&s->usb_connected, connected,
                            &s->reg[PCF5063X_REG_INT1],
                            &s->reg[PCF5063X_REG_INT2],
                            pmu_usb_exton2_enabled(getenv("IPOD_PMU_EXTON2")))) {
        return;
    }
    if (getenv("PMUTRACE")) {
        fprintf(stderr, "PMUUSB connected=%d INT1=%02x INT1M=%02x pending=%02x\n",
                connected, s->reg[PCF5063X_REG_INT1],
                s->reg[PCF5063X_REG_INT1M], pmu_irq_pending(s->reg));
    }
    ipod_i2c_journal_arm(connected);
    pmu_update_irq(s);
    qemu_set_irq(s->usb_vbus, connected);
    if (connected && (s->reg[0x0d]&0x44)) { pmu_leave_warm_standby(s); }
}
static void pmu_hold_input(void *opaque, int n, int level)
{
    IPodClassicPMUState *s = opaque;
    if (s->hold == !!level) { return; }
    s->hold = !!level;
    s->reg[PCF50635_REG_INT6] |= 0x02; /* GPIO2 input changed */
    if (wake_trace_on()) {
        wake_trace_rows = 0;
        wake_trace_until = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 30000000000LL;
        pmu_wake_trace(s, "hold_edge", PCF50635_REG_GPIOSTAT,
                       s->hold ? 0 : PCF50635_GPIOSTAT_HOLD_OFF);
    }
    if (getenv("PMUTRACE")) {
        fprintf(stderr, "PMUHOLD locked=%d INT6=%02x INT6M=%02x\n",
                s->hold, s->reg[PCF50635_REG_INT6], s->reg[PCF50635_REG_INT6M]);
    }
    pmu_update_irq(s);
    if (!s->hold && !(s->reg[PCF50635_REG_INT6M]&2)) {
        pmu_leave_warm_standby(s);
    }
}
/* Bounded, default-off USB-related PMU observations. No register semantics. */
static bool pmu_usb_trace_enabled(void)
{
    const char *e = getenv("PMUUSBTRACE");
    return e && !strcmp(e, "1");
}
static int pmu_usb_trace_index(uint8_t addr)
{
    switch (addr) {
    case PCF5063X_REG_INT1: return 0;
    case PCF5063X_REG_INT1M: return 1;
    case PCF5063X_REG_MBCS1: return 2;
    case PCF5063X_REG_MBCS2: return 3;
    case PCF5063X_REG_INT2: return 4;
    case PCF5063X_REG_INT2M: return 5;
    case PCF5063X_REG_OOCSTAT: return 6;
    default: return -1;
    }
}
static void pmu_usb_trace(IPodClassicPMUState *s, bool write, uint8_t addr,
                          uint8_t value, uint8_t int1_before, uint8_t mask_before,
                          uint8_t int2_before, uint8_t mask2_before)
{
    int index = pmu_usb_trace_index(addr);
    if (index < 0 || !pmu_usb_trace_enabled()) { return; }
    uint64_t seq = ++s->usb_trace_seq;
    if (write) { s->usb_trace_writes[index]++; }
    else { s->usb_trace_reads[index]++; }
    if (seq > 1024) {
        s->usb_trace_dropped++;
        if (seq == 1025) {
            fprintf(stderr, "PMUUSBTRACE overflow_begin seq=%" PRIu64
                    " limit=1024 counters_continue=1\n", seq);
        }
        return;
    }
    CPUState *cs = current_cpu;
    uint32_t tbpc = cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;
    uint32_t lr = cs ? ARM_CPU(cs)->env.regs[14] : 0;
    fprintf(stderr, "PMUUSBTRACE seq=%" PRIu64 " op=%s reg=%02x byte=%02x"
            " phase=%s int1_before=%02x mask_before=%02x int1_at_trace=%02x"
            " mask_at_trace=%02x int2_before=%02x mask2_before=%02x"
            " int2_at_trace=%02x mask2_at_trace=%02x"
            " cpu_context=%d observedTBpc~=%08x lr_snapshot=%08x\n",
            seq, write ? "W" : "R", addr, value,
            write ? "after-write" : "after-read-return-byte",
            int1_before, mask_before, s->reg[PCF5063X_REG_INT1],
            s->reg[PCF5063X_REG_INT1M], int2_before, mask2_before,
            s->reg[PCF5063X_REG_INT2], s->reg[PCF5063X_REG_INT2M],
            cs != NULL, tbpc, lr);
}
static char *pmu_usb_trace_summary(Object *obj, Error **errp)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(obj);
    return g_strdup_printf("{\"enabled\":%s,\"limit\":1024,\"seq\":%" PRIu64
        ",\"dropped\":%" PRIu64 ",\"regs\":[\"02\",\"07\",\"4b\",\"4c\",\"03\",\"08\",\"12\"],"
        "\"reads\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
        "\"writes\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]}",
        pmu_usb_trace_enabled() ? "true" : "false", s->usb_trace_seq,
        s->usb_trace_dropped, s->usb_trace_reads[0], s->usb_trace_reads[1],
        s->usb_trace_reads[2], s->usb_trace_reads[3], s->usb_trace_reads[4],
        s->usb_trace_reads[5], s->usb_trace_reads[6], s->usb_trace_writes[0],
        s->usb_trace_writes[1], s->usb_trace_writes[2], s->usb_trace_writes[3],
        s->usb_trace_writes[4], s->usb_trace_writes[5], s->usb_trace_writes[6]);
}
static uint8_t pmu_reg_read(IPodClassicPMUState *s, uint8_t addr)
{
    uint8_t val = s->reg[addr];
    uint8_t usb_int1_before = s->reg[PCF5063X_REG_INT1];
    uint8_t usb_mask_before = s->reg[PCF5063X_REG_INT1M];
    uint8_t usb_int2_before = s->reg[PCF5063X_REG_INT2];
    uint8_t usb_mask2_before = s->reg[PCF5063X_REG_INT2M];

    switch (addr) {
    case PCF5063X_REG_INT1:
    case PCF5063X_REG_INT2:
    case PCF5063X_REG_INT3:
    case PCF5063X_REG_INT4:
    case PCF5063X_REG_INT5:
    case PCF50635_REG_INT6:
        /* Read to clear, which is what makes an interrupt line drop. */
        val = pmu_int_read_clear(s->reg, addr);
        pmu_update_irq(s);
        break;
    case PCF50635_REG_GPIOSTAT:
        /* Physical GPIO2 input is read-only and inverse HOLD. */
        val = s->hold ? 0 : PCF50635_GPIOSTAT_HOLD_OFF;
        break;
    case PCF5063X_REG_OOCSTAT:
        /*
         * On/off-controller status, forced to a healthy powered value. Apple's
         * power manager polls this; with BATOK/SYSOK clear it concludes the
         * battery is empty and software-resets ("battery low", tag "--------").
         * A plain register file leaves it stale/zero. See the header.
         */
        val = pmu_usb_oocstat(PCF5063X_OOCSTAT_HEALTHY, s->usb_connected,
                             pmu_usb_exton2_enabled(getenv("IPOD_PMU_EXTON2")));
        break;
    case PCF5063X_REG_MBCS1:
        /*
         * Main-battery-charger status. The stale value reads as a spurious
         * TBAT_BELOW temperature fault; force the honest "temp OK, no charger"
         * state instead. See the header.
         */
        val = PCF5063X_MBCS1_HEALTHY;
        /*
         * PMUCHARGER=1: report external power present and OK.
         *
         * MEASURED: with the default "on battery, no charger" report the
         * retailOS eventually puts up "Low Battery -- Connect to Power" and
         * stops playing, and raising the battery ADC to full scale does NOT
         * prevent it (see the note above pmu_battery_raw). That is coherent --
         * it is asking to be plugged in, and nothing here ever says it is.
         *
         * An emulated iPod has no cell to discharge, so "sitting in a dock"
         * is at least as honest a description of it as "running on a battery
         * that never moves", and it is the one the firmware can act on. Kept
         * behind a variable because it IS a model choice, not a measurement:
         * which of USB or adapter a real device would report depends on what
         * it is plugged into, and we are plugged into nothing.
         */
        {
            const char *e = getenv("PMUCHARGER");
            if (e && *e) {
                /*
                 * WHICH KIND of external power matters, and getting it wrong
                 * was MEASURED: reporting the wall ADAPTER puts the device
                 * into its full-screen "Charging" mode and it stops playing --
                 * which is what a real iPod does on a charger, so the model was
                 * right and the choice was wrong. USB power is the one that
                 * leaves it usable. Default is USB; PMUCHARGER=adapter keeps
                 * the other one reachable for testing that screen on purpose.
                 */
                if (!strcmp(e, "adapter")) {
                    val |= PCF5063X_MBCS1_ADAPTPRES | PCF5063X_MBCS1_ADAPTOK;
                }
            }
        }
        val = pmu_usb_present_status(val, s->usb_connected);
        break;
    case PCF5063X_REG_RTCSC:
    case PCF5063X_REG_RTCSC + 1:
    case PCF5063X_REG_RTCSC + 2:
    case PCF5063X_REG_RTCSC + 3:
    case PCF5063X_REG_RTCSC + 4:
    case PCF5063X_REG_RTCSC + 5:
    case PCF5063X_REG_RTCSC + 6:
        /*
         * THE CLOCK RUNS. Measured (PMUTRACE=1): the firmware re-reads this
         * whole block, 0x59..0x5f, over and over, and a plain register file
         * answers zero every time -- a real part's seconds field cannot stay
         * still. That is visible on screen too: the Clock and Calendar screens
         * show a fixed nonsense date.
         *
         * PCF506xx keeps these fields in BCD, and the block is
         * second, minute, hour, weekday, day, month, year (year is 00..99).
         * The counter is seeded from QEMU's configured RTC date and clock;
         * guest writes then set its own epoch instead of being ignored.
         */
        {
            uint8_t calendar[7];
            pmu_rtc_calendar(s,calendar);
            val=calendar[addr-PCF5063X_REG_RTCSC];
        }
        break;
    default:
        break;
    }

    /*
     * PMUTRACE=1: which registers does Apple's power manager actually consult?
     *
     * Needed because raising the battery ADC to full scale (PMUBATT=1023) did
     * NOT stop retailOS putting up "low battery -- connect to power", so the
     * verdict is not coming from the channel this model answers with. Only the
     * PMU and the CS42L55 sit on I2C here, so it has to be some other register
     * of this part. Counting each address, and printing the first few reads of
     * each, says which ones without burying the firmware's own semihosting
     * output on the same stream.
     */
    if (getenv("PMUTRACE")) {
        static uint32_t seen[256];
        if (seen[addr]++ < 3) {
            fprintf(stderr, "PMUTRACE: read reg 0x%02x -> 0x%02x\n", addr, val);
        }
    }

    pmu_usb_trace(s, false, addr, val, usb_int1_before, usb_mask_before,
                  usb_int2_before, usb_mask2_before);
    pmu_power_trace(s, "read_return", addr, val);
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < wake_trace_until) {
        pmu_wake_trace(s, "read", addr, val);
    }
    return val;
}

static void pmu_reg_write_impl(IPodClassicPMUState *s, uint8_t addr, uint8_t val)
{
    if (addr>=0x59 && addr<=0x5f) {
        if (!s->rtc_writing) {
            pmu_rtc_calendar(s,s->reg+0x59);
            s->rtc_writing=true;
        }
        s->reg[addr]=val&rtc_field_mask[addr-0x59];
        return;
    }
    if (addr>=0x60 && addr<=0x66) { s->rtc_alarm_match=false; }
    switch (addr) {
    case 0x0c:
        s->reg[addr]=val;
        /* Observed retained-DRAM standby: 22000364(mode1) writes {0c,02}
         * after the kernel publishes its ehib boot descriptor. Mode0's
         * {0c,01} full-power-off command is a different operation. */
        if (val==2) {
            s->warm_standby=true;
            qemu_set_irq(s->warm_standby_out,1);
        }
        return;
    case PCF5063X_REG_INT1:
    case PCF5063X_REG_INT2:
    case PCF5063X_REG_INT3:
    case PCF5063X_REG_INT4:
    case PCF5063X_REG_INT5:
    case PCF50635_REG_INT6:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: INT%d is read-only\n", __func__,
                      addr == PCF50635_REG_INT6 ? 6 : addr - 1);
        return;
    case PCF5063X_REG_ADCC1:
        s->reg[addr] = val;
        if (val & PCF5063X_ADCC1_ADCSTART) {
            pmu_start_adc(s);
        }
        return;
    default:
        s->reg[addr] = val;
        if ((addr >= PCF5063X_REG_INT1M && addr <= PCF5063X_REG_INT5M) ||
            addr == PCF50635_REG_INT6M) {
            if (getenv("PMUTRACE")) {
                fprintf(stderr, "PMUMASK reg=%02x value=%02x\n", addr, val);
            }
            pmu_update_irq(s);
        }
        return;
    }
}

static void pmu_reg_write(IPodClassicPMUState *s, uint8_t addr, uint8_t val)
{
    if ((addr >= 0x07 && addr <= 0x12) || addr == PCF50635_REG_INT6M ||
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < wake_trace_until) {
        pmu_wake_trace(s, "write", addr, val);
    }
    pmu_power_trace(s, "write_register", addr, val);
    uint8_t int1_before = s->reg[PCF5063X_REG_INT1];
    uint8_t mask_before = s->reg[PCF5063X_REG_INT1M];
    uint8_t int2_before = s->reg[PCF5063X_REG_INT2];
    uint8_t mask2_before = s->reg[PCF5063X_REG_INT2M];
    pmu_reg_write_impl(s, addr, val);
    pmu_usb_trace(s, true, addr, val, int1_before, mask_before,
                  int2_before, mask2_before);
}

static int ipod_classic_pmu_event(I2CSlave *i2c, enum i2c_event event)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(i2c);

    pmu_power_trace(s, "bus_event", s->addr, event);
    ipod_i2c_journal_pmu("event",s->addr,event,s->addr_pending,s->wrote_data);
    switch (event) {
    case I2C_START_SEND:
        /* The next byte is the register number, not data. */
        s->addr_pending = true;
        /*
         * Remember where the pointer stood so a register write can put it back
         * at STOP; see the note above ipod_classic_pmu_send.
         */
        s->addr_saved = s->addr;
        s->wrote_data = false;
        break;
    case I2C_START_RECV:
        s->wrote_data = false;
        break;
    case I2C_FINISH:
        pmu_rtc_commit(s);
        /*
         * A register WRITE (an address byte followed by one or more data bytes)
         * must not leave the shared register pointer moved: another task may
         * have a read in flight that already set the pointer and is about to do
         * its own START_RECV. Restore it. An address-only "set the pointer"
         * write (no data byte) leaves the pointer where it was aimed, which is
         * exactly what the set-pointer-then-read pattern needs.
         */
        if (s->wrote_data) {
            s->addr = s->addr_saved;
        }
        s->wrote_data = false;
        break;
    case I2C_NACK:
        break;
    default:
        break;
    }
    return 0;
}

/*
 * THE REGISTER-POINTER RACE, and why the fix lives at STOP.
 *
 * MEASURED (temporary I2C-pointer trace): Apple's keycode dispatcher reads the
 * hold switch (GPIOSTAT, 0x87), and its power manager reads OOCSTAT (0x12) and
 * MBCS1 (0x4b), each as two I2C transactions -- one that sets the register
 * pointer, a STOP, then a one-byte read -- and the pointer survives the STOP by
 * design. Apple's GPIO housekeeping meanwhile WRITES register 0x87. When that
 * write lands between another read's set-pointer and its data phase, it moves
 * the shared pointer, so the read comes back with the wrong register: the hold
 * read saw bit 1 clear and SELECT was dropped (about five boots in eight), and
 * the OOCSTAT read saw BATOK clear and the firmware software-reset as "battery
 * empty" (about one navtest run in six).
 *
 * On real hardware the firmware serialises PMU access so the write never
 * interleaves; our timing does not reproduce that serialisation, so the model
 * compensates: a register write restores the pointer at STOP (above), which
 * un-clobbers whatever pointer a pending read had set. Combined with the forced
 * HOLD_OFF / healthy-status reads, the three status reads are now stable
 * regardless of interleaving. The pointer still auto-advances within a
 * transaction, so multi-byte reads and writes are unchanged.
 */
static uint8_t ipod_classic_pmu_recv(I2CSlave *i2c)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(i2c);
    ipod_i2c_journal_pmu("recv",s->addr,0,s->addr_pending,s->wrote_data);
    uint8_t val = pmu_reg_read(s, s->addr);

    s->addr++;
    return val;
}

static int ipod_classic_pmu_send(I2CSlave *i2c, uint8_t data)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(i2c);

    pmu_power_trace(s, "send_byte", s->addr, data);
    ipod_i2c_journal_pmu("send",s->addr,data,s->addr_pending,s->wrote_data);
    if (s->addr_pending) {
        s->addr = data;
        s->addr_pending = false;
    } else {
        pmu_reg_write(s, s->addr, data);
        s->addr++;
        s->wrote_data = true;
    }
    return 0;
}

static void ipod_classic_pmu_reset(DeviceState *dev)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->warm_standby=false;
    qemu_set_irq(s->warm_standby_out,0);
    memset(s->reg+0x60,0xff,7); /* Reset disables all alarm comparisons. */
    s->addr = 0;
    s->addr_pending = false;
    s->addr_saved = 0;
    s->wrote_data = false;
    pmu_start_adc(s);
    pmu_rtc_start(s);

    /*
     * THE HOLD SWITCH, and it is not decoration: with this bit at zero every
     * button press on the click wheel is silently thrown away.
     *
     * Two independent sources say what GPIOSTAT bit 1 means, and they agree.
     * Rockbox, firmware/target/arm/s5l8702/ipod6g/pmu-6g.c:244 --
     *
     *     pmu_input_holdswitch = !(pmu_read(PCF50635_REG_GPIOSTAT)
     *                                 & PCF50635_GPIOSTAT_GPIO2);
     *
     * -- so the bit SET means the hold switch is OFF and input is allowed.
     * Apple's retailOS, disassembled: 0x08294f80 reads one byte of register
     * 0x87 over I2C and stores (byte >> 1) & 1 ("mov r0,#0x87" / "bl I2C read"
     * / "and r0,r0,#2" / "lsr r0,r0,#1"); 0x080634a4 turns that into "1 - it"
     * ("rsbs r0,r0,#1" / "movcc r0,#0"); and the keycode dispatcher
     * 0x0803f390 calls that chain at 0x0803f3e0 -- through the tail branch
     * 0x08030d48, which is a bare "b 0x080634a4" -- and returns immediately
     * when the answer is non-zero ("cmp r0,#0" / "popne {r4,r5,r6,pc}" at
     * 0x0803f3e4). Same polarity, arrived at from the other end.
     *
     * (0x0803f3e0 is the 21st instruction of that function, not the second:
     * the entry first remaps the keycode through four compares. Nothing
     * depends on where it sits, but an earlier version of this comment said
     * "one instruction after the entry" and that was simply wrong.)
     *
     * MEASURED, before and after. With the register at 0 the wheel worked and
     * the buttons did not: holding ENTER down for three seconds and reading
     * the firmware's own state block showed 0x089116d8+0x1C = 1 (the packet
     * arrived) and +0x14 = 1 (the edge detector ran and called the dispatcher)
     * -- and the screen never changed, because the dispatcher dropped the key
     * one instruction later. The turn of the wheel does not pass through
     * 0x0803f390 at all, which is exactly why it was not affected and why the
     * failure looked like a wheel bug rather than a hold switch.
     *
     * CONVENIENCE VALUE, and marked as one: a machine with no hold switch to
     * flip has to boot with it off or it cannot be used. The other two bits
     * stay zero -- Rockbox marks both "TBC" and nothing we have reads them.
     */
    s->reg[PCF50635_REG_GPIOSTAT] = PCF50635_GPIOSTAT_HOLD_OFF;

    /*
     * Power-status registers seeded to their healthy powered values so a
     * read-back before the first poll is coherent; pmu_reg_read forces them on
     * every read anyway (see the OOCSTAT/MBCS1 cases and the header note). This
     * is what stops Apple's power manager from deciding the battery is empty and
     * software-resetting to the "--------" image a few seconds into the boot.
     */
    s->reg[PCF5063X_REG_OOCSTAT] = pmu_usb_oocstat(PCF5063X_OOCSTAT_HEALTHY,
        s->usb_connected, pmu_usb_exton2_enabled(getenv("IPOD_PMU_EXTON2")));
    s->reg[PCF5063X_REG_MBCS1] = PCF5063X_MBCS1_HEALTHY;
    pmu_update_irq(s);
}

static void pcf50635_instance_init(Object *obj)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(obj);
    s->rtc_timer=timer_new_ns(rtc_clock,pmu_rtc_tick,s);
    qdev_init_gpio_out_named(DEVICE(obj),&s->warm_standby_out,"warm-standby",1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->nirq, "nirq", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->usb_vbus, "usb-vbus", 1);
    qdev_init_gpio_in_named(DEVICE(obj), pmu_hold_input, "hold", 1);
    /* Initial USB power presence remains legacy-compatible; no boot edge. */
    s->usb_connected = pmu_usb_initial_state();
    object_property_add_bool(obj, "usb-connected", pmu_usb_get, pmu_usb_set);
    object_property_add_str(obj, "usb-trace-summary", pmu_usb_trace_summary, NULL);
    object_property_set_description(obj, "usb-connected",
        "Physical USB cable presence; transitions latch PCF50635 USBINS/USBREM");
}
static int pmu_post_load(void *opaque, int version_id)
{
    IPodClassicPMUState *s=opaque;
    if (version_id<6) { s->warm_standby=false; }
    qemu_set_irq(s->warm_standby_out,s->warm_standby);
    if (version_id<5) { pmu_rtc_start(opaque); }
    if (version_id < 4) {
        s->usb_connected = pmu_usb_initial_state();
    }
    pmu_update_irq(opaque);
    return 0;
}
static const VMStateDescription vmstate_ipod_classic_pmu = {
    .name = TYPE_IPOD_CLASSIC_PMU,
    .version_id = 6,
    .post_load = pmu_post_load,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, IPodClassicPMUState),
        VMSTATE_UINT8_ARRAY(reg, IPodClassicPMUState, S5L8702_PMU_NUM_REGS),
        VMSTATE_UINT8(addr, IPodClassicPMUState),
        VMSTATE_BOOL(addr_pending, IPodClassicPMUState),
        VMSTATE_UINT8(addr_saved, IPodClassicPMUState),
        VMSTATE_BOOL(wrote_data, IPodClassicPMUState),
        VMSTATE_BOOL_V(hold, IPodClassicPMUState, 3),
        VMSTATE_BOOL_V(usb_connected, IPodClassicPMUState, 4),
        VMSTATE_INT64_V(rtc_epoch, IPodClassicPMUState, 5),
        VMSTATE_INT64_V(rtc_base_ns, IPodClassicPMUState, 5),
        VMSTATE_UINT8_V(rtc_weekday_offset, IPodClassicPMUState, 5),
        VMSTATE_BOOL_V(rtc_initialized, IPodClassicPMUState, 5),
        VMSTATE_BOOL_V(rtc_writing, IPodClassicPMUState, 5),
        VMSTATE_BOOL_V(rtc_alarm_match, IPodClassicPMUState, 5),
        VMSTATE_TIMER_PTR_V(rtc_timer, IPodClassicPMUState, 5),
        VMSTATE_BOOL_V(warm_standby, IPodClassicPMUState, 6),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_pmu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->desc = "NXP PCF50635 power management unit";
    dc->reset = ipod_classic_pmu_reset;
    dc->vmsd = &vmstate_ipod_classic_pmu;
    k->event = ipod_classic_pmu_event;
    k->recv = ipod_classic_pmu_recv;
    k->send = ipod_classic_pmu_send;
}

static const TypeInfo ipod_classic_pmu_info = {
    .name          = TYPE_IPOD_CLASSIC_PMU,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(IPodClassicPMUState),
    .class_init    = ipod_classic_pmu_class_init,
    .instance_init = pcf50635_instance_init,
};

static void ipod_classic_pmu_register_types(void)
{
    type_register_static(&ipod_classic_pmu_info);
}

type_init(ipod_classic_pmu_register_types)
