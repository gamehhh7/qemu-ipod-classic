"""Resolve exact-file static symbols offline, then relocate to freshly queried base.
Does not attach or stop the emulator. Record PID creation time and SHA explicitly.
"""
from pathlib import Path
import subprocess,json,re,sys,datetime,hashlib
d=Path(__file__).parent;pid=int(sys.argv[1]);label=sys.argv[2]
ps="$taskProcess=Get-Process -Id %d; [pscustomobject]@{pid=$taskProcess.Id;creation_filetime=$taskProcess.StartTime.ToUniversalTime().ToFileTimeUtc();base=$taskProcess.MainModule.BaseAddress.ToInt64();exe=$taskProcess.Path} | ConvertTo-Json -Compress"%pid
meta=json.loads(subprocess.check_output(['powershell.exe','-NoProfile','-Command',ps],text=True))
meta['sha256']=hashlib.sha256(Path(meta['exe']).read_bytes()).hexdigest().upper()
gdb=Path(r'K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin\gdb.exe')
script=(d/'addresses138.gdb').read_text(encoding='utf8').replace('detach\n','')
extra=[]
if len(sys.argv)>3 and sys.argv[3]=='dma':
 extra=['c7_dma_cached_blocks','c7_dma_cached_bytes','c7_dma_request_ns','c7_dma_request_calls','c7_dma_compare_ns[0]','c7_dma_compare_ns[1]','c7_dma_compare_bytes[0]','c7_dma_compare_bytes[1]','c7_dma_compare_calls[0]','c7_dma_compare_calls[1]','c7_dma_compare_seen']
 script=script.replace('quit\n',''.join('p/x &'+name+'\n' for name in extra)+'quit\n')
scriptfile=d/(label+'-offline.gdb');scriptfile.write_text(script,encoding='utf8')
result=subprocess.run([str(gdb),'-nx','-batch',meta['exe'],'-x',str(scriptfile)],capture_output=True,text=True,timeout=30)
(d/(label+'-offline.txt')).write_text(result.stdout+result.stderr,encoding='utf8')
result.check_returncode()
static=[int(x,16) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+)',result.stdout)]
assert len(static)==22+len(extra)
meta.update(method='Exact binary offline GDB symbols plus fresh loaded image base; never attached',utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),addresses=[x-0x140000000+meta['base'] for x in static])
meta['extra_fields']=extra
(d/(label+'.json')).write_text(json.dumps(meta,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in meta.items() if k!='addresses'},indent=2))
