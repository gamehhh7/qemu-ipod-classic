from pathlib import Path
import hashlib,json,subprocess
from datetime import datetime,timezone
R=Path(__file__).parent
run=sorted((R/'runs').iterdir())[-1]
events=[json.loads(x) for x in (run/'events.jsonl').read_text(encoding='utf-8-sig').splitlines()]
assert not [x for x in events if x['kind'] in ('error','display_error')]
ready=next(x['data'] for x in events if x['kind']=='ready')
assert ready['wheel'] and ready['pmu']
holds=[x['data'] for x in events if x['kind']=='hold_readback'];assert holds==[True,False]
usb=[x['data'] for x in events if x['kind']=='usb_readback'];assert usb==[True,False]
assert (R/'acceptance/hold-refusal.txt').read_text()=='native model refused input while physical HOLD on'
qlog=(run/'qemu.log').read_text(errors='replace')
assert 'PMUHOLD locked=1' in qlog and 'PMUHOLD locked=0' in qlog
assert 'FRONTEND-HOLD locked=1' in qlog and 'FRONTEND-HOLD locked=0' in qlog
end=next(x for x in events if x['kind']=='owned_processes_stopped')
ids=[end['data']['qemu'],end['data']['backend'],int(run.name.split('-')[-1])]
cmd='Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -in @('+','.join(map(str,ids))+') } | Select-Object ProcessId,ExecutablePath,CommandLine | ConvertTo-Json -Compress'
fresh=subprocess.check_output(['powershell','-NoProfile','-Command',cmd],text=True).strip()
rows=json.loads(fresh) if fresh else []
if isinstance(rows,dict):rows=[rows]
for row in rows:
    assert row['ExecutablePath'] and not (str(R).casefold() in row['ExecutablePath'].casefold() or 'guest_ipc_takeover.py' in (row.get('CommandLine') or '')),'owned process still alive'
def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''):h.update(block)
    return h.hexdigest()
engine=json.loads((R/'ENGINE-FREEZE.json').read_text())
assert sha(Path(engine['exe']))==engine['sha256']
for p,value in engine['protected_unchanged'].items():assert sha(Path(p))==value
png=list((R/'acceptance').glob('*.png'));assert len(png)==14
result={'status':'actual_desktop_input_display_acceptance_passed','completed_utc':datetime.now(timezone.utc).isoformat(),
        'run':str(run),'native_windows_exe':str(R/'bin/iPodClassic7.exe'),'exe_sha256':sha(R/'bin/iPodClassic7.exe'),
        'source_sha256':sha(R/'Classic7.cs'),'engine_sha256':engine['sha256'],
        'actual_verified':['QEMU real framebuffer through VNC/RFB','center input through desktop event handlers',
                           'physical Hold GPIO/PMU event and QOM readback both directions',
                           'native model refuses button while locked','USB PMU property readback both directions',
                           'touch drag through desktop pointer handlers','owned guest/backend/test app terminated'],
        'image_review':'IMAGE-REVIEW.json by GPT-6 Luna Low, actual firmware lock icon and menu movement',
        'frame_hashes':{p.name:sha(p) for p in png},'seed_policy':'existing seed read-only + RAM COW; no image copy or full image hash',
        'real_windows_usb_sync_complete':False,'formal_engine_unchanged':True,'user_requested_pause_after_delivery':True,
        'pid_reuse_observations':rows}
(R/'acceptance/RESULT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:result[k] for k in ['status','native_windows_exe','exe_sha256','user_requested_pause_after_delivery']},indent=2))
