set pagination off
set confirm off
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
