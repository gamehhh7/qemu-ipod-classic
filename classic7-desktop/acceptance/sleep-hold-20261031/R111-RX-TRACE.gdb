set pagination off
set confirm off
set logging file K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R111-RX-TRACE.txt
set logging overwrite on
set logging enabled on
set $seen = 0
break ipod_classic_wheel_read if offset == 0x18
commands
silent
set $m = (IPodClassicMachineState*)current_machine
set $w = $m->wheel
printf "RX word=%x fifo=%d guestpc=%x lr=%x cpsr=%x\n",$w->fifo[$w->fifo_head],$w->fifo_len,$m->cpu->env.regs[15],$m->cpu->env.regs[14],$m->cpu->env.uncached_cpsr
set $seen=$seen+1
if $seen>=3
 detach
 quit
end
continue
end
continue
