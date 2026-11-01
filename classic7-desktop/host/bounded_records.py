"""Bounded diagnostics. Capacity truncation never changes transport state."""
import json,threading
class BoundedRecords:
    def __init__(self,sink,raw_limit=512,semantic_limit=20000,byte_limit=16*1024*1024,raw_stream_limit=None):
        self.sink=sink;self.raw_limit=raw_limit;self.semantic_limit=semantic_limit;self.byte_limit=byte_limit
        self.lock=threading.Lock();self.rows=[];self.raw=0;self.semantic=0;self.counts={};self.total=0;self.bytes=0;self.stream_dropped=0;self.raw_dropped=0;self.semantic_dropped=0
        self.raw_stream_limit=raw_stream_limit;self.raw_stream_seen=0;self.raw_stream_skipped=0
    def add(self,kind,**kw):
        row={'kind':kind,**kw}
        with self.lock:
            self.total+=1;self.counts[kind]=self.counts.get(kind,0)+1
            if kind in ('request','response'):
                if self.raw<self.raw_limit:self.rows.append(row);self.raw+=1
                else:self.raw_dropped+=1
            elif self.semantic<self.semantic_limit:self.rows.append(row);self.semantic+=1
            else:self.semantic_dropped+=1
            if kind in ('request','response'):
                self.raw_stream_seen+=1
                if self.raw_stream_limit is not None and self.raw_stream_seen>self.raw_stream_limit:
                    self.raw_stream_skipped+=1;return
            line=json.dumps(row,ensure_ascii=True,separators=(',',':'))+'\n'
            size=len(line.encode('ascii'))
            if self.bytes+size<=self.byte_limit:
                self.sink.write(line);self.bytes+=size
            else:self.stream_dropped+=1
    def snapshot(self):
        with self.lock:
            self.sink.flush()
            return list(self.rows),{'total_records':self.total,'counts':dict(self.counts),'raw_retained':self.raw,'semantic_retained':self.semantic,'raw_dropped_from_summary':self.raw_dropped,
                'semantic_dropped_from_summary':self.semantic_dropped,'stream_bytes':self.bytes,'stream_byte_limit':self.byte_limit,'stream_dropped':self.stream_dropped,
                'raw_stream_skipped_by_daily_policy':self.raw_stream_skipped,'capacity_never_changes_USB_effects':True,'all_stream_records_retained':self.stream_dropped==0 and self.raw_stream_skipped==0}
