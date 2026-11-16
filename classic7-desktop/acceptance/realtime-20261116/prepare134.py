from pathlib import Path
import json
p=Path(r'K:\CodexResources\2026-10-28\ipod-classic7-desktop');d=p/'acceptance/realtime-20261116'
a=(d/'r133-pmu.c').read_text(encoding='utf-8-sig')
a=a.replace('const char *e = getenv("CLASSIC7_WAKE_TRACE");\n        enabled = !e || strcmp(e, "0");','const char *e = getenv("CLASSIC7_PMU_TRACE");\n        enabled = e && !strcmp(e, "1");')
assert 'getenv("CLASSIC7_PMU_TRACE")' in a
(d/'r134-pmu.c').write_text(a,encoding='utf8')
r=json.loads((d/'compile133.json').read_text()); job=r[1]
job=[x.replace('r133-pmu','r134-pmu') for x in job]
(d/'compile134.json').write_text(json.dumps([job]))
rsp=(d/'r133b.rsp').read_text().replace('qemu-system-arm-classic7.r133b.exe','qemu-system-arm-classic7.r134.exe').replace('/objects/r133-pmu.obj','/objects/r134-pmu.obj')
(d/'r134.rsp').write_text(rsp)
a=(p/'Classic7.cs').read_text(encoding='utf-8-sig').replace('qemu-system-arm-classic7.exe','qemu-system-arm-classic7.r134.exe')
(d/'Classic7.r134.cs').write_text(a,encoding='utf8')
