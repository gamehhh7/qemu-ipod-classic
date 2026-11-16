from pathlib import Path
import subprocess,os,json
p=Path(__file__).parent
s=(p/'r136-i2s.c').read_text(encoding='utf8')
def function(name):
    start=s.index('static ',s.index('\n',0)) if False else s.rfind('static ',0,s.index(name+'('))
    opening=s.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
c='''#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static struct {uint8_t buf[64]; uint32_t rd,wr,rate,partial_frame; unsigned partial_bytes;} i2s_live;
#define I2S_LIVE_MASK 63u
#define I2S_LIVE_DEFAULT_RATE 8u
#define I2S_LIVE_PRIME_MS 300u
#define I2S_LIVE_MAX_QUEUE_MS 500u
static volatile uint64_t c7_audio_input_bytes,c7_audio_overflow_bytes,c7_audio_stale_bytes,c7_audio_stale_events;
'''
for n in ['i2s_live_used','i2s_live_prime_bytes','i2s_live_enqueue','i2s_live_trim_stale']:c+=function(n)+'\n'
c+='''
static uint32_t at(uint32_t n){uint32_t x; memcpy(&x,i2s_live.buf+(n&63),4);return x;}
static void clear(void){memset(&i2s_live,0,sizeof(i2s_live));c7_audio_input_bytes=c7_audio_overflow_bytes=c7_audio_stale_bytes=c7_audio_stale_events=0;i2s_live.rate=8;}
int main(void){
 clear();
 for(unsigned i=0;i<16;i++)i2s_live_enqueue(i,4);
 i2s_live_enqueue(0x1122,2); /* left channel arrives while full */
 assert(i2s_live.wr==64 && i2s_live.partial_bytes==2);
 i2s_live.rd+=4; /* mixer makes room between the two halfwords */
 i2s_live_enqueue(0x3344,2);
 assert(at(64)==0x33441122 && i2s_live.wr==68 && c7_audio_overflow_bytes==0);
 i2s_live_enqueue(0xaabbccdd,4); /* full queue: drop the whole stereo frame */
 assert(i2s_live.wr==68 && c7_audio_overflow_bytes==4 && i2s_live.partial_bytes==0);
 clear(); i2s_live.rd=i2s_live.wr=0xfffffffc;
 i2s_live_enqueue(0x44332211,4);
 assert(i2s_live.wr==0 && i2s_live_used()==4 && at(0xfffffffc)==0x44332211);
 clear();
 i2s_live_enqueue(0x11,1);i2s_live_enqueue(0x3322,2);i2s_live_enqueue(0x44,1);
 assert(i2s_live_used()==4 && at(0)==0x44332211);
 clear();for(unsigned i=0;i<16;i++)i2s_live_enqueue(i,4);
 i2s_live_trim_stale();
 assert(i2s_live_used()==8 && at(i2s_live.rd)==14 && at(i2s_live.rd+4)==15);
 assert(c7_audio_stale_bytes==56 && c7_audio_stale_events==1);
 i2s_live_trim_stale();assert(c7_audio_stale_events==1);
 puts("PASS: partial-channel overflow, complete-frame discard, counter wrap, mixed write widths, bounded stale queue");
 return 0;
}
'''
(p/'test136-queue.c').write_text(c,encoding='utf8')
compiler=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\cc.exe')
os.environ['PATH']=str(compiler.parent)+';'+os.environ['PATH']
subprocess.run([str(compiler),'-std=c11','-O2','-Wall','-Wextra',str(p/'test136-queue.c'),'-o',str(p/'test136-queue.exe')],check=True)
r=subprocess.run([str(p/'test136-queue.exe')],capture_output=True,text=True,check=True)
print(r.stdout)
(p/'R136-QUEUE-TEST.json').write_text(json.dumps({'method':'compiled unmodified queue functions extracted from candidate source; small ring for boundary cases','result':r.stdout.strip(),'status':'PASS'}),encoding='utf8')
