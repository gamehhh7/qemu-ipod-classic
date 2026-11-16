"""Isolated candidate: tighten adaptive icount tolerance, no formal overwrite."""
from pathlib import Path
import os,json,subprocess,shlex
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=Path(__file__).parent
b=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\build')
s=b.parent/'src/qemu-ipod-classic-1c33be87c7e97b56053142ae9d7f0686feaed504'
cc=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe')
os.chdir(b);os.environ['PATH']=str(cc.parent)+';'+os.environ['PATH']
src=(s/'accel/tcg/icount-common.c').read_text(encoding='utf8')
old='#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)'
assert src.count(old)==1
src=src.replace(old,'''/* R139 experiment: reduce adaptive clock error tolerance from 100 to
 * 10 ms. Preserve adjustment formula, continuity, IRQs and fixed-shift path.
 * Only this separately linked candidate uses the tighter tolerance. */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 100)''')
outsrc=d/'r139-icount.c';outsrc.write_text(src,encoding='utf8')
entry=next(v for v in json.loads((b/'compile_commands.json').read_text(encoding='utf8')) if v['file'].endswith('/icount-common.c'))
args=[x.strip('"') for x in shlex.split(entry['command'],posix=False)]
args[0]=str(cc)
obj=p/'engine/objects/r139-icount.obj'
for flag,new in [('-o',str(obj)),('-MQ',str(obj)),('-MF',str(obj)+'.d')]:args[args.index(flag)+1]=new
args[-1]=str(outsrc)
(d/'compile139.json').write_text(json.dumps([args],indent=2),encoding='utf8')
rsp=(d/'r138.rsp').read_text(encoding='utf8').replace('qemu-system-arm-classic7.r138.exe','qemu-system-arm-classic7.r139.exe').replace('libcommon.fa.p/accel_tcg_icount-common.c.obj',obj.as_posix())
(d/'r139.rsp').write_text(rsp,encoding='utf8')
front=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r139.exe')
(d/'Classic7.r139.cs').write_text(front,encoding='utf8')
subprocess.run(args,check=True)
subprocess.run([str(cc),'@'+str(d/'r139.rsp')],check=True)
subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe','/nologo','/target:winexe','/optimize+','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/out:'+str(p/'bin/iPodClassic7.r139.exe'),str(d/'Classic7.r139.cs')],check=True)
print('R139 separately linked. Not yet launched or accepted.')
