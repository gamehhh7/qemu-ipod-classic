set pagination off
set confirm off
set $m = (IPodClassicMachineState*)current_machine
printf "OBSERVER_ENABLED\n"
p __wrap_arm_cpu_do_interrupt::enabled
p __wrap_arm_cpu_do_interrupt::count
printf "CPU_STATE\n"
p/x $m->cpu->env.regs
p/x $m->cpu->env.exception
p $m->cpu->parent_obj.halted
p/x $m->clock0->reg
printf "WHEEL\n"
p $m->wheel->fifo_len
p $m->wheel->hold
p/x $m->vic0->intenable
p/x $m->vic0->rawintr
printf "RAM_BLOCKS\n"
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 if $_streq($b->idstr,"sdram")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/baseline-code.bin $b->host ($b->host+0x400000)
 end
 set $b = $b->next.le_next
 set $i = $i+1
end
detach
quit
