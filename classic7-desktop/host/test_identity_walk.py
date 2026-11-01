"""Offline sequencing/negative controls, no actual guest or disk."""
import unittest,threading,struct
from identity_walk import IdentityWalk

DEVICE=bytes.fromhex('1201000200000040ac056112000101020301')
CONFIG=bytes.fromhex('0902200001010080320904000002080650000705020200020007058302000200')
class Wire:
    def __init__(self,address=1):self.address=address
    def call(self,op,payload=b''):
        assert op==0x12
        raw=bytearray(64);struct.pack_into('<I',raw,36,self.address<<4)
        return 0,bytes(raw)
class Script(IdentityWalk):
    def __init__(self,current=0,device=DEVICE,config=CONFIG,address=1,stall=False):
        super().__init__(Wire(address),1,2,threading.Event())
        self.requested=[];self.current=current;self.device=device;self.config=config;self.stall=stall
    def control(self,setup):
        self.requested.append(setup)
        if self.stall and setup[1]==5:raise RuntimeError('synthetic actual STALL fixture')
        if setup[1]==5:return b''
        if setup[1]==8:return bytes([self.current])
        assert setup[1]==6
        return (self.device if setup[3]==1 else self.config)[:int.from_bytes(setup[6:8],'little')]
class Tests(unittest.TestCase):
    def test_unconfigured_identity_has_no_configure_or_BOT(self):
        w=Script();r=w.run();self.assertEqual(r['current_configuration'],0)
        self.assertEqual([x[1] for x in w.requested],[6,5,6,6,8]);self.assertFalse(r['preflight_SET_CONFIGURATION_sent']);self.assertFalse(r['preflight_BOT_sent'])
    def test_actual_configured_state_is_not_rewritten(self):
        w=Script(current=1);self.assertEqual(w.run()['current_configuration'],1)
    def test_unknown_configuration_rejected(self):
        w=Script(current=2);self.assertEqual(w.run()['status'],'stopped_at_unsupported_or_failed_identity')
    def test_address_not_forced(self):
        w=Script(address=0);self.assertIn('not observed',w.run()['error']);self.assertEqual(len(w.requested),2)
    def test_STALL_not_retried(self):
        w=Script(stall=True);self.assertIn('STALL',w.run()['error']);self.assertEqual(len(w.requested),2)
    def test_invalid_descriptor_blocks_listener(self):
        w=Script(device=bytes(18));self.assertIn('descriptor',w.run()['error']);self.assertEqual(len(w.requested),1)
if __name__=='__main__':unittest.main()
