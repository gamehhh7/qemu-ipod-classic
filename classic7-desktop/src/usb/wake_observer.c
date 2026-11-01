/* Bounded fault observation, including ordinary desktop launches. Never
 * change guest state: always delegate the actual exception unchanged. */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "exec/cpu-common.h"
#include "qemu/timer.h"

void __real_arm_cpu_do_interrupt(CPUState *cs);
void __wrap_arm_cpu_do_interrupt(CPUState *cs);

void __wrap_arm_cpu_do_interrupt(CPUState *cs)
{
    static int enabled = -1;
    static unsigned count;
    static unsigned wake_irqs;
    bool panic = false;
    if (enabled < 0) {
        const char *e = getenv("CLASSIC7_WAKE_TRACE");
        enabled = !e || strcmp(e, "0");
        fprintf(stderr, "WAKEOBS enabled=%d ordinary_launch=1\n", enabled);
    }
    if (enabled && wake_irqs < 16) {
        CPUARMState *env = &ARM_CPU(cs)->env;
        uint32_t pc = env->regs[15];
        if (pc >= 0x22002d00 && pc < 0x22002f00) {
            fprintf(stderr, "WAKEIRQ n=%u exception=%d pc=%08x cpsr=%08x"
                    " lr=%08x vns=%" PRId64 "\n", ++wake_irqs,
                    cs->exception_index, pc, cpsr_read(env), env->regs[14],
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
    }
    if (enabled && cs->exception_index == EXCP_SEMIHOST) {
        CPUARMState *env = &ARM_CPU(cs)->env;
        uint32_t address = env->regs[1];
        uint8_t args[12], text[96] = {0};
        if (env->regs[0] == 5 &&
            !cpu_memory_rw_debug(cs,address,args,sizeof(args),false)) {
            address = ldl_le_p(args + 4);
        }
        if ((env->regs[0] == 4 || env->regs[0] == 5) &&
            !cpu_memory_rw_debug(cs,address,text,sizeof(text)-1,false)) {
            panic = strstr((char *)text,"CPU_exception_handler") != NULL;
        }
    }
    if (enabled && count < 8 &&
        (panic || cs->exception_index == EXCP_DATA_ABORT ||
         cs->exception_index == EXCP_PREFETCH_ABORT ||
         cs->exception_index == EXCP_UDEF)) {
        CPUARMState *env = &ARM_CPU(cs)->env;
        uint8_t code[64], stack[512];
        uint32_t pc = env->regs[15], sp = env->regs[13];
        fprintf(stderr, "WAKEFAULT n=%u panic_print=%d exception=%d pc=%08x cpsr=%08x"
                " fsr=%08x va=%" PRIx64 " vns=%" PRId64 "\n",
                ++count, panic, cs->exception_index, pc, cpsr_read(env),
                env->exception.fsr, env->exception.vaddress,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        for (unsigned i = 0; i < 16; i++) {
            fprintf(stderr, "WAKEFAULT reg=%u value=%08x\n", i, env->regs[i]);
        }
        fprintf(stderr, "WAKEFAULT spsr=%08x ifsr=%" PRIx64
                " dfsr=%" PRIx64 " far=%" PRIx64 "\n",
                env->spsr, env->cp15.ifsr_ns, env->cp15.dfsr_ns,
                env->cp15.far_el[1]);
        for (unsigned i = 0; i < 8; i++) {
            fprintf(stderr, "WAKEFAULT bank=%u sp=%08x lr=%08x spsr=%" PRIx64 "\n",
                    i, env->banked_r13[i], env->banked_r14[i],
                    env->banked_spsr[i]);
        }
        if (!cpu_memory_rw_debug(cs, pc & ~3u, code, sizeof(code), false)) {
            fprintf(stderr, "WAKEFAULT code=%08x hex=", pc & ~3u);
            for (unsigned i = 0; i < sizeof(code); i++) {
                fprintf(stderr, "%02x", code[i]);
            }
            fprintf(stderr, "\n");
        }
        if (!cpu_memory_rw_debug(cs, sp, stack, sizeof(stack), false)) {
            fprintf(stderr, "WAKEFAULT stack=%08x hex=", sp);
            for (unsigned i = 0; i < sizeof(stack); i++) {
                fprintf(stderr, "%02x", stack[i]);
            }
            fprintf(stderr, "\n");
        }
        const char *folder = getenv("CLASSIC7_WAKE_DUMP_DIR");
        if (!folder) {
            folder = "K:/CodexResources/2026-10-28/ipod-classic7-desktop/"
                     "acceptance/sleep-hold-20261031";
        }
        if (folder && count == 1) {
            uint8_t *memory = g_malloc(0x400000);
            if (!cpu_memory_rw_debug(cs,0x08000000,memory,0x400000,false)) {
                char *path = g_strdup_printf("%s/runtime-code.bin",folder);
                FILE *file = fopen(path,"wb");
                if (file) {
                    size_t wrote = fwrite(memory,1,0x400000,file);
                    fclose(file);
                    fprintf(stderr,"WAKEFAULT runtime_code_bytes=%zu base=08000000\n",wrote);
                }
                g_free(path);
            }
            g_free(memory);
        }
    }
    __real_arm_cpu_do_interrupt(cs);
}
