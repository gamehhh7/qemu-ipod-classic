/* Bounded native RIP sampler. No injected code or memory writes.
 * Each successful SuspendThread is paired immediately with ResumeThread;
 * no allocations, output, waits or symbol work occur while suspended.
 * This intrusive diagnostic is NOT an audio acceptance run. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc,char **argv) {
 if(argc!=4)return 2;
 DWORD pid=strtoul(argv[1],NULL,10);double seconds=atof(argv[2]);
 if(!pid||pid==GetCurrentProcessId()||seconds<=0||seconds>30)return 3;
 FILE *f=fopen(argv[3],"wb");if(!f)return 4;
 HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
 if(snap==INVALID_HANDLE_VALUE){fclose(f);return 5;}
 struct {HANDLE h;DWORD id;ULONG64 initial;} th[64];unsigned n=0;
 THREADENTRY32 e={.dwSize=sizeof(e)};
 if(Thread32First(snap,&e))do {
  if(e.th32OwnerProcessID==pid&&n<64){
   HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,e.th32ThreadID);
   ULONG64 cycles=0;
   if(h&&QueryThreadCycleTime(h,&cycles)&&cycles>100000000){th[n].h=h;th[n].id=e.th32ThreadID;th[n].initial=cycles;n++;}
   else if(h)CloseHandle(h);
  }
 }while(Thread32Next(snap,&e));CloseHandle(snap);
 LARGE_INTEGER fq,start,now,before,after;QueryPerformanceFrequency(&fq);QueryPerformanceCounter(&start);
 fprintf(f,"time_s,tid,rip,cycles_since_start,suspend_us\n");
 unsigned frames=0,fail=0;double total_hold=0,max_hold=0;
 for(;;){
  QueryPerformanceCounter(&now);double elapsed=(double)(now.QuadPart-start.QuadPart)/fq.QuadPart;if(elapsed>=seconds)break;
  for(unsigned i=0;i<n;i++){
   CONTEXT cx;ZeroMemory(&cx,sizeof(cx));cx.ContextFlags=CONTEXT_CONTROL;
   ULONG64 cycles=0;QueryThreadCycleTime(th[i].h,&cycles);
   QueryPerformanceCounter(&before);
   DWORD previous=SuspendThread(th[i].h);
   if(previous==(DWORD)-1){fail++;continue;}
   BOOL ok=GetThreadContext(th[i].h,&cx);
   DWORD resumed=ResumeThread(th[i].h);
   QueryPerformanceCounter(&after);
   if(resumed==(DWORD)-1){fprintf(stderr,"FATAL resume failed tid=%lu error=%lu\n",th[i].id,GetLastError());return 7;}
   double hold=(double)(after.QuadPart-before.QuadPart)*1e6/fq.QuadPart;total_hold+=hold;if(hold>max_hold)max_hold=hold;
   if(ok)fprintf(f,"%.6f,%lu,%llx,%llu,%.3f\n",elapsed,th[i].id,(unsigned long long)cx.Rip,(unsigned long long)(cycles-th[i].initial),hold);
   else fail++;
  }
  frames++;Sleep(7+(frames%7));
 }
 for(unsigned i=0;i<n;i++)CloseHandle(th[i].h);
 fclose(f);printf("threads=%u ticks=%u failed=%u total_hold_ms=%.3f max_hold_ms=%.3f\n",n,frames,fail,total_hold/1000,max_hold/1000);return 0;
}
