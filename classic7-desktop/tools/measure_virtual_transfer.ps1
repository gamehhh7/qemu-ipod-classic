param([Parameter(Mandatory=$true)][string]$Witness,[Parameter(Mandatory=$true)][string]$OutputPath,[ValidateRange(1048576,33554432)][int]$ByteCount=1048576)
$ErrorActionPreference='Stop'
$proof=Get-Content -LiteralPath $Witness -Raw -Encoding utf8 | ConvertFrom-Json
$targets=@($proof.virtual_disks | Where-Object {$_.status -eq 'OK' -and $_.ude_parent_proven -and ($_.parent_chain -contains 'ROOT\USB\0000')})
if($targets.Count -ne 1){throw 'Need exactly one present UDE virtual disk witness'}
$disks=@(Get-CimInstance Win32_DiskDrive | Where-Object {$_.PNPDeviceID -eq $targets[0].instance})
if($disks.Count -ne 1){throw 'Virtual disk identity changed'}
$parts=@(Get-Partition -DiskNumber $disks[0].Index | Where-Object {$_.Size -eq 8589934592})
if($parts.Count -ne 1){throw 'Unexpected virtual data partition capacity'}
$volumes=@($parts | Get-Volume | Where-Object {$_.DriveLetter -and $_.FileSystem -eq 'FAT32'})
if($volumes.Count -ne 1){throw 'Need exactly one mounted virtual FAT32 data volume'}
$volume=$volumes[0]
$file=Join-Path (([string]$volume.DriveLetter)+':\') ('Codex-transfer-'+[Guid]::NewGuid().ToString('N')+'.bin')
if($ByteCount%65536){throw 'Transfer length must be a multiple of64KiB'}
$content=New-Object byte[] $ByteCount
$random=[Security.Cryptography.RandomNumberGenerator]::Create()
try{$random.GetBytes($content)}finally{$random.Dispose()}
$sha=[Security.Cryptography.SHA256]::Create()
try{$expected=[BitConverter]::ToString($sha.ComputeHash($content)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
$result=@{utc_start=[DateTime]::UtcNow.ToString('o');virtual_pnp=$disks[0].PNPDeviceID;file=$file;bytes=$content.Length;physical_devices_touched=$false;success=$false}
try{
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $stream=New-Object IO.FileStream($file,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,65536,[IO.FileOptions]::WriteThrough)
    try{for($at=0;$at -lt $content.Length;$at+=65536){$stream.Write($content,$at,65536)};$stream.Flush($true)}finally{$stream.Dispose()}
    $timer.Stop();$result.write_flush_seconds=$timer.Elapsed.TotalSeconds
    $result.write_bytes_per_second=$content.Length/$result.write_flush_seconds
    $back=[IO.File]::ReadAllBytes($file)
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$actual=[BitConverter]::ToString($sha.ComputeHash($back)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
    if($actual -ne $expected){throw 'Windows readback hash mismatch'}
    $result.sha256=$actual;$result.success=$true
}catch{$result.error=$_.Exception.Message;throw}finally{
    $result.utc_end=[DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutputPath -Encoding utf8
}
$result | ConvertTo-Json -Depth 5
