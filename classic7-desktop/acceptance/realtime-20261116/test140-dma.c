
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PL080_CCTRL_SI 0x04000000
#define PL080_CCTRL_DI 0x08000000
#define MEMTXATTRS_UNSPECIFIED 0
#define RCU_READ_LOCK_GUARD() ((void)0)
typedef struct {uint32_t src,dest,lli,ctrl,conf;} pl080_channel;
typedef struct {uint32_t paced_lines;int downstream_as;} PL080State;
typedef struct {const char *name;} MemoryRegion;
typedef struct {void *ptr;uint64_t xlat;struct {MemoryRegion *mr;} mrs;} MemoryRegionCache;
static uint8_t mem[8192];static MemoryRegion ram={"ram"},i2s={"s5l8702.i2s"};
static int destroyed,which,mode;static unsigned writes,count;static uint32_t start;
static pl080_channel *current;
static int64_t address_space_cache_init(MemoryRegionCache *c,int *as,uint64_t addr,uint64_t len,bool wr){
 (void)as;(void)addr;
 if(!wr){c->ptr=mode==1?NULL:mem;c->mrs.mr=&ram;c->xlat=0;return mode==2?len-1:len;}
 c->ptr=mode==3?mem:NULL;c->mrs.mr=&i2s;c->xlat=mode==4?0x14:0x10;return mode==5?1:2;
}
static const char *memory_region_name(MemoryRegion *r){return mode==6?"other":r->name;}
static void address_space_cache_destroy(MemoryRegionCache *c){assert(c->mrs.mr);destroyed++;}
static uint16_t address_space_lduw_le_cached(MemoryRegionCache *c,uint64_t off,int a,void *b){(void)c;(void)a;(void)b;return mem[off]|mem[off+1]<<8;}
static void address_space_stw_le_cached(MemoryRegionCache *c,uint64_t off,uint16_t v,int a,void *b){
 (void)c;(void)a;(void)b;assert(off==0);assert(v==(uint16_t)(writes*73+19));
 assert(current->src==start+(writes+1)*2);assert((current->ctrl&4095)==count-writes);
 assert(current->lli==0x22000080);assert(current->dest==0x3ca00010);writes++;
}
/* Candidate-only transport optimization. A synchronous paced I2S block is
 * already moved before returning to the guest. Cache its RAM source and exact
 * FIFO mapping for that one block, preserving each 16-bit device transaction,
 * channel cursor/count ordering, LLI completion and interrupt path. */
static volatile uint64_t c7_dma_cached_blocks, c7_dma_cached_bytes;
/* Optional bounded paired measurement, then permanently use the fast path.
 * Both paths execute the same real device transactions. No wall/guest clock,
 * FIFO data, interrupts, or sample rate is adjusted by this comparison. */
