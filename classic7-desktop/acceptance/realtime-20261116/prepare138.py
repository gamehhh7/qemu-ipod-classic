from pathlib import Path
import os,json,subprocess
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=p/'acceptance/realtime-20261116'
b=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\build')
cc=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe')
os.chdir(b);os.environ['PATH']=str(cc.parent)+';'+os.environ['PATH']
s=(d/'r136-i2s.c').read_text(encoding='utf8')
s=s.replace('static volatile uint64_t c7_audio_stale_events;', '''static volatile uint64_t c7_audio_stale_events;
static volatile uint64_t c7_audio_prime_events;
static volatile uint64_t c7_audio_empty_callbacks;
static volatile uint64_t c7_audio_stream_stops;
static volatile uint64_t c7_audio_stop_flush_bytes;''')
s=s.replace('    (void)avail;\n    i2s_live_drain();', '''    if (avail > 0 && i2s_live.primed && i2s_live_used() == 0) {
        c7_audio_empty_callbacks++;
    }
    i2s_live_drain();''')
s=s.replace('        i2s_live.primed = true;\n        AUD_set_active_out', '        i2s_live.primed = true;\n        c7_audio_prime_events++;\n        AUD_set_active_out')
marker='static void ipod_classic_i2s_write(void *opaque, hwaddr offset,'
assert marker in s
s=s.replace(marker,'''/* A stopped transmitter is a stream boundary for the host sink. The
 * boot silence stream must not spend the later music stream's jitter cushion.
 * Rearm the existing priming policy after an actual guest clock/TX stop;
 * neither the guest registers nor DMA progression are changed here. */
static void i2s_live_stream_stop(void)
{
    c7_audio_stream_stops++;
    if (i2s_live.voice) AUD_set_active_out(i2s_live.voice, 0);
    c7_audio_stop_flush_bytes += i2s_live_used();
    i2s_live.rd = i2s_live.wr;
    i2s_live.partial_frame = 0;
    i2s_live.partial_bytes = 0;
    i2s_live.primed = false;
}

'''+marker,1)
s=s.replace('if (was_en && !(value & 1)) {\n            ipod_classic_hle_audio_notify_clock_reconfig();','if (was_en && !(value & 1)) {\n            i2s_live_stream_stop();\n            ipod_classic_hle_audio_notify_clock_reconfig();',1)
s=s.replace('} else if (!s->tx_running && was_running) {\n            i2s_pace_disarm(s);','} else if (!s->tx_running && was_running) {\n            i2s_live_stream_stop();\n            i2s_pace_disarm(s);',1)
(d/'r138-i2s.c').write_text(s,encoding='utf8')
job=json.loads((d/'compile136.json').read_text(encoding='utf8'))[0]
job=[x.replace('r136-i2s','r138-i2s') for x in job]
sm=(d/'r135-sm1.c').read_text(encoding='utf8').replace('static volatile uint64_t c7_sm1_completions;', 'static volatile uint64_t c7_sm1_completions;\nstatic volatile uint32_t c7_sm1_last_hz;')
sm=sm.replace('    c7_sm1_pcm_bytes += out;', '    c7_sm1_last_hz = info.hz;\n    c7_sm1_pcm_bytes += out;',1)
(d/'r138-sm1.c').write_text(sm,encoding='utf8')
sj=json.loads((d/'compile135.json').read_text(encoding='utf8'))[0];sj=[x.replace('r135-sm1','r138-sm1') for x in sj]
(d/'compile138.json').write_text(json.dumps([job,sj]))
rsp=(d/'r137.rsp').read_text(encoding='utf8').replace('qemu-system-arm-classic7.r137.exe','qemu-system-arm-classic7.r138.exe').replace('/objects/r136-i2s.obj','/objects/r138-i2s.obj').replace('/objects/r135-sm1.obj','/objects/r138-sm1.obj')
(d/'r138.rsp').write_text(rsp)
front=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r138.exe')
(d/'Classic7.r138.cs').write_text(front,encoding='utf8')
for j in [job,sj]:subprocess.run(j,check=True)
subprocess.run([str(cc),'@'+str(d/'r138.rsp')],check=True)
subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe','/nologo','/target:winexe','/optimize+','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/out:'+str(p/'bin/iPodClassic7.r138.exe'),str(d/'Classic7.r138.cs')],check=True)
print('r138 candidate built: stream-boundary priming and read-only underrun/source-rate counters')
