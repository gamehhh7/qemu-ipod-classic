import unittest,threading,struct
from usbip_codec import Urb,ProtocolError
from urb_ledger import UrbLedger

def urb(seq=1):return Urb(seq,65537,1,3,0,3,bytes(8))
class Tests(unittest.TestCase):
    def test_cancel_then_complete_never_sends_target_reply(self):
        q=UrbLedger(65537);q.reserve(urb());r=q.cancel_committed(2,1,True)
        self.assertEqual(struct.unpack_from('>i',r,20)[0],-104)
        self.assertIsNone(q.completion_committed(1,0,3,b'abc'))
    def test_complete_then_cancel_reports_already_completed(self):
        q=UrbLedger(65537);q.reserve(urb());self.assertIsNotNone(q.completion_committed(1,0,3,b'abc'))
        self.assertEqual(struct.unpack_from('>i',q.cancel_committed(2,1,True),20)[0],0)
    def test_uncertain_cancel_keeps_target_owned(self):
        q=UrbLedger(65537);q.reserve(urb());self.assertIsNone(q.cancel_committed(2,1,False))
        self.assertIsNotNone(q.completion_committed(1,-32,0))
    def test_duplicate_completion_suppressed(self):
        q=UrbLedger(65537);q.reserve(urb());q.completion_committed(1,0,3,b'abc')
        self.assertIsNone(q.completion_committed(1,0,3,b'abc'))
    def test_malformed_completion_does_not_lose_ownership(self):
        q=UrbLedger(65537);q.reserve(urb())
        with self.assertRaises(ProtocolError):q.completion_committed(1,0,3,b'ab')
        self.assertIsNotNone(q.completion_committed(1,0,3,b'abc'))
    def test_duplicate_sequence_after_completion_is_replay(self):
        q=UrbLedger(65537);q.reserve(urb());q.completion_committed(1,0,0)
        with self.assertRaises(ProtocolError):q.reserve(urb())
    def test_active_bound_does_not_consume_new_sequence(self):
        q=UrbLedger(65537,limit=1);q.reserve(urb())
        with self.assertRaises(ProtocolError):q.reserve(urb(2))
        q.completion_committed(1,0,0);q.reserve(urb(2))
    def test_close_gives_native_adapter_pending_aborts(self):
        q=UrbLedger(65537);q.reserve(urb());self.assertEqual(q.close(),(urb(),))
        self.assertIsNone(q.completion_committed(1,0,3,b'abc'))
        with self.assertRaises(ProtocolError):q.reserve(urb(2))
    def test_threaded_complete_cancel_race_emits_one_target_outcome(self):
        for _ in range(100):
            q=UrbLedger(65537);q.reserve(urb());gate=threading.Barrier(3);results={}
            def complete():gate.wait();results['complete']=q.completion_committed(1,0,3,b'abc')
            def cancel():gate.wait();results['cancel']=q.cancel_committed(2,1,True)
            a=threading.Thread(target=complete);b=threading.Thread(target=cancel);a.start();b.start();gate.wait();a.join();b.join()
            status=struct.unpack_from('>i',results['cancel'],20)[0]
            self.assertEqual(results['complete'] is None,status==-104)
            self.assertFalse(q.pending)
if __name__=='__main__':unittest.main()
