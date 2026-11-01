set pagination off
set confirm off
set print elements 32
printf "MMIO_INPUT_STATE\n"
set $r = system_memory->subregions.tqh_first
set $i = 0
while $r && $i < 128
 if $_streq($r->name,"s5l8702.sysalv")
  set $e = (IPodClassicSysalvState*)$r->opaque
  printf "EIC\n"
  p/x $e->level
  p/x $e->type
  p/x $e->enable
  p/x $e->input
  p/x $e->valid
  p/x $e->edge
  p/x $e->alvcon
 end
 if $_streq($r->name,"s5l8702.gpio")
  set $g = (IPodClassicGPIOState*)$r->opaque
  printf "GPIO14\n"
  p/x $g->pcon[14]
  p/x $g->pdat[14]
 end
 set $r = $r->subregions_link.tqe_next
 set $i = $i+1
end
