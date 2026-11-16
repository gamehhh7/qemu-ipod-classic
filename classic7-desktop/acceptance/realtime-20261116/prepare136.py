from pathlib import Path
import json,subprocess,os
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=p/'acceptance/realtime-20261116'
os.chdir(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\build')
os.environ['PATH']=r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin'+';'+os.environ['PATH']
s=(p/'src/audio/ipod_classic_i2s.c').read_text(encoding='utf-8-sig')
s=s.replace('    uint32_t rd, wr;         /* free-running byte counters */', '''    uint32_t rd, wr;         /* free-running, stereo-frame-aligned counters */
    uint32_t partial_frame;
    unsigned partial_bytes;''')
s=s.replace('#define I2S_LIVE_PRIME_MS 300u', '''#define I2S_LIVE_PRIME_MS 300u
#define I2S_LIVE_MAX_QUEUE_MS 500u
static volatile uint64_t c7_audio_input_bytes;
static volatile uint64_t c7_audio_overflow_bytes;
static volatile uint64_t c7_audio_stale_bytes;
static volatile uint64_t c7_audio_stale_events;''')
s=s.replace('/* Hand as much of the ring as the mixer will take right now. */', '''/* Host queue policy only: never change guest DMA or sample-clock cadence.
 * Preserve stereo frames across 8/16/32-bit FIFO writes. A full host queue
 * discards a complete frame, never half a channel pair. */
static void i2s_live_enqueue(uint32_t value, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        i2s_live.partial_frame |= ((value >> (8 * i)) & 0xffu)
                                 << (8 * i2s_live.partial_bytes++);
        if (i2s_live.partial_bytes == 4) {
            c7_audio_input_bytes += 4;
            if (i2s_live_used() <= sizeof(i2s_live.buf) - 4) {
                uint32_t pos = i2s_live.wr & I2S_LIVE_MASK;
                uint32_t frame = i2s_live.partial_frame;
                for (unsigned j = 0; j < 4; j++) {
                    i2s_live.buf[pos + j] = (frame >> (8 * j)) & 0xffu;
                }
                i2s_live.wr += 4;
            } else {
                c7_audio_overflow_bytes += 4;
            }
            i2s_live.partial_frame = 0;
            i2s_live.partial_bytes = 0;
        }
    }
}

/* A real-time sink must not retain seconds of stale PCM after startup or a
 * host stall. Trim only on actual excess, on stereo boundaries, and count it.
 * Steady playback acceptance requires no continuing trims or overflows. */
static void i2s_live_trim_stale(void)
{
    uint32_t rate = i2s_live.rate ? i2s_live.rate : I2S_LIVE_DEFAULT_RATE;
    uint32_t limit = ((uint64_t)rate * 4 * I2S_LIVE_MAX_QUEUE_MS / 1000) & ~3u;
    uint32_t used = i2s_live_used();
    if (used > limit) {
        uint32_t keep = i2s_live_prime_bytes() & ~3u;
        uint32_t discard = (used - keep) & ~3u;
        i2s_live.rd += discard;
        c7_audio_stale_bytes += discard;
        c7_audio_stale_events++;
    }
}

/* Hand as much of the ring as the mixer will take right now. */''')
s=s.replace('static void i2s_live_drain(void)\n{', 'static void i2s_live_drain(void)\n{\n    i2s_live_trim_stale();')
old='''    for (unsigned i = 0; i < size; i++) {
        if (i2s_live_used() < sizeof(i2s_live.buf)) {
            i2s_live.buf[i2s_live.wr & I2S_LIVE_MASK] = (value >> (8 * i)) & 0xff;
            i2s_live.wr++;
        }
    }'''
assert old in s
s=s.replace(old,'    i2s_live_enqueue((uint32_t)value, size);',1)
s=s.replace('    i2s_live.wr = 0;', '    i2s_live.wr = 0;\n    i2s_live.partial_frame = 0;\n    i2s_live.partial_bytes = 0;')
assert 'i2s_live.wr++' not in s
(d/'r136-i2s.c').write_text(s,encoding='utf8')
r=json.loads((p/'engine/build-recipe.json').read_text(encoding='utf-8-sig'))
job=next(x['argv'] for x in r['compile'] if x['name']=='i2s')
job=[x.replace('objects\\i2s.','objects\\r136-i2s.').replace(str(p/'src/audio/ipod_classic_i2s.c'),str(d/'r136-i2s.c')) for x in job]
job[1:1]=['-iquote',str(p/'src/audio')]
(d/'compile136.json').write_text(json.dumps([job]))
rsp=(d/'r135.rsp').read_text().replace('qemu-system-arm-classic7.r135.exe','qemu-system-arm-classic7.r136.exe').replace('/objects/i2s.obj','/objects/r136-i2s.obj')
assert '/objects/r136-i2s.obj' in rsp
(d/'r136.rsp').write_text(rsp)
front=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r136.exe')
(d/'Classic7.r136.cs').write_text(front,encoding='utf8')
subprocess.run(job,check=True)
subprocess.run([r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe','@'+str(d/'r136.rsp')],check=True)
subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe','/nologo','/target:winexe','/optimize+','/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.Web.Extensions.dll','/out:'+str(p/'bin/iPodClassic7.r136.exe'),str(d/'Classic7.r136.cs')],check=True)
print('r136 candidate compiled; formal files unchanged; not yet accepted')
