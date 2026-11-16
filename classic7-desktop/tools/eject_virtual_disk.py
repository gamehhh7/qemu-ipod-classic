"""Ask Windows to safely eject only the current proved virtual USB instance."""
import ctypes as c,json,sys,datetime
from pathlib import Path
from ctypes import wintypes as w
from virtual_pnp_inventory import witness
proof=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8-sig'))
targets=[x for x in proof['virtual_usb'] if x['status']=='OK' and x.get('ude_parent_proven') and 'ROOT\\USB\\0000' in x['parent_chain']]
if len(targets)!=1:raise SystemExit('Need one exact virtual USB owner')
current=witness(targets[0]['instance'])
if not current['ude_parent_proven'] or current['status']!='OK':raise SystemExit('Virtual owner no longer live')
cm=c.WinDLL('cfgmgr32');node=w.ULONG()
locate=cm.CM_Locate_DevNodeW;locate.argtypes=[c.POINTER(w.ULONG),w.LPWSTR,w.ULONG];locate.restype=w.ULONG
rc=locate(c.byref(node),c.create_unicode_buffer(current['instance']),0)
if rc:raise SystemExit('Current virtual node disappeared')
veto=w.ULONG();name=c.create_unicode_buffer(1024)
eject=cm.CM_Request_Device_EjectW;eject.argtypes=[w.ULONG,c.POINTER(w.ULONG),w.LPWSTR,w.ULONG,w.ULONG];eject.restype=w.ULONG
rc=eject(node.value,c.byref(veto),name,len(name),0)
result=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),instance=current['instance'],fresh_parent_chain=current['parent_chain'],CONFIGRET=rc,veto_type=veto.value,veto_name=name.value,physical_device=False,reboot=False,acceptance='Windows eject result only; persisted file must be read after a new backend')
Path(sys.argv[2]).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(result,ensure_ascii=False))
