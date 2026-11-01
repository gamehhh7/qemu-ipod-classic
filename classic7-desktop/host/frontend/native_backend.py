"""Native controller-backed URBs. No SCSI emulator or descriptor database.

The caller supplies the already qualified/reset bus lease and current actual
guest device/configuration bytes. All control/bulk payloads cross native DMA.
"""
from dataclasses import dataclass
from contextlib import contextmanager
import threading,time,struct,hashlib,os,json
from control_walk import ControlWalk,ActualControlStall
from bulk_walk import BulkWalk
from usbip_codec import GuestExport
from windows_timing import precise_wait

@dataclass(frozen=True)
class Completion:
    status:int
    actual:int
    data:bytes=b''

class Cancelled(Exception):pass
class Stalled(Exception):pass
class PacketOverflow(Exception):pass
class NativeRequestTimeout(TimeoutError):pass

class StopPair:
    def __init__(self,global_stop,local):self.global_stop=global_stop;self.local=local
    def is_set(self):return self.global_stop.is_set() or self.local.is_set()
    def wait(self,duration):
        if duration<=0:return self.is_set()
        if duration<=.01:
            precise_wait(self.local,duration)
            return self.is_set()
        until=time.monotonic()+duration
        while not self.is_set() and time.monotonic()<until:self.local.wait(min(.01,max(0,until-time.monotonic())))
        return self.is_set()

class LockedWire:
    def __init__(self,wire):
        self.wire=wire;self.lock=threading.Lock()
        self.timing=bool(os.environ.get('CLASSIC7_USB_TIMING_FILE'));self.stats={}
    def call(self,op,payload=b''):
        started=time.perf_counter() if self.timing else 0
        with self.lock:
            self.wire.deadline=time.monotonic()+2
            if not self.timing:return self.wire.call(op,payload)
            acquired=time.perf_counter()
            try:return self.wire.call(op,payload)
            finally:
                elapsed=time.perf_counter()-acquired
                row=self.stats.setdefault(str(op),dict(calls=0,rpc_s=0,lock_wait_s=0,max_rpc_s=0))
                row['calls']+=1;row['rpc_s']+=elapsed;row['lock_wait_s']+=acquired-started
                row['max_rpc_s']=max(row['max_rpc_s'],elapsed)
    def timing_snapshot(self):
        with self.lock:return {k:dict(v) for k,v in self.stats.items()}

