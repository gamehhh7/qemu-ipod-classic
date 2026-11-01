"""One actual native reset, current BQL receiver facts, no timestamp lottery.
Every phase is bounded; a failed/late pulse is never extended or retried.
No guest arm, RAM, PC, UI, IRQ, descriptor or completion synthesis.
"""
from legacy_reset import Producer as PreviousProducer,Wire,HELLO,OPEN,CLOSE,NATIVE_INFO,ASSERT,DEASSERT,IDENTITY,native_info_eligibility
from identity_walk import IdentityWalk
from host_cadence import HostCadence
import legacy_reset,json,socket,struct,time,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'frontend'))
from native_backend import NativeBackend
from usbip_server import LoopbackServer
legacy_reset.ALLOWED.add(0x37)
legacy_reset.ALLOWED.add(0x3b)

def read_state(wire):
    status,raw=wire.call(0x37)
    if status or len(raw)!=64:raise RuntimeError('atomic state envelope')
    values=struct.unpack('<8I4Q',raw)
    row=dict(zip(('version','vbus','reset','host_mode','dctl','pcgcctl','receiver_available','soft_qualified','vns','asserted_ns','released_ns','last_ns'),values))
    row['raw_hex']=raw.hex()
    if row['version']!=2:raise RuntimeError('atomic state version')
    return row

def eligible(row):
    return row['vbus']==1 and row['reset']==0 and row['host_mode']==0 and row['receiver_available']==1 and row['soft_qualified']==1 and not row['dctl']&2

class StableReceiver:
    """Require100ms continuous qualified virtual bus time before one reset.
    Ineligible receiver or backwards time breaks the interval; it never
    changes the guest or manufactures an attach indication.
    """
    def __init__(self):self.start=None;self.last=None
    def update(self,row):
        now=row['vns']
        if not eligible(row) or (self.last is not None and now<self.last):
            self.start=None;self.last=now;return False
        self.last=now
        if self.start is None:self.start=now
        return now-self.start>=100_000_000

