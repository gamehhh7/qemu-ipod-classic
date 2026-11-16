set pagination off
set confirm off
set $m = (IPodClassicMachineState*)current_machine
printf "TIME\n"
p (int64_t)timers_state.qemu_icount_bias + (timers_state.qemu_icount << timers_state.icount_time_shift)
printf "TIMERS\n"
set $i=0
while $i<8
 p $m->timer->chan[$i]
 p *$m->timer->chan[$i].ptimer
 set $i=$i+1
end
printf "CPU\n"
p/x $m->cpu->env.regs
p/x $m->vic0->intenable
p/x $m->vic0->rawintr
p $m->wheel->fifo_len
p/x $m->wheel->buttons
detach
quit