class NativeBackend:
    def __init__(self,wire,serial,token,stop,device,configuration,record,initial_out_sequence=0,configuration_value=None,control_cadence=None,batch_out=False):
        self.wire=LockedWire(wire);self.prefix=struct.pack('<QQ',serial,token)
        self.serial=serial;self.token=token;self.stop=stop;self.record=record
        self.batch_out=bool(batch_out) and os.environ.get('CLASSIC7_USB_BATCH_OUT')!='0'
        self.timing_path=os.environ.get('CLASSIC7_USB_TIMING_FILE')
        self.timing_lock=threading.Lock();self.timing_rows=0;self.nak_retries=0;self.nak_wait_s=0
        self.bus_gate=threading.Lock()
        self.control_cadence=control_cadence
        self.device=device;self.configuration=configuration
        self.ep_in,self.ep_out,self.mps=BulkWalk.endpoints(configuration)
        self.locks={0:threading.Lock(),self.ep_in|128:threading.Lock(),self.ep_out:threading.Lock()}
        if type(initial_out_sequence) is not int or not 0<=initial_out_sequence<0xffffffffffffffff:
            raise ValueError('actual accepted OUT sequence handoff')
        self.out_seq=initial_out_sequence;self.fault=False
        # This export is an explicitly emulated high-speed bus. Confirm actual
        # guest DCFG requests HS and actual selected descriptor uses512B bulk.
        st,raw=self.wire.call(0x12)
        if st or len(raw)!=64 or struct.unpack_from('<I',raw,36)[0]&3 or self.mps!=512:
            raise RuntimeError('explicit modeled high-speed export unsupported by current guest registers/descriptors')
        self.export=GuestExport('/classic7/native/session-'+str(serial),'1-1',1,1,3,device,configuration,configuration_value)
        self.export.encode(True)
    def live(self):return not self.fault and not self.stop.is_set()
    def check(self,cancel):
        if not self.live():raise EOFError('native lease disconnected')
        if cancel.is_set():raise Cancelled()
    def snapshot(self):
        if not self.live():return None
        st,raw=self.wire.call(0x37)
        if st or len(raw)!=64:raise RuntimeError('live native controller snapshot')
        version,vbus,reset,host_mode,dctl,clock,receiver,qualified,*_=struct.unpack('<8I4Q',raw)
        if version!=2 or vbus!=1 or reset or host_mode or dctl&2 or clock&3 or not receiver or not qualified:return None
        return self.export
    def rpc(self,op,body,cancel):
        self.check(cancel);st,raw=self.wire.call(op,self.prefix+body)
        if st or len(raw)<16:raise RuntimeError('actual native bulk refusal '+repr((op,st,raw.hex())))
        rc,n,nonce=struct.unpack_from('<iIQ',raw)
        if n>512 or len(raw)!=(16+n if op==0x35 else 16) or rc not in (0,1,2):raise RuntimeError('native bulk envelope/state')
        return rc,n,nonce,raw[16:]
    def acquire_bus(self,cancel,until):
        while time.monotonic()<until:
            self.check(cancel)
            if self.bus_gate.acquire(timeout=.01):return
        raise NativeRequestTimeout('native bus arbitration timeout, no replay')
    @contextmanager
    def packet(self,op,body,cancel,until):
        held=False
        try:
            while time.monotonic()<until:
                self.acquire_bus(cancel,until);held=True
                answer=self.rpc(op,body,cancel)
                if answer[0]!=1:
                    yield answer
                    return
                # A real NAK consumed no data/sequence. Let pending EP0 recovery
                # execute before polling again, rather than monopolizing bus.
                self.bus_gate.release();held=False
                if self.timing_path:self.nak_retries+=1
                wait_started=time.perf_counter() if self.timing_path else 0
                # Poll the real HS endpoint again on the next microframe.
                # A NAK consumes no data or sequence; ACK ownership is unchanged.
                cancelled=cancel.wait(.000125)
                if self.timing_path:self.nak_wait_s+=time.perf_counter()-wait_started
                if cancelled:raise Cancelled()
            raise NativeRequestTimeout('actual endpoint did not arm before bounded native URB timeout')
        finally:
            if held:self.bus_gate.release()
    def drain(self,ep,direction,cancel):
        for _ in range(32):
            self.check(cancel)
            st,raw=self.wire.call(0x3a,self.prefix+struct.pack('<II',ep,direction))
            if st or len(raw)!=48:raise RuntimeError('endpoint snapshot envelope')
            version,actual_ep,actual_dir,ctl,tsiz,dma,irq,gint,gen,vns=struct.unpack('<8I2Q',raw)
            if (version,actual_ep,actual_dir)!=(1,ep,direction):raise RuntimeError('endpoint snapshot identity')
            if not irq&1:return
            if cancel.wait(.002):raise Cancelled()
        raise NativeRequestTimeout('actual guest bulk completion ISR did not drain')
    def transfer(self,urb,payload,cancel):
        """Return only accepted/ACKed actual packet counts, with real failures.
        Timeout/lost native reply is terminal, never an uncertain OUT replay.
        """
        transfer_start=time.perf_counter() if self.timing_path else 0
        actual=0;data=bytearray();key=0 if urb.ep==0 else urb.ep|(128 if urb.direction else 0)
        if key not in self.locks:return Completion(-95,0)
        # Existing native peer structurally excludes all vendor/auth SETUPs.
        # Report bridge EOPNOTSUPP, not a fake device STALL or transport loss.
        if urb.ep==0 and urb.setup[0]&0x60==0x40:
            self.record('native_unsupported_control_scope',seq=urb.seq,setup=urb.setup.hex(),status=-95,device_response=False)
            return Completion(-95,0)
        # DMA bookkeeping flags are local metadata; ZERO_PACKET is unsupported
        # rather than silently discarding a requested terminating bus packet.
        allowed=0x1|0x4|0x80|0x100|0x200|0xff0000
        if urb.flags&~allowed:return Completion(-95,0)
        if len(payload)!=(0 if urb.direction else urb.length):return Completion(-22,0)
        if not urb.direction and len(payload)==31 and payload[:4]==b'USBC':self.record('native_submitted_CBW',seq=urb.seq,raw_hex=payload.hex())
        stop=StopPair(self.stop,cancel)
        try:
            with self.locks[key]:
                self.check(stop)
                if urb.ep==0:
                    if payload:return Completion(-95,0)
                    self.acquire_bus(stop,time.monotonic()+30)
                    try:
                        walker=ControlWalk(self.wire,self.serial,self.token,stop,cadence=self.control_cadence)
                        data.extend(walker.control(urb.setup));actual=len(data)
                    finally:self.bus_gate.release()
                elif urb.direction:
                    until=time.monotonic()+30
                    while actual<urb.length or (urb.length==0 and actual==0):
                        with self.packet(0x35,struct.pack('<II',urb.ep,0),stop,until) as packet:
                            rc,n,nonce,part=packet
                            if rc==2:raise Stalled()
                            if not nonce or n!=len(part):raise RuntimeError('actual IN packet identity')
                            if n>urb.length-actual:
                                # An insufficient host buffer is an URB overflow,
                                # not loss of the native lease. The modeled host
                                # received a valid complete USB packet (<=MPS).
                                # ACK that actual packet so the device can finish
                                # its transaction; URB buffer size is host-side.
                                # Do not split/truncate it into a successful read.
                                self.check(stop)
                                reply=self.rpc(0x36,struct.pack('<IIQ',urb.ep,0,nonce),stop)
                                if reply[:3]!=(0,0,nonce):raise RuntimeError('overflow IN ACK provenance')
                                self.record('native_IN_URB_overflow',seq=urb.seq,ep=urb.ep,
                                    requested=urb.length,already_accepted=actual,packet_length=n,
                                    nonce=nonce,packet_hex=part.hex(),packet_acked=True,
                                    overflow_packet_buffered=0,status=-75)
                                self.drain(urb.ep,1,stop)
                                raise PacketOverflow()
                            self.check(stop)
                            reply=self.rpc(0x36,struct.pack('<IIQ',urb.ep,0,nonce),stop)
                            if reply[:3]!=(0,0,nonce):raise RuntimeError('actual IN ACK provenance')
                            data.extend(part);actual+=n
                        if n<self.mps or actual==urb.length:break
                else:
                    until=time.monotonic()+30;at=0
                    while at<urb.length or (urb.length==0 and at==0):
                        if self.batch_out and urb.length-at>self.mps:
                            part=payload[at:at+16384]
                            self.acquire_bus(stop,until)
                            try:
                                self.check(stop)
                                st,raw=self.wire.call(0x3b,self.prefix+struct.pack('<IIQ',urb.ep,len(part),self.out_seq+1)+part)
                                if st or len(raw)!=16:raise RuntimeError('batch OUT reply lost/refused; no replay')
                                rc,n,last_seq=struct.unpack('<iIQ',raw)
                                if n>len(part) or (n%self.mps and n!=len(part)) or last_seq!=self.out_seq+(n+self.mps-1)//self.mps:
                                    raise RuntimeError('batch OUT accepted-prefix identity')
                                self.out_seq=last_seq;actual+=n;at+=n
                                if rc==2:raise Stalled()
                                if rc not in (0,1) or (rc==0 and n!=len(part)):
                                    raise RuntimeError('batch OUT controller failure after known prefix')
                                self.check(stop)
                            finally:self.bus_gate.release()
                            if rc==1 and stop.wait(.000125):raise Cancelled()
                            if time.monotonic()>=until and at<urb.length:
                                raise NativeRequestTimeout('batch OUT bounded endpoint wait')
                            continue
                        part=payload[at:at+self.mps];seq=self.out_seq+1
                        with self.packet(0x34,struct.pack('<IIQ',urb.ep,len(part),seq)+part,stop,until) as packet:
                            rc,n,nonce,_=packet
                            if rc==2:raise Stalled()
                            if n!=len(part) or nonce!=seq:raise RuntimeError('actual OUT acceptance identity')
                            self.out_seq=seq;actual+=n;at+=n
                        if not part:break
            status=-121 if urb.flags&1 and actual<urb.length else 0
            result=Completion(status,actual,bytes(data) if urb.direction else b'')
        except (Stalled,ActualControlStall):result=Completion(-32,actual,bytes(data) if urb.direction else b'')
        except PacketOverflow:result=Completion(-75,actual,bytes(data) if urb.direction else b'')
        except NativeRequestTimeout as e:
            # These local waits have a known packet acceptance count. A real
            # device may NAK until its host cancels a request; that is not a
            # disconnected device. Keep the lease for host recovery requests.
            # Wire/RPC timeouts still follow the uncertain-effect fault path.
            self.record('native_URB_request_timeout',seq=urb.seq,error=str(e),known_accepted=actual,
                lease_retained=True,status=-110)
            result=Completion(-110,actual,bytes(data) if urb.direction else b'')
        except Cancelled:
            # A partly cancelled control cannot accept another SETUP in this
            # narrow peer. Disconnect this import; never fake its completion.
            if urb.ep==0:self.fault=True;self.stop.set()
            result=Completion(-104,actual,bytes(data) if urb.direction else b'')
        except BaseException as e:
            self.fault=True;self.stop.set()
            self.record('native_URB_terminal_fault',seq=urb.seq,error=f'{type(e).__name__}: {e}',known_accepted=actual)
            result=Completion(-5,actual,bytes(data) if urb.direction else b'')
        self.record('native_URB_result',seq=urb.seq,ep=urb.ep,direction=urb.direction,status=result.status,
            requested=urb.length,actual=result.actual,payload_sha256=hashlib.sha256(result.data if urb.direction else payload[:actual]).hexdigest())
        if self.timing_path:
            row=dict(wall_time=time.time(),seq=urb.seq,ep=urb.ep,direction=urb.direction,
                requested=urb.length,actual=result.actual,status=result.status,
                transfer_s=time.perf_counter()-transfer_start,nak_retries_total=self.nak_retries,
                nak_wait_s_total=self.nak_wait_s,
                rpc_cumulative=self.wire.timing_snapshot())
            with self.timing_lock:
                if self.timing_rows<4096:
                    with open(self.timing_path,'a',encoding='utf-8') as sink:sink.write(json.dumps(row)+'\n')
                    self.timing_rows+=1
        return result
