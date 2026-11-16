from pathlib import Path
import ctypes as c,sys,json,struct,datetime
p=Path(sys.argv[1]);m=json.loads(p.read_text(encoding='utf8'));k=c.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[c.c_uint,c.c_int,c.c_uint];k.OpenProcess.restype=c.c_void_p
k.ReadProcessMemory.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes=[c.c_void_p]
k.GetProcessTimes.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
h=k.OpenProcess(0x1010,False,m['pid'])
if not h:raise c.WinError(c.get_last_error())
def read(addr):
 buf=c.create_string_buffer(8);n=c.c_size_t()
 if not k.ReadProcessMemory(h,addr,buf,8,c.byref(n)) or n.value!=8:raise c.WinError(c.get_last_error())
 return struct.unpack('<Q',buf.raw)[0]
try:
 times=[c.c_ulonglong() for _ in range(4)];assert k.GetProcessTimes(h,*[c.byref(x) for x in times])
 assert times[0].value==m['creation_filetime'],'PID reused'
 vals={name:read(addr) for name,addr in zip(m['extra_fields'],m['addresses'][22:])}
 vals['decoded_frames']=read(m['addresses'][8])
 vals['utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
 if vals['c7_dma_compare_bytes[0]'] and vals['c7_dma_compare_bytes[1]']:
  slow=vals['c7_dma_compare_ns[0]']/vals['c7_dma_compare_bytes[0]'];fast=vals['c7_dma_compare_ns[1]']/vals['c7_dma_compare_bytes[1]']
  vals.update(legacy_ns_per_byte=slow,cached_ns_per_byte=fast,transport_speedup=slow/fast,transport_time_reduction=1-fast/slow)
 out=p.parent/('dma-perf-'+datetime.datetime.now().strftime('%H%M%S')+'.json');out.write_text(json.dumps(vals,indent=2),encoding='utf8');print(json.dumps(vals,indent=2))
finally:k.CloseHandle(h)
