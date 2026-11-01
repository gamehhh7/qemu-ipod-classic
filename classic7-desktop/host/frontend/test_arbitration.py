"""Concurrent adapter scheduling fixtures only, no guest/HCD or storage."""
import unittest,threading,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
from unittest.mock import patch
from native_backend import NativeBackend
from usbip_codec import Urb
from test_native_backend import NativeWire
from test_codec import DEVICE,CONFIG
class Tests(unittest.TestCase):
    def backend(self,w):return NativeBackend(w,3,5,threading.Event(),DEVICE,CONFIG,lambda *a,**k:None)
    def control(self):return Urb(1,65537,1,0,0,18,bytes.fromhex('8006000100001200'))
    def bulk(self):return Urb(2,65537,1,3,0,3,bytes(8))
    def test_EP0_excludes_bulk_until_actual_control_finishes(self):
        entered=threading.Event();release=threading.Event();bulk_called=threading.Event();results={}
        class Wire(NativeWire):
            def call(self,op,raw=b''):
                if op==0x35:bulk_called.set()
                return super().call(op,raw)
        class Control:
            def __init__(self,*args,**kwargs):pass
            def control(self,setup):entered.set();assert release.wait(1);return b'd'*18
        w=Wire([b'abc']);b=self.backend(w)
        with patch('native_backend.ControlWalk',Control):
            t=threading.Thread(target=lambda:results.update(c=b.transfer(self.control(),b'',threading.Event())))
            u=threading.Thread(target=lambda:results.update(b=b.transfer(self.bulk(),b'',threading.Event())))
            t.start();self.assertTrue(entered.wait(1));u.start()
            try:self.assertFalse(bulk_called.wait(.03))
            finally:release.set();t.join(1);u.join(1)
        self.assertEqual((results['c'].status,results['b'].status),(0,0));self.assertTrue(bulk_called.is_set())
    def test_NAK_loop_yields_for_control_recovery(self):
        nak=threading.Event();ready=threading.Event();results={}
        class Wire(NativeWire):
            def call(self,op,raw=b''):
                if op==0x35 and not ready.is_set():nak.set();return 0,struct.pack('<iIQ',1,0,0)
                return super().call(op,raw)
        class Control:
            def __init__(self,*args,**kwargs):pass
            def control(self,setup):ready.set();return b'd'*18
        w=Wire([b'abc']);b=self.backend(w)
        with patch('native_backend.ControlWalk',Control):
            t=threading.Thread(target=lambda:results.update(b=b.transfer(self.bulk(),b'',threading.Event())))
            u=threading.Thread(target=lambda:results.update(c=b.transfer(self.control(),b'',threading.Event())))
            t.start();self.assertTrue(nak.wait(1));u.start();u.join(1);t.join(1)
            if u.is_alive() or t.is_alive():b.stop.set();u.join(1);t.join(1)
        self.assertTrue(ready.is_set());self.assertEqual((results['c'].status,results['b'].status),(0,0));self.assertEqual(results['b'].data,b'abc')
    def test_cancel_waiting_for_bus_sends_no_packet(self):
        w=NativeWire([b'abc']);b=self.backend(w);cancel=threading.Event();started=threading.Event();result=[]
        b.bus_gate.acquire()
        def work():started.set();result.append(b.transfer(self.bulk(),b'',cancel))
        t=threading.Thread(target=work);t.start();self.assertTrue(started.wait(1));cancel.set();t.join(1);b.bus_gate.release()
        self.assertFalse(t.is_alive());self.assertEqual(result[0].status,-104);self.assertEqual(w.ops,[0x12])
if __name__=='__main__':unittest.main()
