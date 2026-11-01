"""Read a consistent virtual FAT snapshot; never open a physical disk or write media."""
from pathlib import Path
import argparse,hashlib,json,sqlite3,struct

R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--verify-transfer',type=Path,action='append',default=[])
a=p.parse_args()
verify={}
for manifest in a.verify_transfer:
    row=json.loads(manifest.read_text(encoding='utf-8-sig'))
    name=Path(row['file']).name
    if not row['success'] or not name.startswith('Codex-transfer-') or not 0<row['bytes']<=33554432:
        raise ValueError('only previously created bounded transfer files can be verified')
    verify[name]=(row['bytes'],row['sha256'])
cfg=json.loads((R/'userdata/windows-disk.json').read_text(encoding='utf-8'))
db=sqlite3.connect((R/'userdata'/cfg['overlay_file']).as_uri()+'?mode=ro',uri=True)
db.execute('BEGIN')
base=Path(cfg['original_seed']).open('rb')
def read(lba,n):
    assert 0<n<=8192 and 0<=lba<=cfg['virtual_sectors_512']-n
    rows=dict(db.execute('SELECT lba,data FROM sectors WHERE lba>=? AND lba<?',(lba,lba+n)))
    result=[]
    for k in range(lba,lba+n):
        if k in rows:result.append(rows[k])
        elif k>=cfg['zero_from_sector_512']:result.append(bytes(512))
        else:base.seek(k*512);result.append(base.read(512))
    return b''.join(result)
try:
    m=read(0,1)
    start=struct.unpack_from('<I',m,470)[0]*8
    b=read(start,1)
    bps=struct.unpack_from('<H',b,11)[0]
    scale=bps//512
    spc=b[13]
    reserved=struct.unpack_from('<H',b,14)[0]
    nf=b[16]
    fatlen=struct.unpack_from('<I',b,36)[0]
    root=struct.unpack_from('<I',b,44)[0]
    total=struct.unpack_from('<I',b,32)[0]
    assert bps==4096 and spc==8 and nf==2 and b[510:512]==b'\x55\xaa'
    clusters=(total-reserved-nf*fatlen)//spc
    first=start+(reserved+nf*fatlen)*scale
    fat=read(start+reserved*scale,fatlen*scale)
    def chain(c):
        seen=set()
        while c<0x0ffffff8:
            if not 2<=c<clusters+2 or c in seen or len(seen)>16384:raise ValueError('invalid FAT chain')
            seen.add(c)
            yield c
            c=struct.unpack_from('<I',fat,c*4)[0]&0x0fffffff
    def data(c):return read(first+(c-2)*spc*scale,spc*scale)
    files=[]
    dirs=set()
    def walk(c,path='',depth=0):
        if depth>24 or c in dirs:raise ValueError('directory cycle/depth')
        dirs.add(c)
        long={}
        for block in chain(c):
            z=data(block)
            for off in range(0,len(z),32):
                e=z[off:off+32]
                if not e[0]:return
                if e[0]==229:long={};continue
                if e[11]==15:
                    if e[0]&64:long={}
                    long[e[0]&31]=e[1:11]+e[14:26]+e[28:32]
                    continue
                if long:name=b''.join(long[k] for k in sorted(long)).decode('utf-16le').split('\0',1)[0].rstrip('\uffff')
                else:
                    stem=e[:8].decode('cp437').rstrip();ext=e[8:11].decode('cp437').rstrip()
                    name=stem+('.'+ext if ext else '')
                long={}
                if e[11]&8 or name in ('.','..'):continue
                c1=(struct.unpack_from('<H',e,20)[0]<<16)|struct.unpack_from('<H',e,26)[0]
                size=struct.unpack_from('<I',e,28)[0]
                target=path+'/'+name if path else name
                if e[11]&16:walk(c1,target,depth+1)
                else:
                    item={'path':target,'size':size}
                    if target in verify:
                        expected_size,expected_hash=verify[target]
                        if size!=expected_size:raise ValueError('persisted transfer size mismatch')
                        digest=hashlib.sha256();remaining=size
                        for cluster in chain(c1):
                            part=data(cluster)[:remaining];digest.update(part);remaining-=len(part)
                            if not remaining:break
                        if remaining or digest.hexdigest()!=expected_hash:
                            raise ValueError('persisted transfer hash mismatch')
                        item.update(sha256=digest.hexdigest(),transfer_verified=True)
                    if size<=1024*1024:
                        content=b''.join(data(x) for x in chain(c1))[:size] if size else b''
                        assert len(content)==size
                        item['sha256']=hashlib.sha256(content).hexdigest()
                        if name.lower()=='itunesdb':
                            item['mhbd']=content[:4].decode('ascii',errors='replace')
                            pos=content.find(b'mhlt')
                            if 0<=pos<=len(content)-12:item['database_track_count']=struct.unpack_from('<I',content,pos+8)[0]
                    files.append(item)
                    if len(files)>10000:raise ValueError('file count bound')
    walk(root)
    if set(verify)!={x['path'] for x in files if x.get('transfer_verified')}:
        raise ValueError('a created transfer file is missing from persisted FAT')
    free=sum((struct.unpack_from('<I',fat,k*4)[0]&0x0fffffff)==0 for k in range(2,clusters+2))
    result={'snapshot':'SQLite readonly transaction, original seed rb',
            'reserved_marker_hex':b[52:55].hex(),'bytes_per_sector':bps,
            'cluster_bytes':bps*spc,'data_volume_bytes':total*bps,
            'total_clusters':clusters,'free_clusters':free,'free_bytes':free*bps*spc,
            'files':files,'physical_device':False,'media_written':False}
    a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'files':len(files),'volume_bytes':total*bps,'free_bytes':result['free_bytes'],
                      'critical':[x for x in files if x['path'].lower().endswith(('preferences','itunesdb'))]}))
finally:
    base.close()
    db.close()
