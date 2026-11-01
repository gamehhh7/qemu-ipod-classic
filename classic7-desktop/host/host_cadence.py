"""Conservative host EP0 dispatch policy; no guest software readiness tests.

One full USB-frame unit of quiet time between completed status and next SETUP
is a chosen host scheduling policy, NOT a mandatory USB control delay.
"""
import struct
class HostCadence:
    INTERVAL_NS=1_000_000
    def __init__(self):self.last_end=None;self.last_seen=None
    def observe(self,wire):
        status,raw=wire.call(0x37)
        if status or len(raw)!=64:raise RuntimeError('host cadence actual clock snapshot refused')
        version,vbus,reset,host,dctl,clock,receiver,qualified,now,*_=struct.unpack('<8I4Q',raw)
        if version!=2 or vbus!=1 or reset or host or dctl&2:raise RuntimeError('host cadence actual bus disconnected/reset')
        if self.last_seen is not None and now<self.last_seen:raise RuntimeError('host cadence actual clock moved backwards')
        self.last_seen=now;return now
    def before(self,wire,stop):
        due=None if self.last_end is None else self.last_end+self.INTERVAL_NS
        for polls in range(32):
            if stop.is_set():raise RuntimeError('host cadence cancelled before SETUP')
            now=self.observe(wire)
            if due is None or now>=due:return {'actual_start_vns':now,'earliest_vns':due,'policy_interval_ns':self.INTERVAL_NS,'polls':polls+1,'mandatory_USB_delay':False}
            if stop.wait(.001):raise RuntimeError('host cadence cancelled before SETUP')
        raise TimeoutError('actual virtual host service interval did not elapse; no SETUP or retry')
    def after(self,wire):
        self.last_end=self.observe(wire);return self.last_end
