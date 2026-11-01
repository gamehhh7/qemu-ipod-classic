set pagination off
set confirm off
set $task_m=(IPodClassicMachineState*)current_machine
p/x $task_m->cpu->env.cp15.sctlr_el[1]
p/x $task_m->cpu->env.cp15.ttbr0_el[1]
p/x $task_m->cpu->env.regs[15]
detach
quit
