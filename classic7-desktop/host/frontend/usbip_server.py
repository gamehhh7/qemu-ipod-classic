"""Single-import loopback USB/IP endpoint; native backend owns USB effects.

No command line starts this module. A currently reviewed native bus owner must
construct it with actual guest descriptors and a live NativeBackend.
"""
import socket,socketserver,threading,concurrent.futures,struct,time
from dataclasses import dataclass,field
from usbip_codec import operation,decode_string,devlist,imported,submit,unlink,ProtocolError
from urb_ledger import UrbLedger

@dataclass
class Job:
    urb:object
    cancel:threading.Event=field(default_factory=threading.Event)
    done:threading.Event=field(default_factory=threading.Event)
    result:object=None

class Registry:
    def __init__(self,backend,record):self.backend=backend;self.record=record;self.used=False;self.lock=threading.Lock()
    def snapshot(self):return None if self.used else self.backend.snapshot()
    def claim(self,busid):
        with self.lock:
            export=self.snapshot()
            if self.used or export is None or busid!=export.busid:return None
            self.used=True;self.record('USBIP_import_claimed',busid=busid)
            return self.backend

class Handler(socketserver.BaseRequestHandler):
    def exact(self,n):
        if n<0 or n>1024*1024:raise ProtocolError('read bound')
        data=bytearray()
        while len(data)<n:
            try:part=self.request.recv(n-len(data))
            except socket.timeout:
                if self.server.stop_event.is_set():raise EOFError('server stopping')
                if self.deadline is not None and time.monotonic()>self.deadline:raise TimeoutError('single USBIP connection bound')
                continue
            if not part:raise EOFError('USBIP TCP EOF')
            data.extend(part)
        return bytes(data)
    def handle(self):
        registry=self.server.registry;backend=None;executor=None;ledger=None;jobs={}
        self.deadline=None if self.server.continuous else time.monotonic()+340;self.request.settimeout(.5)
        send_lock=threading.Lock()
        try:
            if self.client_address[0]!='127.0.0.1':raise ProtocolError('loopback only')
            op=operation(self.exact(8))
            if op==0x8005:
                self.request.sendall(devlist(registry.snapshot()));return
            busid=decode_string(self.exact(32));backend=registry.claim(busid)
            self.request.sendall(imported(backend.export if backend else None))
            if backend is None:return
            devid=(backend.export.busnum<<16)|backend.export.devnum
            ledger=UrbLedger(devid);executor=concurrent.futures.ThreadPoolExecutor(max_workers=8,thread_name_prefix='actual-native-urb')
            def worker(job,payload):
                try:job.result=backend.transfer(job.urb,payload,job.cancel)
                except BaseException as e:
                    backend.stop.set()
                    try:registry.record('USBIP_backend_exception',seq=job.urb.seq,error=repr(e))
                    except Exception:pass  # logging failure must not leave a live writer
                finally:job.done.set()
                with send_lock:
                    if job.cancel.is_set() or job.result is None:return
                    r=job.result;wire=ledger.completion_committed(job.urb.seq,r.status,r.actual,r.data)
                    if wire is not None:
                        try:self.request.sendall(wire)
                        except OSError:backend.stop.set()
                    jobs.pop(job.urb.seq,None)
            while not self.server.stop_event.is_set() and backend.live():
                raw=self.exact(48);command=struct.unpack_from('>I',raw)[0]
                if command==1:
                    urb=submit(raw,devid)
                    payload=self.exact(urb.length) if not urb.direction and urb.length else b''
                    with send_lock:
                        ledger.reserve(urb);job=Job(urb);jobs[urb.seq]=job
                    registry.record('USBIP_submit',seq=urb.seq,ep=urb.ep,direction=urb.direction,flags=urb.flags,length=urb.length,setup=urb.setup.hex())
                    executor.submit(worker,job,payload)
                elif command==2:
                    registry.record('USBIP_unlink_received',header_hex=raw.hex())
                    seq,target=unlink(raw,devid)
                    if any(raw[24:]):registry.record('USBIP_unlink_unused_tail_ignored',seq=seq,target=target,unused_hex=raw[24:].hex())
                    with send_lock:
                        job=jobs.get(target)
                        if job is None:
                            self.request.sendall(ledger.cancel_committed(seq,target,False));continue
                        if job.done.is_set() and job.result is not None:
                            r=job.result;wire=ledger.completion_committed(target,r.status,r.actual,r.data)
                            if wire is not None:self.request.sendall(wire)
                            self.request.sendall(ledger.cancel_committed(seq,target,False));jobs.pop(target,None);continue
                        job.cancel.set()
                    # No completion can now be sent by worker. It must stop
                    # issuing actual USB packets before cancellation is replied.
                    if not job.done.wait(3):raise TimeoutError('native cancellation not confirmed; abort import')
                    with send_lock:
                        wire=ledger.cancel_committed(seq,target,True)
                        self.request.sendall(wire);jobs.pop(target,None)
                    registry.record('USBIP_unlinked',seq=seq,target=target,native_worker_stopped=True)
                else:raise ProtocolError('unsupported USBIP command')
        except Exception as e:
            registry.record('USBIP_connection_end',imported=backend is not None,reason=f'{type(e).__name__}: {e}')
        finally:
            if backend is not None:
                backend.stop.set()
                with send_lock:
                    for job in jobs.values():job.cancel.set()
                    if ledger is not None:ledger.close()
            if executor is not None:executor.shutdown(wait=True,cancel_futures=True)

class LoopbackServer(socketserver.ThreadingTCPServer):
    allow_reuse_address=False;daemon_threads=True
    def __init__(self,backend,record,port=0,continuous=False):
        self.continuous=continuous
        self.registry=Registry(backend,record);self.stop_event=threading.Event()
        super().__init__(('127.0.0.1',port),Handler)
        self.thread=None
    def start(self):
        self.thread=threading.Thread(target=self.serve_forever,name='one-native-usbip-import',daemon=True);self.thread.start()
        return self.server_address[1]
    def close(self):
        self.stop_event.set();self.registry.backend.stop.set();self.shutdown();self.server_close()
        if self.thread:self.thread.join(timeout=2)
