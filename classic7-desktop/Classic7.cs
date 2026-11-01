using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

sealed class Qmp : IDisposable {
    TcpClient client; StreamReader reader; StreamWriter writer; object gate=new object();long requestSequence;
    JavaScriptSerializer json=new JavaScriptSerializer();
    public Qmp(int port) {
        client=new TcpClient();client.Connect("127.0.0.1",port);client.ReceiveTimeout=5000;
        reader=new StreamReader(client.GetStream(),Encoding.UTF8);
        writer=new StreamWriter(client.GetStream(),new UTF8Encoding(false));writer.AutoFlush=true;
        reader.ReadLine();Call("qmp_capabilities",null);
    }
    public object Call(string command,object args) { lock(gate) {
        long requestId=++requestSequence;
        var req=new Dictionary<string,object>();req["execute"]=command;req["id"]=requestId;if(args!=null)req["arguments"]=args;
        writer.WriteLine(json.Serialize(req));
        for(int n=0;n<200;n++) {
            string line=reader.ReadLine();if(line==null)throw new IOException("模拟器控制连接已关闭");
            var value=json.Deserialize<Dictionary<string,object>>(line);
            // A timed-out request can leave its response in the stream. Match
            // IDs before interpreting either success or error; events have no ID.
            if(!value.ContainsKey("id")||Convert.ToString(value["id"])!=requestId.ToString())continue;
            if(value.ContainsKey("error"))throw new IOException(json.Serialize(value["error"]));
            if(value.ContainsKey("return"))return value["return"];
        } throw new IOException("控制响应超时");
    }}
    public string Find(string type,string path,int depth) {
        if(depth<=0)return null;
        object value=Call("qom-list",new {path=path});
        var children=value as System.Collections.IEnumerable;if(children==null)return null;
        foreach(var obj in children) {
            var row=obj as Dictionary<string,object>;if(row==null)continue;
            string t=Convert.ToString(row["type"]),name=Convert.ToString(row["name"]);
            if(t=="child<"+type+">")return path+"/"+name;
        }
        foreach(var obj in children) {
            var row=obj as Dictionary<string,object>;if(row==null)continue;
            if(Convert.ToString(row["type"]).StartsWith("child<")) {
                string found=Find(type,path+"/"+row["name"],depth-1);if(found!=null)return found;
            }
        } return null;
    }
    public object Get(string path,string property) {return Call("qom-get",new {path=path,property=property});}
    public void Set(string path,string property,object value) {Call("qom-set",new {path=path,property=property,value=value});}
    public void Dispose(){try{client.Close();}catch{}}
}

