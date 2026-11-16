"""Read-only, non-suspending sample of this process's verified native fields."""
import ctypes as c, re, pathlib, sys, struct, time, json
p = pathlib.Path(__file__).parent
pid = int(sys.argv[1]); label = sys.argv[2]
address_file = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else p.joinpath('auto-addresses.txt')
address_text = address_file.read_text(encoding='utf-8-sig')
if address_file.suffix=='.json':
    metadata=json.loads(address_text)
    if metadata['pid']!=pid: raise RuntimeError('Mapping belongs to another PID')
    addresses=metadata['addresses']
else:
    captured_pid = re.search(r'process (\d+)\) detached', address_text)
    if not captured_pid or int(captured_pid.group(1)) != pid:
        raise RuntimeError('Address capture belongs to another PID; regenerate addresses.gdb output first')
    addresses = [int(x,16) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+)', address_text)]
k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes=[c.c_uint,c.c_int,c.c_uint]; k.OpenProcess.restype=c.c_void_p
k.ReadProcessMemory.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes=[c.c_void_p]
h=k.OpenProcess(0x1010,False,pid)
if not h: raise c.WinError(c.get_last_error())
def read(addr,fmt):
    b=c.create_string_buffer(struct.calcsize(fmt)); n=c.c_size_t()
    if not k.ReadProcessMemory(h,addr,b,len(b),c.byref(n)) or n.value!=len(b):
        raise c.WinError(c.get_last_error())
    return struct.unpack(fmt,b.raw)[0]
def sample():
    rate,rd,wr,seq,bias,count,shift=addresses[:7]
    for _ in range(1000):
        a=read(seq,'<I')
        if a&1: continue
        v=read(bias,'<q') + (read(count,'<q') << read(shift,'<h'))
        if a==read(seq,'<I'): break
    else: raise RuntimeError('clock seqlock busy')
    r,w=read(rd,'<I'),read(wr,'<I')
    return dict(wall=time.perf_counter(),virtual_ns=v,rate=read(rate,'<I'),rd=r,wr=w,ring=(w-r)&0xffffffff,clock='adaptive icount',shift=read(shift,'<h'),decode_bytes=read(addresses[7],'<Q'),decode_frames=read(addresses[8],'<Q'),decode_completions=read(addresses[9],'<Q'),input_bytes=read(addresses[10],'<Q'),overflow_bytes=read(addresses[11],'<Q'),stale_bytes=read(addresses[12],'<Q'),stale_events=read(addresses[13],'<Q'),timer_calls=read(addresses[14],'<Q'),timer_gap_max_ns=read(addresses[15],'<Q'),timer_gap_over90=read(addresses[16],'<Q'),prime_events=read(addresses[17],'<Q'),empty_callbacks=read(addresses[18],'<Q'),stream_stops=read(addresses[19],'<Q'),stop_flush_bytes=read(addresses[20],'<Q'),decoded_hz=read(addresses[21],'<I'))
try:
    if len(sys.argv) > 4 and sys.argv[4] == 'wait-playing':
        ready_deadline = time.perf_counter() + 90
        while sample()['decode_frames'] < 100:
            if time.perf_counter() > ready_deadline:
                raise RuntimeError('No decoded playback within90s; no acceptance result')
            time.sleep(0.25)
        print('Decoded playback active; starting bounded120s sample',flush=True)
    rows=[]
    for n in range(25):
        rows.append(sample())
        print(json.dumps(rows[-1]),flush=True)
        if n<24: time.sleep(5)
    a,b=rows[0],rows[-1]; wall=b['wall']-a['wall']; virt=(b['virtual_ns']-a['virtual_ns'])/1e9
    result=dict(pid=pid,label=label,method='ReadProcessMemory only; no suspend or writes',wall_s=wall,virtual_s=virt,virtual_wall_ratio=virt/wall,pcm_Bps=((b['wr']-a['wr'])&0xffffffff)/wall,drain_Bps=(((b['rd']-a['rd'])&0xffffffff)-(b['stale_bytes']-a['stale_bytes'])-(b['stop_flush_bytes']-a['stop_flush_bytes']))/wall,nominal=b['rate']*4,decode_Bps=(b['decode_bytes']-a['decode_bytes'])/wall,decode_fps=(b['decode_frames']-a['decode_frames'])/wall,input_Bps=(b['input_bytes']-a['input_bytes'])/wall,overflow_bytes=b['overflow_bytes']-a['overflow_bytes'],stale_bytes=b['stale_bytes']-a['stale_bytes'],stale_events=b['stale_events']-a['stale_events'],timer_calls=b['timer_calls']-a['timer_calls'],timer_gap_over90=b['timer_gap_over90']-a['timer_gap_over90'],timer_max_gap_ms=b['timer_gap_max_ns']/1e6,prime_events=b['prime_events']-a['prime_events'],empty_callbacks=b['empty_callbacks']-a['empty_callbacks'],stream_stops=b['stream_stops']-a['stream_stops'],stop_flush_bytes=b['stop_flush_bytes']-a['stop_flush_bytes'],decoded_hz=b['decoded_hz'],ring_min=min(x['ring'] for x in rows),ring_max=max(x['ring'] for x in rows),samples=rows)
    p.joinpath(label+'.json').write_text(json.dumps(result,indent=2),encoding='utf8')
    print(json.dumps({k:v for k,v in result.items() if k!='samples'}),flush=True)
finally: k.CloseHandle(h)
