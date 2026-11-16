"""Passive observation of USER-triggered restore of the proved virtual iPod.
Read only COW boot sectors and actual UDE PnP inventory; never drive iTunes.
"""
from pathlib import Path
import datetime,json,sqlite3,struct,sys,time,subprocess
R=Path(__file__).resolve().parents[1];O=R/'acceptance/user-restore-20261029';O.mkdir(exist_ok=True)
db=R/'userdata/windows-8g.sqlite3';end=time.monotonic()+360
def save(name,x):(O/name).write_text(json.dumps(x,ensure_ascii=False,indent=2),encoding='utf-8')
def disk():
    with sqlite3.connect('file:'+db.as_posix()+'?mode=ro',uri=True,timeout=2) as c:
        c.execute('PRAGMA query_only=ON')
        def sector(n):
            x=c.execute('SELECT data FROM sectors WHERE lba=?',(n,)).fetchone();return bytes(x[0]) if x else bytes(512)
        m=sector(0);start,count=struct.unpack_from('<II',m,470);b=sector(start*8)
        return dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),MBR_signature=m[510:512].hex(),partition2_start_units4096=start,partition2_sectors4096=count,
                    FAT_BPB_bytes_per_sector=struct.unpack_from('<H',b,11)[0],FAT_BPB_sectors_per_cluster=b[13],FAT_BPB_total_sectors=struct.unpack_from('<I',b,32)[0],
                    FAT_type_marker=b[82:90].decode('ascii',errors='replace'),boot_signature=b[510:512].hex(),MBR_hex=m.hex(),BPB_first512_hex=b.hex(),
                    physical_device=False,observer_writes_to_virtual_disk=False)
before=disk();save('BEFORE.json',before)
p=subprocess.run([sys.executable,'-B',str(R/'tools/virtual_pnp_inventory.py'),str(O/'PNP-BEFORE.json')],capture_output=True,text=True,timeout=10)
proof=json.loads((O/'PNP-BEFORE.json').read_text(encoding='utf-8'))
if p.returncode or len(proof['virtual_usb'])!=1 or not proof['virtual_usb'][0]['ude_parent_proven']:raise SystemExit('No unique current UDE virtual target; not armed')
save('ARMED.json',dict(pid=__import__('os').getpid(),utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),duration_s=360,target=proof['virtual_usb'][0],role='passive only; user clicks Restore; no simulator/driver/reboot/format actions'))
print('ARMED: user may operate restore on the unique virtual iPod',flush=True)
last={k:v for k,v in before.items() if k!='utc'};changes=0
try:
    with (O/'OBSERVATIONS.jsonl').open('x',encoding='utf-8') as f:
        while time.monotonic()<end:
            try:
                row=disk();current={k:v for k,v in row.items() if k!='utc'}
                if current!=last:
                    f.write(json.dumps(row,ensure_ascii=False)+'\n');f.flush();changes+=1;last=current
                    save('LATEST.json',row);print('Actual virtual boot metadata changed:',row['FAT_type_marker'],row['FAT_BPB_bytes_per_sector'],flush=True)
            except Exception as e:
                f.write(json.dumps(dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),error=repr(e)))+'\n');f.flush()
            time.sleep(1)
finally:
    save('END.json',dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),changes=changes,disk=disk(),user_session_stopped=False,restore_success_not_inferred=True))
    print('Passive observer ended; user simulator session left running',flush=True)
