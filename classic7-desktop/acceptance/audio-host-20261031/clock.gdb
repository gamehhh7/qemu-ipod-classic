set pagination off
set confirm off
set print elements 8
attach 1824
set $audio_m = (IPodClassicMachineState*)current_machine
p $audio_m->i2s->next_tick_ns
p $audio_m->i2s->tx_samples
p timers_state
detach
quit