static uint64_t c7_dma_compare_seen;
static unsigned c7_dma_compare_path, c7_dma_compare_size;
static unsigned pl080_cached_i2s_transfer(PL080State *s, pl080_channel *ch,
                                         unsigned count, unsigned flow,
                                         unsigned swidth, unsigned dwidth)
{
    static int enabled = -1;
    static bool compare;
    MemoryRegionCache source = { 0 }, sink = { 0 };
    unsigned copied = 0;
    uint64_t bytes = (uint64_t)count * 2;

    if (enabled < 0) {
        const char *e = getenv("CLASSIC7_DMA_CACHE");
        enabled = !e || strcmp(e, "0") != 0;
        e = getenv("CLASSIC7_DMA_COMPARE");
        compare = e && strcmp(e, "1") == 0;
    }
    if (!enabled || flow != 1 || swidth != 2 || dwidth != 2 || count < 2 ||
        (ch->ctrl & (PL080_CCTRL_SI | PL080_CCTRL_DI)) != PL080_CCTRL_SI ||
        ((ch->src | ch->dest) & 1) || bytes > UINT32_MAX - (uint64_t)ch->src ||
        !(s->paced_lines & (1u << ((ch->conf >> 6) & 31)))) {
        return 0;
    }
    if (compare && c7_dma_compare_seen < 1024) {
        c7_dma_compare_size = bytes;
        c7_dma_compare_path = (++c7_dma_compare_seen & 1) ? 1 : 2;
        if (c7_dma_compare_path == 1) {
            return 0;
        }
    }
    RCU_READ_LOCK_GUARD();
    int64_t srclen = address_space_cache_init(&source, &s->downstream_as,
                                             ch->src, bytes, false);
    int64_t dstlen = address_space_cache_init(&sink, &s->downstream_as,
                                             ch->dest, 2, true);
    /* No IOMMU, MMIO source, RAM destination, partial mapping, or other device
     * is accelerated. Those all retain the exact original element loop. */
    if (!source.ptr || srclen < (int64_t)bytes || sink.ptr || dstlen < 2 ||
        sink.xlat != 0x10 ||
        strcmp(memory_region_name(sink.mrs.mr), "s5l8702.i2s") != 0) {
        c7_dma_compare_path = 0;
        goto out;
    }
    for (unsigned i = 0; i < count; i++) {
        uint16_t value = address_space_lduw_le_cached(&source, i * 2,
                                                     MEMTXATTRS_UNSPECIFIED, NULL);
        ch->src += 2;
        address_space_stw_le_cached(&sink, 0, value,
                                     MEMTXATTRS_UNSPECIFIED, NULL);
        ch->ctrl = (ch->ctrl & 0xfffff000) | (count - i - 1);
    }
    copied = count;
    c7_dma_cached_blocks++;
    c7_dma_cached_bytes += bytes;
out:
    address_space_cache_destroy(&sink);
    address_space_cache_destroy(&source);
    return copied;
}

int main(void){
 PL080State s={.paced_lines=1u<<10};pl080_channel ch;current=&ch;unsigned cases=0;
 for(unsigned n=2;n<=4095;n+=17){
  count=n;start=0x08000000;writes=destroyed=mode=0;
  for(unsigned i=0;i<n;i++){uint16_t v=i*73+19;mem[i*2]=v;mem[i*2+1]=v>>8;}
  ch=(pl080_channel){start,0x3ca00010,0x22000080,0x80400000|PL080_CCTRL_SI|n,10<<6};
  uint32_t flags=ch.ctrl&~4095u;
  unsigned got=pl080_cached_i2s_transfer(&s,&ch,n,1,2,2);
  if(getenv("CLASSIC7_DMA_CACHE")&&!strcmp(getenv("CLASSIC7_DMA_CACHE"),"0")){assert(!got&&!writes&&!destroyed);puts("disabled fallback PASS");return 0;}
  assert(got==n&&writes==n&&destroyed==2&&ch.ctrl==flags&&ch.src==start+n*2);cases++;
 }
 count=32;start=0x08000000;
 for(mode=1;mode<=6;mode++){
  writes=destroyed=0;ch=(pl080_channel){start,0x3ca00010,0x22000080,PL080_CCTRL_SI|count,10<<6};pl080_channel before=ch;
  assert(pl080_cached_i2s_transfer(&s,&ch,count,1,2,2)==0);assert(!writes&&destroyed==2&&!memcmp(&ch,&before,sizeof(ch)));cases++;
 }
 mode=0;
 for(which=0;which<8;which++){
  writes=destroyed=0;ch=(pl080_channel){start,0x3ca00010,0x22000080,PL080_CCTRL_SI|count,10<<6};
  unsigned flow=1,sw=2,dw=2,n=count;
  if(which==0)flow=0;if(which==1)sw=4;if(which==2)dw=1;if(which==3)n=1;
  if(which==4)ch.ctrl|=PL080_CCTRL_DI;if(which==5)ch.src++;if(which==6)ch.src=0xfffffff0;if(which==7)ch.conf=0;
  pl080_channel before=ch;assert(!pl080_cached_i2s_transfer(&s,&ch,n,flow,sw,dw));assert(!writes&&!destroyed&&!memcmp(&ch,&before,sizeof(ch)));cases++;
 }
 printf("%u transaction-order/boundary/fallback cases PASS\n",cases);return 0;
}
