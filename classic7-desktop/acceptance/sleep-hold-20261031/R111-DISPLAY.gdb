set pagination off
set confirm off
set $m=(IPodClassicMachineState*)current_machine
printf "LCD\n"
p $m->lcd->display_on
p $m->lcd->composed_frames
p/x $m->lcd->con_reg
p/x $m->lcd->intcon
p/x $m->lcd->fr_hold
printf "DISPREGS\n"
p/x $m->disp->reg[2]
x/40wx &$m->disp->reg[0x58/4]
dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R111-LCD.raw &$m->lcd->fb[0] (&$m->lcd->fb[0]+76800)
set $b=ram_list.blocks.lh_first
set $i=0
while $b && $i<24
 if $_streq($b->idstr,"sdram")
  set $k=0
  while $k<3
   set $src=$m->disp->reg[(0x60+$k*0x18)/4]
   printf "LAYER_%d src=%x\n",$k,$src
   if $src>=0x08000000 && $src<0x0bfb5000
    if $k==0
     dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R111-layer0.raw ($b->host+$src-0x08000000) ($b->host+$src-0x08000000+0x4b000)
    end
    if $k==1
     dump binary memory K:/CodexResources/2026-10-28/ipod-classic7-desktop/acceptance/sleep-hold-20261031/R111-layer1.raw ($b->host+$src-0x08000000) ($b->host+$src-0x08000000+0x4b000)
    end
   end
   set $k=$k+1
  end
 end
 set $b=$b->next.le_next
 set $i=$i+1
end
detach
quit
