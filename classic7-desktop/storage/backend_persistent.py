"""Unmodified FC1307 R3/ATA contract with a durable virtual sector overlay."""
import atexit,base64,hashlib,json,os,sys,time,cProfile,pstats
from pathlib import Path
sys.dont_write_bytecode=True
FORMAL=Path('K:/CodexResources/2026-09-30/ipod-emulation/qemu-fc1307/playback-runtime/bridge/large-capacity/src')
sys.path.insert(0,str(FORMAL))
import qemu_fc1307_backend as backend
from persistent_store import PersistentStore
backend.FileBackedStore=PersistentStore
import fc1307_ipc_server as server
original=server.FC1307IpcHandler.handle_text
log=open(os.environ['HIGH_LBA_IPC_LOG'],'x',buffering=1);n=0;stores={}
profile_limit=min(32,max(0,int(os.environ.get('CLASSIC7_PROFILE_COMMANDS','0'))))
profile=cProfile.Profile() if profile_limit else None

def tracked(self,line):
    global n
    store=self.router.bridge.store;stores[id(store)]=store
    started=time.perf_counter();cpu_started=time.process_time();store.begin()
    work_started=time.perf_counter();commit_s=0
    try:
        if profile and n<profile_limit:profile.enable()
        response=original(self,line)
        work_s=time.perf_counter()-work_started
        if profile and n<profile_limit:profile.disable()
        parts=response.split()
        failed=response.startswith('ERROR') or (parts and parts[0]=='RESULT' and (int(parts[1])&1))
        if failed:store.rollback()
        else:
            commit_started=time.perf_counter();store.commit();commit_s=time.perf_counter()-commit_started
    except BaseException:
        if profile:profile.disable()
        store.rollback();raise
    n+=1
    if n==profile_limit and profile:
        stats=pstats.Stats(profile);hot=sorted(stats.stats.items(),key=lambda x:x[1][3],reverse=True)[:24]
        Path(os.environ['HIGH_LBA_IPC_LOG']).with_name('ACTUAL-BACKEND-PROFILE.json').write_text(json.dumps({'actual_first_commands':n,'not_a_fixture':True,'functions':[dict(file=k[0],line=k[1],function=k[2],primitive_calls=v[0],calls=v[1],self_s=v[2],cumulative_s=v[3]) for k,v in hot]},indent=2),encoding='utf-8')
    payload=base64.b64decode(parts[8]) if len(parts)>8 and parts[8]!='-' else b''
    if n==1:
        files={name:{'file':sys.modules[name].__file__,'sha256':hashlib.sha256(Path(sys.modules[name].__file__).read_bytes()).hexdigest()} for name in ('fc1307_ipc_server','fc1307_backend','m51core_merged','apple_controller_backend','qemu_fc1307_backend','qemu_fc1307_router','ata_controller','fc1307_adapter','persistent_store')}
        Path(os.environ['HIGH_LBA_IPC_LOG']).with_name('LOADED-MODULES.json').write_text(json.dumps(files,indent=2))
    request_words=line.split()
    if len(payload)==512 and (line.strip().upper()=='IDENTIFY' or (len(request_words)>1 and request_words[0].upper()=='TASKFILE' and request_words[1]=='236')):
        Path(os.environ['HIGH_LBA_IPC_LOG']).with_name('ACTUAL-ATA-IDENTIFY.bin').write_bytes(payload)
    log.write(json.dumps(dict(n=n,t=time.time(),wall_s=time.perf_counter()-started,work_s=work_s,commit_s=commit_s,cpu_s=time.process_time()-cpu_started,req=' '.join(line.split()[:12]),resp=' '.join(response.split()[:8]),origin='QEMU_TCP_ONLY',r3='real8051',payload_sha256=hashlib.sha256(payload).hexdigest(),decoded_payload_bytes=len(payload),persistent_cow=True))+'\n')
    return response

def finish():
    for store in stores.values():store.close()
    log.close()
atexit.register(finish)
server.FC1307IpcHandler.handle_text=tracked
server.main()
