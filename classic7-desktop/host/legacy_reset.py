"""One downstream-host reset after real guest soft-connect, then native control tokens.

No claim of physical attach/HCD. Trigger observations and actual virtual timing
are audited independently; wall15ms is only a single scheduling attempt.
"""
from control_walk import ControlWalk
import json,os
from pathlib import Path
import re
import socket
import struct
import threading
import time

MAGIC=0x58445049
VERSION=2
PORT=45117
IDENTITY=0x1C3300C0DE
HEADER=struct.Struct('<IHHHHI')
HELLO,OPEN,CLOSE,NATIVE_INFO,ASSERT,DEASSERT=1,2,14,16,20,21
ALLOWED={HELLO,OPEN,CLOSE,NATIVE_INFO,ASSERT,DEASSERT,0x12,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x38,0x3a}
FRESH_NS=20_000_000
MAX_LOG_BYTES=16*1024*1024
RECORD_LIMIT=10000

def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)',line))

class Trace:
    """Strict timestamped MMIO/write/control triple; never use request bits as state."""
    def __init__(self,path):
        self.path=Path(path);self.offset=0;self.tail='';self.lines=[]
        self.cable=None;self.control=None;self.mmio=None;self.write=None
        self.limited=set();self.hostbus=[];self.last_observed_wall=0
        self.latest_vns=None
    def refresh(self):
        if not self.path.exists():return
        size=self.path.stat().st_size
        if size>MAX_LOG_BYTES:raise RuntimeError('log cap; no absence inference')
        if size<self.offset:raise RuntimeError('log truncated/replaced')
        with self.path.open('rb') as f:
            f.seek(self.offset);chunk=f.read();self.offset=f.tell()
        text=self.tail+chunk.decode('utf8',errors='replace')
        lines=text.split('\n');self.tail=lines.pop()
        for line in lines:self.consume(line)
    def invalidate(self):self.control=None
    def consume(self,line):
        self.lines.append(line);index=len(self.lines)-1;f=fields(line)
        if 'vns' in f:
            value=int(f['vns'])
            self.latest_vns=value if self.latest_vns is None else max(value,self.latest_vns)
        if line.startswith('USBMMIO_INPUT ') and f.get('kind')=='vbus':
            if f.get('level')=='1' and f.get('accepted')=='1':
                self.cable={'vns':int(f['vns']),'line':index}
            else:self.cable=None
            self.invalidate()
        if line.startswith('USBMMIO ') and f.get('region')=='core':
            off=int(f['off'],16)
            if f.get('kind')=='bucket_limit' and f.get('op')=='W' and off in (0x10,0x804,0xe00):
                self.limited.add(off);self.invalidate()
            if f.get('op')=='W':
                self.mmio={'off':off,'value':int(f['value'],16),'supported':f.get('supported')=='1',
                           'vns':int(f['vns']),'line':index,'cpu':f.get('cpu_context')=='1',
                           'pc':int(f['pc_approx'],16),'kind':f.get('kind')}
                if off==0x10 and (int(f['value'],16)&1 or f.get('supported')!='1'):self.invalidate()
                if off in (0x804,0xe00):self.invalidate()
        if line.startswith('USBDEV write '):
            off=int(f['offset'],16);value=int(f['value'],16)
            if off==0x10 and (value&1 or f.get('supported')!='1'):self.invalidate()
            if off in (0x804,0xe00):
                self.invalidate();m=self.mmio
                self.write=None
                if (m and m['line']==index-1 and m['off']==off and m['value']==value and
                    m['supported'] and f.get('supported')=='1' and m['cpu'] and m['pc'] and
                    m['kind']=='sample' and not self.limited):self.write=m
        if line.startswith('USBDEV control '):
            m=self.write
            if m and m['line']==index-2 and self.cable and m['vns']>=self.cable['vns']:
                self.control={'dctl':int(f['dctl'],16),'pcgcctl':int(f['pcgcctl'],16),
                              'vns':m['vns'],'mmio_line':m['line'],'control_line':index,
                              'pc_approx':m['pc'],'source':'supported guest-context MMIO + adjacent actual post-write control state'}
                self.last_observed_wall=time.monotonic()
            self.write=None
        if line.startswith('HOSTBUS '):
            self.hostbus.append({**f,'line':index})
    def snapshot(self,now_vns=None):
        c=self.control
        if not self.cable:return None,'no observed PMU-derived VBUS input'
        if self.limited:return None,'control/reset write bucket limited; later state unknown'
        if not c:return None,'no adjacent supported timestamped post-cable guest control state'
        if c['dctl']&2:return None,'guest SDIS asserted'
        if c['pcgcctl']&3:return None,'guest clock gated'
        if now_vns is not None and not 0<=now_vns-c['vns']<=FRESH_NS:return None,'control state stale in virtual time'
        if now_vns is None and self.latest_vns is not None and not 0<=self.latest_vns-c['vns']<=FRESH_NS:return None,'known control state stale against latest observed virtual time'
        if now_vns is None and time.monotonic()-self.last_observed_wall>0.1:return None,'control observation stale in wall receipt time'
        return {**c,'cable':dict(self.cable)},None
    def audit_at_assert(self,token):
        a=next((e for e in self.hostbus if e.get('token')==str(token) and e.get('seq')=='1' and e.get('asserted')=='1'),None)
        if not a:return None,'no actual ASSERT virtual log evidence'
        prefix=Trace(self.path)
        for line in self.lines[:a['line']]:prefix.consume(line)
        return prefix.snapshot(int(a['vns']))

