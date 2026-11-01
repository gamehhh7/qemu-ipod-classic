set pagination off
set confirm off
set logging file K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/EIC6-CALLER-TRACE.txt
set logging overwrite on
set logging enabled on
set $seen = 0
break sysalv_write if offset == 0xd8
commands
silent
set $m = (IPodClassicMachineState*)current_machine
printf "EIC6_WRITE value=%x guestpc=%x lr=%x r0=%x r1=%x r2=%x r3=%x\n",value,$m->cpu->env.regs[15],$m->cpu->env.regs[14],$m->cpu->env.regs[0],$m->cpu->env.regs[1],$m->cpu->env.regs[2],$m->cpu->env.regs[3]
set $seen = $seen+1
if $seen >= 4
 detach
 quit
end
continue
end
continue
