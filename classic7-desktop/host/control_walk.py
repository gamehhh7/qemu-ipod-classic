"""Bounded abstract host walk. Every returned byte and ACK is controller-backed."""
import struct,time

class ActualControlStall(RuntimeError):pass

class ControlWalk:
    def __init__(self, wire, serial, token, stopped,cadence=None):
        self.wire=wire;self.serial=serial;self.token=token;self.stopped=stopped
        self.cadence=cadence
        self.calls=0;self.result={'status':'in_progress','controls':[],
                                 'hcd_or_windows_claim':False}
    def rpc(self,op,payload):
        if self.stopped.is_set(): raise RuntimeError('control walk cancelled')
        if self.calls>=2000: raise RuntimeError('2000 control RPC bound')
        self.calls+=1
        status,data=self.wire.call(op,payload)
        if len(data)<36: raise RuntimeError('native control refused '+repr((op,status,data.hex())))
        result,n,cid,epoch,nonce,done=struct.unpack_from('<iIQQQI',data)
        if n>64 or len(data)!=36+n: raise RuntimeError('native response size mismatch')
        if status:
            if result==-5 and op==0x31 and len(payload)>=32 and (cid,epoch)==struct.unpack_from('<QQ',payload,16):
                raise ActualControlStall('actual guest EP0 STALL; no ACK or made-up data')
            raise RuntimeError('native control refused '+repr((op,status,result)))
        return {'result':result,'length':n,'id':cid,'epoch':epoch,'nonce':nonce,
                'done':bool(done),'data':data[36:]}
    def wait(self,fn,accepted,transient,limit):
        for _ in range(limit):
            reply=fn()
            if accepted(reply): return reply
            if not transient(reply): raise RuntimeError('non-transient native result '+str(reply['result']))
            if self.stopped.wait(0.05): raise RuntimeError('control cancelled')
        raise TimeoutError('native control phase bound')
    def control(self,setup):
        row={'setup_hex':setup.hex(),'status':'started'};self.result['controls'].append(row)
        if self.cadence:row['host_service_interval']=self.cadence.before(self.wire,self.stopped)
        prefix=struct.pack('<QQ',self.serial,self.token)
        answer=self.wait(lambda:self.rpc(0x30,prefix+setup),lambda a:a['result']==0,
                         lambda a:a['result'] in (105,106,108,110),16)
        cid,epoch=answer['id'],answer['epoch']
        if not cid or not epoch: raise RuntimeError('missing accepted SETUP identity')
        row.update(id=cid,epoch=epoch,setup_accepted=True)
        req=prefix+struct.pack('<QQ',cid,epoch)
        requested=setup[6]|(setup[7]<<8)
        if requested>4096 or (requested and not setup[0]&128):raise RuntimeError('unsupported control data direction/size')
        gathered=bytearray();nonces=set();row['packets']=[]
        for _ in range(65):
            try:
                packet=self.wait(lambda:self.rpc(0x31,req),lambda a:a['result']==0,
                                 lambda a:a['result']==1,24)
            except ActualControlStall:
                row['status']='actual_guest_STALL';raise
            if packet['id']!=cid or packet['epoch']!=epoch or not packet['nonce'] or packet['nonce'] in nonces:
                raise RuntimeError('packet provenance/replay mismatch')
            if len(packet['data'])>requested-len(gathered) or (not requested and packet['data']):
                raise RuntimeError('data/status packet length mismatch')
            part={'nonce':packet['nonce'],'actual_data_hex':packet['data'].hex(),'length':len(packet['data'])}
            row['packets'].append(part);nonces.add(packet['nonce'])
            ack=self.rpc(0x32,req+struct.pack('<Q',packet['nonce']))
            if ack['result'] or ack['id']!=cid or ack['epoch']!=epoch:
                raise RuntimeError('ACK not committed to accepted packet')
            part['actual_ACK_committed']=True;gathered.extend(packet['data'])
            if len(gathered)==requested or len(packet['data'])<64:break
        else:raise RuntimeError('control packet bound')
        row.update(actual_data_hex=gathered.hex(),arm_nonce=packet['nonce'],actual_length=len(gathered),
                   packet_observed=True,packet_ack_committed=True)
        if requested:
            if ack['done']: raise RuntimeError('read completed before status OUT')
            status=self.wait(lambda:self.rpc(0x33,req),lambda a:a['result']==0,
                             lambda a:a['result']==1,24)
            if not status['done'] or status['id']!=cid or status['epoch']!=epoch:
                raise RuntimeError('status OUT identity/completion mismatch')
            row['status_out_committed']=True
        elif not ack['done']:
            raise RuntimeError('status IN ACK did not complete no-data control')
        # One conservative host transaction boundary. Do not supersede a
        # completed EP0 transaction while its actual completion IRQ is pending.
        for drain_poll in range(32):
            status,raw=self.wire.call(0x12)
            if status or len(raw)!=64:raise RuntimeError('EP0 completion snapshot refused')
            out_int,in_int=struct.unpack_from('<II',raw,24)
            if not ((out_int&9) or (in_int&1)):break
            if self.stopped.wait(.005):raise RuntimeError('completion drain cancelled')
        else:raise TimeoutError('firmware did not consume actual EP0 completion IRQ')
        row['completion_irq_drained']=True;row['completion_drain_polls']=drain_poll+1
        if self.cadence:row['actual_end_vns']=self.cadence.after(self.wire)
        row['status']='controller_control_complete' 
        return bytes(gathered)
    def run(self):
        try:
            device=self.control(bytes.fromhex('8006000100001200'))
            if len(device)!=18 or device[:2]!=bytes([18,1]) or device[7]!=64:
                raise RuntimeError('device descriptor unsupported/invalid')
            self.result['device_descriptor']={'raw_hex':device.hex(),
                'vid':int.from_bytes(device[8:10],'little'),
                'pid':int.from_bytes(device[10:12],'little'),'ep0_max_packet':device[7]}
            self.control(bytes.fromhex('0005010000000000'))
            # Firmware, never the host, writes DCFG. Observe its actual effect.
            address=None
            for _ in range(12):
                status,raw=self.wire.call(0x12)
                if status or len(raw)!=64: raise RuntimeError('EP0 query refused')
                address=(struct.unpack_from('<I',raw,36)[0]>>4)&127
                if address==1:break
                self.stopped.wait(0.05)
            self.result['guest_dcfg_address']=address
            if address!=1: raise RuntimeError('guest did not select USB address1')
            header=self.control(bytes.fromhex('8006000200000900'))
            if len(header)!=9 or header[:2]!=bytes([9,2]):raise RuntimeError('config header invalid')
            total=int.from_bytes(header[2:4],'little')
            if not 9<=total<=64:raise RuntimeError('configuration requires unsupported multi-packet data')
            config=self.control(struct.pack('<BBHHH',0x80,6,0x200,0,total))
            if len(config)!=total or config[:4]!=header[:4] or config[5]==0:
                raise RuntimeError('full config descriptor invalid')
            self.result['configuration_descriptor']={'raw_hex':config.hex(),'value':config[5],
                                                       'total_length':total}
            self.control(struct.pack('<BBHHH',0,9,config[5],0,0))
            configured=self.control(bytes.fromhex('8008000000000100'))
            self.result['get_configuration_hex']=configured.hex()
            if configured!=bytes([config[5]]):raise RuntimeError('guest GET_CONFIGURATION disagrees')
            self.result['status']='controller_configuration_verified'
        except BaseException as exc:
            self.result.update(status='stopped_at_unsupported_or_failed_phase',
                               error=f'{type(exc).__name__}: {exc}')
        self.result['rpc_calls']=self.calls
        return self.result
