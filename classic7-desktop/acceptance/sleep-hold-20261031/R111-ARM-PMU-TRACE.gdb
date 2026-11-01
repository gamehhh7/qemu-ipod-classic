set pagination off
set confirm off
printf "DIAGNOSTIC_TRACE_ONLY\n"
p wake_trace_until
p wake_trace_rows
set variable wake_trace_rows = 0
set variable wake_trace_until = (int64_t)timers_state.qemu_icount_bias + (timers_state.qemu_icount << timers_state.icount_time_shift) + 180000000000
p wake_trace_until
detach
quit
