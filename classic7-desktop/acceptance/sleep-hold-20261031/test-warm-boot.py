from pathlib import Path
import struct,re,sys
sys.path.insert(0,'K:/CodexResources/2026-10-31/classic7-sleep-deps/unicorn')
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_R0
r=Path('.');a=r/'acceptance/sleep-hold-20261031';tag=(a/'R115-ID.txt').read_text().strip();real=(a/(tag+'-code.bin')).read_bytes()[:20]
boot=b''.join(struct.pack('<I',int(w,16)) for w in re.findall(r'0x([0-9a-f]{8}),',(r/'src/usb/hibernate_boot.inc').read_text()))
for base in [0,0x20000000]:
 results=[];park=base+0x58
 for label,off,value in [('real-header',None,None),('bad-magic',0,0),('bad-version',4,0),('bad-state',8,0),('unaligned-entry',16,0x080d24d1),('below-ram',16,0x07000000),('past-ram',16,0x0c000000)]:
  header=bytearray(real)
  if off is not None:struct.pack_into('<I',header,off,value)
  expected=0x080d24d0 if off is None else park
  u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(base,0x1000);u.mem_write(base,boot);u.mem_map(0x08000000,0x4000000);u.mem_write(0x08000000,bytes(header));seen=[]
  def hook(cpu,pc,size,data):
   if pc in [0x080d24d0,park]:seen.append(pc);cpu.emu_stop()
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(base,base+0x1000,count=40);assert seen==[expected],(base,label,seen)
  if off is None:assert u.reg_read(UC_ARM_REG_SP)==0x22020000 and u.reg_read(UC_ARM_REG_R0)==0
  results.append(label);del u
 print(f'PASS executed ARM shim base={base:08x}: '+', '.join(results))
