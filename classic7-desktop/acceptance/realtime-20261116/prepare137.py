from pathlib import Path
import ctypes as c,json,subprocess,os
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=p/'acceptance/realtime-20261116'
b=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\build')
sroot=b.parent/'src/qemu-ipod-classic-1c33be87c7e97b56053142ae9d7f0686feaed504'
cc=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe')
os.chdir(b);os.environ['PATH']=str(cc.parent)+';'+os.environ['PATH']
s=(sroot/'audio/audio.c').read_text(encoding='utf8')
marker='static void audio_reset_timer (AudioState *s)'
helper='''/* DirectSound consumes PCM on the host clock. Keep its refill timer on
 * that same clock, independent of icount adjustment. Existing VM-state and
 * active-voice checks still stop/deactivate output; guest I2S/DMA clocks and
 * all other audio backends are unchanged. */
static QEMUClockType classic7_audio_timer_clock(AudioState *s)
{
    return s->dev->driver == AUDIODEV_DRIVER_DSOUND
           ? QEMU_CLOCK_REALTIME : QEMU_CLOCK_VIRTUAL;
}

static volatile uint64_t c7_audio_timer_calls;
static volatile uint64_t c7_audio_timer_gap_max_ns;
static volatile uint64_t c7_audio_timer_gap_over_90ms;
static int64_t c7_audio_timer_last_host_ns;

'''
assert marker in s;s=s.replace(marker,helper+marker,1)
start=s.index(marker);end=s.index('/*\n * Public API',start)
chunk=s[start:end].replace('qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)','qemu_clock_get_ns(classic7_audio_timer_clock(s))')
chunk=chunk.replace('    int64_t now, diff;', '    int64_t now, diff;\n    int64_t host_now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);')
needle='    now = qemu_clock_get_ns(classic7_audio_timer_clock(s));'
chunk=chunk.replace(needle,'''    if (s->dev->driver == AUDIODEV_DRIVER_DSOUND) {
        if (c7_audio_timer_last_host_ns) {
            uint64_t gap = host_now - c7_audio_timer_last_host_ns;
            if (gap > c7_audio_timer_gap_max_ns) c7_audio_timer_gap_max_ns = gap;
            if (gap > 90000000) c7_audio_timer_gap_over_90ms++;
        }
        c7_audio_timer_last_host_ns = host_now;
        c7_audio_timer_calls++;
    }
'''+needle,1)
s=s[:start]+chunk+s[end:]
s=s.replace('    if (dev->timer_period <= 0) {', '''    /* Select after driver initialization: s->dev is NULL at allocation.
     * Keep the initial inactive timer valid for all existing failure paths. */
    if (classic7_audio_timer_clock(s) == QEMU_CLOCK_REALTIME) {
        timer_free(s->ts);
        s->ts = timer_new_ns(QEMU_CLOCK_REALTIME, audio_timer, s);
    }

    if (dev->timer_period <= 0) {''',1)
assert 'timer_new_ns(QEMU_CLOCK_REALTIME, audio_timer, s)' in s
(d/'r137-audio.c').write_text(s,encoding='utf8')
entry=next(x for x in json.loads((b/'compile_commands.json').read_text()) if x['file'].endswith('/audio/audio.c'))
parse=c.windll.shell32.CommandLineToArgvW;parse.argtypes=[c.c_wchar_p,c.POINTER(c.c_int)];parse.restype=c.POINTER(c.c_wchar_p)
n=c.c_int();args= parse(entry['command'],c.byref(n));job=[args[i] for i in range(n.value)]
c.windll.kernel32.LocalFree(c.cast(args,c.c_void_p))
job[0]=str(cc)
for flag,val in [('-o',str(p/'engine/objects/r137-audio.obj')),('-MF',str(p/'engine/objects/r137-audio.d')),('-MQ',str(p/'engine/objects/r137-audio.obj'))]:
    job[job.index(flag)+1]=val
job[job.index('-c')+1]=str(d/'r137-audio.c')
job[1:1]=['-iquote',str(sroot/'audio')]
(d/'compile137.json').write_text(json.dumps([job]))
rsp=(d/'r136.rsp').read_text().replace('qemu-system-arm-classic7.r136.exe','qemu-system-arm-classic7.r137.exe')
old=entry['output'].replace('\\','/')
assert old in rsp,old
rsp=rsp.replace(old,str(p/'engine/objects/r137-audio.obj').replace('\\','/'))
(d/'r137.rsp').write_text(rsp)
front=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r137.exe')
(d/'Classic7.r137.cs').write_text(front,encoding='utf8')
subprocess.run(job,check=True)
subprocess.run([str(cc),'@'+str(d/'r137.rsp')],check=True)
subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe','/nologo','/target:winexe','/optimize+','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/out:'+str(p/'bin/iPodClassic7.r137.exe'),str(d/'Classic7.r137.cs')],check=True)
print('r137 compiled: only DirectSound refill scheduling uses host clock; guest clocks unchanged')
