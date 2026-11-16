/*
 * S5L8702 AES custom-key engine, measured retailOS loader path.
 * Register map: Rockbox firmware/export/s5l87xx.h.
 * The payload's driver at file 0x39754..0x39964 programs TYPE=0, 16-byte
 * key in KEY[4..7] as big-endian words, 16-byte IV, KEYLEN=14/15, then
 * clears STATUS with 7 and polls bit 0 after GO. The game loader supplies
 * its own key and IV. Hardware GID/UID modes are deliberately unsupported.
 * Uses real AES-128-CBC, never identity data or patched game instructions.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "crypto/cipher.h"
#include "exec/address-spaces.h"
#include "exec/cpu-common.h"
#include "migration/vmstate.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#define TYPE_CLASSIC_AES "s5l8702.classic-aes"
OBJECT_DECLARE_SIMPLE_TYPE(ClassicAESState, CLASSIC_AES)
struct ClassicAESState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t reg[0x90 / 4];
    unsigned trace_transfers;
};
enum {
    CONTROL=0x00, GO=0x04, RESET=0x08, STATUS=0x0c, KEYLEN=0x14,
    OUTSIZE=0x18, OUTADDR=0x20, INSIZE=0x24, INADDR=0x28,
    AUXSIZE=0x2c, AUXADDR=0x30, SIZE3=0x34, KEYBASE=0x4c,
    TYPE=0x6c, IVBASE=0x74, TYPE2=0x88,
};
static bool aes_trace(void)
{
    const char *e = getenv("AESOBS");
    return e && *e && *e != '0';
}
static bool aes_known_reg(hwaddr offset)
{
    return offset <= OUTSIZE || (offset >= OUTADDR && offset <= SIZE3) ||
           (offset >= KEYBASE && offset <= TYPE) ||
           (offset >= IVBASE && offset <= 0x80) || offset == TYPE2 ||
           offset == 0x8c;
}
static bool aes_ram_buffer(uint32_t addr, uint32_t len, bool write)
{
    MemoryRegionSection section = memory_region_find(get_system_memory(), addr, len);
    bool valid = section.mr && memory_region_is_ram(section.mr) &&
                 (!write || !section.readonly) &&
                 int128_ge(section.size, int128_make64(len));
    if (section.mr) {
        memory_region_unref(section.mr);
    }
    return valid;
}
static uint32_t aes_fnv(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i=0; i<len; i++) {
        hash = (hash ^ data[i]) * 16777619u;
    }
    return hash;
}
static void aes_execute(ClassicAESState *s)
{
    uint32_t n=s->reg[INSIZE/4], in=s->reg[INADDR/4], out=s->reg[OUTADDR/4];
    uint32_t direction=s->reg[KEYLEN/4];
    uint8_t key[16], iv[16];
    Error *err=NULL;
    QCryptoCipher *cipher;
    uint8_t *src, *dst;
    int rc;
    s->reg[STATUS/4] &= ~1u;
    if (!(s->reg[CONTROL/4] & 1) || s->reg[TYPE/4] != 0 ||
        (direction != 14 && direction != 15) ||
        n == 0 || n > 32 * 1024 * 1024 || (n & 15) ||
        s->reg[OUTSIZE/4] != n ||
        !aes_ram_buffer(in,n,false) || !aes_ram_buffer(out,n,true)) {
        fprintf(stderr,"AES unsupported request type=%u keylen=%u in=0x%08x out=0x%08x bytes=%u; no completion\n",
                s->reg[TYPE/4],direction,in,out,n);
        return;
    }
    for (int i=0; i<4; i++) {
        stl_be_p(key+4*i,s->reg[KEYBASE/4+4+i]);
        stl_be_p(iv+4*i,s->reg[IVBASE/4+i]);
    }
    cipher=qcrypto_cipher_new(QCRYPTO_CIPHER_ALG_AES_128,
                              QCRYPTO_CIPHER_MODE_CBC,key,sizeof(key),&err);
    if (!cipher) {
        fprintf(stderr,"AES cipher error: %s\n",error_get_pretty(err));
        error_free(err);
        return;
    }
    src=g_malloc(n); dst=g_malloc(n);
    if (address_space_read(&address_space_memory,in,MEMTXATTRS_UNSPECIFIED,src,n) != MEMTX_OK) {
        fprintf(stderr,"AES input transfer failed; no completion\n");
        qcrypto_cipher_free(cipher);g_free(dst);g_free(src);
        return;
    }
    if (aes_trace() && s->trace_transfers++ < 2) {
        fprintf(stderr,"AESDATA first64=");
        for (unsigned i=0;i<MIN(n,64u);i++) fprintf(stderr,"%02x",src[i]);
        fprintf(stderr," keyregs=");
        for (int i=0;i<8;i++) fprintf(stderr,"%08x",s->reg[KEYBASE/4+i]);
        fprintf(stderr," ivregs=");
        for (int i=0;i<4;i++) fprintf(stderr,"%08x",s->reg[IVBASE/4+i]);
        fprintf(stderr,"\n");
    }
    rc=qcrypto_cipher_setiv(cipher,iv,sizeof(iv),&err);
    if (rc==0) {
        rc=(direction&1) ? qcrypto_cipher_encrypt(cipher,src,dst,n,&err) :
                           qcrypto_cipher_decrypt(cipher,src,dst,n,&err);
    }
    if (rc==0) {
        /* Completion follows a successful real transfer, never STATUS writes. */
        if (address_space_write(&address_space_memory,out,MEMTXATTRS_UNSPECIFIED,dst,n) != MEMTX_OK) {
            fprintf(stderr,"AES output transfer failed; no completion\n");
            qcrypto_cipher_free(cipher);g_free(dst);g_free(src);
            return;
        }
        s->reg[STATUS/4] |= 1;
        if (aes_trace()) {
            fprintf(stderr,"AES complete type=0 AES128-CBC %s in=0x%08x out=0x%08x bytes=%u input_fnv=%08x output_fnv=%08x vns=%" PRId64 "\n",
                    (direction&1)?"encrypt":"decrypt",in,out,n,
                    aes_fnv(src,n),aes_fnv(dst,n),qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
    } else {
        fprintf(stderr,"AES operation error: %s; no completion\n",error_get_pretty(err));
    }
    error_free(err); qcrypto_cipher_free(cipher); g_free(dst); g_free(src);
}
static uint64_t classic_aes_read(void *opaque, hwaddr offset, unsigned size)
{
    ClassicAESState *s=opaque;
    if (offset >= sizeof(s->reg) || !aes_known_reg(offset)) {
        qemu_log_mask(LOG_UNIMP,"S5L8702 AES unknown read +0x%" HWADDR_PRIx "\n",offset);
        return 0;
    }
    return s->reg[offset/4];
}
static void classic_aes_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    ClassicAESState *s=opaque;
    if (aes_trace()) {
        fprintf(stderr,"AESOBS write +0x%03" HWADDR_PRIx " size=%u value=0x%08" PRIx64 " vns=%" PRId64 "\n",
                offset,size,value,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    if (offset >= sizeof(s->reg) || !aes_known_reg(offset)) {
        qemu_log_mask(LOG_UNIMP,"S5L8702 AES unknown write +0x%" HWADDR_PRIx "\n",offset);
        return;
    }
    if (offset==STATUS) {
        s->reg[STATUS/4] &= ~(uint32_t)value;
        return;
    }
    s->reg[offset/4]=value;
    if (offset==RESET && (value&1)) {
        /* Driver programs key/IV before this engine reset: preserve them. */
        s->reg[STATUS/4]=0;
    } else if (offset==GO && (value&1)) {
        aes_execute(s);
    }
}
static const MemoryRegionOps classic_aes_ops={
    .read=classic_aes_read,.write=classic_aes_write,.endianness=DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size=4,.valid.max_access_size=4,
    .impl.min_access_size=4,.impl.max_access_size=4,
};
static void classic_aes_reset(DeviceState *dev)
{
    ClassicAESState *s=CLASSIC_AES(dev);
    memset(s->reg,0,sizeof(s->reg));
    s->trace_transfers=0;
}
static void classic_aes_init(Object *obj)
{
    ClassicAESState *s=CLASSIC_AES(obj);
    memory_region_init_io(&s->iomem,obj,&classic_aes_ops,s,TYPE_CLASSIC_AES,0x100000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),&s->iomem);
}
static const VMStateDescription vmstate_classic_aes={
    .name=TYPE_CLASSIC_AES,.version_id=1,.minimum_version_id=1,
    .fields=(VMStateField[]) {VMSTATE_UINT32_ARRAY(reg,ClassicAESState,0x90/4),VMSTATE_END_OF_LIST()},
};
static void classic_aes_class_init(ObjectClass *klass,void *data)
{
    DeviceClass *dc=DEVICE_CLASS(klass);
    dc->desc="S5L8702 AES128-CBC guest custom-key engine";
    dc->reset=classic_aes_reset;dc->vmsd=&vmstate_classic_aes;
}
static const TypeInfo classic_aes_info={
    .name=TYPE_CLASSIC_AES,.parent=TYPE_SYS_BUS_DEVICE,
    .instance_size=sizeof(ClassicAESState),.instance_init=classic_aes_init,
    .class_init=classic_aes_class_init,
};
static void classic_aes_register_types(void){type_register_static(&classic_aes_info);}
type_init(classic_aes_register_types)
