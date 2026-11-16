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

static class QmpTimeoutRegression {
 static void Main(){
  var listen=new TcpListener(IPAddress.Loopback,0);listen.Start();int port=((IPEndPoint)listen.LocalEndpoint).Port;
  var server=new Thread(()=>{using(var c=listen.AcceptTcpClient()){var rd=new StreamReader(c.GetStream());var wr=new StreamWriter(c.GetStream(),new UTF8Encoding(false));wr.AutoFlush=true;var j=new JavaScriptSerializer();wr.WriteLine("{\"QMP\":{}}");
   var first=j.Deserialize<Dictionary<string,object>>(rd.ReadLine());wr.WriteLine(j.Serialize(new {id=first["id"],@return=new {}}));
   var slow=j.Deserialize<Dictionary<string,object>>(rd.ReadLine());Thread.Sleep(5400);wr.WriteLine(j.Serialize(new {id=slow["id"],@return=new {}}));
   var next=j.Deserialize<Dictionary<string,object>>(rd.ReadLine());wr.WriteLine("{\"event\":\"RESUME\"}");wr.WriteLine(j.Serialize(new {id=next["id"],@return=false}));
  }});server.Start();using(var qmp=new Qmp(port)){bool timedout=false;try{qmp.Call("slow",null);}catch(IOException){timedout=true;}if(!timedout)throw new Exception("missing timeout");object got=qmp.Call("qom-get",null);if(!(got is bool)||Convert.ToBoolean(got))throw new Exception("stale reply not filtered");Console.WriteLine("PASS: stale empty reply after timeout and unsolicited event ignored; matching boolean response returned");}server.Join();listen.Stop();
 }
}