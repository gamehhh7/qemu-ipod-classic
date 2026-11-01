set pagination off
set confirm off
set print elements 24
set $m = (IPodClassicMachineState*)current_machine
set $w = $m->wheel
printf "GUEST_REGS\n"
p/x $m->cpu->env.regs
p/x $m->cpu->env.uncached_cpsr
p $m->cpu->parent_obj.halted
printf "WHEEL_STATE\n"
p/x $w->con
p/x $w->mask
p/x $w->pend
p $w->irq_level
p $w->fifo_len
p/x $w->buttons
p $w->hold
p $w->touched
p $w->armed
p $w->pending_steps
p/x $w->sent_buttons
p wheel_packets
p wheel_dropped
p wheel_commands
p wheel_irq_raises
printf "CLOCK_REGS\n"
p/x $m->clock0->reg
printf "RAM_BLOCKS\n"
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 printf "%s host=%p size=%llu\n",$b->idstr,$b->host,$b->used_length
 if $_streq($b->idstr,"sdram")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261030/live-code.bin $b->host ($b->host+0x400000)
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261030/live-kernel.bin ($b->host+0xa00000) ($b->host+0xb00000)
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261030/live-wheel.bin ($b->host+0x900000) ($b->host+0x920000)
 end
 set $b = $b->next.le_next
 set $i = $i+1
end
