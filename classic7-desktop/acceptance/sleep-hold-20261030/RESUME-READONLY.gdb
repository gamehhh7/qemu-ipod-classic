set pagination off
set confirm off
info address __wrap_arm_cpu_do_interrupt
info variables wake_observer.c
p __wrap_arm_cpu_do_interrupt::enabled
p __wrap_arm_cpu_do_interrupt::count
set $m = (IPodClassicMachineState*)current_machine
p/x $m->cpu->env.regs
p/x $m->cpu->env.exception
p $m->cpu->parent_obj.halted
detach
quit
