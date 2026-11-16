"""Real loopback TCP framing with SYNTHETIC backend, no Windows/device/media."""
import unittest,socket,threading,time,struct
from types import SimpleNamespace
from usbip_server import LoopbackServer
from usbip_codec import COMMON,BASIC,c_string
from test_codec import exported,header,DEVICE

class SyntheticBackend:
    def __init__(self):self.stop=threading.Event();self.export=exported();self.received=[];self.block=False
    def live(self):return not self.stop.is_set()
    def snapshot(self):return self.export if self.live() else None
    def transfer(self,urb,payload,cancel):
        self.received.append((urb,payload))
        while self.block and not cancel.wait(.005) and not self.stop.is_set():pass
        if cancel.is_set() or self.stop.is_set():return SimpleNamespace(status=-104,actual=0,data=b'')
        raw=DEVICE[:urb.length] if urb.direction else b''
        return SimpleNamespace(status=0,actual=len(raw) if urb.direction else len(payload),data=raw)

class Tests(unittest.TestCase):
    def setUp(self):
        self.backend=SyntheticBackend();self.logs=[]
        self.server=LoopbackServer(self.backend,lambda kind,**kw:self.logs.append((kind,kw)))
        self.port=self.server.start();self.sockets=[]
    def tearDown(self):
        for s in self.sockets:s.close()
        self.server.close()
    def socket(self):
        s=socket.create_connection(('127.0.0.1',self.port),timeout=1);self.sockets.append(s);return s
    def exact(self,s,n):
        b=b''
        while len(b)<n:
            p=s.recv(n-len(b))
            if not p:raise EOFError()
            b+=p
        return b
    def imported(self):
        s=self.socket();s.sendall(COMMON.pack(0x111,0x8003,0)+c_string('1-1',32))
        self.assertEqual(self.exact(s,8),bytes.fromhex('0111000300000000'));self.assertEqual(len(self.exact(s,312)),312);return s
    def test_fragmented_devlist_exact_layout(self):
        s=self.socket();raw=COMMON.pack(0x111,0x8005,0)
        for byte in raw:s.sendall(bytes([byte]))
        r=self.exact(s,328);self.assertEqual(r[:12],bytes.fromhex('011100050000000000000001'));self.assertEqual(r[-4:],bytes([8,6,0x50,0]))
    def test_single_import_control_IN(self):
        s=self.imported();s.sendall(header(ep=0,length=18,setup=bytes.fromhex('8006000100001200')))
        r=self.exact(s,66);self.assertEqual(BASIC.unpack_from(r),(3,7,0,0,0));self.assertEqual(r[48:],DEVICE)
    def test_fragmented_OUT_body_not_echoed_as_IN(self):
        s=self.imported();raw=header(direction=0,ep=2,length=5)+b'abcde'
        for at in range(0,len(raw),3):s.sendall(raw[at:at+3])
        r=self.exact(s,48);self.assertEqual(struct.unpack_from('>ii',r,20),(0,5));self.assertEqual(self.backend.received[0][1],b'abcde')
    def test_duplicate_import_refused_without_stopping_owner(self):
        self.imported();s=self.socket();s.sendall(COMMON.pack(0x111,0x8003,0)+c_string('1-1',32))
        self.assertEqual(self.exact(s,8),bytes.fromhex('0111000300000001'));self.assertFalse(self.backend.stop.is_set())
    def test_confirmed_cancel_has_no_late_target_response(self):
        self.backend.block=True;s=self.imported();s.sendall(header(seq=1,ep=3,length=18))
        until=time.monotonic()+1
        while not self.backend.received and time.monotonic()<until:time.sleep(.001)
        s.sendall(BASIC.pack(2,2,65537,0,0)+struct.pack('>I',1)+bytes(24))
        r=self.exact(s,48);self.assertEqual(BASIC.unpack_from(r),(4,2,0,0,0));self.assertEqual(struct.unpack_from('>i',r,20)[0],-104)
        s.settimeout(.1)
        with self.assertRaises(socket.timeout):s.recv(1)
    def test_negative_allocation_rejected_before_backend(self):
        s=self.imported();s.sendall(header(length=-1));self.assertEqual(s.recv(1),b'');self.assertFalse(self.backend.received)
    def test_disconnected_backend_is_not_exported(self):
        self.backend.stop.set();s=self.socket();s.sendall(COMMON.pack(0x111,0x8005,0))
        self.assertEqual(self.exact(s,12),bytes.fromhex('011100050000000000000000'))
if __name__=='__main__':unittest.main()
