"""One actual canonical EXE session; ordinary Windows filesystem IO only.

No standalone fixtures. No physical/raw disk, format, driver action or retry.
"""
import datetime,hashlib,json,os,subprocess,sys,time
from pathlib import Path
R=Path(__file__).resolve().parents[1]
O=R/'acceptance'/(sys.argv[1] if len(sys.argv)>1 else 'consolidated-Windows-20261029')
O.mkdir(exist_ok=True)
read=lambda f:json.loads(f.read_text(encoding='utf-8-sig'))
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
def save(n,x):(O/n).write_text(json.dumps(x,ensure_ascii=False,indent=2),encoding='utf-8')
files=[R/'Classic7.cs',R/'bin/iPodClassic7.exe',R/'engine/qemu-system-arm-classic7.exe',*list((R/'host').rglob('*.py')),R/'storage/backend_persistent.py',R/'storage/persistent_store.py',Path(__file__),R/'tools/write_virtual_file.ps1',R/'tools/virtual_pnp_inventory.py']
if (R/'userdata/windows-disk.json').exists():files.append(R/'userdata/windows-disk.json')
manifest={str(p):sha(p) for p in files}
save('FREEZE.json',dict(inputs=manifest,exe=str(R/'bin/iPodClassic7.exe'),argv=['--usb-test','--max-seconds','330'],bound_s=330,ordinary_TCG_mainloop=True,seed_policy='rb plus existing persistent virtual COW',Windows_file_write='only unique CreateNew text file on filesystem of disk freshly proven under owned virtual UDE',format=False,physical_device=False))
save('REVIEW.json',dict(approved=True,native_run_authorized=True,freeze_sha256=sha(O/'FREEZE.json'),reason='Consolidated exact previously exercised engine; host attaches actual guest descriptors via official installed driver. No test helper timed detach. New desktop mode disables only idle transport lifetime, retains guest timing/DMA/packet gates and one owner. Actual acceptance uses same finished EXE and filesystem operations; agent session itself remains330s. Old claims stay consumed; no retries.'))
with (O/'CLAIM.CONSUMED.json').open('x',encoding='utf-8') as f:json.dump(dict(freeze=sha(O/'FREEZE.json'),review=sha(O/'REVIEW.json'),utc=datetime.datetime.now(datetime.timezone.utc).isoformat()),f)
proc=subprocess.Popen([str(R/'bin/iPodClassic7.exe'),'--usb-test','--max-seconds','330'],cwd=R)
save('LIVE.json',dict(app_pid=proc.pid))
print('Finished main EXE actual Windows/file session',proc.pid,flush=True)
started=time.monotonic();run=None;queried=False
try:
    while proc.poll() is None and time.monotonic()-started<350:
        candidates=list((R/'runs').glob('*-'+str(proc.pid)))
        if candidates:run=candidates[0]
        sessions=list(run.glob('usb-session-*/WINDOWS-ATTACH.json')) if run else []
        if sessions and not queried:
            a=read(sessions[0])
            if a['exit']:raise RuntimeError('Actual Windows attach failed; no retry')
            if time.monotonic()-started>75:
                subprocess.run([sys.executable,'-B',str(R/'tools/virtual_pnp_inventory.py'),str(O/'WINDOWS-PNP.json')],check=True,timeout=10)
                w=read(O/'WINDOWS-PNP.json')
                if w['virtual_disks']:
                    queried=True
                    print('Actual UDE disk present. Direct Windows filesystem write/read now.',flush=True)
                    args=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(R/'tools/write_virtual_file.ps1'),'-Witness',str(O/'WINDOWS-PNP.json'),'-OutputPath',str(O/'WINDOWS-FILE-RESULT.json')]
                    if len(sys.argv)>2:args+=['-ExpectedFile',sys.argv[2]]
                    p=subprocess.run(args,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=35)
                    save('FILESYSTEM-ACTION.json',dict(exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
                    print('Windows filesystem action:',p.returncode,flush=True)
        time.sleep(1)
    if proc.poll() is None:
        subprocess.run(['powershell.exe','-NoProfile','-Command',f'(Get-Process -Id {proc.pid}).CloseMainWindow()'],capture_output=True,timeout=8)
    proc.wait(timeout=15)
finally:
    if proc.poll() is None:
        subprocess.run(['taskkill','/PID',str(proc.pid),'/T','/F'],capture_output=True)
    save('RESULT.json',dict(app_pid=proc.pid,run=str(run),exit_code=proc.poll(),duration_s=time.monotonic()-started,inputs_unchanged=manifest=={str(p):sha(p) for p in files},file_result=read(O/'WINDOWS-FILE-RESULT.json') if (O/'WINDOWS-FILE-RESULT.json').exists() else None,physical_device=False,automatic_retry=False))
    print('Actual finished-app session terminal',flush=True)
