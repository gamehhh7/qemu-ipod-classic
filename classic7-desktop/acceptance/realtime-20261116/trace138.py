"""Bounded read-only PCM/counter trace. No debugger, suspend or guest writes."""
import ctypes as c, re, pathlib, sys, struct, time, json, datetime, wave
d=pathlib.Path(__file__).parent
pid=int(sys.argv[1]); label=sys.argv[2]; duration=float(sys.argv[3])
addrfile=pathlib.Path(sys.argv[4]) if len(sys.argv)>4 else d/'addresses138.txt'
txt=addrfile.read_text(encoding='utf-8-sig')
if addrfile.suffix=='.json':
 m=json.loads(txt);assert m['pid']==pid;a=m['addresses']
else:
 assert int(re.search(r'process (\d+)\) detached',txt).group(1))==pid
 a=[int(x,16) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+)',txt)]
k=c.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[c.c_uint,c.c_int,c.c_uint];k.OpenProcess.restype=c.c_void_p
k.ReadProcessMemory.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
k.GetProcessTimes.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
k.CloseHandle.argtypes=[c.c_void_p]
h=k.OpenProcess(0x1010,False,pid)
if not h:raise c.WinError(c.get_last_error())
def raw(addr,n):
 b=c.create_string_buffer(n);got=c.c_size_t()
 if not k.ReadProcessMemory(h,addr,b,n,c.byref(got)) or got.value!=n:raise c.WinError(c.get_last_error())
 return b.raw
def val(i,fmt='<Q'):return struct.unpack(fmt,raw(a[i],struct.calcsize(fmt)))[0]
def cpusec():
 vals=[c.c_ulonglong() for _ in range(4)]
 if not k.GetProcessTimes(h,*[c.byref(v) for v in vals]):raise c.WinError(c.get_last_error())
 return (vals[2].value+vals[3].value)/1e7
def virtual():
 for _ in range(1000):
  seq=val(3,'<I')
  if seq&1:continue
  sh=val(6,'<h');v=val(4,'<q')+(val(5,'<q')<<sh)
  if seq==val(3,'<I'):return v,sh
 raise RuntimeError('seqlock busy')
rows=[];pcm=bytearray();start=time.perf_counter();utc=datetime.datetime.now(datetime.timezone.utc).isoformat()
wrprev=val(2,'<I');bufaddr=a[1]-(1<<20);mask=(1<<20)-1
try:
 while time.perf_counter()-start<duration:
  t=time.perf_counter();v,sh=virtual();rd=val(1,'<I');wr=val(2,'<I')
  size=(wr-wrprev)&0xffffffff
  if size>1<<20:raise RuntimeError('trace reader lost a whole ring')
  pos=wrprev&mask;n=min(size,(1<<20)-pos)
  if n:pcm.extend(raw(bufaddr+pos,n))
  if size>n:pcm.extend(raw(bufaddr,size-n))
  wrprev=wr
  rows.append([t-start,v,rd,wr,sh,val(8),val(12),val(13),val(14),val(18),cpusec()])
  time.sleep(.005)
finally:k.CloseHandle(h)
with wave.open(str(d/(label+'-producer.wav')),'wb') as f:
 f.setnchannels(2);f.setsampwidth(2);f.setframerate(44117);f.writeframes(pcm)
obj=dict(pid=pid,start_utc=utc,end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),method='ReadProcessMemory no suspend/write; process CPU includes all emulator threads',columns=['wall_s','virtual_ns','rd','wr','shift','decoded_frames','stale_bytes','stale_events','hosttimer_calls','empty_callbacks','process_cpu_s'],rows=rows,pcm_bytes=len(pcm))
(d/(label+'.json')).write_text(json.dumps(obj),encoding='utf8')
print(json.dumps({k:v for k,v in obj.items() if k!='rows'}),flush=True)
