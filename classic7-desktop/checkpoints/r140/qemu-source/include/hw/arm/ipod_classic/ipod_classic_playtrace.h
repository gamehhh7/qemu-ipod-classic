#ifndef HW_IPOD_CLASSIC_PLAYTRACE_H
#define HW_IPOD_CLASSIC_PLAYTRACE_H
/* Bounded debug observers only; never modify guest state. */
void ipod_playtrace_key(int keycode);
void ipod_playtrace_sample(void);
void ipod_playtrace_keyentry(uint32_t pc, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3, uint32_t lr);
#endif
