"""Single-sector probe through actual guest MSC. Private virtual overlay only.
No direct sector/SQL writes; original block is captured from USB before writing.
"""
from pathlib import Path
import struct,hashlib,json
from bulk_walk import BulkWalk
ROOT=Path(__file__).resolve().parents[1]
digest=lambda data:hashlib.sha256(data).hexdigest()

class WriteProbe(BulkWalk):
    def __init__(self,*args,mode,**kwargs):
        super().__init__(*args,**kwargs)
        if mode not in ('write','verify_restore'):raise ValueError('probe mode')
        self.mode=mode
    @staticmethod
    def cdb(opcode,lba):return struct.pack('>BBIBHB',opcode,8 if opcode==0x2a else 0,lba,0,1,0)
    def write10(self,lba,payload,tag):
        if len(payload)!=4096 or lba!=16383:raise ValueError('isolated probe location mismatch')
        cdb=self.cdb(0x2a,lba)
        row={'tag':tag,'cdb_hex':cdb.hex(),'direction':'OUT','expected_data':len(payload),'status':'started',
             'payload_sha256':digest(payload),'accepted_data_packets':0}
        self.result['commands'].append(row)
        cbw=struct.pack('<IIIBBB16s',0x43425355,tag,len(payload),0,0,len(cdb),cdb)
        self.out(cbw);row['cbw_accepted']=True
        for at in range(0,len(payload),self.mps):
            self.out(payload[at:at+self.mps]);row['accepted_data_packets']+=1
        csw=self.packet()
        if csw is None or len(csw)!=13:raise RuntimeError('WRITE CSW missing/stalled')
        row['actual_csw_hex']=csw.hex()
        signature,returned_tag,residue,status=struct.unpack('<IIIB',csw)
        if (signature,returned_tag,residue,status)!=(0x53425355,tag,0,0):
            raise RuntimeError('actual WRITE CSW failure '+repr((signature,returned_tag,residue,status)))
        row['status']='actual_guest_WRITE10_CSW_success'
    def run(self):
        super().run()
        if self.result['status']!='actual_guest_BOT_read_verified':return self.result
        try:
            if self.result['capacity']!={'last_lba':16383,'block_bytes':4096}:
                raise RuntimeError('different medium; no probe write allowed')
            pattern=(ROOT/'probe-pattern.bin').read_bytes()
            if len(pattern)!=4096:raise RuntimeError('probe pattern length')
            current=self.command(self.cdb(0x28,16383),4096,30)
            original=ROOT/'observation/original-tail.bin'
            witness=ROOT/'observation/PROBE-ORIGINAL.json'
            if self.mode=='write':
                with original.open('xb') as f:f.write(current)
                witness.write_text(json.dumps({'lba':16383,'block_bytes':4096,'original_sha256':digest(current),
                    'pattern_sha256':digest(pattern),'origin':'actual USB READ10 before first WRITE10'},indent=2),encoding='utf-8')
                self.write10(16383,pattern,40)
                self.command(bytes([0x35])+bytes(9),0,44)
                self.result["actual_SYNCHRONIZE_CACHE10_success"]=True
                back=self.command(self.cdb(0x28,16383),4096,41)
                if back!=pattern:raise RuntimeError('actual USB write/readback mismatch')
                self.result.update(status='actual_guest_BOT_WRITE10_readback_verified',probe_lba=16383,
                    probe_sha256=digest(back),original_sha256=digest(current),probe_left_in_private_overlay=True)
            else:
                recorded=json.loads(witness.read_text(encoding='utf-8'));saved=original.read_bytes()
                if recorded.get('lba')!=16383 or recorded.get('block_bytes')!=4096 or len(saved)!=4096 or digest(saved)!=recorded['original_sha256'] or digest(pattern)!=recorded['pattern_sha256']:
                    raise RuntimeError('original USB backup identity mismatch; no restore')
                if current!=pattern:raise RuntimeError('marker not persisted after backend restart; no restore')
                self.result['actual_marker_persisted_after_restart']=True
                self.write10(16383,saved,42)
                self.command(bytes([0x35])+bytes(9),0,45)
                self.result["actual_restore_SYNCHRONIZE_CACHE10_success"]=True
                restored=self.command(self.cdb(0x28,16383),4096,43)
                if restored!=saved:raise RuntimeError('actual USB restore/readback mismatch')
                self.result.update(status='actual_guest_BOT_persistence_and_restore_verified',probe_lba=16383,
                    original_sha256=digest(restored),probe_left_in_private_overlay=False)
        except BaseException as exc:self.result.update(status='probe_failed_no_retry',error=f'{type(exc).__name__}: {exc}')
        self.result['rpc_calls']=self.calls;return self.result
