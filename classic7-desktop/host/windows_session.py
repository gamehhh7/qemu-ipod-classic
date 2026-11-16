"""Official USBip CLI; own only the exact port returned by our import."""
import datetime,json,re,subprocess,threading
from pathlib import Path

class WindowsSession:
    def __init__(self,folder,stop):
        self.folder=Path(folder);self.stop=stop;self.lock=threading.Lock();self.port=None;self.closed=False
        self.cli=Path('C:/Program Files/USBip/usbip.exe')
    def call(self,args,name,timeout):
        argv=[str(self.cli),*args]
        proc=subprocess.run(argv,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=timeout,creationflags=subprocess.CREATE_NO_WINDOW)
        row=dict(argv=argv,exit=proc.returncode,stdout=proc.stdout,stderr=proc.stderr,utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        (self.folder/name).write_text(json.dumps(row,ensure_ascii=False,indent=2),encoding='utf-8');return row
    def open(self,listener):
        with self.lock:
            if self.closed or self.stop.is_set():return
            if listener['host']!='127.0.0.1' or listener['busid']!='1-1' or not listener['one_import_only']:raise RuntimeError('Refusing non-owned USB export')
            if not self.cli.exists():raise RuntimeError('USBip driver tools are not installed')
            result=self.call(['--tcp-port',str(listener['port']),'attach','--remote','127.0.0.1','--bus-id','1-1','--once','--terse'],'WINDOWS-ATTACH.json',20)
            if result['exit'] or not re.fullmatch(r'\d+',result['stdout'].strip()):raise RuntimeError('Windows import failed; no automatic retry: '+result['stderr'])
            self.port=int(result['stdout'].strip())
            if not 1<=self.port<=255:self.port=None;raise RuntimeError('Unexpected imported Windows port')
            print('USB: 已接入 Windows（等待系统识别磁盘）',flush=True)
    def close(self):
        with self.lock:
            self.closed=True
            if self.port is not None:
                port=self.port;self.port=None
                self.call(['detach','--port',str(port)],'WINDOWS-DETACH.json',5)
