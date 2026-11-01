"""Fast CfgMgr32 read-only present-node witness. No disk/device handles.

Only USB nodes under the installed virtual UDE parent are returned. USBSTOR
disk identity additionally requires that same USB instance in its parent chain.
Geometry/partition/file access is intentionally absent.
"""
import ctypes as c,json,sys,datetime
from ctypes import wintypes as w
from pathlib import Path
cm=c.WinDLL('cfgmgr32');U=w.ULONG;PU=c.POINTER(U);W=w.LPWSTR
def fn(name,args):
    f=getattr(cm,name);f.argtypes=args;f.restype=U;return f
ids_size=fn('CM_Get_Device_ID_List_SizeW',[PU,w.LPCWSTR,U]);ids=fn('CM_Get_Device_ID_ListW',[w.LPCWSTR,W,U,U])
locate=fn('CM_Locate_DevNodeW',[PU,W,U]);id_size=fn('CM_Get_Device_ID_Size',[PU,U,U]);get_id=fn('CM_Get_Device_IDW',[U,W,U,U]);parent=fn('CM_Get_Parent',[PU,U,U]);status=fn('CM_Get_DevNode_Status',[PU,PU,U,U])
prop=fn('CM_Get_DevNode_Registry_PropertyW',[U,U,PU,c.c_void_p,PU,U])
def checked(rc,label):
    if rc:raise RuntimeError(label+' CONFIGRET='+hex(rc))
def dev_id(node):
    n=U();checked(id_size(c.byref(n),node,0),'id_size');assert n.value<4096
    buf=c.create_unicode_buffer(n.value+1);checked(get_id(node,buf,len(buf),0),'get_id');return buf.value.upper()
def property_text(node,key):
    buf=c.create_unicode_buffer(2048);n=U(c.sizeof(buf));dtype=U()
    return buf.value if prop(node,key,c.byref(dtype),buf,c.byref(n),0)==0 and dtype.value in (1,2) else None
def witness(instance):
    node=U();checked(locate(c.byref(node),c.create_unicode_buffer(instance),0),'locate')
    flags=U();problem=U();checked(status(c.byref(flags),c.byref(problem),node,0),'status')
    original=node.value;chain=[]
    for _ in range(24):
        name=dev_id(node.value)
        if name in chain:break
        chain.append(name);p=U()
        if parent(c.byref(p),node.value,0):break
        node=p
    return {'instance':instance,'status':'OK' if flags.value&8 and not flags.value&0x400 and problem.value==0 else 'not_started_or_problem',
        'devnode_flags':flags.value,'problem_code':problem.value,'name':property_text(original,0x0d) or property_text(original,1),'service':property_text(original,5),'class':property_text(original,8),'parent_chain':chain,'ude_parent_proven':'ROOT\\USB\\0000' in chain}
def main(output):
    n=U();checked(ids_size(c.byref(n),None,0x100),'list_size');assert 0<n.value<=1024*1024
    buf=c.create_unicode_buffer(n.value);checked(ids(None,buf,n.value,0x100),'list_present')
    names=[x.upper() for x in ''.join(buf).split('\0') if x];assert len(names)<=10000
    virtual=[];disks=[];errors=[];root=None
    for name in names:
        if name=='ROOT\\USB\\0000' or name.startswith('USB\\VID_05AC&PID_1261\\'):
            try:
                row=witness(name)
                if name=='ROOT\\USB\\0000':root=row
                elif row['ude_parent_proven']:virtual.append(row)
            except RuntimeError as e:errors.append(str(e))
    for name in names:
        if not name.startswith(('USBSTOR\\DISK','SCSI\\DISK')):continue
        try:
            row=witness(name)
            if row['ude_parent_proven'] and any(x['instance'] in row['parent_chain'] for x in virtual) and (row.get('class') or '').casefold()=='diskdrive':
                row.update(pnp=name,partitions=[],geometry_not_queried=True);disks.append(row)
        except RuntimeError as e:errors.append(str(e))
    result={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'api':'readonly CfgMgr32 present IDs/status/parent chain','stage':'fast_identity_only','root_ude':root,'virtual_usb':virtual,'virtual_disks':disks,'errors':errors,'physical_devices_touched':False,'file_writes':False,'rawdisk_handles':False}
    Path(output).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps({'USB':len(virtual),'disks':len(disks),'root_status':root.get('status') if root else None,'errors':errors},ensure_ascii=False))
if __name__=='__main__':main(sys.argv[1])
