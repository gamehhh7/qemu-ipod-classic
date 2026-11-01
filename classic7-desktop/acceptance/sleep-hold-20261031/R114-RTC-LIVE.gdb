set pagination off
set confirm off
break pmu_rtc_calendar
commands
silent
printf "RTC_NATIVE\n"
p s->rtc_initialized
p s->rtc_epoch
p s->rtc_base_ns
p s->rtc_timer->expire_time
x/7bx (s->reg+0x60)
x/12bx s->reg
detach
quit
end
continue