class Producer(PreviousProducer):
    def run(self):
        sock=None;wire=None;opened=False;usbip=None
        try:
            self.result.update(status='waiting_cable',qualification='controller current-state BQL gate; snapshots are facts, OPEN is not attach')
            while not self.stop_event.is_set() and not self.cable_event.wait(.05):pass
            if self.stop_event.is_set():self.result['status']='cancelled_no_cable';return
            self.result['cable_fact']=self.cable_fact
            self.trace.refresh()
            if not any(x.startswith('native-host: listening on 127.0.0.1:') and 'mode=native' in x for x in self.trace.lines):
                self.result['status']='unsupported_no_listener_evidence';return
            sock=socket.create_connection(('127.0.0.1',self.port),timeout=.5);self.active_socket=sock
            sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            wire=Wire(sock,self.record,time.monotonic()+self.trigger_timeout+10)
            st,raw=wire.call(HELLO)
            if st or len(raw)!=32:raise RuntimeError('HELLO envelope')
            version,caps,identity=struct.unpack_from('<IIQ',raw)
            if version!=2 or identity!=IDENTITY or not caps&128 or not caps&256 or caps&(1|2|32):raise RuntimeError('mode/capability mismatch')
            st,raw=wire.call(NATIVE_INFO)
            if st:raise RuntimeError('native info refused')
            info,reasons=native_info_eligibility(raw);self.result['native_info_before_open']=info
            if reasons:raise RuntimeError('native line snapshot refused: '+str(reasons))
            until=time.monotonic()+self.trigger_timeout;polls=0
            while not self.stop_event.is_set() and time.monotonic()<until and polls<181:
                polls+=1;state=read_state(wire)
                self.result['last_live_state']=state
                if eligible(state):break
                self.stop_event.wait(.5)
            else:self.result.update(status='unsupported_current_receiver',live_polls=polls);return
            stable=StableReceiver();debounce=[];deadline=time.monotonic()+2.5
            while time.monotonic()<deadline and not self.stop_event.is_set():
                state=read_state(wire);debounce.append(state)
                if stable.update(state):break
                self.stop_event.wait(.025)
            else:
                self.result.update(status='unsupported_unstable_receiver_no_reset',debounce=debounce);return
            self.result['debounce']={'minimum_virtual_ns':100_000_000,'first_qualified_ns':stable.start,
                'last_ns':state['vns'],'samples':debounce,'basis':'USB2 attach debounce100ms; Linux hub.c HUB_DEBOUNCE_STABLE100',
                'source':'https://github.com/torvalds/linux/blob/master/drivers/usb/core/hub.c'}
            self.result.update(live_polls=polls,trigger_live_before_open=state)
            st,raw=wire.call(OPEN,struct.pack('<Q',IDENTITY))
            if st or len(raw)!=32:raise RuntimeError('OPEN failed')
            serial,identity,token,min_ns=struct.unpack('<QQQQ',raw)
            if identity!=IDENTITY or not serial or not token or min_ns!=10_000_000:raise RuntimeError('lease contract')
            opened=True;self.result['lease']={'serial':serial,'token':token,'min_ns':min_ns}
            state=read_state(wire);self.result['trigger_live_before_assert']=state
            if not eligible(state):self.result['status']='unsupported_changed_receiver';return
            if self.stop_event.is_set():self.result['status']='cancelled_before_assert';return
            wire.deadline=time.monotonic()+4
            self.result['attempts']=1;self.result['pulse_start_requests']=1;self.result['port_model']='root_port_target60ms_min50ms_USB2_7.1.7.5'
            st,raw=wire.call(0x38,struct.pack('<QQQQ',serial,token,1,60_000_000))
            if st or len(raw)!=32:raise RuntimeError('timed reset refused/uncertain; no retry')
            a=struct.unpack('<IIQQQ',raw)
            if (a[0],a[1],a[2],a[4])!=(0,1,1,60_000_000):raise RuntimeError('timed reset shape')
            self.result['assert_response_vns']=a[3]
            # Poll facts only. A controller virtual timer supplies one release.
            for _ in range(100):
                state=read_state(wire)
                if state['released_ns']:break
                if self.stop_event.wait(.002):raise RuntimeError('cancel during reset')
            else:raise RuntimeError('timed release absent; no compensating pulse')
            self.result['edge_requests']=2
            # Wall time is not guest bus time under icount. Schedule the first
            # transaction after the actual USB reset recovery interval, rather
            # than rejecting a healthy bus because wall15ms advanced guest9ms.
            # This is the same pulse, without a compensating reset or gate cut.
            released=state['released_ns'];asserted=state['asserted_ns']
            recovery_deadline=time.monotonic()+1;recovery_polls=0
            while state['vns']-released<10_000_000:
                if self.stop_event.wait(.001):raise RuntimeError('cancel during reset recovery')
                if time.monotonic()>=recovery_deadline:raise RuntimeError('virtual reset recovery clock stalled; no retry')
                state=read_state(wire);recovery_polls+=1
                if state['released_ns']!=released or state['asserted_ns']!=asserted or not eligible(state):
                    raise RuntimeError('actual bus changed during reset recovery')
            state=read_state(wire);self.result['actual_controller_after_reset']=state
            self.result['recovery_schedule']={'basis':'actual virtual USB clock','minimum_ns':10_000_000,'polls':recovery_polls,'new_reset_sent':False}
            pulse=state['released_ns']-state['asserted_ns'];recovery=state['vns']-state['released_ns']
            self.result.update(actual_pulse_ns=pulse,actual_recovery_ns=recovery)
            if not 50_000_000<=pulse<=100_000_000 or recovery<10_000_000 or state['reset']:
                raise RuntimeError('controller measured pulse/recovery outside bound')
            wire.deadline=time.monotonic()+20
            cadence=HostCadence();walk=IdentityWalk(wire,serial,token,self.stop_event,cadence=cadence);self.result['native_control']=walk.run()
            self.result['usb_token_transactions']=sum(int(c.get('setup_accepted',False))+int(c.get('packet_observed',False))+int(c.get('status_out_committed',False)) for c in self.result['native_control']['controls'])
            if self.result['native_control']['status']=='actual_identity_ready_for_single_Windows_owner':
                config=bytes.fromhex(self.result['native_control']['configuration_descriptor']['raw_hex'])
                current=self.result['native_control']['current_configuration']
                backend=NativeBackend(wire,serial,token,self.stop_event,
                    bytes.fromhex(self.result['native_control']['device_descriptor']['raw_hex']),config,self.record,
                    initial_out_sequence=0,configuration_value=current,control_cadence=cadence,
                    batch_out=bool(caps&512))
                usbip=LoopbackServer(backend,self.record,continuous=getattr(self,'desktop_continuous',False));tcp_port=usbip.start()
                listener={'host':'127.0.0.1','port':tcp_port,'busid':backend.export.busid,
                    'native_session_serial':serial,'actual_device_hex':backend.device.hex(),
                    'actual_configuration_descriptor_hex':config.hex(),'actual_current_configuration':current,
                    'out_sequence_handoff':0,'preflight_BOT_sent':False,'preflight_SET_CONFIGURATION_sent':False,
                    'one_import_only':True,'physical_device':False,'Windows_success':False}
                (self.run_dir/'USBIP-LISTENER.json').write_text(json.dumps(listener,indent=2),encoding='utf-8')
                self.result.update(USBIP_listener=listener,status='actual_identity_exported_waiting_single_Windows_owner')
                self.persist()
                callback=getattr(self,'on_export',None)
                if callback:callback(listener)
                while not self.stop_event.wait(.1):pass
            if usbip:usbip.close();usbip=None
            wire.deadline=time.monotonic()+2
            st,raw=wire.call(CLOSE)
            if st or len(raw)!=8 or struct.unpack('<Q',raw)[0]!=serial:raise RuntimeError('CLOSE shape')
            opened=False
            self.result['status']='qualified_modeled_reset_input_observed'
            self.result['acceptance_limit']='actual controller-derived reset facts/control packets only; UI/USBIP/Windows still require separate observation'
        except BaseException as exc:
            self.result.update(status='failed_no_retry',error=f'{type(exc).__name__}: {exc}')
            print('USB: 连接失败：'+str(exc),flush=True)
        finally:
            if usbip:usbip.close()
            if sock:
                try:sock.close() # abort on uncertain input; never compensate or retry
                except OSError:pass
            self.active_socket=None
            try:
                self.trace.refresh();token=self.result.get('lease',{}).get('token')
                self.result['raw_log_edges']=[x for x in self.trace.hostbus if x.get('token')==str(token) and 'seq' in x]
            except BaseException as exc:self.result['raw_log_error']=str(exc)
            self.persist()
