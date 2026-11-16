"""Actual native device identity only; Windows owns configuration and SCSI.

No SET_CONFIGURATION, BOT warm-up, USB data substitution, or forced state.
"""
import struct
from control_walk import ControlWalk

class IdentityWalk(ControlWalk):
    def run(self):
        try:
            device=self.control(bytes.fromhex('8006000100001200'))
            if len(device)!=18 or device[:2]!=bytes([18,1]) or device[7]!=64:
                raise RuntimeError('actual device descriptor invalid/unsupported')
            self.result['device_descriptor']={'raw_hex':device.hex(),'vid':int.from_bytes(device[8:10],'little'),'pid':int.from_bytes(device[10:12],'little')}
            self.control(bytes.fromhex('0005010000000000'))
            address=None
            for _ in range(12):
                status,raw=self.wire.call(0x12)
                if status or len(raw)!=64:raise RuntimeError('actual address query refused')
                address=(struct.unpack_from('<I',raw,36)[0]>>4)&127
                if address==1:break
                if self.stopped.wait(.05):raise RuntimeError('identity cancelled')
            self.result['guest_dcfg_address']=address
            if address!=1:raise RuntimeError('actual guest address1 not observed')
            header=self.control(bytes.fromhex('8006000200000900'))
            if len(header)!=9 or header[:2]!=bytes([9,2]):raise RuntimeError('actual config header invalid')
            total=int.from_bytes(header[2:4],'little')
            if not 9<=total<=4096:raise RuntimeError('bounded actual configuration required')
            config=self.control(struct.pack('<BBHHH',0x80,6,0x200,0,total))
            if len(config)!=total or config[:9]!=header or config[5]==0:
                raise RuntimeError('actual configuration descriptor mismatch')
            self.result['configuration_descriptor']={'raw_hex':config.hex(),'value':config[5],'total_length':total}
            current=self.control(bytes.fromhex('8008000000000100'))
            self.result['get_configuration_hex']=current.hex()
            if len(current)!=1 or current[0] not in (0,config[5]):raise RuntimeError('current configuration not represented by actual descriptor')
            self.result.update(status='actual_identity_ready_for_single_Windows_owner',current_configuration=current[0],preflight_SET_CONFIGURATION_sent=False,preflight_BOT_sent=False)
        except BaseException as exc:
            self.result.update(status='stopped_at_unsupported_or_failed_identity',error=f'{type(exc).__name__}: {exc}')
        self.result['rpc_calls']=self.calls
        return self.result
