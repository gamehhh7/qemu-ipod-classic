set pagination off
set confirm off
set print elements 24
set $m = (IPodClassicMachineState*)current_machine
printf "NATURAL_ACCEPT_BEFORE\n"
p/x $m->cpu->env.regs
p/x $m->cpu->env.uncached_cpsr
p/x $m->cpu->env.daif
p/x $m->cpu->parent_obj.interrupt_request
p $m->cpu->parent_obj.halted
printf "CLOCK\n"
p/x $m->clock0->reg
printf "VIC\n"
p/x $m->vic0->rawintr
p/x $m->vic0->intenable
p/x $m->vic0->irq_status
p/x $m->vic0->current
p/x $m->vic0->priority
p $m->vic0->stack_i
printf "WHEEL\n"
p/x $m->wheel->con
p $m->wheel->fifo_len
p $m->wheel->hold
p/x $m->wheel->mask
p $m->wheel->irq_level
printf "EIC\n"
set $r = system_memory->subregions.tqh_first
set $i = 0
while $r && $i < 128
 if $_streq($r->name,"s5l8702.sysalv")
  set $e = (IPodClassicSysalvState*)$r->opaque
  p/x $e->alvcon
  p/x $e->level
  p/x $e->type
  p/x $e->enable
  p/x $e->input
  p/x $e->valid
  p/x $e->edge
 end
 set $r = $r->subregions_link.tqe_next
 set $i = $i+1
end
printf "GPIO\n"
set $r=system_memory->subregions.tqh_first
while $r
 if $_streq($r->name,"s5l8702.gpio")
  set $gp=(IPodClassicGPIOState*)$r->opaque
  p/x $gp->pcon
  p/x $gp->pdat
  p/x $gp->puna
  p/x $gp->punb
 end
 set $r=$r->subregions_link.tqe_next
end
printf "RAM\n"
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 if $_streq($b->idstr,"iram0")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/20261031T045410Z-R116-after-enter-iram0.bin $b->host ($b->host+0x20000)
 end
 if $_streq($b->idstr,"sdram")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/20261031T045410Z-R116-after-enter-code.bin $b->host ($b->host+0x400000)
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/20261031T045410Z-R116-after-enter-kernel.bin ($b->host+0xa00000) ($b->host+0xb00000)
 end
 set $b = $b->next.le_next
 set $i = $i+1
end
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 if $_streq($b->idstr,"sdram")
  printf "SYSINFO_BOOT_COPY\n"
  x/12wx ($b->host+0xa11cd8+0xd8)
  printf "SYSINFO_CACHE\n"
  x/4wx ($b->host+0x9caaac)
 end
 if $_streq($b->idstr,"iram1")
  printf "SYSINFO_HANDOFF\n"
  x/12wx ($b->host+0x1fde0+0xd8)
 end
 set $b = $b->next.le_next
 set $i = $i+1
end
printf "POWER_DOMAIN\n"
p $m->cpu->power_state
p $m->warm_standby
printf "PMU_STATE\n"
set $task_bus_child=$m->i2c0->bus->qbus.children.tqh_first
while $task_bus_child
 set $task_slave=(I2CSlave*)$task_bus_child->child
 if $task_slave->address==0x73
  set $task_pmu=(IPodClassicPMUState*)$task_slave
  p $task_pmu->warm_standby
  p $task_pmu->usb_connected
  x/24bx $task_pmu->reg
  x/7bx ($task_pmu->reg+0x60)
 end
 set $task_bus_child=$task_bus_child->sibling.tqe_next
end
set $task_ram=ram_list.blocks.lh_first
while $task_ram
 if $_streq($task_ram->idstr,"sdram")
  printf "HIBERNATION_DESCRIPTOR\n"
  x/8wx $task_ram->host
  x/4wx ($task_ram->host+0xa0fc80)
 end
 set $task_ram=$task_ram->next.le_next
end
detach
quit

