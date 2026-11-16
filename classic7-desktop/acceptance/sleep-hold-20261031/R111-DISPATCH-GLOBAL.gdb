set pagination off
set confirm off
set $b=ram_list.blocks.lh_first
set $i=0
while $b && $i<24
 if $_streq($b->idstr,"sdram")
  printf "WHEEL_FIRMWARE_GLOBAL\n"
  x/12wx ($b->host+0x9ca550)
  printf "KEYDISPATCH_GLOBAL\n"
  x/12wx ($b->host+0x9ca864)
  printf "KEY_ROTATE_GLOBAL\n"
  x/4wx ($b->host+0x9caf50)
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R111-wheel-global.bin ($b->host+0x9ca550) ($b->host+0x9ca580)
 end
 set $b=$b->next.le_next
 set $i=$i+1
end
detach
quit
