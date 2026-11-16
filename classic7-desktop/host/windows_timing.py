"""Per-worker high-resolution waits; no system timer resolution change."""
import ctypes,sys,threading
from ctypes import wintypes as w

_local=threading.local()

class _Timer:
    def __init__(self):
        self.kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        k=self.kernel
        k.CreateWaitableTimerExW.argtypes=[w.LPVOID,w.LPCWSTR,w.DWORD,w.DWORD]
        k.CreateWaitableTimerExW.restype=w.HANDLE
        k.SetWaitableTimer.argtypes=[w.HANDLE,ctypes.POINTER(ctypes.c_longlong),w.LONG,w.LPVOID,w.LPVOID,w.BOOL]
        k.SetWaitableTimer.restype=w.BOOL
        k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD];k.WaitForSingleObject.restype=w.DWORD
        k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
        self.handle=k.CreateWaitableTimerExW(None,None,2,0x1f0003)
    def wait(self,seconds):
        if not self.handle:return False
        due=ctypes.c_longlong(-max(1,int(seconds*10000000)))
        return bool(self.kernel.SetWaitableTimer(self.handle,ctypes.byref(due),0,None,None,False)) and self.kernel.WaitForSingleObject(self.handle,1000)==0
    def __del__(self):
        if getattr(self,'handle',None):self.kernel.CloseHandle(self.handle)

def precise_wait(event,seconds):
    if event.is_set():return True
    if sys.platform=='win32':
        if not hasattr(_local,'timer'):_local.timer=_Timer()
        if _local.timer.wait(seconds):return event.is_set()
    return event.wait(seconds)
