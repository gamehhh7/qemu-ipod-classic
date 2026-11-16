"""Actual FC1307 R3 IPC with a tiny synthetic disk and durable COW, loopback only."""
from pathlib import Path
import base64,json,os,socket,subprocess,sys,threading,time,tempfile
R=Path(__file__).parents[1];T=R/'storage-tests';T.mkdir(exist_ok=True)
def command(sock,line):
    sock.sendall((line+'\n').encode());raw=b''
    while not raw.endswith(b'\n'):
        chunk=sock.recv(65536)
        if not chunk:raise EOFError()
        raw+=chunk
    return raw.decode().strip()
checks=[]
with tempfile.TemporaryDirectory(dir=T) as folder:
    f=Path(folder);base=f/'synthetic.img';base.write_bytes(b'A'*16384);overlay=f/'disk.sqlite3'
    for round in range(2):
        listener=socket.socket();listener.bind(('127.0.0.1',0));port=listener.getsockname()[1];listener.close()
        env=dict(os.environ,CLASSIC7_OVERLAY=str(overlay),HIGH_LBA_IPC_LOG=str(f/f'ipc{round}.jsonl'))
        ready=threading.Event();errors=[]
        proc=subprocess.Popen([sys.executable,'-u','-B',str(R/'storage/backend_persistent.py'),'--tcp',f'127.0.0.1:{port}','--image',str(base),'--firmware-profile','stock15'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
        def drain():
            for line in proc.stderr:
                errors.append(line)
                if 'IPC server (text protocol)' in line:ready.set()
        thread=threading.Thread(target=drain);thread.start()
        try:
            assert ready.wait(15),''.join(errors)
            with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                if round==0:
                    answer=command(sock,'TASKFILE 202 0 2 1 0 0 0 0 0 64 1024 -')
                    assert answer.startswith('RESULT 88 '),answer
                    payload=b'B'*512+b'C'*512
                    answer=command(sock,'TASKFILE 202 0 2 1 0 0 0 0 0 64 1024 '+base64.b64encode(payload).decode())
                    assert answer.startswith('RESULT 80 '),answer;checks.append('real-R3-write-2sectors')
                answer=command(sock,'TASKFILE 200 0 2 1 0 0 0 0 0 64 0 -');parts=answer.split()
                assert parts[:2]==['RESULT','88'] and base64.b64decode(parts[8])==b'B'*512+b'C'*512,answer[:100]
                checks.append('readback-round'+str(round))
                assert command(sock,'RESET').startswith('RESULT 0 ')
                answer=command(sock,'TASKFILE 200 0 2 1 0 0 0 0 0 64 0 -')
                assert base64.b64decode(answer.split()[8])==b'B'*512+b'C'*512
                checks.append('reset-preserves-cow-round'+str(round))
                assert command(sock,'SHUTDOWN')=='BYE'
            assert proc.wait(timeout=5)==0
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            thread.join(2)
        (T/f'actual-r3-tcp-{round}.log').write_text(''.join(errors),encoding='utf8')
    assert base.read_bytes()==b'A'*16384;checks.append('base-unchanged')
(T/'R3-TCP-RESULT.json').write_text(json.dumps({'status':'PASS','checks':checks,'scope':'actual FC1307 R3 commands / synthetic media / loopback TCP; not ARM guest or USB/Windows disk'},indent=2))
print('actual R3 durable TCP PASS',len(checks))