sealed class Engine : IDisposable {
    public string RunDir, WheelPath, PmuPath, Status="正在启动模拟器…";
    public bool Ready,Hold,Usb; public Bitmap Frame; public long FrameSequence,ReceivedUpdates,ReceivedPixels; public event Action Changed;
    public readonly object ImageGate=new object(); object controlGate=new object();
    public bool NativeUsb,PersistentDisk,ConnectOnStart;public Process Backend,Guest,Host;StreamWriter hostInput;int nativePort; Qmp qmp; TcpClient vnc; Thread displayThread;
    volatile bool closed; StreamWriter events; StreamWriter backendLog,guestLog;
    readonly bool diagnostics=Environment.GetEnvironmentVariable("CLASSIC7_DIAGNOSTICS")=="1";
    readonly bool deterministic=Environment.GetEnvironmentVariable("CLASSIC7_DETERMINISTIC")!="0";
    string root; int qmpPort,ipcPort,vncPort; JavaScriptSerializer json=new JavaScriptSerializer();
    const string Python=@"C:\Users\hhh\AppData\Local\Programs\Python\Python311\python.exe";
    const string Runtime=@"K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\playback-runtime";
    const string Payload=@"K:\CodexResources\2026-10-08\ipod-game-recovery\candidate\revb204-osos.recovered.payload.bin";
    const string Seed=@"K:\CodexResources\2026-10-01\zcode-delegation\quota-fallback\acceptance\media\E-revb35-two-songs.img";
    const string Dlls=@"K:\CodexResources\2026-09-30\ipod-emulation\qemu\msys2\msys64\mingw64\bin";
    public Engine(string appRoot) {root=appRoot;}
    void Notify(){var h=Changed;if(h!=null)h();}
    public void Log(string kind,object data){lock(this){if(events!=null){events.WriteLine(json.Serialize(new {utc=DateTime.UtcNow.ToString("o"),kind=kind,data=data}));events.Flush();}}}
    static string Quote(string s){return "\""+s.Replace("\"","\\\"")+"\"";}
    static int FreePort(){var l=new TcpListener(IPAddress.Loopback,0);l.Start();int p=((IPEndPoint)l.LocalEndpoint).Port;l.Stop();return p;}
    public void Start() {try {
        RunDir=Path.Combine(root,"runs",DateTime.UtcNow.ToString("yyyyMMddTHHmmssZ")+"-"+Process.GetCurrentProcess().Id);
        Directory.CreateDirectory(RunDir);Directory.CreateDirectory(Path.Combine(RunDir,"tmp"));
        events=new StreamWriter(Path.Combine(RunDir,"events.jsonl"),false,new UTF8Encoding(false));
        string exe=Path.Combine(root,"engine","qemu-system-arm-classic7.exe");
        foreach(string p in new[]{Python,exe,Payload,Seed})if(!File.Exists(p))throw new FileNotFoundException("缺少模拟器运行文件",p);
        qmpPort=FreePort();ipcPort=FreePort();while(ipcPort==qmpPort)ipcPort=FreePort();
        for(int port=5901;port<6000;port++){if(port==qmpPort||port==ipcPort)continue;try{var l=new TcpListener(IPAddress.Loopback,port);l.Start();l.Stop();vncPort=port;break;}catch(SocketException){}}
        nativePort=FreePort();while(nativePort==qmpPort||nativePort==ipcPort||nativePort==vncPort)nativePort=FreePort();
        if(vncPort==0)throw new IOException("没有可用的本机显示端口");
        backendLog=new StreamWriter(Path.Combine(RunDir,"backend.log"),false,Encoding.UTF8);backendLog.AutoFlush=true;
        var backendInfo=Info(Python,"-u -B "+Quote(PersistentDisk?Path.Combine(root,"storage","backend_persistent.py"):Path.Combine(Runtime,"tools","guest_ipc_takeover.py"))+" --tcp 127.0.0.1:"+ipcPort+" --image "+Quote(Seed)+" --firmware-profile stock15");
        backendInfo.EnvironmentVariables["HIGH_LBA_IPC_LOG"]=Path.Combine(RunDir,"guest-ipc.jsonl");
        if(PersistentDisk){string config=Path.Combine(root,"userdata","windows-disk.json");string overlay=Environment.GetEnvironmentVariable("CLASSIC7_OVERLAY");if(File.Exists(config)){var media=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(config));backendInfo.EnvironmentVariables["CLASSIC7_VIRTUAL_DISK_JSON"]=config;if(overlay==null)overlay=Path.Combine(root,"userdata",Convert.ToString(media["overlay_file"]));}backendInfo.EnvironmentVariables["CLASSIC7_OVERLAY"]=overlay??Path.Combine(root,"userdata","disk.sqlite3");}
        bool listening=false;
        Backend=new Process();Backend.StartInfo=backendInfo;
        Backend.ErrorDataReceived+=(s,e)=>{if(e.Data!=null){WriteBounded(backendLog,e.Data);if(e.Data.Contains("IPC server (text protocol)"))listening=true;}};
        Backend.OutputDataReceived+=(s,e)=>{if(e.Data!=null)WriteBounded(backendLog,e.Data);};
        Backend.Start();Backend.BeginErrorReadLine();Backend.BeginOutputReadLine();
        for(int i=0;i<200&&!listening;i++){if(closed)return;if(Backend.HasExited)throw new IOException("存储后端启动失败");Thread.Sleep(50);}
        if(!listening)throw new IOException("存储后端未就绪");
        string dataDir=@"K:\CodexResources\2026-09-30\ipod-emulation\qemu-fc1307\src\qemu-ipod-classic-1c33be87c7e97b56053142ae9d7f0686feaed504\pc-bios";
        string machine="iPod-Classic";string identityPath=Path.Combine(root,"userdata","device-identity.json");if(File.Exists(identityPath)){var identity=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(identityPath));string serial=Convert.ToString(identity["serial"]),model=Convert.ToString(identity["model"]);if(!System.Text.RegularExpressions.Regex.IsMatch(serial,"^[A-Z0-9]{11,12}$")||!System.Text.RegularExpressions.Regex.IsMatch(model,"^[A-Z0-9]{5,15}$"))throw new IOException("虚拟设备身份格式无效");machine+=",serial-number="+serial+",model-number="+model;if(identity.ContainsKey("firewire_guid")){string guid=Convert.ToString(identity["firewire_guid"]);if(!System.Text.RegularExpressions.Regex.IsMatch(guid,"^[0-9A-F]{16}$"))throw new IOException("虚拟GUID格式无效");machine+=",firewire-guid="+guid;}}
        // Peripheral deadlines must track emulated CPU progress. With host-time
        // clocking, I2C can finish the next byte before the previous ISR tail
        // clears/resumes it, replaying IICDS and corrupting PMU measurements.
        // Keep the unpaced clock available only as an explicit diagnostic mode.
        string timing=deterministic?" -icount shift=3,align=off,sleep=on":"";
        string args="-L "+Quote(dataDir)+" -M "+machine+" -kernel "+Quote(Payload)+" -accel tcg,thread=single"+timing+" -display none -serial null -monitor none -audio dsound,id=classic7,out.frequency=44100,out.channels=2,out.format=s16 -qmp tcp:127.0.0.1:"+qmpPort+",server=on,wait=off -vnc 127.0.0.1:"+(vncPort-5900)+" -global pcf50635.usb-connected=off -global s5l8702-usb-device-core.experimental-link-v0=on";
        string debugPortText=Environment.GetEnvironmentVariable("CLASSIC7_GDB_PORT");int debugPort;
        if(!String.IsNullOrEmpty(debugPortText)){if(!Int32.TryParse(debugPortText,out debugPort)||debugPort<1024||debugPort>65535)throw new IOException("Invalid local debugger port");args+=" -gdb tcp:127.0.0.1:"+debugPort;}
        Log("execution_mode",new {tcg="normal dynamic translation",clock=deterministic?"instruction-count":"standard virtual clock",diagnostics=diagnostics});
        var qi=Info(exe,args);qi.EnvironmentVariables["FC1307_BACKEND"]="1";qi.EnvironmentVariables["FC1307_IPC_ADDR"]="127.0.0.1:"+ipcPort;
        if(diagnostics)qi.EnvironmentVariables["FC1307TRACE"]="1";qi.EnvironmentVariables["IPOD_USB_CORE"]="1";qi.EnvironmentVariables["IPOD_PMU_EXTON2"]="1";
        if(diagnostics){qi.EnvironmentVariables["PMUTRACE"]="1";qi.EnvironmentVariables["WHEELTRACE"]="1";qi.EnvironmentVariables["IPOD_USB_EP0_CAUSE"]="1";qi.EnvironmentVariables["IPOD_USB_RX_JOURNAL"]="1";qi.EnvironmentVariables["IPOD_I2C_JOURNAL"]="1";}
        qi.EnvironmentVariables["IPOD_FULL_TB_OBSERVER"]="0";
        if(NativeUsb){qi.EnvironmentVariables["IPOD_USB_DESKTOP_SERVICE"]="1";qi.EnvironmentVariables["IPOD_USB_NATIVE_HOST"]="1";qi.EnvironmentVariables["IPOD_USB_NATIVE_PORT"]=nativePort.ToString();qi.EnvironmentVariables["IPOD_USB_NATIVE_LIFETIME_MS"]="350000";if(diagnostics){qi.EnvironmentVariables["IPOD_USB_MMIO_OBSERVER"]="1";qi.EnvironmentVariables["USBTRACE"]="1";qi.EnvironmentVariables["PMUUSBTRACE"]="1";}}
        guestLog=new StreamWriter(Path.Combine(RunDir,"qemu.log"),false,Encoding.UTF8);guestLog.AutoFlush=true;
        Guest=new Process();Guest.StartInfo=qi;
        Guest.ErrorDataReceived+=(s,e)=>{if(e.Data!=null)WriteBounded(guestLog,e.Data);};
        Guest.OutputDataReceived+=(s,e)=>{if(e.Data!=null)WriteBounded(guestLog,e.Data);};
        Guest.Start();Guest.BeginErrorReadLine();Guest.BeginOutputReadLine();
        Log("processes",new {backend=Backend.Id,qemu=Guest.Id,exe=exe,argv=args,seed_policy="existing seed rb plus isolated SQLite WAL FULL sector overlay; no full image copy/hash",qmp_port=qmpPort,vnc_port=vncPort});
        for(int i=0;i<60;i++){if(closed)return;if(Guest.HasExited)throw new IOException("模拟器启动失败，请查看运行日志");try{qmp=new Qmp(qmpPort);break;}catch{Thread.Sleep(100);}}
        if(qmp==null)throw new IOException("模拟器控制端口未就绪");
        WheelPath=qmp.Find("s5l8702.wheel","/machine",8);PmuPath=qmp.Find("pcf50635","/machine",8);
        Log("qom_paths",new {wheel=WheelPath,pmu=PmuPath});
        if(WheelPath==null||PmuPath==null)throw new IOException("找不到模拟器的滚轮或电源芯片");
        Hold=Convert.ToBoolean(qmp.Get(WheelPath,"physical-hold"));Usb=Convert.ToBoolean(qmp.Get(PmuPath,"usb-connected"));
        Ready=true;Status="已启动";Log("ready",new {wheel=WheelPath,pmu=PmuPath,hold=Hold,usb=Usb});Notify();
        displayThread=new Thread(ReadDisplay);displayThread.IsBackground=true;displayThread.Start();if(ConnectOnStart)SetUsb(true);
    }catch(Exception ex){Status="启动失败："+ex.Message;Log("error",Status);Notify();Dispose();}}
    ProcessStartInfo Info(string exe,string args) {
        var i=new ProcessStartInfo(exe,args);i.UseShellExecute=false;i.CreateNoWindow=true;i.RedirectStandardError=true;i.RedirectStandardOutput=true;i.WorkingDirectory=Runtime;
        var remove=new List<string>();foreach(string key in i.EnvironmentVariables.Keys)if(key.StartsWith("HLE_",StringComparison.OrdinalIgnoreCase)||key.StartsWith("SM1",StringComparison.OrdinalIgnoreCase)||key.StartsWith("IPOD_USB_",StringComparison.OrdinalIgnoreCase)||key.StartsWith("IPOD_PCDUMP",StringComparison.OrdinalIgnoreCase)||key=="SHA1_ZERO"||key=="SHA1_OFF"||key=="PMUCHARGER")remove.Add(key);
        foreach(string key in remove)i.EnvironmentVariables.Remove(key);
        foreach(string key in new[]{"USBTRACE","PMUUSBTRACE","PMUTRACE","WHEELTRACE","FC1307TRACE"})i.EnvironmentVariables.Remove(key);
        i.EnvironmentVariables["PATH"]=Dlls+";"+i.EnvironmentVariables["PATH"];i.EnvironmentVariables["PYTHONIOENCODING"]="utf-8";
        i.EnvironmentVariables["TMP"]=i.EnvironmentVariables["TEMP"]=Path.Combine(RunDir,"tmp");
        i.EnvironmentVariables["DISP_ALPHA"]="1";i.EnvironmentVariables["SM1ENGINE"]="1";i.EnvironmentVariables["AUDIOPACE"]="1";i.EnvironmentVariables["AUDIOLIVE"]="1";if(diagnostics)i.EnvironmentVariables["SM1FLOWTRACE"]="1";
        return i;
    }
    static void WriteBounded(StreamWriter writer,string line){lock(writer){if(writer.BaseStream.Position+Encoding.UTF8.GetByteCount(line)+2<=16777216)writer.WriteLine(line);}}
    public void Buttons(int bits){lock(controlGate){if(!Ready)return;qmp.Set(WheelPath,"physical-buttons",bits.ToString());Log("buttons",bits);}}
    public void Touch(int position){lock(controlGate){if(!Ready)return;qmp.Set(WheelPath,"touch-position",position.ToString());}}
    public void SetHold(bool value){lock(controlGate){if(!Ready)return;if(value){qmp.Set(WheelPath,"physical-buttons","0");qmp.Set(WheelPath,"touch-position","-1");}qmp.Set(WheelPath,"physical-hold",value);Hold=Convert.ToBoolean(qmp.Get(WheelPath,"physical-hold"));if(Hold!=value)throw new IOException("Hold 状态回读不一致");Log("hold_readback",Hold);Notify();}}
    public void SetUsb(bool value){lock(controlGate){if(!Ready)return;
        if(NativeUsb&&value&&Host==null){var hi=Info(Python,"-u -B "+Quote(Path.Combine(root,"host","desktop_host.py"))+" "+Quote(RunDir)+" "+nativePort);hi.RedirectStandardInput=true;Host=new Process();Host.StartInfo=hi;Host.OutputDataReceived+=(a,b)=>{if(b.Data!=null){Log("host_stdout",b.Data);if(b.Data.StartsWith("USB: ")){Status=b.Data.Substring(5);Notify();}}};Host.ErrorDataReceived+=(a,b)=>{if(b.Data!=null)Log("host_error",b.Data);};Host.Start();Host.BeginOutputReadLine();Host.BeginErrorReadLine();hostInput=Host.StandardInput;hostInput.AutoFlush=true;Log("host_started",new{pid=Host.Id,port=nativePort});}
        if(NativeUsb&&!value&&hostInput!=null)hostInput.WriteLine("{\"cable\":false}");
        qmp.Set(PmuPath,"usb-connected",value);Usb=Convert.ToBoolean(qmp.Get(PmuPath,"usb-connected"));if(Usb!=value)throw new IOException("USB 状态回读不一致");Log("usb_readback",Usb);if(NativeUsb&&value)hostInput.WriteLine("{\"cable\":true}");Notify();}}
    public void Poll(){lock(controlGate){if(!Ready)return;Hold=Convert.ToBoolean(qmp.Get(WheelPath,"physical-hold"));Usb=Convert.ToBoolean(qmp.Get(PmuPath,"usb-connected"));}}
    uint[] ReadWords(uint address,int count){if(count<1||count>8||(address&3)!=0||address<0x08000000||address>0x10000000-count*4)throw new IOException("bounded RAM read");string raw=Convert.ToString(qmp.Call("human-monitor-command",new Dictionary<string,object>{{"command-line","xp /"+count+"wx 0x"+address.ToString("x")}}));var words=new List<uint>();foreach(string line in raw.Split('\n')){int colon=line.IndexOf(':');if(colon>=0)foreach(System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(line.Substring(colon+1),"0x([0-9a-fA-F]{8})\\b"))words.Add(Convert.ToUInt32(m.Groups[1].Value,16));}if(words.Count!=count)throw new IOException("RAM read response shape: "+raw);return words.ToArray();}
    public void SampleState(int label){lock(controlGate){if(!Ready)return;var task=ReadWords(0x08a6e76c,8);var getter=ReadWords(0x0814e230,3);if(getter[0]!=0xe59f0018||getter[1]!=0xe92d4010||getter[2]!=0xe5900004)throw new IOException("UI getter runtime code mismatch");uint globalBase=ReadWords(0x0814e250,1)[0];var global=ReadWords(globalBase,2);uint pointer=global[1];var objectHead=(pointer>=0x08000000&&pointer<0x0ffffff0&&(pointer&3)==0)?ReadWords(pointer,4):null;uint[] ui=null,usb=null;if(pointer>=0x08000000&&pointer<0x0fffff40&&(pointer&3)==0)ui=ReadWords(pointer+0xb0,4);if(task[0]==0x089865f8&&task[6]>=0x08000000&&task[6]<0x0fffff30&&(task[6]&3)==0)usb=ReadWords(task[6]+0xb0,8);Log("readonly_firmware_state",new{at=label,task_words=task,task_vtable_match=task[0]==0x089865f8,task_state=(task[1]>>16)&255,ui_getter=getter,ui_global_base=globalBase,ui_global=global,ui_object_head=objectHead,ui_b0_words=ui,usb_b0_words=usb});}}
    static byte[] Read(Stream s,int n){byte[] b=new byte[n];int p=0;while(p<n){int got=s.Read(b,p,n-p);if(got==0)throw new EndOfStreamException();p+=got;}return b;}
    static int U16(byte[] b,int p){return b[p]*256+b[p+1];}
    static int I32(byte[] b,int p){return (b[p]<<24)|(b[p+1]<<16)|(b[p+2]<<8)|b[p+3];}
    static void Send(Stream s,byte[] b){s.Write(b,0,b.Length);s.Flush();}
    static byte[] UpdateRequest(int w,int h,bool incremental){return new byte[]{3,(byte)(incremental?1:0),0,0,0,0,(byte)(w>>8),(byte)w,(byte)(h>>8),(byte)h};}
    void ReadDisplay(){try{
        vnc=new TcpClient();vnc.Connect("127.0.0.1",vncPort);vnc.NoDelay=true;var ns=vnc.GetStream();
        string version=Encoding.ASCII.GetString(Read(ns,12));if(!version.StartsWith("RFB"))throw new IOException("未知的显示协议");Send(ns,Encoding.ASCII.GetBytes("RFB 003.008\n"));
        int count=Read(ns,1)[0];byte[] types=Read(ns,count);if(Array.IndexOf(types,(byte)1)<0)throw new IOException("本机显示认证类型不支持");Send(ns,new byte[]{1});if(I32(Read(ns,4),0)!=0)throw new IOException("本机显示连接失败");
        Send(ns,new byte[]{1});byte[] init=Read(ns,24);int width=U16(init,0),height=U16(init,2),nameLength=I32(init,20);if(nameLength<0||nameLength>65536)throw new IOException("显示头无效");Read(ns,nameLength);
        Send(ns,new byte[]{0,0,0,0,32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0});
        Send(ns,new byte[]{2,0,0,1,0,0,0,0});
        if(width!=320||height!=240)throw new IOException("LCD must be320x240");Bitmap working=new Bitmap(width,height,PixelFormat.Format32bppRgb);Send(ns,UpdateRequest(width,height,false));
        while(!closed){int kind=Read(ns,1)[0];if(kind==0){byte[] header=Read(ns,3);int rects=U16(header,1);if(rects>1024)throw new IOException("too many rectangles");
            for(int j=0;j<rects;j++){byte[] rect=Read(ns,12);int x=U16(rect,0),y=U16(rect,2),w=U16(rect,4),h=U16(rect,6),encoding=I32(rect,8);
                if(encoding!=0||w<1||h<1||x+w>width||y+h>height)throw new IOException("显示区域格式无效");
                byte[] pixels=Read(ns,checked(w*h*4));Interlocked.Add(ref ReceivedPixels,(long)w*h);var region=new Rectangle(x,y,w,h);BitmapData bd=working.LockBits(region,ImageLockMode.WriteOnly,PixelFormat.Format32bppRgb);
                for(int row=0;row<h;row++)Marshal.Copy(pixels,row*w*4,IntPtr.Add(bd.Scan0,row*bd.Stride),w*4);working.UnlockBits(bd);
            }
            if(rects>0)Publish(working);Send(ns,UpdateRequest(width,height,true));
        }else if(kind==2){}else if(kind==3){byte[] cut=Read(ns,7);int n=I32(cut,3);if(n<0||n>1048576)throw new IOException("剪贴板数据超限");Read(ns,n);}else throw new IOException("未知的显示消息");}
        working.Dispose();
    }catch(Exception ex){if(!closed){Status="显示连接中断："+ex.Message;Log("display_error",Status);Notify();}}}
    public void Publish(Bitmap bitmap){var copy=(Bitmap)bitmap.Clone();lock(ImageGate){var old=Frame;Frame=copy;Interlocked.Increment(ref FrameSequence);if(old!=null)old.Dispose();}Interlocked.Increment(ref ReceivedUpdates);Notify();}
    public Bitmap CopyFrame(){if(!Monitor.TryEnter(ImageGate))return null;try{return Frame==null?null:(Bitmap)Frame.Clone();}finally{Monitor.Exit(ImageGate);}}
    public void BeginDisplayTest(string folder){Status="SYNTHETIC DISPLAY TEST";RunDir=Path.Combine(root,folder);Directory.CreateDirectory(RunDir);events=new StreamWriter(Path.Combine(RunDir,"events.jsonl"),false,new UTF8Encoding(false));}
    public void StartRfbTest(int port){BeginDisplayTest("rfb-display-test");Status="SYNTHETIC RFB TEST";vncPort=port;displayThread=new Thread(ReadDisplay);displayThread.IsBackground=true;displayThread.Start();}
    public void SaveFrame(string path){lock(ImageGate){if(Frame!=null)Frame.Save(path,ImageFormat.Png);}}
    public void Dispose(){if(closed)return;try{if(hostInput!=null)hostInput.Close();}catch{}closed=true;Ready=false;try{if(Host!=null&&!Host.HasExited)Host.WaitForExit(7000);}catch{}try{if(qmp!=null)qmp.Call("quit",null);}catch{}try{if(vnc!=null)vnc.Close();}catch{}
        foreach(var p in new[]{Host,Guest,Backend})if(p!=null){try{if(!p.WaitForExit(1200))p.Kill();}catch{}}
        Log("owned_processes_stopped",new {qemu=Guest==null?0:Guest.Id,backend=Backend==null?0:Backend.Id});
        try{if(qmp!=null)qmp.Dispose();}catch{} }
}