class Wire:
    def __init__(self,sock,record,deadline):self.sock=sock;self.record=record;self.deadline=deadline
    def exact(self,n):
        b=b''
        while len(b)<n:
            remaining=self.deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('producer deadline')
            self.sock.settimeout(min(0.5,remaining))
            c=self.sock.recv(n-len(b))
            if not c:raise EOFError('peer disconnected')
            b+=c
        return b
    def call(self,op,payload=b''):
        if op not in ALLOWED:raise AssertionError('USB token/data opcode forbidden')
        self.record('request',op=op,bytes=len(payload))
        self.sock.sendall(HEADER.pack(MAGIC,VERSION,op,0,0,len(payload))+payload)
        magic,version,rop,status,flags,length=HEADER.unpack(self.exact(16))
        if (magic,version,rop,flags)!=(MAGIC,VERSION,op,0) or length>1024:raise RuntimeError('bad v2 response envelope')
        data=self.exact(length)
        self.record('response',op=op,status=status,bytes=length)
        return status,data

def native_info_eligibility(data):
    if len(data) != 48:raise ValueError('NATIVE_INFO length')
    raw=list(data[16:20])
    info=dict(zip(('vbus','reset_line','host_mode','dma_enabled'),raw))
    reasons=[]
    for name,expected in (('vbus',1),('reset_line',0),('host_mode',0)):
        if info[name] != expected:reasons.append(name+' expected'+str(expected)+' actual'+str(info[name]))
    # DMA belongs to packet ingress, not external bus-reset input qualification.
    return {**info,'raw4':raw,'raw_payload_hex':data.hex()},reasons


