set pagination off
set confirm off
set $task_ram=ram_list.blocks.lh_first
while $task_ram
 if $_streq($task_ram->idstr,"sdram")
  printf "SLEEP_HEADER_AND_CONTEXT\n"
  x/12wx $task_ram->host
  x/8wx ($task_ram->host+0xa00000+0x478)
  dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R115-FIRST-LONG-SAVE.bin ($task_ram->host+0xa25940) ($task_ram->host+0xa4d940)
 end
 set $task_ram=$task_ram->next.le_next
end
detach
quit
