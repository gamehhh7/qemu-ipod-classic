#include <stdint.h>
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
static uint32_t i2s_live_used(void)
{
    return i2s_live.wr - i2s_live.rd;
}
static uint32_t i2s_live_prime_bytes(void)
{
    uint32_t rate = i2s_live.rate ? i2s_live.rate : I2S_LIVE_DEFAULT_RATE;
    uint32_t bytes = (uint32_t)((uint64_t)rate * 4 * I2S_LIVE_PRIME_MS / 1000);
    /* Never ask for a cushion the ring cannot hold. */
    if (bytes > sizeof(i2s_live.buf) / 2) {
        bytes = sizeof(i2s_live.buf) / 2;
    }
    return bytes;
}
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
