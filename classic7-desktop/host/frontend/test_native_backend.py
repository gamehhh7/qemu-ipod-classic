"""SYNTHETIC native wire fault tests; no guest, driver, or disk writes."""
from pathlib import Path
import sys,unittest,threading,struct
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
from native_backend import NativeBackend
from usbip_codec import Urb
from test_codec import DEVICE,CONFIG

class NativeWire:
    def __init__(self,parts=None,lose_out=False,stall=False):
        self.parts=list(parts or []);self.nonce=10;self.ops=[];self.out=[];self.lose_out=lose_out;self.stall=stall
    def call(self,op,raw=b''):
        self.ops.append(op)
        if op==0x12:
            r=bytearray(64);struct.pack_into('<I',r,36,16);return 0,bytes(r)
        if op==0x37:return 0,struct.pack('<8I4Q',2,1,0,0,0,0,1,1,1,0,0,0)
        if op==0x3a:
            ep,direction=struct.unpack_from('<II',raw,16);return 0,struct.pack('<8I2Q',1,ep,direction,0,0,0,0,0,0,0)
        if op==0x35:
            if self.stall:return 0,struct.pack('<iIQ',2,0,0)
            p=self.parts[0];return 0,struct.pack('<iIQ',0,len(p),self.nonce)+p
        if op==0x36:
            nonce=struct.unpack_from('<Q',raw,24)[0]
            assert nonce==self.nonce;self.parts.pop(0);self.nonce+=1
            return 0,struct.pack('<iIQ',0,0,nonce)
        if op==0x34:
            ep,n,seq=struct.unpack_from('<IIQ',raw,16);self.out.append((seq,raw[32:]));assert n==len(raw)-32
            if self.lose_out:raise EOFError('synthetic reply lost after accept')
            return 0,struct.pack('<iIQ',0,n,seq)
        raise AssertionError(op)

class Tests(unittest.TestCase):
    def backend(self,wire):return NativeBackend(wire,3,5,threading.Event(),DEVICE,CONFIG,lambda *a,**k:None)
    def urb(self,direction=1,length=512,flags=0,ep=3):return Urb(1,65537,direction,ep,flags,length,bytes(8))
    def test_two_IN_packets_are_real_data_and_distinct_ACKs(self):
        w=NativeWire([bytes([7])*512,bytes([8])*512]);b=self.backend(w);r=b.transfer(self.urb(length=1024),b'',threading.Event())
        self.assertEqual((r.status,r.actual,r.data),(0,1024,bytes([7])*512+bytes([8])*512));self.assertEqual(w.ops.count(0x36),2)
    def test_short_IN_is_returned_unpadded(self):
        w=NativeWire([b'x'*13]);r=self.backend(w).transfer(self.urb(),b'',threading.Event());self.assertEqual((r.status,r.actual,r.data),(0,13,b'x'*13))
    def test_short_not_OK_reports_real_short_bytes(self):
        w=NativeWire([b'x'*13]);r=self.backend(w).transfer(self.urb(flags=1),b'',threading.Event());self.assertEqual((r.status,r.actual),(-121,13))
    def test_actual_zero_length_IN_requires_packet_and_ACK(self):
        w=NativeWire([b'']);r=self.backend(w).transfer(self.urb(length=0),b'',threading.Event());self.assertEqual((r.status,r.actual),(0,0));self.assertIn(0x36,w.ops)
    def test_OUT_sequences_are_monotonic_actual_packets(self):
        w=NativeWire();b=self.backend(w);r=b.transfer(self.urb(direction=0,length=1024,ep=2),b'z'*1024,threading.Event())
        self.assertEqual((r.status,r.actual,r.data),(0,1024,b''));self.assertEqual([x[0] for x in w.out],[1,2])
    def test_live_handoff_keeps_preflight_accepted_OUT_sequence(self):
        w=NativeWire();b=NativeBackend(w,3,5,threading.Event(),DEVICE,CONFIG,lambda *a,**k:None,initial_out_sequence=8)
        r=b.transfer(self.urb(direction=0,ep=2,length=31),bytes(31),threading.Event())
        self.assertEqual(r.status,0);self.assertEqual(w.out[0][0],9)
    def test_lost_accepted_OUT_not_replayed(self):
        w=NativeWire(lose_out=True);b=self.backend(w);r=b.transfer(self.urb(direction=0,length=1024,ep=2),b'z'*1024,threading.Event())
        self.assertEqual(r.status,-5);self.assertEqual(len(w.out),1);self.assertTrue(b.stop.is_set())
    def test_actual_STALL_has_no_ACK(self):
        w=NativeWire(stall=True);r=self.backend(w).transfer(self.urb(),b'',threading.Event());self.assertEqual((r.status,r.actual),(-32,0));self.assertNotIn(0x36,w.ops)
    def test_cancelled_before_token_sends_no_packet(self):
        w=NativeWire();b=self.backend(w);cancel=threading.Event();cancel.set();r=b.transfer(self.urb(),b'',cancel)
        self.assertEqual(r.status,-104);self.assertEqual(w.ops,[0x12])
    def test_unsupported_zero_packet_flag_is_not_silently_ignored(self):
        w=NativeWire();r=self.backend(w).transfer(self.urb(direction=0,flags=0x40,ep=2),b'x'*512,threading.Event());self.assertEqual(r.status,-95);self.assertFalse(w.out)
    def test_unconfigured_endpoint_sends_no_packet(self):
        w=NativeWire();r=self.backend(w).transfer(self.urb(ep=7),b'',threading.Event());self.assertEqual(r.status,-95);self.assertEqual(w.ops,[0x12])
    def test_Windows_control_obeys_handed_off_service_clock(self):
        from host_cadence import HostCadence
        w=NativeWire();p=HostCadence();p.last_end=1
        b=NativeBackend(w,3,5,threading.Event(),DEVICE,CONFIG,lambda *a,**k:None,control_cadence=p)
        u=Urb(1,65537,1,0,0,18,bytes.fromhex('8006000100001200'))
        r=b.transfer(u,b'',threading.Event())
        self.assertEqual(r.status,-5);self.assertTrue(b.stop.is_set());self.assertNotIn(0x30,w.ops)
    def test_vendor_IN_reports_bridge_unsupported_and_keeps_bulk_alive(self):
        w=NativeWire([b'abc']);b=self.backend(w)
        u=Urb(10,65537,1,0,0,2,bytes.fromhex('c040000000000200'))
        r=b.transfer(u,b'',threading.Event())
        self.assertEqual((r.status,r.actual),(-95,0));self.assertEqual(w.ops,[0x12]);self.assertTrue(b.live())
        r=b.transfer(self.urb(length=3),b'',threading.Event());self.assertEqual((r.status,r.data),(0,b'abc'))
    def test_vendor_OUT_does_not_send_auth_or_data(self):
        w=NativeWire();b=self.backend(w)
        u=Urb(10,65537,0,0,0,2,bytes.fromhex('4040000000000200'))
        r=b.transfer(u,b'ab',threading.Event())
        self.assertEqual((r.status,r.actual),(-95,0));self.assertEqual(w.ops,[0x12]);self.assertTrue(b.live())
if __name__=='__main__':unittest.main()
