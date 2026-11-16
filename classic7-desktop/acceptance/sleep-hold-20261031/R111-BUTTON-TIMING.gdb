set pagination off
set confirm off
set logging file K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R111-BUTTON-TIMING.txt
set logging overwrite on
set logging enabled on
set $seen=0
break frontend_buttons_set
commands
silent
printf "BUTTON value=%s virtual_ns=%lld\n",text,(int64_t)timers_state.qemu_icount_bias + (timers_state.qemu_icount << timers_state.icount_time_shift)
set $seen=$seen+1
if $seen>=2
 detach
 quit
end
continue
end
continue
