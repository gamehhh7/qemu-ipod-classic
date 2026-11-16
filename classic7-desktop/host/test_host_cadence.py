import unittest,struct
from host_cadence import HostCadence
class Stop:
    def __init__(self,cancel=False):self.cancel=cancel;self.waits=0
    def is_set(self):return self.cancel
    def wait(self,t):self.waits+=1;return self.cancel
class Wire:
    def __init__(self,times,refuse=False):self.times=list(times);self.last=0;self.ops=[];self.refuse=refuse
    def call(self,op,payload=b''):
        self.ops.append(op);assert op==0x37 and not payload
        if self.refuse:return 1,b''
        if self.times:self.last=self.times.pop(0)
        return 0,struct.pack('<8I4Q',2,1,0,0,0,0,0,0,self.last,0,0,0)
class Tests(unittest.TestCase):
    def test_first_request_has_no_added_wait(self):
        w=Wire([100]);s=Stop();self.assertIsNone(HostCadence().before(w,s)['earliest_vns']);self.assertEqual(s.waits,0)
    def test_exact_interval_from_actual_completion(self):
        p=HostCadence();w=Wire([100,100,500000,1000100]);p.after(w);r=p.before(w,Stop());self.assertEqual(r['actual_start_vns'],1000100);self.assertEqual(r['polls'],3)
    def test_no_elapsed_clock_never_assumed_ready(self):
        p=HostCadence();w=Wire([0]);p.after(w)
        with self.assertRaises(TimeoutError):p.before(w,Stop())
        self.assertTrue(all(x==0x37 for x in w.ops));self.assertEqual(len(w.ops),33)
    def test_cancel_before_clock_query(self):
        w=Wire([1])
        with self.assertRaises(RuntimeError):HostCadence().before(w,Stop(True))
        self.assertEqual(w.ops,[])
    def test_backwards_clock_rejected(self):
        p=HostCadence();w=Wire([20,19]);p.after(w)
        with self.assertRaisesRegex(RuntimeError,'backwards'):p.before(w,Stop())
    def test_actual_refusal_not_ignored(self):
        with self.assertRaises(RuntimeError):HostCadence().before(Wire([],True),Stop())
    def test_handoff_preserves_last_completed_time(self):
        p=HostCadence();p.after(Wire([400]));r=p.before(Wire([2000000]),Stop());self.assertEqual(r['earliest_vns'],1000400)
if __name__=='__main__':unittest.main()
