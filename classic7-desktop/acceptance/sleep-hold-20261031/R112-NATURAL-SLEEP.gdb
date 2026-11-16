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
printf "RAM\n"
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 if $_streq($b->idstr,"iram0")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R112-NATURAL-SLEEP-iram0.bin $b->host ($b->host+0x20000)
 end
 if $_streq($b->idstr,"sdram")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R112-NATURAL-SLEEP-code.bin $b->host ($b->host+0x400000)
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R112-NATURAL-SLEEP-kernel.bin ($b->host+0xa00000) ($b->host+0xb00000)
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
detach
quit

