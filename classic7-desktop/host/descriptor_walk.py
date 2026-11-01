"""Read actual guest strings and all configurations; no descriptor synthesis."""
import struct
from control_walk import ActualControlStall

class DescriptorWalk:
    def __init__(self,control,device):self.control=control;self.device=device
    def run(self):
        result={'status':'in_progress','strings':[],'configurations':[]}
        try:
            indices=sorted(set(self.device[14:17])-{0})
            if indices:
                languages=self.control(struct.pack('<BBHHH',0x80,6,0x300,0,255))
                if len(languages)<4 or languages[0]!=len(languages) or languages[1]!=3 or len(languages)%2:
                    raise RuntimeError('actual string0 length/type/languages invalid')
                result['language_descriptor_hex']=languages.hex()
                language=int.from_bytes(languages[2:4],'little')
                if not language:raise RuntimeError('actual language zero')
                for index in indices:
                    raw=self.control(struct.pack('<BBHHH',0x80,6,0x300|index,language,255))
                    if len(raw)<2 or raw[0]!=len(raw) or raw[1]!=3 or len(raw)%2:
                        raise RuntimeError('actual USB string invalid')
                    result['strings'].append({'index':index,'language':language,'raw_hex':raw.hex(),'utf16_text':raw[2:].decode('utf-16le')})
            if not 1<=self.device[17]<=8:raise RuntimeError('configuration count bound')
            for index in range(self.device[17]):
                header=self.control(struct.pack('<BBHHH',0x80,6,0x200|index,0,9))
                if len(header)!=9 or header[:2]!=bytes([9,2]):raise RuntimeError('actual configuration header')
                total=int.from_bytes(header[2:4],'little')
                if not 9<=total<=4096:raise RuntimeError('actual configuration size bound')
                raw=self.control(struct.pack('<BBHHH',0x80,6,0x200|index,0,total))
                if len(raw)!=total or raw[:9]!=header:raise RuntimeError('actual full configuration identity')
                result['configurations'].append({'index':index,'raw_hex':raw.hex(),'actual_bytes':total})
            result['status']='actual_guest_descriptors_verified'
        except ActualControlStall as e:result.update(status='actual_guest_STALL',error=str(e))
        except Exception as e:result.update(status='descriptor_observation_failed',error=f'{type(e).__name__}: {e}')
        return result
