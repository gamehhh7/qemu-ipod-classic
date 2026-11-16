"""Restore the accepted seed's reserved FAT volume marker, without formatting.

The retail classifier's final fallback at 0x0808dbdc compares VBR+0x34
with FAT. An ordinary FAT32 VBR with empty nested partition entries otherwise
fails classification before the FAT driver sees it. Preserve all allocated
files, FAT entries, geometry, firmware and original seed.
"""
from pathlib import Path
import datetime,hashlib,json,msvcrt,sqlite3,struct

R=Path(__file__).resolve().parents[1]
O=R/'acceptance/mount-20261030'
O.mkdir(exist_ok=True)
cfg=json.loads((R/'userdata/windows-disk.json').read_text(encoding='utf-8'))
overlay=R/'userdata'/cfg['overlay_file']
lock=open(str(overlay)+'.lock','r+b')
db=None
locked=False
result={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'purpose':'repair native volume classification using existing seed convention',
        'format':False,'files_relocated':False,'firmware_changed':False,
        'seed_written':False,'physical_device':False,'changes':[]}
try:
    lock.seek(0)
    msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
    locked=True
    db=sqlite3.connect(overlay.as_uri()+'?mode=rw',uri=True,isolation_level=None)
    db.execute('PRAGMA synchronous=FULL')
    db.execute('BEGIN IMMEDIATE')
    mbr=db.execute('SELECT data FROM sectors WHERE lba=0').fetchone()[0]
    start=struct.unpack_from('<I',mbr,470)[0]*8
    assert start==cfg['zero_from_sector_512']==182880
    boot=db.execute('SELECT data FROM sectors WHERE lba=?',(start,)).fetchone()[0]
    bps=struct.unpack_from('<H',boot,11)[0]
    backup=struct.unpack_from('<H',boot,50)[0]
    assert bps==4096 and backup==6
    with Path(cfg['original_seed']).open('rb') as seed:
        seed.seek(start*512)
        old=seed.read(512)
    assert old[0x34:0x37]==b'FAT', 'accepted original seed convention changed'
    # Touch only the two existing VBR sectors; no full-image copy or read.
    targets=[start,start+backup*(bps//512)]
    for lba in targets:
        before=db.execute('SELECT data FROM sectors WHERE lba=?',(lba,)).fetchone()[0]
        assert len(before)==512 and before[510:512]==b'\x55\xaa'
        assert before[0x34:0x37] in (bytes(3),b'FAT')
        after=bytearray(before)
        after[0x34:0x37]=b'FAT'
        assert before[:0x34]==after[:0x34] and before[0x37:]==after[0x37:]
        if before!=after:
            db.execute('UPDATE sectors SET data=? WHERE lba=?',(bytes(after),lba))
        result['changes'].append({'ATA512_lba':lba,'offset':52,
            'before_hex':before[52:55].hex(),'after_hex':bytes(after[52:55]).hex(),
            'before_sector_sha256':hashlib.sha256(before).hexdigest(),
            'after_sector_sha256':hashlib.sha256(after).hexdigest()})
    db.execute('COMMIT')
    result['status']='reserved_marker_repaired'
    result['firmware_mount_acceptance']='pending real finished app'
except BaseException as error:
    if db is not None and db.in_transaction:db.execute('ROLLBACK')
    result.update(status='failed',error=repr(error))
    raise
finally:
    if db is not None:db.close()
    if locked:
        lock.seek(0)
        msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)
    lock.close()
    (O/'MARKER-REPAIR.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))
