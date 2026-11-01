import unittest,io,threading
from bounded_records import BoundedRecords
class Tests(unittest.TestCase):
    def test_raw_capacity_never_raises_or_crowds_semantic_results(self):
        j=BoundedRecords(io.StringIO(),raw_limit=2)
        for i in range(10001):j.add('request',op=0x35)
        j.add('native_URB_result',status=0,actual=13)
        rows,s=j.snapshot();self.assertEqual(len(rows),3);self.assertEqual(s['total_records'],10002);self.assertEqual(s['raw_dropped_from_summary'],9999)
    def test_semantic_capacity_is_explicit_not_transport_failure(self):
        j=BoundedRecords(io.StringIO(),semantic_limit=2)
        for i in range(10):j.add('native_URB_result',actual=i)
        rows,s=j.snapshot();self.assertEqual(len(rows),2);self.assertEqual(s['semantic_dropped_from_summary'],8);self.assertEqual(s['counts']['native_URB_result'],10)
    def test_stream_size_remains_bounded_and_counters_continue(self):
        f=io.StringIO();j=BoundedRecords(f,byte_limit=100)
        for i in range(100):j.add('request',payload='x'*1000)
        rows,s=j.snapshot();self.assertLessEqual(len(f.getvalue().encode()),100);self.assertEqual(s['stream_dropped'],100);self.assertEqual(s['total_records'],100)
    def test_concurrent_exact_counts_and_serialized_stream(self):
        f=io.StringIO();j=BoundedRecords(f);threads=[threading.Thread(target=lambda:[j.add('request',op=0x35) for i in range(100)]) for _ in range(4)]
        for t in threads:t.start()
        for t in threads:t.join()
        rows,s=j.snapshot();self.assertEqual(s['total_records'],400);self.assertEqual(len(f.getvalue().splitlines()),400);self.assertEqual(len(rows),400)
if __name__=='__main__':unittest.main()
