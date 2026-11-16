"""Desktop-owned cable sessions: keep the imported port until disconnect/exit."""
import json
import os
import sys
from pathlib import Path
from reset_producer import Producer
from legacy_reset import Trace
from windows_session import WindowsSession

def main():
    run=Path(sys.argv[1]);port=int(sys.argv[2]);producer=None;attachment=None;session=0
    def stop():
        nonlocal producer,attachment
        try:
            if attachment is not None:attachment.close()
        finally:
            if producer is not None:
                producer.stop()
                if producer.thread and producer.thread.is_alive():
                    producer.abort_connection();producer.thread.join(timeout=3)
                if producer.thread and producer.thread.is_alive():
                    raise RuntimeError('Previous native owner did not stop; no new session')
                producer.journal.sink.close()
            producer=None;attachment=None
    try:
        for line in sys.stdin:
            cmd=json.loads(line)
            if cmd['cable']:
                if producer is not None:raise RuntimeError('Duplicate live cable session')
                session+=1;folder=run/('usb-session-%03d'%session);folder.mkdir(exist_ok=False)
                if os.environ.get('CLASSIC7_USB_TIMING')=='1':
                    os.environ['CLASSIC7_USB_TIMING_FILE']=str(folder/'NATIVE-TIMING.jsonl')
                producer=Producer(folder,port=port,trigger_timeout=90)
                producer.trace=Trace(run/'qemu.log')
                attachment=WindowsSession(folder,producer.stop_event)
                producer.on_export=attachment.open;producer.desktop_continuous=True
                producer.start();producer.cable_connected({'readback':True,'origin':'desktop PMU usb-connected actual readback'})
                print('USB: 正在连接虚拟 iPod…',flush=True)
            else:
                stop();print('USB: 已断开',flush=True)
    finally:stop()

if __name__=='__main__':main()
