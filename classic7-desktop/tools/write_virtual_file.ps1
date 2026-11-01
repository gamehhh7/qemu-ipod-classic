param([Parameter(Mandatory=$true)][string]$Witness,[Parameter(Mandatory=$true)][string]$OutputPath,[string]$ExpectedFile)
$ErrorActionPreference='Stop'
$proof=Get-Content -LiteralPath $Witness -Raw -Encoding utf8 | ConvertFrom-Json
$targets=@($proof.virtual_disks | Where-Object { $_.status -eq 'OK' -and $_.ude_parent_proven -and ($_.parent_chain -contains 'ROOT\USB\0000') })
if($targets.Count -ne 1){throw 'Need exactly one current virtual iPod disk witness'}
$target=$targets[0]
$disk=@(Get-CimInstance Win32_DiskDrive | Where-Object { $_.PNPDeviceID -eq $target.instance })
if($disk.Count -ne 1){throw 'Current Windows disk no longer matches virtual witness'}
$parts=@(Get-Partition -DiskNumber $disk[0].Index -ErrorAction SilentlyContinue)
$result=@{disk_number=$disk[0].Index;pnp=$disk[0].PNPDeviceID;bytes_per_sector=$disk[0].BytesPerSector;size=$disk[0].Size;virtual_parent_chain=$target.parent_chain;partitions=@();file_IO_success=$false;format_performed=$false}
foreach($part in $parts){
    $vol=@(Get-Volume -Partition $part -ErrorAction SilentlyContinue)
    $result.partitions+=@{offset=$part.Offset;size=$part.Size;drive_letter=[string]$part.DriveLetter;volumes=@($vol | Select-Object DriveLetter,FileSystem,FileSystemLabel,HealthStatus,Size,SizeRemaining)}
    foreach($v in $vol){
        if(-not $v.DriveLetter -or $v.FileSystem -notin @('FAT','FAT32','exFAT','NTFS')){continue}
        $rootPath=([string]$v.DriveLetter)+':\'
        if($ExpectedFile){
            $prior=Get-Content -LiteralPath $ExpectedFile -Raw -Encoding utf8 | ConvertFrom-Json
            if(-not $prior.file_IO_success){throw 'Prior Windows file write was not successful'}
            $file=Join-Path $rootPath ([IO.Path]::GetFileName($prior.file.path))
            $content=$null
        }else{
            $file=Join-Path $rootPath ('Codex-iPod-'+[Guid]::NewGuid().ToString('N')+'.txt')
            $content=[Text.Encoding]::UTF8.GetBytes(('Virtual iPod Windows file write '+[DateTime]::UtcNow.ToString('o')+"`r`n")*64)
            $stream=[IO.File]::Open($file,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
            try{$stream.Write($content,0,$content.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        }
        $back=[IO.File]::ReadAllBytes($file)
        $sha=[Security.Cryptography.SHA256]::Create()
        try{$actual=[BitConverter]::ToString($sha.ComputeHash($back)).Replace('-','').ToLowerInvariant();if($ExpectedFile){$expected=$prior.file.sha256}else{$expected=[BitConverter]::ToString($sha.ComputeHash($content)).Replace('-','').ToLowerInvariant()}}finally{$sha.Dispose()}
        if($expected -ne $actual){throw 'Actual Windows file readback differs'}
        $result.file_IO_success=$true;$result.file=@{path=$file;length=$back.Length;sha256=$actual;flushed=$true;reconnected_persistence_verified=[bool]$ExpectedFile}
    }
}
$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding utf8
