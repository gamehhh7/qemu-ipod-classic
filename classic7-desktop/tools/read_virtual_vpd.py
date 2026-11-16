"""Read actual firmware VPD through Windows SCSI on a freshly proved virtual volume.
Never a physical disk path. No SCSI write/format/reset/restore operations.
"""
import ctypes as c,json,subprocess,sys,time,plistlib
from ctypes import wintypes as w
from pathlib import Path
from virtual_pnp_inventory import witness
out=Path(sys.argv[1]);prior=json.loads(out.read_text(encoding='utf-8')) if '--complete' in sys.argv and out.exists() else None
result={'operations':'INQUIRY only, actual C0-announced VPD identity pages','physical_device':False,'writes':False,'pages':list(prior['pages']) if prior else []}
kernel=c.WinDLL('kernel32',use_last_error=True)
class SPT(c.Structure):
    _fields_=[('Length',w.USHORT),('ScsiStatus',c.c_ubyte),('PathId',c.c_ubyte),('TargetId',c.c_ubyte),('Lun',c.c_ubyte),('CdbLength',c.c_ubyte),('SenseInfoLength',c.c_ubyte),('DataIn',c.c_ubyte),('DataTransferLength',w.ULONG),('TimeOutValue',w.ULONG),('DataBufferOffset',c.c_size_t),('SenseInfoOffset',w.ULONG),('Cdb',c.c_ubyte*16)]
create=kernel.CreateFileW;create.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE];create.restype=w.HANDLE
io=kernel.DeviceIoControl;io.argtypes=[w.HANDLE,w.DWORD,c.c_void_p,w.DWORD,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.c_void_p];io.restype=w.BOOL
close=kernel.CloseHandle;close.argtypes=[w.HANDLE]
h=None
try:
    usb=witness(r'USB\VID_05AC&PID_1261\0000000000000000')
    if not usb['ude_parent_proven'] or usb['status']!='OK':raise RuntimeError('Current exact virtual USB owner not proved')
    # Query only ordinary Windows disk/volume metadata to resolve our own drive.
    script="$d=@(Get-CimInstance Win32_DiskDrive|Where-Object {$_.PNPDeviceID -like 'USBSTOR\\DISK&VEN_APPLE&PROD_IPOD*'});$r=@();foreach($x in $d){foreach($p in @(Get-Partition -DiskNumber $x.Index -ErrorAction SilentlyContinue)){if($p.DriveLetter){$r+=@{disk=[int]$x.Index;pnp=$x.PNPDeviceID;letter=[string]$p.DriveLetter}}}};$r|ConvertTo-Json -Compress"
    p=subprocess.run(['powershell.exe','-NoProfile','-Command',script],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=15,check=True)
    rows=json.loads(p.stdout);rows=[rows] if isinstance(rows,dict) else rows
    matches=[]
    for r in rows:
        disk=witness(r['pnp'])
        if disk['ude_parent_proven'] and usb['instance'] in disk['parent_chain']:matches.append((r,disk))
    if len(matches)!=1:raise RuntimeError('Need one actual virtual disk with a volume')
    r,disk=matches[0];result.update(usb=usb,disk=disk,volume=r)
    h=create('\\\\.\\'+r['letter']+':',0xc0000000,3,None,3,0,None)
    if h==c.c_void_p(-1).value:
        h=None;raise OSError(c.get_last_error(),'Cannot open this virtual volume for actual SCSI metadata')
    number=c.create_string_buffer(12);returned=w.DWORD()
    if not io(h,0x2d1080,None,0,number,12,c.byref(returned),None):raise c.WinError(c.get_last_error())
    import struct
    if struct.unpack_from('<I',number.raw,4)[0]!=r['disk']:raise RuntimeError('Opened volume is not the proven disk')
    if prior:
        c0=bytes.fromhex(next(p['raw_hex'] for p in prior['pages'] if p['page']==0xc0))
        announced=list(c0[4:4+int.from_bytes(c0[2:4],'big')])
        if len(announced)>64 or any(x<0xc2 for x in announced):raise RuntimeError('Identity page list outside bound')
        pages=[x for x in announced if x not in {p['page'] for p in prior['pages']}]
    else:pages=[0xc0,0xc2]
    deadline=time.monotonic()+60
    for page in pages:
        if time.monotonic()>deadline:raise TimeoutError('Actual identity read deadline')
        buf=c.create_string_buffer(512);s=SPT.from_buffer(buf);s.Length=c.sizeof(SPT);s.CdbLength=6;s.SenseInfoLength=32;s.DataIn=1;s.DataTransferLength=255;s.TimeOutValue=5;s.SenseInfoOffset=c.sizeof(SPT);s.DataBufferOffset=c.sizeof(SPT)+32
        for i,v in enumerate((0x12,1,page,0,255,0)):s.Cdb[i]=v
        if not io(h,0x4d004,buf,512,buf,512,c.byref(returned),None):raise c.WinError(c.get_last_error())
        data=buf.raw[s.DataBufferOffset:s.DataBufferOffset+min(s.DataTransferLength,255)]
        result['pages'].append(dict(page=page,scsi_status=s.ScsiStatus,raw_hex=data.hex(),text=data.decode('ascii',errors='replace'),sense=buf.raw[s.SenseInfoOffset:s.SenseInfoOffset+32].hex()))
    result['status']='actual_firmware_VPD_read'
    if prior:
        rows={p['page']:bytes.fromhex(p['raw_hex']) for p in result['pages']}
        xml=b''.join(rows[x][4:4+int.from_bytes(rows[x][2:4],'big')] for x in announced)
        out.with_suffix('.plist').write_bytes(xml)
        try:result['actual_identity_plist']=plistlib.loads(xml)
        except BaseException as e:result['plist_parse_error']=repr(e)
except BaseException as e:result.update(status='not_read',error=repr(e))
finally:
    if h:close(h)
    out.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in result.items() if k not in ('pages','usb','disk')},ensure_ascii=True))
