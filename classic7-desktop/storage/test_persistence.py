"""Small synthetic virtual media only, never the actual iPod seed."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile
sys.path.insert(0,'K:/CodexResources/2026-09-30/ipod-emulation/qemu-fc1307/playback-runtime/bridge/large-capacity/src')
from persistent_store import PersistentStore
R=Path(__file__).parents[1]/'storage-tests';R.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=R) as folder:
    f=Path(folder);base=f/'synthetic.img';base.write_bytes(bytes(range(256))*8);before=hashlib.sha256(base.read_bytes()).hexdigest();overlay=f/'disk.sqlite3'
    checks=[]
    def check(name,value):assert value,name;checks.append(name)
    s=PersistentStore(base,overlay=overlay)
    check('base-read',s[1]==bytes(range(256))*2)
    s.begin();s[1]=b'X'*512;s[2]=b'Y'*512;s.commit()
    check('read-after-commit',s[1]==b'X'*512 and s[2]==b'Y'*512)
    s.begin();s[1]=b'Z'*512;s.rollback();check('transaction-rollback',s[1]==b'X'*512)
    try:second=PersistentStore(base,overlay=overlay)
    except OSError:checks.append('exclusive-writer')
    else:second.close();raise AssertionError('second writer allowed')
    for lba,data in [(4,b'a'*512),(-1,b'a'*512),(True,b'a'*512),(1,b'a')]:
        try:s[lba]=data
        except ValueError:checks.append('invalid-sector-rejected')
        else:raise AssertionError('bad write allowed')
    s.close();s=PersistentStore(base,overlay=overlay);check('reopen-persistence',s[1]==b'X'*512 and s[2]==b'Y'*512);s.close()
    code="import sys;sys.path[:0]=[sys.argv[1],sys.argv[2]];from persistent_store import PersistentStore;s=PersistentStore(sys.argv[3],overlay=sys.argv[4]);s.begin();s[3]=b'C'*512;s.commit();import os;os._exit(0)"
    subprocess.run([sys.executable,'-c',code,str(Path(__file__).parent),sys.path[0],str(base),str(overlay)],check=True)
    s=PersistentStore(base,overlay=overlay);check('committed-data-survives-process-exit',s[3]==b'C'*512);s.close()
    other=f/'different.img';other.write_bytes(b'0'*2048)
    try:PersistentStore(other,overlay=overlay)
    except ValueError:checks.append('wrong-base-rejected')
    else:raise AssertionError('wrong base accepted')
    check('original-base-unchanged',hashlib.sha256(base.read_bytes()).hexdigest()==before)
(R/'RESULT.json').write_text(json.dumps({'status':'PASS','checks':checks,'scope':'synthetic 4-sector persistent COW only; no guest, USB or Windows device claim'},indent=2))
print('persistent COW PASS',len(checks))
