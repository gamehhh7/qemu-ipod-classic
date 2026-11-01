param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
$result=@{utc=[DateTime]::UtcNow.ToString('o');library_changed=$false;device_changed=$false;sources=@()}
try{
    # Existing running iTunes only. Do not activate the system registered EXE.
    try{$app=[Runtime.InteropServices.Marshal]::GetActiveObject('iTunes.Application')}
    catch{
        $running=@(Get-CimInstance Win32_Process -Filter "Name='iTunes.exe'")
        if($running.Count -ne 1){throw 'Only the intended lab iTunes may own this automation session'}
        $imagePath=$running[0].ExecutablePath
        if(-not $imagePath){
            # WMI may omit this field even when limited process query is allowed.
            Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Classic7ProcessImage {
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder name,ref uint size);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 public static string Read(uint pid){
  IntPtr process=OpenProcess(0x1000,false,pid);
  if(process==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
  try{var name=new StringBuilder(32768);uint size=32768;
   if(!QueryFullProcessImageName(process,0,name,ref size))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
   return name.ToString();
  }finally{CloseHandle(process);}
 }
}
'@
            $imagePath=[Classic7ProcessImage]::Read([uint32]$running[0].ProcessId)
        }
        if($imagePath -ne 'K:\CodexResources\2026-10-18\genius-lab\iTunes-lab\iTunes.exe'){throw 'Only the intended lab iTunes may own this automation session'}
        $app=New-Object -ComObject iTunes.Application
    }
    $result.version=[string]$app.Version
    foreach($source in $app.Sources){
        $result.sources+=@{name=[string]$source.Name;kind=[int]$source.Kind;source_id=$source.SourceID;playlist_count=$source.Playlists.Count}
    }
    $result.status='actual_running_iTunes_sources_read'
}catch{$result.status='COM_not_available';$result.error=$_.Exception.Message}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding utf8
