"""File-level migration source: readonly old virtual FAT, never whole-image copy."""
from pathlib import Path
import hashlib,json,sqlite3,struct

R=Path(__file__).resolve().parents[1]
O=R/'acceptance/MEDIA-IMPORT-20261029'
O.mkdir(exist_ok=True)
dest=O/'source-files';dest.mkdir(exist_ok=True)
seed=Path('K:/CodexResources/2026-10-01/zcode-delegation/quota-fallback/acceptance/media/E-revb35-two-songs.img')
db=sqlite3.connect((R/'userdata/disk.sqlite3').as_uri()+'?mode=ro',uri=True)
db.execute('BEGIN')
base=seed.open('rb');base_sectors=seed.stat().st_size//512
reads=0
def read(lba,n):
    global reads
    if not 0<=lba<=base_sectors-n or not 0<n<=8192:raise ValueError('bounded original virtual sector read')
    rows=dict(db.execute('SELECT lba,data FROM sectors WHERE lba>=? AND lba<?',(lba,lba+n)))
    result=[]
    for k in range(lba,lba+n):
        if k in rows:result.append(rows[k])
        else:base.seek(k*512);result.append(base.read(512))
    reads+=n
    return b''.join(result)
mbr=read(0,1);start,count=struct.unpack_from('<II',mbr,470);start*=8
boot=read(start,1);bps=struct.unpack_from('<H',boot,11)[0];spc=boot[13]
reserved=struct.unpack_from('<H',boot,14)[0];nf=boot[16];fatlen=struct.unpack_from('<I',boot,36)[0]
root=struct.unpack_from('<I',boot,44)[0];total=struct.unpack_from('<I',boot,32)[0]
if bps!=512 or not spc or total>count*8:raise ValueError('original FAT geometry changed')
first=start+reserved+nf*fatlen;limit=(total-reserved-nf*fatlen)//spc+2
fatcache={}
def chain(cluster):
    seen=set()
    while cluster<0x0ffffff8:
        if not 2<=cluster<limit or cluster in seen or len(seen)>131072:raise ValueError('invalid/cyclic original FAT chain')
        seen.add(cluster);yield cluster
        s=cluster*4//512
        if s not in fatcache:fatcache[s]=read(start+reserved+s,1)
        cluster=struct.unpack_from('<I',fatcache[s],cluster*4%512)[0]&0x0fffffff
def clusterdata(c):return read(first+(c-2)*spc,spc)
def checksum(name):
    v=0
    for b in name:v=(((v&1)<<7)|(v>>1))+b;v&=255
    return v
manifest=[];dirs=[];directory_clusters=set();byte_total=0
def walk(c,relative=Path(),depth=0):
    global byte_total
    if depth>24 or c in directory_clusters:raise ValueError('directory depth/cycle')
    directory_clusters.add(c);lfn={};last=None;cs=None
    for block in chain(c):
        data=clusterdata(block)
        for off in range(0,len(data),32):
            e=data[off:off+32]
            if not e[0]:return
            if e[0]==229:lfn={};last=None;continue
            if e[11]==15:
                n=e[0]&31
                if e[0]&64:lfn={};last=n;cs=e[13]
                if not 1<=n<=20 or cs!=e[13]:raise ValueError('invalid original LFN')
                lfn[n]=e[1:11]+e[14:26]+e[28:32];continue
            short=e[:11];attr=e[11]
            if lfn:
                if last is None or set(lfn)!=set(range(1,last+1)) or checksum(short)!=cs:raise ValueError('original LFN checksum/order')
                raw=b''.join(lfn[x] for x in range(1,last+1));name=raw.decode('utf-16le').split('\0',1)[0].rstrip('\uffff')
            else:
                stem=short[:8].decode('cp437').rstrip();ext=short[8:].decode('cp437').rstrip();name=stem+('.'+ext if ext else '')
            lfn={};last=None
            if attr&8 or name in ('.','..'):continue
            if not name or any(x in name for x in '/\\:') or name.endswith((' ','.')):raise ValueError('unsafe original filename '+repr(name))
            rel=relative/name;target=dest/rel
            cluster=(struct.unpack_from('<H',e,20)[0]<<16)|struct.unpack_from('<H',e,26)[0]
            size=struct.unpack_from('<I',e,28)[0]
            if attr&16:
                target.mkdir(exist_ok=True);dirs.append(dict(path=rel.as_posix(),attributes=attr));walk(cluster,rel,depth+1)
            else:
                byte_total+=size
                if byte_total>64*1024**2 or len(manifest)>=5000:raise ValueError('bounded file migration limit')
                h=hashlib.sha256();left=size
                with target.open('xb') as f:
                    if left:
                        for chunk in chain(cluster):
                            part=clusterdata(chunk)[:left];f.write(part);h.update(part);left-=len(part)
                            if not left:break
                    if left:raise ValueError('short original file chain')
                manifest.append(dict(path=rel.as_posix(),attributes=attr,size=size,sha256=h.hexdigest()))
try:
    walk(root)
    result=dict(source='original virtual seed rb + original readonly SQLite COW snapshot',whole_image_copy=False,physical_device=False,files=manifest,directories=dirs,total_bytes=byte_total,source_sectors_read=reads)
    (O/'SOURCE.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(files=len(manifest),directories=len(dirs),bytes=byte_total,paths=[x['path'] for x in manifest]),ensure_ascii=True))
finally:base.close();db.close()
