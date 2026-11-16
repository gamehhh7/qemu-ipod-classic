set pagination off
set confirm off
set logging file K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R112-EIC-ARM-CALLER.txt
set logging overwrite on
set logging enabled on
break sysalv_write if offset==0xd8 && value!=0
commands
silent
set $m=(IPodClassicMachineState*)current_machine
printf "EIC6_ARM value=%x\n",value
p/x $m->cpu->env.regs
set $b=ram_list.blocks.lh_first
while $b
 if $_streq($b->idstr,"sdram")
  x/96wx ($b->host+$m->cpu->env.regs[13]-0x08000000)
 end
 set $b=$b->next.le_next
end
detach
quit
end
continue