class Trace(Trace):
    """Separate the modeled reset receiver from the unchanged packet clock gate.
    An initial zero DCTL after PCGCCTL write is not a guest attach act.
    Require actual post-cable SDIS assertion then release and causal link state.
    """
    def __init__(self,path):
        super().__init__(path)
        self.sdis_seen=False;self.soft_connected=False;self.soft_connect_vns=None
        self.receiver=None
    def consume(self,line):
        super().consume(line)
        f=fields(line)
        if line.startswith('USBMMIO_INPUT ') and f.get('kind')=='vbus':
            self.sdis_seen=False;self.soft_connected=False;self.soft_connect_vns=None;self.receiver=None
        if line.startswith('USBMMIO ') and f.get('region')=='core' and f.get('op')=='W':
            if int(f['off'],16)==0x10 and (int(f['value'],16)&1 or f.get('supported')!='1'):
                self.sdis_seen=False;self.soft_connected=False;self.soft_connect_vns=None
        if line.startswith('USBDEV control ') and self.control and self.control['control_line']==len(self.lines)-1:
            if self.mmio and self.mmio['off']==0x804:
                if self.control['dctl']&2:
                    self.sdis_seen=True;self.soft_connected=False;self.soft_connect_vns=None
                elif self.sdis_seen:
                    self.soft_connected=True;self.soft_connect_vns=self.control['vns']
        if line.startswith('USBLINK hypothesis=v0 ') and 'available' in f:
            self.receiver=dict(f)
    def snapshot(self,now_vns=None):
        c=self.control;link=self.receiver
        if not self.cable:return None,'no observed PMU-derived VBUS input'
        if self.limited:return None,'control/reset write bucket limited; later state unknown'
        if not c:return None,'no adjacent supported timestamped post-cable guest control state'
        if not self.sdis_seen or not self.soft_connected:return None,'no actual post-cable guest SDIS assertion/release'
        if c['dctl']&2:return None,'guest SDIS asserted'
        if not link or (link.get('available'),link.get('hprt'),link.get('vbus'),link.get('soc_clocks'))!=('1','c00','1','1'):
            return None,'causal modeled PHY reset receiver unavailable'
        at=now_vns if now_vns is not None else self.latest_vns
        if at is not None and not 0<=at-c['vns']<=FRESH_NS:return None,'known control state stale in virtual time'
        if now_vns is None and time.monotonic()-self.last_observed_wall>0.1:return None,'control observation stale in wall receipt time'
        return {**c,'cable':dict(self.cable),'actual_soft_connect_vns':self.soft_connect_vns,
                'receiver':dict(link),'pcgcctl_scope':'packet gate unchanged; reset input uses causal receiver availability'},None

from bounded_records import BoundedRecords

