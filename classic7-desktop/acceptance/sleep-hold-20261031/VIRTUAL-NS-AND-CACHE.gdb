set pagination off
set confirm off
printf "VIRTUAL_NS\n"
p (int64_t)timers_state.qemu_icount_bias + (timers_state.qemu_icount << timers_state.icount_time_shift)
printf "IRAM_SAVE_CACHE\n"
set $b = ram_list.blocks.lh_first
set $i = 0
while $b && $i < 24
 if $_streq($b->idstr,"sdram")
  x/wx ($b->host+0x9caab8)
 end
 set $b = $b->next.le_next
 set $i = $i+1
end
detach
quit
