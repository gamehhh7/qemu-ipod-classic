"""Offline checkpoint before relocating the one existing virtual COW."""
from pathlib import Path
import argparse,json,sqlite3,msvcrt,datetime
p=argparse.ArgumentParser();p.add_argument('database',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
assert a.database.is_file() and a.database.suffix=='.sqlite3'
with open(str(a.database)+'.lock','r+b') as lock:
    lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
    db=sqlite3.connect(a.database.as_uri()+'?mode=rw',uri=True,isolation_level=None)
    try:
        result=db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone();assert result[0]==0,result
        check=db.execute('PRAGMA quick_check').fetchall();assert check==[('ok',)],check
        base=db.execute("SELECT value FROM metadata WHERE key='base'").fetchone();assert base
        out=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),database=str(a.database),checkpoint=result,quick_check='ok',base_identity=json.loads(base[0]),bytes=a.database.stat().st_size,sole_writer_lock_acquired=True)
        a.output.write_text(json.dumps(out,indent=2),encoding='utf-8');print(json.dumps(dict(database=str(a.database),quick_check='ok',bytes=out['bytes'])))
    finally:db.close()
