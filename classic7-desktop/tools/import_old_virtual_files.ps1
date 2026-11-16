param([Parameter(Mandatory=$true)][string]$Folder)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$proof=Get-Content -LiteralPath (Join-Path $Folder 'WINDOWS-PNP.json') -Raw -Encoding utf8 | ConvertFrom-Json
$source=Get-Content -LiteralPath (Join-Path $Folder 'SOURCE.json') -Raw -Encoding utf8 | ConvertFrom-Json
$identity=Get-Content -LiteralPath (Join-Path $root 'userdata/device-identity.json') -Raw -Encoding utf8 | ConvertFrom-Json
$targets=@($proof.virtual_disks|Where-Object {$_.status -eq 'OK' -and $_.ude_parent_proven -and ($_.parent_chain -contains 'ROOT\USB\0000')})
if($targets.Count -ne 1){throw 'One currently proved virtual iPod required'}
$disks=@(Get-CimInstance Win32_DiskDrive|Where-Object {$_.PNPDeviceID -eq $targets[0].instance})
if($disks.Count -ne 1){throw 'Current virtual disk identity differs'}
$volumes=@(Get-Partition -DiskNumber $disks[0].Index | Get-Volume | Where-Object {$_.DriveLetter -and $_.FileSystem -eq 'FAT32'})
if($volumes.Count -ne 1 -or $volumes[0].Size -lt 5GB){throw 'Expected one existing >=5GiB FAT32 virtual volume'}
$destination=([string]$volumes[0].DriveLetter)+':\'
$result=@{purpose='User requested old virtual iPod file import, then stop for manual feedback';virtual_disk=$disks[0].PNPDeviceID;parent_chain=$targets[0].parent_chain;destination=$destination;files=@();directories=@();format_performed=$false;physical_device=$false;status='copying'}
function Save-Result { $result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $Folder 'IMPORT-RESULT.json') -Encoding utf8 }
try {
    foreach($d in $source.directories){
        $path=Join-Path $destination $d.path
        if(-not [IO.Directory]::Exists($path)){[IO.Directory]::CreateDirectory($path)|Out-Null}
        $result.directories+=@{path=$d.path}
    }
    foreach($file in $source.files){
        $from=Join-Path (Join-Path $Folder 'source-files') $file.path
        $to=Join-Path $destination $file.path
        if(Test-Path -LiteralPath $to){
            $hash=(Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash.ToLowerInvariant()
            if($hash -ne $file.sha256){throw ('Existing destination differs; do not overwrite user data: '+$file.path)}
            $result.files+=@{path=$file.path;action='already_identical';sha256=$hash}
            continue
        }
        $data=[IO.File]::ReadAllBytes($from)
        $sha=[Security.Cryptography.SHA256]::Create()
        try {$actual=[BitConverter]::ToString($sha.ComputeHash($data)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
        if($actual -ne $file.sha256){throw ('Source export changed: '+$file.path)}
        $adjustment=$null
        if($file.path -eq 'iPod_Control/Device/SysInfo'){
            # File metadata describes this configured virtual device. Do not
            # reintroduce the old disk's xMB147 / all-zero manufacturing IDs.
            $text='ModelNumStr: '+$identity.model+"`r`nFirewireGuid: "+$identity.firewire_guid+"`r`npszSerialNumber: "+$identity.serial+"`r`n"
            $data=[Text.Encoding]::ASCII.GetBytes($text)
            $adjustment='Existing configured native manufacturing profile, replacing old zero IDs in imported metadata only'
        }
        $stream=[IO.File]::Open($to,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try{$stream.Write($data,0,$data.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        $sha=[Security.Cryptography.SHA256]::Create()
        try{$expected=[BitConverter]::ToString($sha.ComputeHash($data)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
        $back=(Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash.ToLowerInvariant()
        if($back -ne $expected){throw ('Copy readback mismatch: '+$file.path)}
        [IO.File]::SetAttributes($to,[IO.FileAttributes]([int]$file.attributes))
        $result.files+=@{path=$file.path;action='copied_and_flushed';bytes=$data.Length;sha256=$back;metadata_adjustment=$adjustment}
        Save-Result
    }
    foreach($d in @($source.directories|Sort-Object {$_.path.Length} -Descending)){
        [IO.File]::SetAttributes((Join-Path $destination $d.path),[IO.FileAttributes]([int]$d.attributes))
    }
    $result.status='import_complete_waiting_user_manual_feedback'
}catch {$result.status='import_stopped';$result.error=$_.Exception.Message;throw}
finally {Save-Result}
$result|ConvertTo-Json -Depth 4