class Producer:
    def __init__(self,run_dir,port=PORT,trigger_timeout=60.0):
        self.run_dir=Path(run_dir);self.trace=Trace(self.run_dir/'qemu.log')
        self.port=port;self.trigger_timeout=trigger_timeout;self.stop_event=threading.Event()
        self.active_socket=None
        self.journal=BoundedRecords((self.run_dir/'HOST-TRACE.jsonl').open('w',encoding='utf-8',buffering=65536),raw_stream_limit=None if os.environ.get('CLASSIC7_DIAGNOSTICS')=='1' else 512)
        self.cable_event=threading.Event();self.thread=None;self.cable_fact=None
        self.result={'status':'not_started','physical_attach_claimed':False,'attempts':0,'edge_requests':0,
                     'usb_token_transactions':0,'events':[],'timing_semantics':'actual virtual log edges decide; wall15ms is scheduling attempt only'}
    def record(self,kind,**kw):
        self.journal.add(kind,wall_monotonic=time.monotonic(),**kw)
    def start(self):
        self.thread=threading.Thread(target=self.run,name='host-reset-producer',daemon=True);self.thread.start()
    def cable_connected(self,fact):
        if self.cable_fact is not None:raise RuntimeError('duplicate cable notification')
        if fact.get('readback') is not True:raise RuntimeError('cable readback missing')
        self.cable_fact={**fact,'notified_monotonic':time.monotonic()};self.cable_event.set()
    def stop(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=2.0)
        if self.thread and self.thread.is_alive():self.result['stop_incomplete']=True
    def abort_connection(self):
        sock=self.active_socket
        if sock:
            try:sock.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            try:sock.close()
            except OSError:pass
    def persist(self):
        self.result.update(finished_wall_monotonic=time.monotonic(),last_trace_virtual_ns=self.trace.latest_vns,trigger_wait_bound_s=self.trigger_timeout)
        out=dict(self.result);out['events'],out['journal_summary']=self.journal.snapshot()
        (self.run_dir/'HOST-RESET-RESULT.json').write_text(json.dumps(out,indent=2)+'\n',encoding='utf8')
    def run(self):
        sock=None;token=None;assert_reply=None;deassert_reply=None
        try:
            self.result['status']='waiting_model_trigger'
            while not self.stop_event.is_set() and not self.cable_event.wait(0.01):self.trace.refresh()
            if self.stop_event.is_set():self.result['status']='unsupported_no_cable';return
            self.result['cable_fact']=self.cable_fact
            until=time.monotonic()+self.trigger_timeout
            while not self.stop_event.is_set() and time.monotonic()<until:
                self.trace.refresh();snap,why=self.trace.snapshot()
                ready=any(line.startswith('native-host: listening on 127.0.0.1:') and 'mode=native' in line for line in self.trace.lines)
                if snap and ready:break
                self.result['trigger_last_missing']=why or 'native listener readiness not observed'
                self.stop_event.wait(0.005)
            else:self.result['status']='unsupported_model_trigger';return
            self.result['trigger_before_socket']=snap
            # One connection attempt; listener readiness never means physical attach.
            sock=socket.create_connection(('127.0.0.1',self.port),timeout=0.5)
            self.active_socket=sock
            self.result['socket_connected']=True
            sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            wire=Wire(sock,self.record,time.monotonic()+4.0)
            st,data=wire.call(HELLO)
            if st or len(data)<24:raise RuntimeError('HELLO failed')
            version,caps,identity=struct.unpack_from('<IIQ',data)
            if version!=2 or identity!=IDENTITY or not caps&128 or not caps&256 or caps&(1|2|32):raise RuntimeError('v2 reset-only capability/identity mismatch')
            st,data=wire.call(NATIVE_INFO)
            if st or len(data)!=48:raise RuntimeError('NATIVE_INFO failed')
            info,reasons=native_info_eligibility(data)
            self.result['native_info_before_open']=info
            self.result['native_info_rejection_reasons']=reasons
            self.record('native_info_snapshot',**info,rejection_reasons=reasons)
            if reasons:raise RuntimeError('external reset snapshot not eligible: '+ '; '.join(reasons))
            st,data=wire.call(OPEN,struct.pack('<Q',IDENTITY))
            if st or len(data)!=32:raise RuntimeError('OPEN failed')
            serial,identity,token,min_ns=struct.unpack('<QQQQ',data)
            if not serial or not token or identity!=IDENTITY or min_ns!=10_000_000:raise RuntimeError('invalid live lease contract')
            self.result['lease']={'serial':serial,'token':token,'min_ns':min_ns}
            self.trace.refresh();snap,why=self.trace.snapshot()
            if not snap:self.result.update(status='unsupported_trigger_changed_before_assert',trigger_last_missing=why);return
            self.result['trigger_immediately_before_assert']=snap
            if self.stop_event.is_set():self.result['status']='cancelled_before_assert';return
            self.result['attempts']=1
            anchor=time.monotonic();self.result['edge_requests']+=1
            st,data=wire.call(ASSERT,struct.pack('<QQQ',serial,token,1))
            if st or len(data)!=32:raise RuntimeError('ASSERT refused/uncertain; disconnect abort, no retry')
            code,level,seq,vns,recovery=struct.unpack('<IIQQQ',data)
            if (code,level,seq,recovery)!=(0,1,1,10_000_000):raise RuntimeError('ASSERT response contract mismatch')
            assert_reply=vns;self.result['assert_response_vns']=vns
            # Never extend/retry a late attempt; send the one deassert immediately
            # if its single target time has already elapsed or stop was requested.
            remaining=anchor+0.015-time.monotonic()
            if remaining>0:self.stop_event.wait(remaining)
            self.result['edge_requests']+=1
            st,data=wire.call(DEASSERT,struct.pack('<QQQ',serial,token,2))
            if len(data)!=32:raise RuntimeError('DEASSERT response shape')
            code,level,seq,vns,recovery=struct.unpack('<IIQQQ',data)
            deassert_reply=vns;self.result.update(deassert_response_vns=vns,deassert_status=st,deassert_code=code)
            if (level,seq,recovery)!=(0,2,10_000_000):raise RuntimeError('DEASSERT response mismatch')
            # Control tokens follow only after reset audit; the controller checks recovery. The
            # later QEMU CLOSE-abort timestamp supplies that measured evidence.
            self.stop_event.wait(0.015)

            # Never issue control tokens for a bad/short reset. The controller
            # independently checks recovery before every native control RPC.
            self.trace.refresh()
            evidence=[e for e in self.trace.hostbus if e.get('token')==str(token) and 'seq' in e]
            trigger,why=self.trace.audit_at_assert(token)
            if (st==0 and code==0 and len(evidence)==2 and
                [e.get('seq') for e in evidence]==['1','2'] and
                all(e.get('result')=='0' for e in evidence) and trigger and
                10_000_000<=int(evidence[1]['vns'])-int(evidence[0]['vns'])<=20_000_000):
                wire.deadline=time.monotonic()+20.0
                walk=ControlWalk(wire,serial,token,self.stop_event)
                self.result['native_control']=walk.run()
                self.result['native_control_rpc_requests']=sum(1 for e in self.result['events'] if e.get('kind')=='request' and e.get('op') in (0x30,0x31,0x32,0x33))
                self.result['usb_token_transactions']=sum(int(c.get('setup_accepted',False))+int(c.get('packet_observed',False))+int(c.get('status_out_committed',False)) for c in self.result['native_control']['controls'])
            else:
                self.result['native_control']={'status':'not_run_reset_input_unqualified','trigger_reason':why}

            if self.result.get('native_control',{}).get('status')=='controller_configuration_verified':
                self.result['live_host_hold']={'started_monotonic':time.monotonic(),'planned_seconds':'until cable disconnect','queries':[]}
                self.result['status']='controller_configuration_verified_live_host'
                self.persist()
                while not self.stop_event.wait(0.1):pass
                self.result['live_host_hold']['stopped_monotonic']=time.monotonic()
                wire.deadline=time.monotonic()+2
            close_status,close_data=wire.call(CLOSE)
            if close_status or len(close_data)!=8 or struct.unpack('<Q',close_data)[0]!=serial:
                raise RuntimeError('CLOSE response invalid; recovery not qualified')
            sock.close();sock=None
            until=time.monotonic()+0.5
            while time.monotonic()<until:
                self.trace.refresh()
                edges=[e for e in self.trace.hostbus if e.get('token')==str(token) and 'seq' in e]
                close=next((e for e in self.trace.hostbus if e.get('token')==str(token) and e.get('reason')=='protocol-close-abort'),None)
                if len(edges)>=2 and close:break
                self.stop_event.wait(0.005)
            self.result['actual_edges']=edges;self.result['close_abort_log']=close
            actual={e.get('seq'):e for e in edges}
            if len(edges)!=2 or [e.get('seq') for e in edges]!=['1','2']:
                raise RuntimeError('missing/extra/reordered actual edge records')
            if actual['1'].get('asserted')!='1' or actual['2'].get('asserted')!='0':
                raise RuntimeError('actual edge levels do not match requested pulse')
            a,d=int(actual['1']['vns']),int(actual['2']['vns'])
            self.result['actual_pulse_ns']=d-a
            self.result['actual_recovery_before_close_ns']=int(close['vns'])-d if close else None
            self.result['response_virtual_delta_ns']=deassert_reply-assert_reply
            if assert_reply<a or deassert_reply<d or deassert_reply<assert_reply:
                self.result['status']='invalid_response_virtual_clock';return
            trigger,why=self.trace.audit_at_assert(token)
            self.result['trigger_at_actual_assert']=trigger
            if not trigger:self.result.update(status='invalid_trigger_at_assert',trigger_last_missing=why);return
            if not (10_000_000<=d-a<=20_000_000):self.result['status']='invalid_virtual_pulse_duration';return
            if st or code or actual['1'].get('result')!='0' or actual['2'].get('result')!='0':self.result['status']='invalid_edge_response';return
            if not close or int(close['vns'])-d<10_000_000:self.result['status']='invalid_recovery_not_observed';return
            self.result['status']='qualified_modeled_reset_input_observed'
            self.result['acceptance_limit']='only input/virtual timing; no proof of IRQ handling, task wakeup, UI, PHY, HCD or enumeration'
        except BaseException as exc:
            self.result.update(status='failed_no_retry',error=f'{type(exc).__name__}: {exc}')
        finally:
            if self.result.get('stop_incomplete'):self.result['status']='failed_thread_not_stopped'
            if sock:
                try:sock.close() # owner cleanup is abort, not a valid completed pulse
                except OSError:pass
            self.active_socket=None
            try:self.trace.refresh()
            except BaseException as exc:self.result['final_trace_error']=str(exc)
            self.persist()
