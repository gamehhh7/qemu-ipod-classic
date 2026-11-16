/* PCF50633/35 UM rev06 section8.15: BCD calendar; an alarm field's
 * all-one reset value disables that comparison. Linux rtc-pcf50633 also
 * uses weekday7 to disable weekday comparison. Only the real PMU nIRQ
 * path is driven, through INT1 status and the existing mask logic. */
static const uint8_t rtc_field_mask[7] = {0x7f,0x7f,0x3f,7,0x3f,0x1f,0xff};

static uint8_t rtc_bcd(unsigned n) { return (n / 10) * 16 + n % 10; }
static unsigned rtc_unbcd(uint8_t n) { return (n >> 4) * 10 + (n & 15); }

static void pmu_rtc_calendar(IPodClassicPMUState *s, uint8_t out[7])
{
    int64_t now = qemu_clock_get_ns(rtc_clock);
    time_t epoch = s->rtc_epoch + (now - s->rtc_base_ns) / NANOSECONDS_PER_SECOND;
    struct tm tm;
    gmtime_r(&epoch, &tm);
    out[0]=rtc_bcd(tm.tm_sec); out[1]=rtc_bcd(tm.tm_min);
    out[2]=rtc_bcd(tm.tm_hour);
    out[3]=(tm.tm_wday+s->rtc_weekday_offset)%7;
    out[4]=rtc_bcd(tm.tm_mday); out[5]=rtc_bcd(tm.tm_mon+1);
    out[6]=rtc_bcd(tm.tm_year%100);
}

static void pmu_rtc_commit(IPodClassicPMUState *s)
{
    uint8_t *r = s->reg+0x59;
    struct tm tm = {0}, checked;
    time_t epoch;
    if (!s->rtc_writing) { return; }
    s->rtc_writing=false;
    for (unsigned i=0;i<7;i++) {
        if ((r[i]&15)>9 || (r[i]>>4)>9) { return; }
    }
    tm.tm_sec=rtc_unbcd(r[0]); tm.tm_min=rtc_unbcd(r[1]);
    tm.tm_hour=rtc_unbcd(r[2]); tm.tm_mday=rtc_unbcd(r[4]);
    tm.tm_mon=rtc_unbcd(r[5])-1; tm.tm_year=rtc_unbcd(r[6])+100;
    if (tm.tm_sec>59 || tm.tm_min>59 || tm.tm_hour>23 || r[3]>6 ||
        tm.tm_mday<1 || tm.tm_mday>31 || tm.tm_mon<0 || tm.tm_mon>11) { return; }
    epoch=mktimegm(&tm);
    gmtime_r(&epoch,&checked);
    if (checked.tm_sec!=rtc_unbcd(r[0]) || checked.tm_min!=rtc_unbcd(r[1]) ||
        checked.tm_hour!=rtc_unbcd(r[2]) || checked.tm_mday!=rtc_unbcd(r[4]) ||
        checked.tm_mon!=rtc_unbcd(r[5])-1 || checked.tm_year!=rtc_unbcd(r[6])+100) { return; }
    s->rtc_epoch=epoch;
    s->rtc_base_ns=qemu_clock_get_ns(rtc_clock);
    s->rtc_weekday_offset=(r[3]+7-checked.tm_wday)%7;
    s->rtc_alarm_match=false;
}

static void pmu_rtc_tick(void *opaque)
{
    IPodClassicPMUState *s=opaque;
    uint8_t current[7];
    bool match=true, enabled=false;
    pmu_rtc_calendar(s,current);
    for (unsigned i=0;i<7;i++) {
        unsigned a=s->reg[0x60+i]&rtc_field_mask[i];
        if (a!=rtc_field_mask[i]) {
            enabled=true;
            if (a!=current[i]) { match=false; }
        }
    }
    match &= enabled;
    s->reg[PCF5063X_REG_INT1] |= 0x80; /* SECOND latches even while masked */
    if (match && !s->rtc_alarm_match) {
        s->reg[PCF5063X_REG_INT1] |= 0x40;
        if (s->reg[0x0d]&0x10) { pmu_leave_warm_standby(s); }
        fprintf(stderr,"PMURTC alarm matched calendar=%02x:%02x:%02x mask1=%02x\n",
                current[2],current[1],current[0],s->reg[PCF5063X_REG_INT1M]);
    }
    s->rtc_alarm_match=match;
    pmu_update_irq(s);
    int64_t now=qemu_clock_get_ns(rtc_clock);
    int64_t elapsed=now-s->rtc_base_ns;
    timer_mod(s->rtc_timer,now+NANOSECONDS_PER_SECOND-
              (elapsed%NANOSECONDS_PER_SECOND));
}

static void pmu_rtc_start(IPodClassicPMUState *s)
{
    if (!s->rtc_initialized) {
        struct tm tm;
        qemu_get_timedate(&tm,0);
        s->rtc_epoch=mktimegm(&tm);
        s->rtc_base_ns=qemu_clock_get_ns(rtc_clock);
        s->rtc_weekday_offset=0;
        s->rtc_initialized=true;
    }
    s->rtc_writing=false;
    s->rtc_alarm_match=false;
    timer_mod(s->rtc_timer,qemu_clock_get_ns(rtc_clock)+NANOSECONDS_PER_SECOND);
}
