"""Bounded BOT host. Only guest DMA packets and actual CSWs are accepted.

Read-only SCSI commands here. A timeout/lost OUT response aborts the session;
there is no USB reset recovery loop or automatic replay of uncertain writes.
"""
import struct,time

class DeviceCommandFailed(RuntimeError):
    pass

class BulkWalk:
    def __init__(self,wire,serial,token,stopped,configuration,control=None):
        self.wire=wire;self.prefix=struct.pack('<QQ',serial,token)
        self.stopped=stopped;self.sequence=0;self.calls=0;self.control=control
        self.result={'status':'in_progress','commands':[],'windows_device':False}
        self.ep_in,self.ep_out,self.mps=self.endpoints(configuration)
    @staticmethod
    def endpoints(raw):
        found={};offset=0;interface=False
        while offset<len(raw):
            n=raw[offset]
            if n<2 or offset+n>len(raw):raise ValueError('descriptor length')
            d=raw[offset:offset+n]
            if d[1]==4:
                if n<9:raise ValueError('interface length')
                interface=d[5:8]==bytes([8,6,0x50]) and d[3]==0
            if interface and d[1]==5:
                if n<7 or d[3]&3!=2:raise ValueError('not bulk')
                address=d[2];mps=int.from_bytes(d[4:6],'little')
                if not address&15 or address&0x70 or not 1<=mps<=512:raise ValueError('endpoint unsupported')
                if bool(address&128) in found:raise ValueError('ambiguous endpoints')
                found[bool(address&128)]=(address&15,mps)
            offset+=n
        if set(found)!={False,True} or found[False][1]!=found[True][1]:raise ValueError('need one bulk IN and OUT')
        return found[True][0],found[False][0],found[True][1]
    def rpc(self,op,body):
        if self.stopped.is_set():raise RuntimeError('bulk cancelled')
        if self.calls>=10000:raise RuntimeError('bulk RPC bound')
        self.calls+=1;status,raw=self.wire.call(op,self.prefix+body)
        if status or len(raw)<16:raise RuntimeError('bulk refused '+repr((op,status,raw.hex())))
        rc,n,nonce=struct.unpack_from('<iIQ',raw)
        if n>512 or len(raw)!=(16+n if op==0x35 else 16):raise RuntimeError('bulk envelope')
        if rc not in (0,1,2):raise RuntimeError('bulk unsupported '+str(rc))
        return rc,n,nonce,raw[16:]
    def wait(self,op,body):
        # Only explicit WAIT means no transaction was accepted and permits poll.
        until=time.monotonic()+30
        for _ in range(3000):
            reply=self.rpc(op,body)
            if reply[0]!=1:return reply
            if time.monotonic()>=until:break
            if self.stopped.wait(.01):raise RuntimeError('bulk cancelled')
        raise TimeoutError('bulk endpoint did not arm within30s')
    def out(self,data):
        if len(data)>self.mps:raise ValueError('OUT packet too large')
        seq=self.sequence+1
        rc,n,nonce,_=self.wait(0x34,struct.pack('<IIQ',self.ep_out,len(data),seq)+data)
        if rc:raise RuntimeError('actual CBW OUT STALL; no replay/reset')
        if n!=len(data) or nonce!=seq:raise RuntimeError('OUT acceptance identity')
        self.sequence=seq
    def packet(self):
        rc,n,nonce,data=self.wait(0x35,struct.pack('<II',self.ep_in,0))
        if rc==2:
            if self.control is None:raise RuntimeError('actual IN STALL; no control recovery peer')
            setup=struct.pack('<BBHHH',2,1,0,0x80|self.ep_in,0)
            self.result.setdefault('stall_recovery',[]).append({'clear_feature_setup_hex':setup.hex(),'actual_stall_observed':True,'status':'started'})
            self.control(setup)
            self.result['stall_recovery'][-1]['status']='guest_status_IN_ACK_verified'
            return None
        if not nonce or n!=len(data) or n>self.mps:raise RuntimeError('IN identity')
        ack=self.rpc(0x36,struct.pack('<IIQ',self.ep_in,0,nonce))
        if ack[0] or ack[1] or ack[2]!=nonce:raise RuntimeError('IN ACK not consumed')
        # Conservative host bus sequencing: observe guest handling of this
        # actual transfer-complete IRQ before the next native token. Never
        # clear it or assume that a packet ACK means the ISR has executed.
        for drain_poll in range(32):
            status,raw=self.wire.call(0x3a,self.prefix+struct.pack('<II',self.ep_in,1))
            if status or len(raw)!=48:raise RuntimeError('endpoint snapshot refused')
            version,ep,direction,ctl,tsiz,dma,interrupt,gint,gen,vns=struct.unpack('<8I2Q',raw)
            if (version,ep,direction)!=(1,self.ep_in,1):raise RuntimeError('endpoint snapshot identity')
            if not interrupt&1:break
            if self.stopped.wait(.002):raise RuntimeError('bulk drain cancelled')
        else:raise TimeoutError('guest bulk completion IRQ not consumed')
        self.result['bulk_completion_snapshots']=self.result.get('bulk_completion_snapshots',0)+drain_poll+1
        return data
    def command(self,cdb,length,tag):
        if not 1<=len(cdb)<=16 or not 0<=length<=4096:raise ValueError('CBW bounds')
        row={'tag':tag,'cdb_hex':cdb.hex(),'expected_data':length,'status':'started'}
        self.result['commands'].append(row)
        cbw=struct.pack('<IIIBBB16s',0x43425355,tag,length,0x80 if length else 0,0,len(cdb),cdb)
        self.out(cbw);row['cbw_accepted']=True;data=bytearray();csw=None
        while len(data)<length:
            p=self.packet()
            if p is None:break
            # Unexpected CSW in data phase is a short/error transaction, never
            # data padded by the host. Stop to preserve raw evidence.
            if len(p)==13 and p[:4]==b'USBS':
                csw=p;break
            if not p or len(p)>length-len(data):raise RuntimeError('unexpected bulk data length')
            data.extend(p)
            if len(p)<self.mps:break
        row['actual_data_hex']=data.hex()
        if csw is None:csw=self.packet()
        if csw is None:raise RuntimeError('actual STALL in CSW phase')
        row['actual_csw_hex']=csw.hex()
        if len(csw)!=13:raise RuntimeError('CSW length')
        signature,returned_tag,residue,status=struct.unpack('<IIIB',csw)
        if signature!=0x53425355 or returned_tag!=tag or residue!=length-len(data) or status>2:
            raise RuntimeError('actual CSW reports mismatch/failure '+repr((signature,returned_tag,residue,status)))
        row.update(csw_residue=residue,csw_status=status)
        if status:
            row['status']='guest_command_failed' if status==1 else 'guest_phase_error'
            if status==1:raise DeviceCommandFailed('actual guest CSW command failed')
            raise RuntimeError('actual guest CSW phase error')
        if residue or len(data)!=length:raise RuntimeError('short successful data is unsupported here')
        row['status']='guest_data_and_csw_verified';return bytes(data)
    def ready(self):
        self.result['readiness']=[]
        for poll in range(6):
            row={'poll':poll+1};self.result['readiness'].append(row)
            try:
                self.command(bytes(6),0,10+poll*2)
                row['status']='actual_TUR_success';return
            except DeviceCommandFailed:
                sense=self.command(bytes([3,0,0,0,18,0]),18,11+poll*2)
                if len(sense)<14 or sense[0]&0x7f not in (0x70,0x71):raise RuntimeError('unsupported SENSE response')
                row.update(status='actual_not_ready',sense_hex=sense.hex(),key=sense[2]&15,asc=sense[12],ascq=sense[13])
                if (row['key'],row['asc'],row['ascq'])!=(2,4,1):
                    raise RuntimeError('TUR failed with non-becoming-ready SENSE')
                if poll<5 and self.stopped.wait(3):raise RuntimeError('readiness cancelled')
        raise RuntimeError('six actual TUR/SENSE polls remained becoming-ready; no capacity/read attempted')
    def run(self):
        try:
            inquiry=self.command(bytes([0x12,0,0,0,36,0]),36,1)
            self.result['inquiry_hex']=inquiry.hex()
            self.ready()
            cap=self.command(bytes([0x25])+bytes(9),8,2)
            last,block=struct.unpack('>II',cap)
            self.result['capacity']={'last_lba':last,'block_bytes':block}
            if block not in (512,1024,2048,4096) or not last:raise RuntimeError('unsupported actual capacity')
            sector=self.command(bytes([0x28,0,0,0,0,0,0,0,1,0]),block,3)
            import hashlib
            self.result['read10_lba0_sha256']=hashlib.sha256(sector).hexdigest()
            self.result['status']='actual_guest_BOT_read_verified'
        except DeviceCommandFailed as exc:
            self.result.update(status='actual_guest_reported_SCSI_failure',error=str(exc))
            try:
                sense=self.command(bytes([3,0,0,0,18,0]),18,4)
                self.result['request_sense_hex']=sense.hex()
                if len(sense)>=14:
                    self.result['sense']={'response':sense[0]&0x7f,'key':sense[2]&15,'asc':sense[12],'ascq':sense[13]}
            except BaseException as sense_error:self.result['sense_error']=f'{type(sense_error).__name__}: {sense_error}'
        except BaseException as exc:
            self.result.update(status='stopped_at_unsupported_or_failed_phase',error=f'{type(exc).__name__}: {exc}')
        self.result['rpc_calls']=self.calls;return self.result
