"""Mutually exclusive completion/cancellation ownership for one USB/IP import.

Transport cancellation must be confirmed before cancel_committed is called.
Closing a connection returns pending URBs to the caller for native lease abort;
it does not invent a successful USB cancellation or completion.
"""
from threading import Lock
from usbip_codec import ProtocolError,completed,unlinked

class UrbLedger:
    def __init__(self,devid,limit=32,max_requests=100000):
        self.devid=devid;self.limit=limit;self.max_requests=max_requests
        self.pending={};self.last_seq=0;self.count=0;self.closed=False;self.lock=Lock()
    def _sequence(self,seq):
        if self.closed or not self.last_seq<seq<=0xffffffff or self.count>=self.max_requests:raise ProtocolError('closed/replayed/out-of-order/request-bound sequence')
        self.last_seq=seq;self.count+=1
    def reserve(self,urb):
        with self.lock:
            if urb.devid!=self.devid:raise ProtocolError('different native device')
            if len(self.pending)>=self.limit:raise ProtocolError('pending URB limit')
            self._sequence(urb.seq);self.pending[urb.seq]=urb
    def completion_committed(self,seq,status,actual,payload=b''):
        with self.lock:
            urb=self.pending.get(seq)
            if urb is None or self.closed:return None
            wire=completed(urb,status,actual,payload)
            del self.pending[seq]
            return wire
    def cancel_committed(self,unlink_seq,target_seq,transport_cancelled):
        """The native adapter must serialize effects before confirming cancel.
        None means cancellation was not confirmed; pending URB stays owned and
        caller must not send a fake successful RET_UNLINK.
        """
        with self.lock:
            self._sequence(unlink_seq)
            if target_seq in self.pending:
                if transport_cancelled is not True:return None
                del self.pending[target_seq]
                return unlinked(unlink_seq,True)
            return unlinked(unlink_seq,False)
    def close(self):
        with self.lock:
            self.closed=True;pending=tuple(self.pending.values());self.pending.clear();return pending
