set pagination off
set confirm off
set $m = (IPodClassicMachineState*)current_machine
printf "CPU_INTERRUPT\n"
p/x $m->cpu->parent_obj.interrupt_request
p/x $m->cpu->env.daif
printf "VIC0\n"
p/x $m->vic0->irq_status
p/x $m->vic0->rawintr
p/x $m->vic0->intenable
p/x $m->vic0->intselect
p/x $m->vic0->priority
p/x $m->vic0->current
p/x $m->vic0->sw_priority_mask
p $m->vic0->stack_i
printf "VIC1\n"
p/x $m->vic1->irq_status
p/x $m->vic1->rawintr
p/x $m->vic1->intenable
p/x $m->vic1->priority
p $m->vic1->stack_i
printf "FIFO\n"
p/x $m->wheel->fifo
p $m->wheel->fifo_head
printf "IRAM_DUMP\n"
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 if $_streq($b->idstr,"iram0")
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261030/live-iram0.bin $b->host ($b->host+0x20000)
 end
 set $b = $b->next.le_next
 set $i = $i+1
end
