from pathlib import Path
import json
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop'); d=p/'acceptance/realtime-20261116'
# Trace configuration is immutable for a running process. Cache only flags,
# preserving all register/data/timer/IRQ behaviour byte-for-byte.
a=(p/'src/usb/observed_i2c.c').read_text(encoding='utf-8-sig')
a=a.replace('    const char *power = getenv("CLASSIC7_POWER_TRACE");\n    bool power_trace = power && !strcmp(power, "1");','    static int power_trace = -1;\n    if (power_trace < 0) {\n        const char *power = getenv("CLASSIC7_POWER_TRACE");\n        power_trace = power && !strcmp(power, "1");\n    }')
(d/'r133-i2c.c').write_text(a,encoding='utf8')
a=(p/'src/usb/observed_pmu.c').read_text(encoding='utf-8-sig')
a=a.replace('    const char *e = getenv("CLASSIC7_WAKE_TRACE");\n    return !e || strcmp(e, "0");','    static int enabled = -1;\n    if (enabled < 0) {\n        const char *e = getenv("CLASSIC7_WAKE_TRACE");\n        enabled = !e || strcmp(e, "0");\n    }\n    return enabled;')
a=a.replace('    const char *enabled = getenv("CLASSIC7_POWER_TRACE");\n    if (!enabled || strcmp(enabled, "1") || sequence >= 32768) {','    static int enabled = -1;\n    if (enabled < 0) {\n        const char *e = getenv("CLASSIC7_POWER_TRACE");\n        enabled = e && !strcmp(e, "1");\n    }\n    if (!enabled || sequence >= 32768) {')
# PMUTRACE is also a launch-time switch. Cache its defined/not-defined semantics.
a=a.replace('static int64_t wake_trace_until;', '''static bool pmu_trace_on(void)
{
    static int enabled = -1;
    if (enabled < 0) { enabled = getenv("PMUTRACE") != NULL; }
    return enabled;
}
static int64_t wake_trace_until;''')
a=a.replace('if (getenv("PMUTRACE"))', 'if (pmu_trace_on())')
(d/'r133-pmu.c').write_text(a,encoding='utf8')
r=json.loads((p/'engine/build-recipe.json').read_text(encoding='utf-8-sig')); jobs=[]
for name in ('i2c','pmu'):
    base=next(x['argv'] for x in r['compile'] if x['name']==name)
    args=[x.replace('objects\\'+name+'.','objects\\r133-'+name+'.').replace(str(p/('src/usb/observed_'+name+'.c')),str(d/('r133-'+name+'.c'))) for x in base]
    args[1:1]=['-iquote',str(p/'src/usb')]; jobs.append(args)
(d/'compile133.json').write_text(json.dumps(jobs),encoding='utf8')
rsp=(p/'engine/classic7.rsp').read_text(encoding='utf-8-sig')
rsp=rsp.replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r133.exe')
for name in ('i2c','pmu'):
    old='/objects/'+name+'.obj'; assert old in rsp
    rsp=rsp.replace(old,'/objects/r133-'+name+'.obj')
(d/'r133.rsp').write_text(rsp,encoding='utf8')
cs=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r133.exe')
(d/'Classic7.r133.cs').write_text(cs,encoding='utf8')
