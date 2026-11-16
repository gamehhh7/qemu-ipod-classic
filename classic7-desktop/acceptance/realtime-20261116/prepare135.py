from pathlib import Path
import json,subprocess
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=p/'acceptance/realtime-20261116'
s=(p/'src/audio/observed_sm1.c').read_text(encoding='utf-8-sig')
s=s.replace('/* one audio frame at 44.1 kHz: 1152 / 44100 s = 26.122 ms */\n#define SM1_ENGINE_FRAME_NS 26122449ll', '''/* A compressed frame's playback duration is not its decode latency.
 * Decode is already completed before this report is queued. Keep delivery
 * asynchronous through the existing timer and level/ack path, but do not
 * add a whole playback frame of idle time after every executed command.
 * I2S/DMA remains the sample-clock authority; slots still belong to guest. */
#define SM1_ENGINE_FRAME_NS 0ll
static volatile uint64_t c7_sm1_pcm_bytes;
static volatile uint64_t c7_sm1_frames;
static volatile uint64_t c7_sm1_completions;''')
assert '#define SM1_ENGINE_FRAME_NS 0ll' in s
s=s.replace('    size_t out = (size_t)nsamples * info.channels * sizeof(int16_t);','    size_t out = (size_t)nsamples * info.channels * sizeof(int16_t);\n    c7_sm1_pcm_bytes += out;\n    c7_sm1_frames++;',1)
s=s.replace('    s->frame_pending = true;\n    if (s->irq_armed', '    s->frame_pending = true;\n    c7_sm1_completions++;\n    if (s->irq_armed',1)
start=s.index('    /* completion: bit12 for the pump, and the guest\'s own interrupt --')
end=s.index('    if (engine_done_timer) {',start)
s=s[:start]+'''    /* Report the completed command on the next timer dispatch, never inline
     * in the MMIO write. The existing level IRQ stays asserted until the
     * guest's real callback acknowledges it. */
'''+s[end:]
(d/'r135-sm1.c').write_text(s,encoding='utf8')
r=json.loads((p/'engine/build-recipe.json').read_text(encoding='utf-8-sig'))
job=next(x['argv'] for x in r['compile'] if x['name']=='sm1fast')
job=[x.replace('objects\\sm1fast.','objects\\r135-sm1.').replace(str(p/'src/audio/observed_sm1.c'),str(d/'r135-sm1.c')) for x in job]
job[1:1]=['-iquote',str(p/'src/audio')]
(d/'compile135.json').write_text(json.dumps([job]))
rsp=(d/'r134.rsp').read_text().replace('qemu-system-arm-classic7.r134.exe','qemu-system-arm-classic7.r135.exe').replace('/objects/sm1fast.obj','/objects/r135-sm1.obj')
assert '/objects/r135-sm1.obj' in rsp
(d/'r135.rsp').write_text(rsp)
s=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r135.exe')
(d/'Classic7.r135.cs').write_text(s,encoding='utf8')
subprocess.run(job,check=True)
subprocess.run([r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe','@'+str(d/'r135.rsp')],check=True)
subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe','/nologo','/target:winexe','/optimize+','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/out:'+str(p/'bin/iPodClassic7.r135.exe'),str(d/'Classic7.r135.cs')],check=True)
print('r135 candidate compiled; no acceptance claim')
