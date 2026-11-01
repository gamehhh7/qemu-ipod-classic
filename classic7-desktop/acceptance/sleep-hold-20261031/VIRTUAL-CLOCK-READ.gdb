set pagination off
set confirm off
ptype timers_state
p/x timers_state.qemu_icount
p/x timers_state.qemu_icount_bias
p timers_state.icount_time_shift
detach
quit