sealed class ClassicForm : Form {
    public bool ConnectOnStart {set{engine.ConnectOnStart=value;}}
    Engine engine; string root; bool closing; RectangleF body,lcd,wheel,center,holdSwitch,usbButton;
    float sx=1; PointF downPoint; bool mouseHeld,dragged,longHeld;int pressed; double lastAngle; int rotaryPosition;
    System.Windows.Forms.Timer holdTimer,pollTimer,paintTimer; Queue<Action> inputs=new Queue<Action>(); AutoResetEvent inputSignal=new AutoResetEvent(false);Thread inputThread;bool hKeyDown;
    string notice=""; int dirty=1,lcdScale;long shownSequence=-1,paintCount,presentCount,metricReceived,metricPresented;DateTime metricTime=DateTime.UtcNow;Bitmap chrome,uiFrame;string chromeKey="";int rfbTestPort;bool displayTest,usbTest,persistInit,earlyUsb,postBootUsb,reconnectUsb;public bool StateSamples;HashSet<int> snapshotTimes=new HashSet<int>(); int maxSeconds; DateTime started=DateTime.UtcNow;bool testMode;int testStage;
    public ClassicForm(string path,int seconds,bool selfTest,bool synthetic,bool usbScenario,bool initializeDisk,bool earlyCable,bool postBootCable,bool reconnect,int rfbPort){root=path;maxSeconds=seconds;testMode=selfTest;displayTest=synthetic;usbTest=usbScenario;persistInit=initializeDisk;earlyUsb=earlyCable;postBootUsb=postBootCable;reconnectUsb=reconnect;rfbTestPort=rfbPort;
        Text="iPod Classic 7";AutoScaleMode=AutoScaleMode.None;ClientSize=new Size(528,820);MinimumSize=new Size(538,859);BackColor=Color.FromArgb(245,245,243);
        SetStyle(ControlStyles.AllPaintingInWmPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.UserPaint,true);
        Icon=SystemIcons.Application;KeyPreview=true;StartPosition=FormStartPosition.CenterScreen;
        engine=new Engine(root);engine.NativeUsb=!persistInit;engine.PersistentDisk=true;engine.Changed+=()=>Interlocked.Exchange(ref dirty,1);
        inputThread=new Thread(()=>{while(!closing){inputSignal.WaitOne(100);while(true){Action action=null;lock(inputs){if(inputs.Count>0)action=inputs.Dequeue();}if(action==null)break;try{action();}catch(Exception ex){notice=ex.Message;engine.Log("input_error",ex.Message);InvalidateSafe();}}}});inputThread.IsBackground=true;inputThread.Start();
        holdTimer=new System.Windows.Forms.Timer();holdTimer.Interval=420;holdTimer.Tick+=(s,e)=>{holdTimer.Stop();if(mouseHeld&&!dragged&&!engine.Hold&&pressed!=0){longHeld=true;int heldBits=pressed;Queue(()=>{engine.Buttons(heldBits);Thread.Sleep(150);});Invalidate();}};
        pollTimer=new System.Windows.Forms.Timer();pollTimer.Interval=600;pollTimer.Tick+=(s,e)=>{if(engine.Ready)Queue(()=>engine.Poll());if(maxSeconds>0&&((DateTime.UtcNow-started).TotalSeconds>maxSeconds||(testMode&&engine.Status.StartsWith("启动失败"))))Close();if(testMode)SelfTestTick();};pollTimer.Start();
        paintTimer=new System.Windows.Forms.Timer();paintTimer.Interval=15;paintTimer.Tick+=(s,e)=>{
            if(Interlocked.Exchange(ref dirty,0)!=0||Interlocked.Read(ref engine.FrameSequence)!=shownSequence)Invalidate();
            if(displayTest&&rfbTestPort==0){using(var pattern=Pattern((int)(DateTime.UtcNow-started).TotalMilliseconds/16))engine.Publish(pattern);}
            if((DateTime.UtcNow-metricTime).TotalSeconds>=3){double dt=(DateTime.UtcNow-metricTime).TotalSeconds;long received=Interlocked.Read(ref engine.ReceivedUpdates);engine.Log("display_metrics",new{seconds=dt,received_fps=(received-metricReceived)/dt,presented_fps=(presentCount-metricPresented)/dt,received_total=received,presented_total=presentCount,paints=paintCount,pixel_scale=lcdScale,lcd_x=lcd.X,lcd_y=lcd.Y,lcd_w=lcd.Width,lcd_h=lcd.Height,client_w=ClientSize.Width,client_h=ClientSize.Height,dpi=DeviceDpiValue()});metricReceived=received;metricPresented=presentCount;metricTime=DateTime.UtcNow;}
        };paintTimer.Start();
        Shown+=(s,e)=>{if(displayTest&&rfbTestPort>0){engine.StartRfbTest(rfbTestPort);}else if(displayTest){Directory.CreateDirectory(Path.Combine(root,"display-test"));engine.BeginDisplayTest("display-test");ClientSize=new Size(850,900);using(var pattern=Pattern(0))engine.Publish(pattern);}else Task.Run(()=>engine.Start());};
        FormClosing+=(s,e)=>{if(maxSeconds>0&&engine.Ready){try{SaveView("bounded-final");}catch(Exception ex){engine.Log("capture_error",ex.Message);}}closing=true;inputSignal.Set();holdTimer.Stop();pollTimer.Stop();paintTimer.Stop();engine.Dispose();if(chrome!=null)chrome.Dispose();if(uiFrame!=null)uiFrame.Dispose();};
    }
    void Queue(Action a){lock(inputs){if(inputs.Count<64)inputs.Enqueue(a);}inputSignal.Set();}
    void InvalidateSafe(){Interlocked.Exchange(ref dirty,1);}
    static GraphicsPath Round(RectangleF r,float radius){var p=new GraphicsPath();float d=radius*2;p.AddArc(r.X,r.Y,d,d,180,90);p.AddArc(r.Right-d,r.Y,d,d,270,90);p.AddArc(r.Right-d,r.Bottom-d,d,d,0,90);p.AddArc(r.X,r.Bottom-d,d,d,90,90);p.CloseFigure();return p;}
    static void FillRound(Graphics g,Brush brush,RectangleF r,float radius){using(var p=Round(r,radius))g.FillPath(brush,p);}
    static void TextAt(Graphics g,string text,float size,Color color,RectangleF area,bool bold){using(var f=new Font("Segoe UI",size,bold?FontStyle.Bold:FontStyle.Regular,GraphicsUnit.Pixel))using(var b=new SolidBrush(color))using(var sf=new StringFormat{Alignment=StringAlignment.Center,LineAlignment=StringAlignment.Center})g.DrawString(text,f,b,area,sf);}
    float offsetY;
    void LayoutDevice(){lcdScale=Math.Max(1,(int)Math.Floor(Math.Min(ClientSize.Width/440.0,ClientSize.Height/690.0)*270.0/320));sx=320f*lcdScale/270f;float x=(ClientSize.Width-440*sx)/2;offsetY=Math.Max(0,(ClientSize.Height-690*sx)/2);body=new RectangleF(x+58*sx,offsetY+36*sx,324*sx,542*sx);lcd=new RectangleF((int)Math.Round(body.X+27*sx),(int)Math.Round(body.Y+34*sx),320*lcdScale,240*lcdScale);wheel=new RectangleF(body.X+55*sx,body.Y+295*sx,214*sx,214*sx);center=new RectangleF(wheel.X+75*sx,wheel.Y+75*sx,64*sx,64*sx);holdSwitch=new RectangleF(body.X+44*sx,body.Y-5*sx,59*sx,11*sx);usbButton=new RectangleF(x+127*sx,offsetY+603*sx,186*sx,39*sx);}
    int DeviceDpiValue(){using(var g=CreateGraphics())return (int)g.DpiX;}
    protected override void WndProc(ref Message m){base.WndProc(ref m);if(m.Msg==0x84&&m.Result==(IntPtr)1&&WindowState==FormWindowState.Normal){long packed=m.LParam.ToInt64();Point point=PointToClient(new Point((short)(packed&65535),(short)((packed>>16)&65535)));if(point.X>=ClientSize.Width-18&&point.Y>=ClientSize.Height-18)m.Result=(IntPtr)17;}}
    protected override void OnResize(EventArgs e){base.OnResize(e);chromeKey="";Invalidate();}
    protected override void OnPaint(PaintEventArgs e){base.OnPaint(e);LayoutDevice();paintCount++;
        string key=ClientSize.ToString()+engine.Hold+engine.Usb+notice+engine.Status;
        if(chrome==null||key!=chromeKey){var next=new Bitmap(Math.Max(1,ClientSize.Width),Math.Max(1,ClientSize.Height));using(var cg=Graphics.FromImage(next)){cg.Clear(BackColor);DrawChrome(cg);}if(chrome!=null)chrome.Dispose();chrome=next;chromeKey=key;}
        e.Graphics.DrawImageUnscaled(chrome,0,0);
        long seq=Interlocked.Read(ref engine.FrameSequence);if(seq!=shownSequence){var next=engine.CopyFrame();if(next!=null){if(uiFrame!=null)uiFrame.Dispose();uiFrame=next;shownSequence=seq;presentCount++;}}
        if(uiFrame!=null){var g=e.Graphics;var saved=g.Save();g.SetClip(lcd);g.SmoothingMode=SmoothingMode.None;g.InterpolationMode=InterpolationMode.NearestNeighbor;g.PixelOffsetMode=PixelOffsetMode.Half;g.CompositingMode=CompositingMode.SourceCopy;if(lcdScale==1)g.DrawImageUnscaled(uiFrame,(int)lcd.X,(int)lcd.Y);else g.DrawImage(uiFrame,Rectangle.Round(lcd),0,0,uiFrame.Width,uiFrame.Height,GraphicsUnit.Pixel);g.Restore(saved);}
    }
    void DrawChrome(Graphics g){g.SmoothingMode=SmoothingMode.AntiAlias;ControlPaint.DrawSizeGrip(g,Color.FromArgb(190,190,190),ClientSize.Width-16,ClientSize.Height-16,16,16);
        TextAt(g,"iPod classic",19*sx,Color.FromArgb(58,61,64),new RectangleF(0,offsetY+2*sx,ClientSize.Width,27*sx),false);
        for(int i=9;i>0;i--)using(var b=new SolidBrush(Color.FromArgb(3,0,0,0)))FillRound(g,b,new RectangleF(body.X-i*sx,body.Y+6*sx,body.Width+2*i*sx,body.Height+i*sx),31*sx);
        using(var metallic=new LinearGradientBrush(body,Color.FromArgb(234,235,235),Color.FromArgb(184,186,187),LinearGradientMode.Horizontal))FillRound(g,metallic,body,28*sx);
        var face=new RectangleF(body.X+4*sx,body.Y+3*sx,body.Width-8*sx,body.Height-7*sx);
        using(var finish=new LinearGradientBrush(face,Color.FromArgb(224,225,225),Color.FromArgb(208,209,210),LinearGradientMode.Horizontal))FillRound(g,finish,face,25*sx);
        using(var path=Round(face,25*sx)){var save=g.Save();g.SetClip(path);for(int i=0;i<640;i++){float xx=face.X+i*face.Width/640;using(var p=new Pen(Color.FromArgb(i%3==0?9:4,255,255,255),sx*.5f))g.DrawLine(p,xx,face.Y,xx,face.Bottom);}g.Restore(save);}
        using(var path=Round(body,28*sx))using(var p=new Pen(Color.FromArgb(151,153,155),1.2f*sx))g.DrawPath(p,path);
        using(var b=new SolidBrush(Color.FromArgb(87,88,88)))FillRound(g,b,new RectangleF(lcd.X-7*sx,lcd.Y-7*sx,lcd.Width+14*sx,lcd.Height+14*sx),7*sx);
        using(var b=new SolidBrush(Color.FromArgb(35,36,37)))g.FillRectangle(b,lcd);
        if(uiFrame==null)TextAt(g,engine.Status,12*sx,Color.Silver,lcd,false);
        using(var p=new Pen(Color.FromArgb(115,115,115),sx))g.DrawRectangle(p,lcd.X,lcd.Y,lcd.Width,lcd.Height);
        using(var b=new LinearGradientBrush(wheel,Color.FromArgb(248,248,246),Color.FromArgb(226,227,225),LinearGradientMode.ForwardDiagonal))g.FillEllipse(b,wheel);
        using(var p=new Pen(Color.FromArgb(178,180,179),1.3f*sx))g.DrawEllipse(p,wheel);
        using(var b=new LinearGradientBrush(center,Color.FromArgb(225,226,225),Color.FromArgb(249,249,247),LinearGradientMode.ForwardDiagonal))g.FillEllipse(b,center);
        using(var p=new Pen(Color.FromArgb(179,181,180),sx))g.DrawEllipse(p,center);
        Color ink=Color.FromArgb(148,150,149);float cx=wheel.X+wheel.Width/2,cy=wheel.Y+wheel.Height/2;
        TextAt(g,"MENU",12*sx,ink,new RectangleF(cx-30*sx,wheel.Y+19*sx,60*sx,19*sx),true);
        DrawSkip(g,cx-78*sx,cy,false,ink);DrawSkip(g,cx+78*sx,cy,true,ink);
        using(var b=new SolidBrush(ink)){float yy=cy+77*sx;g.FillPolygon(b,new[]{new PointF(cx-10*sx,yy-5*sx),new PointF(cx-10*sx,yy+5*sx),new PointF(cx-2*sx,yy)});g.FillRectangle(b,cx+2*sx,yy-5*sx,2*sx,10*sx);g.FillRectangle(b,cx+6*sx,yy-5*sx,2*sx,10*sx);}
        using(var b=new SolidBrush(Color.FromArgb(95,96,95)))FillRound(g,b,holdSwitch,4*sx);
        if(engine.Hold)using(var b=new SolidBrush(Color.FromArgb(225,117,44)))FillRound(g,b,new RectangleF(holdSwitch.X+3*sx,holdSwitch.Y+2*sx,holdSwitch.Width-6*sx,holdSwitch.Height-4*sx),3*sx);
        var tab=new RectangleF(holdSwitch.X+(engine.Hold?30:2)*sx,holdSwitch.Y+1*sx,27*sx,9*sx);using(var b=new LinearGradientBrush(tab,Color.WhiteSmoke,Color.Gray,LinearGradientMode.Vertical))FillRound(g,b,tab,3*sx);
        TextAt(g,engine.Hold?"HOLD · 已锁定":"HOLD",9*sx,Color.FromArgb(124,125,126),new RectangleF(body.X+103*sx,body.Y-9*sx,89*sx,15*sx),false);
        Color button=engine.Usb?Color.FromArgb(54,76,68):Color.FromArgb(229,230,227);using(var b=new SolidBrush(button))FillRound(g,b,usbButton,20*sx);
        TextAt(g,engine.Usb?"USB 线缆已连接 · 断开":"连接 USB 线缆",13*sx,engine.Usb?Color.White:Color.FromArgb(69,72,71),usbButton,true);
        string footer=notice.Length>0?notice:engine.Hold?"机身 Hold 已锁定":"沿滚轮滑动 · 按压滚轮四个方向";
        TextAt(g,footer,10.5f*sx,Color.FromArgb(136,139,138),new RectangleF(12*sx,offsetY+657*sx,ClientSize.Width-24*sx,19*sx),false);
    }
    void DrawSkip(Graphics g,float x,float y,bool forward,Color ink){float sign=forward?1:-1;using(var b=new SolidBrush(ink)){for(int n=0;n<2;n++){float xx=x+(n-1)*7*sx*sign;g.FillPolygon(b,new[]{new PointF(xx-3*sx*sign,y-4*sx),new PointF(xx-3*sx*sign,y+4*sx),new PointF(xx+3*sx*sign,y)});}g.FillRectangle(b,x+7*sx*sign-sx,y-4*sx,1.5f*sx,8*sx);}}
    bool InCircle(PointF p,RectangleF r){float x=p.X-(r.X+r.Width/2),y=p.Y-(r.Y+r.Height/2);return x*x+y*y<=r.Width*r.Width/4;}
    double Angle(PointF p){return Math.Atan2(p.Y-wheel.Y-wheel.Height/2,p.X-wheel.X-wheel.Width/2);}
    int Position(PointF p){double a=Angle(p)+Math.PI/2;if(a<0)a+=Math.PI*2;return ((int)Math.Round(a/(Math.PI*2)*96))%96;}
    int Direction(PointF p){double a=Angle(p)*180/Math.PI;if(a>=-135&&a< -45)return 16;if(a>=-45&&a<45)return 2;if(a>=45&&a<135)return 8;return 4;}
    protected override void OnMouseDown(MouseEventArgs e){base.OnMouseDown(e);LayoutDevice();if(e.Button!=MouseButtons.Left)return;
        if(holdSwitch.Contains(e.Location)){Queue(()=>engine.SetHold(!engine.Hold));return;}
        if(usbButton.Contains(e.Location)){Queue(()=>engine.SetUsb(!engine.Usb));return;}
        if(!engine.Ready||engine.Hold||!InCircle(e.Location,wheel))return;
        Capture=true;mouseHeld=true;dragged=false;longHeld=false;downPoint=e.Location;lastAngle=Angle(e.Location);pressed=InCircle(e.Location,center)?1:Direction(e.Location);
        if(pressed==1){longHeld=true;Queue(()=>{engine.Buttons(1);Thread.Sleep(150);});}else{rotaryPosition=Position(e.Location);int p=rotaryPosition;Queue(()=>engine.Touch(p));holdTimer.Start();}
    }
    protected override void OnMouseMove(MouseEventArgs e){base.OnMouseMove(e);if(!mouseHeld||pressed==1||longHeld||engine.Hold||!InCircle(e.Location,wheel)||InCircle(e.Location,center))return;
        double a=Angle(e.Location),delta=a-lastAngle;while(delta>Math.PI)delta-=2*Math.PI;while(delta< -Math.PI)delta+=2*Math.PI;
        if(!dragged&&(Math.Abs(e.X-downPoint.X)+Math.Abs(e.Y-downPoint.Y)>7*sx)){dragged=true;holdTimer.Stop();}
        if(dragged){int p=Position(e.Location);if(p!=rotaryPosition){rotaryPosition=p;Queue(()=>engine.Touch(p));}}lastAngle=a;
    }
    protected override void OnMouseUp(MouseEventArgs e){base.OnMouseUp(e);if(!mouseHeld)return;mouseHeld=false;Capture=false;holdTimer.Stop();int bits=pressed;pressed=0;
        bool wasHeld=longHeld,wasDragged=dragged;
        Queue(()=>{engine.Touch(-1);if(wasHeld)engine.Buttons(0);else if(!wasDragged&&!engine.Hold){engine.Buttons(bits);Thread.Sleep(150);engine.Buttons(0);}});Invalidate();
    }
    protected override void OnMouseWheel(MouseEventArgs e){base.OnMouseWheel(e);if(!engine.Ready||engine.Hold)return;int baseline=rotaryPosition,delta=e.Delta>0?-6:6;rotaryPosition=(rotaryPosition+delta+96)%96;int p=rotaryPosition;Queue(()=>{engine.Touch(baseline);engine.Touch(p);});}
    protected override void OnKeyDown(KeyEventArgs e){base.OnKeyDown(e);if(!engine.Ready)return;if(e.KeyCode==Keys.H){if(!hKeyDown){hKeyDown=true;Queue(()=>engine.SetHold(!engine.Hold));}e.Handled=true;return;}if(engine.Hold)return;int bit=e.KeyCode==Keys.Enter?1:e.KeyCode==Keys.Up||e.KeyCode==Keys.M?16:e.KeyCode==Keys.Left?4:e.KeyCode==Keys.Right?2:e.KeyCode==Keys.Space||e.KeyCode==Keys.Down?8:0;if(bit!=0){Queue(()=>{engine.Buttons(bit);Thread.Sleep(150);});e.Handled=true;}}
    protected override void OnKeyUp(KeyEventArgs e){base.OnKeyUp(e);if(e.KeyCode==Keys.H){hKeyDown=false;return;}if(engine.Ready)Queue(()=>engine.Buttons(0));}
    void SaveView(string label){string dir=Path.Combine(engine.RunDir,"captures");Directory.CreateDirectory(dir);using(var bmp=new Bitmap(ClientSize.Width,ClientSize.Height)){using(var graphics=Graphics.FromImage(bmp))OnPaint(new PaintEventArgs(graphics,new Rectangle(Point.Empty,ClientSize)));bmp.Save(Path.Combine(dir,label+"-window.png"),ImageFormat.Png);}engine.SaveFrame(Path.Combine(dir,label+"-lcd.png"));}
    void SelfTestTick(){double t=(DateTime.UtcNow-started).TotalSeconds;if(!engine.Ready)return;
        if(StateSamples)foreach(int at in new[]{20,40,55,85,125,145,175,215,280,315})if(t>at&&snapshotTimes.Add(1000+at))Queue(()=>engine.SampleState(at));
        if(reconnectUsb){
            if(t>32&&testStage==0){testStage++;SaveView("reconnect-01-language");TestClick(new Point((int)(center.X+center.Width/2),(int)(center.Y+center.Height/2)));}
            else if(t>43&&testStage==1){testStage++;Queue(()=>engine.SetHold(true));}
            else if(t>46&&testStage==2){testStage++;SaveView("reconnect-02-hold");Queue(()=>engine.SetHold(false));}
            else if(t>55&&testStage==3){testStage++;SaveView("reconnect-03-mainmenu");Queue(()=>engine.SetUsb(true));}
            else if(t>85&&testStage==4){testStage++;SaveView("reconnect-04-connected1");}
            else if(t>110&&testStage==5){testStage++;SaveView("reconnect-05-connected1-long");Queue(()=>engine.SetUsb(false));}
            else if(t>125&&testStage==6){testStage++;SaveView("reconnect-06-disconnected");}
            else if(t>135&&testStage==7){testStage++;Queue(()=>engine.SetUsb(true));}
            else if(t>170&&testStage==8){testStage++;SaveView("reconnect-07-connected2");}
            else if(t>215&&testStage==9){testStage++;SaveView("reconnect-08-connected2-long");}
            else if(t>280&&testStage==10){testStage++;SaveView("reconnect-09-connected2-final");}
            else if(t>300&&testStage==11){testStage++;Queue(()=>engine.SetUsb(false));}
            else if(t>315&&testStage==12){testStage++;SaveView("reconnect-10-disconnected-final");}
            return;
        }
        if(earlyUsb){if(t>2&&testStage==0){testStage++;Queue(()=>engine.SetUsb(true));}foreach(int at in new[]{20,30,45,65,95,120,145,175,195,205})if(t>at&&snapshotTimes.Add(at))SaveView("initialized-eject-early-"+at);return;}
        if(persistInit){if(t>32&&testStage==0){testStage++;SaveView("persistent-before-select");TestClick(new Point((int)(center.X+center.Width/2),(int)(center.Y+center.Height/2)));}if(t>65&&testStage==1){testStage++;SaveView("persistent-after-select");}return;}
        if(usbTest){
            foreach(int at in new[]{175,195,205,285,315,325})if(t>at&&snapshotTimes.Add(at))SaveView("initialized-eject-final-"+at);
            if(t>32&&testStage==0){testStage++;SaveView("usb-01-language");if(!postBootUsb)TestClick(new Point((int)(center.X+center.Width/2),(int)(center.Y+center.Height/2)));}
            else if(t>48&&testStage==1){testStage++;SaveView("usb-02-mainmenu");TestClick(new Point((int)(usbButton.X+usbButton.Width/2),(int)(usbButton.Y+usbButton.Height/2)));}
            else if(t>60&&testStage==2){testStage++;SaveView("usb-03-after-connect");}
            else if(t>85&&testStage==3){testStage++;SaveView("usb-04-after-enumeration");}
            else if(t>125&&testStage==4){testStage++;SaveView("usb-05-connected-long");}
            else if(t>145&&testStage==5){testStage++;SaveView("usb-06-final");}
            return;
        }
        if(t>32&&testStage==0){testStage++;SaveView("01-before-input");TestClick(new Point((int)(center.X+center.Width/2),(int)(center.Y+center.Height/2)));}
        else if(t>43&&testStage==1){testStage++;SaveView("02-select");TestClick(new Point((int)(holdSwitch.X+holdSwitch.Width/2),(int)(holdSwitch.Y+holdSwitch.Height/2)));}
        else if(t>49&&testStage==2){testStage++;SaveView("03-hold");Queue(()=>{bool refused=false;try{engine.Buttons(16);}catch{refused=true;}File.WriteAllText(Path.Combine(root,"acceptance","hold-refusal.txt"),refused?"native model refused input while physical HOLD on":"FAIL: native input allowed");engine.SetHold(false);});}
        else if(t>55&&testStage==3){testStage++;SaveView("04-unlock");TestClick(new Point((int)(usbButton.X+usbButton.Width/2),(int)(usbButton.Y+usbButton.Height/2)));}
        else if(t>62&&testStage==4){testStage++;SaveView("05-usb-connected");TestClick(new Point((int)(usbButton.X+usbButton.Width/2),(int)(usbButton.Y+usbButton.Height/2)));}
        else if(t>68&&testStage==5){testStage++;SaveView("06-usb-disconnected");int cx=(int)(wheel.X+wheel.Width/2),cy=(int)(wheel.Y+wheel.Height/2),rr=(int)(wheel.Width*.36f);OnMouseDown(new MouseEventArgs(MouseButtons.Left,1,cx,cy-rr,0));OnMouseMove(new MouseEventArgs(MouseButtons.Left,1,cx+rr,cy,0));OnMouseUp(new MouseEventArgs(MouseButtons.Left,1,cx+rr,cy,0));}
        else if(t>75&&testStage==6){testStage++;SaveView("07-wheel");}
    }
    void TestClick(Point p){OnMouseDown(new MouseEventArgs(MouseButtons.Left,1,p.X,p.Y,0));OnMouseUp(new MouseEventArgs(MouseButtons.Left,1,p.X,p.Y,0));}
    static Bitmap Pattern(int index){var bmp=new Bitmap(320,240,PixelFormat.Format32bppRgb);byte[] bytes=new byte[320*240*4];for(int y=0;y<240;y++)for(int x=0;x<320;x++){int p=(y*320+x)*4;bytes[p]=(byte)(x^y);bytes[p+1]=(byte)y;bytes[p+2]=(byte)(x+index);}var data=bmp.LockBits(new Rectangle(0,0,320,240),ImageLockMode.WriteOnly,PixelFormat.Format32bppRgb);Marshal.Copy(bytes,0,data.Scan0,bytes.Length);bmp.UnlockBits(data);return bmp;}
    public void RenderRegression(){Directory.CreateDirectory(Path.Combine(root,"display-test"));using(var pattern=Pattern(0)){engine.Publish(pattern);foreach(var size in new[]{new Size(528,820),new Size(740,920),new Size(1056,1640),new Size(600,900)}){ClientSize=size;LayoutDevice();using(var view=new Bitmap(size.Width,size.Height)){using(var graphics=Graphics.FromImage(view))OnPaint(new PaintEventArgs(graphics,new Rectangle(Point.Empty,size)));int mismatch=0;for(int y=0;y<240;y++)for(int x=0;x<320;x++)for(int dy=0;dy<lcdScale;dy++)for(int dx=0;dx<lcdScale;dx++)if(view.GetPixel((int)lcd.X+x*lcdScale+dx,(int)lcd.Y+y*lcdScale+dy).ToArgb()!=pattern.GetPixel(x,y).ToArgb())mismatch++;view.Save(Path.Combine(root,"display-test","mapping-"+size.Width+"x"+size.Height+".png"));if(mismatch!=0)throw new Exception("Pixel mapping mismatch: "+mismatch);}}}File.WriteAllText(Path.Combine(root,"display-test","pixel-mapping.txt"),"4 sizes, 320x240 source, every pixel and integer replica matched exactly; SYNTHETIC DISPLAY ONLY");}
    [STAThread]static void Main(string[] args){Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);int seconds=0;int rfbPort=0;bool connectOnStart=false,test=false,synthetic=false,render=false,usbScenario=false,initializeDisk=false,earlyCable=false,postBootCable=false,reconnect=false,stateSamples=false;for(int i=0;i<args.Length;i++){if(args[i]=="--connect-usb")connectOnStart=true;if(args[i]=="--max-seconds"&&i+1<args.Length)seconds=int.Parse(args[++i]);if(args[i]=="--self-test")test=true;if(args[i]=="--rfb-test"){rfbPort=int.Parse(args[++i]);if(rfbPort<1||rfbPort>65535)throw new ArgumentException("port");synthetic=true;}if(args[i]=="--reconnect-usb"){reconnect=true;test=true;}if(args[i]=="--state-samples")stateSamples=true;if(args[i]=="--postboot-usb"){postBootCable=true;usbScenario=true;test=true;}if(args[i]=="--persist-init"){initializeDisk=true;test=true;}if(args[i]=="--early-usb"){usbScenario=true;earlyCable=true;test=true;}if(args[i]=="--usb-test"){usbScenario=true;test=true;}if(args[i]=="--display-test")synthetic=true;if(args[i]=="--render-regression"){synthetic=true;render=true;}}string root=Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,".."));using(var form=new ClassicForm(root,seconds,test,synthetic,usbScenario,initializeDisk,earlyCable,postBootCable,reconnect,rfbPort)){form.StateSamples=stateSamples;form.ConnectOnStart=connectOnStart;if(render)form.RenderRegression();else Application.Run(form);}}
}
