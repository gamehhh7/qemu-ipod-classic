#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
static struct tm *gmtime_r(const time_t *v,struct tm *out){gmtime_s(out,v);return out;}
#define NANOSECONDS_PER_SECOND 1000000000LL
#define PCF5063X_REG_INT1 2
#define PCF5063X_REG_INT1M 7
#define rtc_clock 0
#define mktimegm _mkgmtime64
static int64_t test_ns;
static int64_t qemu_clock_get_ns(int clock){return test_ns;}
typedef int QEMUTimer;
static void timer_mod(QEMUTimer *t,int64_t when){}
static void qemu_get_timedate(struct tm *tm,int offset){memset(tm,0,sizeof(*tm));tm->tm_year=124;tm->tm_mon=1;tm->tm_mday=28;tm->tm_hour=23;tm->tm_min=59;tm->tm_sec=59;}
typedef struct {uint8_t reg[256];QEMUTimer *rtc_timer;int64_t rtc_epoch,rtc_base_ns;uint8_t rtc_weekday_offset;bool rtc_initialized,rtc_writing,rtc_alarm_match,warm_standby;} IPodClassicPMUState;
static bool irq;
static void pmu_update_irq(IPodClassicPMUState*s){irq=(s->reg[2]&~s->reg[7])!=0;}
static void pmu_leave_warm_standby(IPodClassicPMUState *s){s->warm_standby=false;}
#include "../../src/usb/pmu_rtc.inc"
int main(void){IPodClassicPMUState s={0};uint8_t t[7];memset(s.reg+0x60,255,7);s.reg[7]=0x80;pmu_rtc_start(&s);test_ns=1000000000LL;pmu_rtc_tick(&s);pmu_rtc_calendar(&s,t);assert(t[0]==0&&t[4]==0x29&&t[5]==2);assert(s.reg[2]==0x80&&!irq);
 s.warm_standby=true;s.reg[0x0d]=0x10;s.reg[0x60]=1;s.reg[0x63]=7;s.rtc_alarm_match=false;test_ns+=1000000000LL;pmu_rtc_tick(&s);assert((s.reg[2]&0x40)&&irq&&!s.warm_standby);s.reg[2]=0;pmu_update_irq(&s);assert(!irq);test_ns+=1000000000LL;pmu_rtc_tick(&s);assert(!(s.reg[2]&0x40)&&!irq);
 uint8_t wanted[7]={5,0x15,8,6,0x31,0x10,0x26};memcpy(s.reg+0x59,wanted,7);s.rtc_writing=true;pmu_rtc_commit(&s);test_ns+=3000000000LL;pmu_rtc_calendar(&s,t);assert(t[0]==8&&t[1]==0x15&&t[2]==8&&t[3]==6&&t[4]==0x31&&t[5]==0x10&&t[6]==0x26);
 int64_t before=s.rtc_epoch;memcpy(s.reg+0x59,wanted,7);s.reg[0x5e]=2;s.rtc_writing=true;pmu_rtc_commit(&s);assert(s.rtc_epoch==before);
 puts("PASS RTC leap-day rollover, masked SECOND, alarm weekday wildcard/nIRQ, read-clear, clock write/advance, invalid date rejection");return 0;}
