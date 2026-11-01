set pagination off
set confirm off
set $m=(IPodClassicMachineState*)current_machine
set $task_bus_child=$m->i2c0->bus->qbus.children.tqh_first
while $task_bus_child
 set $sl=(I2CSlave*)$task_bus_child->child
 if $sl->address==0x73
  set $pm=(IPodClassicPMUState*)$sl
  printf "PMU_IRQ_STATUS_MASKS\n"
  x/20bx $pm->reg
  printf "RTC\n"
  x/16bx ($pm->reg+0x59)
 end
 set $task_bus_child=$task_bus_child->sibling.tqe_next
end
detach
quit
