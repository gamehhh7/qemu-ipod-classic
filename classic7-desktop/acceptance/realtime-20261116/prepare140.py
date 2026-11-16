from pathlib import Path
import os,json,subprocess,shlex
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=Path(__file__).parent
b=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\build')
s=b.parent/'src/qemu-ipod-classic-1c33be87c7e97b56053142ae9d7f0686feaed504'
cc=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe')
os.chdir(b);os.environ['PATH']=str(cc.parent)+';'+os.environ['PATH']
src=(s/'hw/dma/pl080.c').read_text(encoding='utf8')
marker='static void pl080_run(PL080State *s)'
src=src.replace(marker,(d/'r140-dma-cache.inc').read_text(encoding='utf8')+'\n'+marker,1)
start=src.index('            for (n = 0; n < dwidth; n+= swidth) {')
end=src.index('            ch->ctrl = (ch->ctrl & 0xfffff000) | size;',start)
old=src[start:end]
src=src[:start]+'''            unsigned cached = pl080_cached_i2s_transfer(s, ch, size,
                                                           flow, swidth, dwidth);
            if (cached) {
                size -= cached;
            } else {
'''+old+'''            }
'''+src[end:]
src=src.replace('void pl080_pace_request(PL080State *s, int line)','''static volatile uint64_t c7_dma_request_ns, c7_dma_request_calls;
static volatile uint64_t c7_dma_compare_ns[2], c7_dma_compare_bytes[2], c7_dma_compare_calls[2];
void pl080_pace_request(PL080State *s, int line)''',1)
src=src.replace('    s->req_burst |= (1u << line);\n    pl080_run(s);','''    int64_t work_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    c7_dma_compare_path = 0;
    s->req_burst |= (1u << line);
    pl080_run(s);
    int64_t work_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - work_start;
    c7_dma_request_ns += work_ns;
    if (c7_dma_compare_path) {
        unsigned i = c7_dma_compare_path - 1;
        c7_dma_compare_ns[i] += work_ns;
        c7_dma_compare_bytes[i] += c7_dma_compare_size;
        c7_dma_compare_calls[i]++;
    }
    c7_dma_request_calls++;''',1)
outsrc=d/'r140-pl080.c';outsrc.write_text(src,encoding='utf8')
entry=next(v for v in json.loads((b/'compile_commands.json').read_text(encoding='utf8')) if v['file'].endswith('/pl080.c'))
args=[x.strip('"') for x in shlex.split(entry['command'],posix=False)];args[0]=str(cc)
obj=p/'engine/objects/r140-pl080.obj'
for flag,new in [('-o',str(obj)),('-MQ',str(obj)),('-MF',str(obj)+'.d')]:args[args.index(flag)+1]=new
args[-1]=str(outsrc)
(d/'compile140.json').write_text(json.dumps([args],indent=2),encoding='utf8')
sm=(d/'r138-sm1.c').read_text(encoding='utf8').replace('static volatile uint64_t c7_sm1_frames;', 'volatile uint64_t c7_sm1_frames;')
(d/'r140-sm1.c').write_text(sm,encoding='utf8')
sj=json.loads((d/'compile138.json').read_text(encoding='utf8'))[1]
sj=[x.replace('r138-sm1','r140-sm1') for x in sj]
(d/'compile140.json').write_text(json.dumps([args,sj],indent=2),encoding='utf8')
# Deliberately inherit R138, NOT the untested R139 clock-tolerance experiment.
rsp=(d/'r138.rsp').read_text(encoding='utf8').replace('qemu-system-arm-classic7.r138.exe','qemu-system-arm-classic7.r140.exe').replace('libcommon.fa.p/hw_dma_pl080.c.obj',obj.as_posix())
rsp=rsp.replace('/objects/r138-sm1.obj','/objects/r140-sm1.obj')
assert str(obj.as_posix()) in rsp
(d/'r140.rsp').write_text(rsp,encoding='utf8')
front=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r140.exe')
(d/'Classic7.r140.cs').write_text(front,encoding='utf8')
subprocess.run(args,check=True)
subprocess.run(sj,check=True)
subprocess.run([str(cc),'@'+str(d/'r140.rsp')],check=True)
subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe','/nologo','/target:winexe','/optimize+','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/out:'+str(p/'bin/iPodClassic7.r140.exe'),str(d/'Classic7.r140.cs')],check=True)
print('R140 separately built: per-block DMA mapping cache; guest timing unchanged.')
