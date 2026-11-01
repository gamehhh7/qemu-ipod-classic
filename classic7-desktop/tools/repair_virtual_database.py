"""Migrate the imported file's normal hash58 to this virtual device's GUID.

Only the existing dedicated SQLite virtual medium is writable. Require its
exclusive runtime lock and exact original file digest. No firmware/hash-check
patch, physical device, full image read, format, or database track changes.
"""
from pathlib import Path
import hashlib,json,msvcrt,sqlite3,struct

R=Path(__file__).resolve().parents[1]
O=R/'acceptance/restore-fix-20261029'
plan=json.loads((O/'DATABASE-REPAIR-PLAN.json').read_text(encoding='utf-8'))
identity=json.loads((R/'userdata/device-identity.json').read_text(encoding='utf-8'))
config=json.loads((R/'userdata/windows-disk.json').read_text(encoding='utf-8'))
if plan['guid']!=identity['firewire_guid']:raise ValueError('Current native manufacturing GUID changed')
new=(O/'iTunesDB.current-device').read_bytes()
if hashlib.sha256(new).hexdigest()!=plan['new_sha256']:raise ValueError('Prepared file changed')
overlay=R/'userdata'/config['overlay_file']
lock=open(str(overlay)+'.lock','r+b');db=None;base=None;locked=False
result={'purpose':'normal imported database integrity migration to configured native GUID','physical_device':False,'format':False,'firmware_modified':False,'whole_image_read':False,'guid':plan['guid']}
try:
    lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1);locked=True
    db=sqlite3.connect(overlay.as_uri()+'?mode=rw',uri=True,isolation_level=None)
    db.execute('PRAGMA synchronous=FULL');db.execute('BEGIN IMMEDIATE')
    base=Path(config['original_seed']).open('rb')
    cap=config['virtual_sectors_512'];zero=config['zero_from_sector_512']
    def read(lba,n):
        if not 0<=lba<=cap-n or not 0<n<=2048:raise ValueError('bounded virtual sector span')
        rows=dict(db.execute('SELECT lba,data FROM sectors WHERE lba>=? AND lba<?',(lba,lba+n)));data=[]
        for k in range(lba,lba+n):
            if k in rows:data.append(rows[k])
            elif k>=zero:data.append(bytes(512))
            else:base.seek(k*512);data.append(base.read(512))
        return b''.join(data)
    m=read(0,1);start=struct.unpack_from('<I',m,470)[0]*8
    b=read(start,1);bps=struct.unpack_from('<H',b,11)[0];spc=b[13];reserved=struct.unpack_from('<H',b,14)[0];nf=b[16]
    fat=struct.unpack_from('<I',b,36)[0];root=struct.unpack_from('<I',b,44)[0];total=struct.unpack_from('<I',b,32)[0]
    if bps!=4096 or not spc or b[510:512]!=b'\x55\xaa':raise ValueError('Existing FAT32 geometry changed')
    scale=bps//512;first=start+(reserved+nf*fat)*scale;maxcluster=(total-reserved-nf*fat)//spc+2
    def chain(c):
        seen=set()
        while c<0x0ffffff8:
            if not 2<=c<maxcluster or c in seen or len(seen)>64:raise ValueError('bounded target chain invalid')
            seen.add(c);yield c
            offset=c*4;raw=read(start+reserved*scale+offset//512,1);c=struct.unpack_from('<I',raw,offset%512)[0]&0x0fffffff
    def sector(c):return first+(c-2)*spc*scale
    def find(c,name):
        long={}
        for cluster in chain(c):
            data=read(sector(cluster),spc*scale)
            for off in range(0,len(data),32):
                e=data[off:off+32]
                if not e[0]:raise FileNotFoundError(name)
                if e[0]==229:long={};continue
                if e[11]==15:
                    if e[0]&64:long={}
                    long[e[0]&31]=e[1:11]+e[14:26]+e[28:32];continue
                if long:n=b''.join(long[k] for k in sorted(long)).decode('utf-16le').split('\0',1)[0].rstrip('\uffff')
                else:
                    stem=e[:8].decode('cp437').rstrip();ext=e[8:11].decode('cp437').rstrip();n=stem+('.'+ext if ext else '')
                long={}
                if not e[11]&8 and n.casefold()==name.casefold():return e
        raise FileNotFoundError(name)
    c=root
    for part in ['iPod_Control','iTunes']:
        e=find(c,part)
        if not e[11]&16:raise ValueError('Target parent is not a directory')
        c=(struct.unpack_from('<H',e,20)[0]<<16)|struct.unpack_from('<H',e,26)[0]
    e=find(c,'iTunesDB');length=struct.unpack_from('<I',e,28)[0]
    if length!=len(new):raise ValueError('User restore changed database length; do not overwrite')
    c=(struct.unpack_from('<H',e,20)[0]<<16)|struct.unpack_from('<H',e,26)[0];clusters=list(chain(c))
    old=b''.join(read(sector(x),spc*scale) for x in clusters)[:length];digest=hashlib.sha256(old).hexdigest()
    if digest==plan['new_sha256']:result.update(status='already_current',changed=False)
    elif digest!=plan['old_sha256']:raise ValueError('User restore changed database bytes; do not overwrite')
    else:
        if old[:88]!=new[:88] or old[108:]!=new[108:]:raise ValueError('Prepared change is not checksum-only')
        target=sector(c);data=bytearray(read(target,1));data[88:108]=new[88:108]
        db.execute('INSERT INTO sectors(lba,data) VALUES(?,?) ON CONFLICT(lba) DO UPDATE SET data=excluded.data',(target,bytes(data)))
        result.update(status='database_identity_updated',changed=True,changed_ATA512_sector=target,changed_file_offset=88,changed_bytes=20,old_sha256=digest,new_sha256=plan['new_sha256'],tracks_preserved=True)
    db.execute('COMMIT')
except BaseException as e:
    if db is not None and db.in_transaction:db.execute('ROLLBACK')
    result.update(status='not_changed',error=repr(e));raise
finally:
    if base:base.close()
    if db:db.close()
    if locked:lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)
    lock.close();(O/'DATABASE-REPAIR-RESULT.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))
