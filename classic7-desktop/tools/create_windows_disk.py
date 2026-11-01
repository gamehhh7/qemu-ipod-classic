"""Create the dedicated 8GiB virtual FAT32 data disk in a sparse COW.

Read only base MBR/identity bounds. No full seed read/copy, physical disk,
firmware patch or USB packet translation. The guest firmware retains mapping.
"""
from pathlib import Path
import hashlib,json,os,struct,sys,time
R=Path(__file__).resolve().parents[1]
config=R/'userdata/windows-disk.json';overlay=R/'userdata/windows-8g.sqlite3'
if config.exists() or overlay.exists():raise SystemExit('Dedicated disk already exists; never format it again')
seed=Path('K:/CodexResources/2026-10-01/zcode-delegation/quota-fallback/acceptance/media/E-revb35-two-songs.img')
with seed.open('rb') as f:mbr=bytearray(f.read(512))
if mbr[510:512]!=b'\x55\xaa':raise SystemExit('base MBR missing')
entry=446+16
start,total_old=struct.unpack_from('<II',mbr,entry+8)
if (start,total_old)!=(22860,16384):raise SystemExit('known seed data partition differs')
start512=start*8
total=8*1024**3//4096
virtual_sectors=start512+total*8
media=dict(format=1,overlay_file=overlay.name,virtual_sectors_512=virtual_sectors,zero_from_sector_512=start512,
           data_bytes=8*1024**3,USB_logical_sector_bytes=4096,ATA_sector_bytes=512,
           FAT32_sector_bytes=4096,cluster_bytes=32768,original_seed=str(seed),firmware_partition='unchanged readonly base',
           original_userdata_preserved=True,USB_mapping='unchanged guest firmware')
config.write_text(json.dumps(media,indent=2),encoding='utf-8')
os.environ['CLASSIC7_VIRTUAL_DISK_JSON']=str(config);os.environ['CLASSIC7_OVERLAY']=str(overlay)
formal=Path('K:/CodexResources/2026-09-30/ipod-emulation/qemu-fc1307/playback-runtime/bridge/large-capacity/src')
sys.path[:0]=[str(R/'storage'),str(formal)]
from persistent_store import PersistentStore
store=PersistentStore(str(seed))
def write512(lba,data):
    if len(data)%512:raise ValueError('unaligned virtual metadata')
    for i in range(0,len(data),512):store[lba+i//512]=data[i:i+512]
def logical(n,data):
    if len(data)!=4096:raise ValueError('logical FAT sector')
    write512(start512+n*8,data)
reserved=32;spc=8;fat=1
while True:
    clusters=(total-reserved-2*fat)//spc
    needed=((clusters+2)*4+4095)//4096
    if needed==fat:break
    fat=needed
if not 65525<=clusters<0x0ffffff5:raise ValueError('FAT32 cluster count')
boot=bytearray(4096);boot[:11]=b'\xeb\x58\x90MSWIN4.1'
struct.pack_into('<HBHBHHBHHHII',boot,11,4096,spc,reserved,2,0,0,0xf8,0,63,255,0,total)
struct.pack_into('<IHHIHH',boot,36,fat,0,0,2,1,6)
# Match the existing accepted seed's volume classifier convention. The retail
# classifier at 0x0808dbdc checks FAT at +0x34 after finding no nested MBR.
# +0x34 is reserved FAT32 metadata; cluster locations and BPB geometry stay intact.
boot[0x34:0x37]=b'FAT'
boot[64]=0x80;boot[66]=0x29;struct.pack_into('<I',boot,67,int(time.time())&0xffffffff)
boot[71:82]=b'VIRTUALIPOD';boot[82:90]=b'FAT32   ';boot[510:512]=b'\x55\xaa';boot[-2:]=b'\x55\xaa'
info=bytearray(4096);struct.pack_into('<I',info,0,0x41615252);struct.pack_into('<I',info,484,0x61417272)
struct.pack_into('<II',info,488,clusters-1,3);struct.pack_into('<I',info,508,0xaa550000)
fat0=bytearray(4096);struct.pack_into('<III',fat0,0,0x0ffffff8,0xffffffff,0x0fffffff)
root=bytearray(4096);root[:11]=b'VIRTUALIPOD';root[11]=8
mbr[entry+4]=0x0b;struct.pack_into('<I',mbr,entry+12,total)
store.begin()
try:
    write512(0,bytes(mbr));logical(0,bytes(boot));logical(1,bytes(info));logical(6,bytes(boot));logical(7,bytes(info))
    # Unwritten new-disk sectors are sparse zero, including unused FAT/root.
    logical(reserved,bytes(fat0));logical(reserved+fat,bytes(fat0));logical(reserved+2*fat,bytes(root))
    store.commit()
except BaseException:store.rollback();raise
finally:store.close()
result=dict(media,FAT_sectors=fat,clusters=clusters,created=True,original_seed_write=False,physical_devices=False,
            initialized_metadata_bytes=512+7*4096,volume_acceptance='pending actual finished EXE + Windows')
(R/'acceptance/DEDICATED-DISK.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('Created dedicated 8GiB FAT32 sparse virtual disk;',clusters,'clusters; original media preserved')
