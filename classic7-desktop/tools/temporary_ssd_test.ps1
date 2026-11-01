$ErrorActionPreference='Stop'
$root='K:\CodexResources\2026-10-28\ipod-classic7-desktop'
$output=Join-Path $root 'acceptance\sync-performance-20261030'
$python='C:\Users\hhh\AppData\Local\Programs\Python\Python311\python.exe'
$config=Join-Path $root 'userdata\windows-disk.json'
$source=[IO.Path]::GetFullPath((Join-Path $root 'userdata\windows-8g.sqlite3'))
$destination=[IO.Path]::GetFullPath('G:\CodexResources\2026-10-30\ipod-classic7-userdata-test\windows-8g.sqlite3')
if(-not $source.StartsWith($root+'\userdata\',[StringComparison]::OrdinalIgnoreCase) -or -not $destination.StartsWith('G:\CodexResources\2026-10-30\ipod-classic7-userdata-test\',[StringComparison]::OrdinalIgnoreCase)){throw 'Relocation outside explicit task folders'}
function Assert-NoOwner {
    $owners=@(Get-CimInstance Win32_Process | Where-Object {($_.Name -match 'iPodClassic|qemu.*classic') -or ($_.Name -eq 'python.exe' -and $_.CommandLine -like '*ipod-classic7-desktop*storage*')})
    if($owners.Count){throw 'A virtual disk owner is still running; no relocation'}
}
Assert-NoOwner
if(Test-Path -LiteralPath $destination){throw 'Temporary SSD destination already exists'}
$originalConfig=[IO.File]::ReadAllBytes($config)
$state=@{temporary_only=$true;source=$source;destination=$destination;restored=$false;phase='starting';user_choice='non-C SSD, temporary test only';original_config_json=[Text.Encoding]::UTF8.GetString($originalConfig)}
function Save-State {$state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'TEMP-SSD-STATE.json') -Encoding utf8}
$app=$null;$moved=$false
try{
    & $python -B (Join-Path $root 'tools\checkpoint_virtual_store.py') $source (Join-Path $output 'K-BEFORE-MOVE.json')
    if($LASTEXITCODE){throw 'Source checkpoint failed'}
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
    Move-Item -LiteralPath $source -Destination $destination
    $moved=$true;$state.phase='moved_to_G';Save-State
    Move-Item -LiteralPath ($source+'.lock') -Destination ($destination+'.lock')
    $cfg=[Text.Encoding]::UTF8.GetString($originalConfig) | ConvertFrom-Json
    $cfg.overlay_file=$destination
    [IO.File]::WriteAllText($config,($cfg | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    & $python -B (Join-Path $root 'tools\checkpoint_virtual_store.py') $destination (Join-Path $output 'G-AFTER-MOVE.json')
    if($LASTEXITCODE){throw 'Destination validation failed'}
    $env:CLASSIC7_WAKE_TRACE='1';$env:CLASSIC7_USB_TIMING='1'
    # This is the actual interactive emulator, which must remain visible and
    # closable through its normal window lifecycle.
    $app=Start-Process -FilePath (Join-Path $root 'bin\iPodClassic7.exe') -ArgumentList '--connect-usb' -WorkingDirectory $root -WindowStyle Normal -PassThru
    $state.app_pid=$app.Id;$state.phase='G_test_running';Save-State
    $witness=Join-Path $output 'G-SSD-PNP.json';$ready=$false
    for($i=0;$i -lt 35;$i++){
        if($app.HasExited){throw 'Actual main EXE exited before enumeration'}
        & $python -B (Join-Path $root 'tools\virtual_pnp_inventory.py') $witness | Out-Null
        $proof=Get-Content -LiteralPath $witness -Raw -Encoding utf8 | ConvertFrom-Json
        if(@($proof.virtual_disks).Count -eq 1){
            $disk=@(Get-CimInstance Win32_DiskDrive | Where-Object {$_.PNPDeviceID -eq $proof.virtual_disks[0].instance})
            if($disk.Count -eq 1){
                $mounted=@(Get-Partition -DiskNumber $disk[0].Index -ErrorAction SilentlyContinue | Get-Volume -ErrorAction SilentlyContinue | Where-Object {$_.DriveLetter -and $_.FileSystem -eq 'FAT32'})
                if($mounted.Count -eq 1){$ready=$true;break}
            }
        }
        Start-Sleep -Seconds 3
    }
    if(-not $ready){throw 'Actual virtual disk did not enumerate'}
    & (Join-Path $root 'tools\measure_virtual_transfer.ps1') -Witness $witness -OutputPath (Join-Path $output 'G-SSD-16M-TRANSFER.json') -ByteCount 16777216
    & $python -B (Join-Path $root 'tools\eject_virtual_disk.py') $witness (Join-Path $output 'G-SSD-EJECT.json')
    if($LASTEXITCODE){throw 'Safe eject failed'}
    $eject=Get-Content -LiteralPath (Join-Path $output 'G-SSD-EJECT.json') -Raw -Encoding utf8 | ConvertFrom-Json
    if($eject.CONFIGRET -ne 0){throw 'Safe eject vetoed; preserve current owner'}
    $state.phase='G_test_safely_ejected';Save-State
}finally{
    if($null -ne $app -and -not $app.HasExited){$app.Refresh();$null=$app.CloseMainWindow();$null=$app.WaitForExit(15000)}
    Assert-NoOwner
    if($moved){
        & $python -B (Join-Path $root 'tools\checkpoint_virtual_store.py') $destination (Join-Path $output 'G-BEFORE-RETURN.json')
        if($LASTEXITCODE){throw 'Keep SSD COW in place because checkpoint failed; do not move live/invalid data'}
        if(Test-Path -LiteralPath $source){throw 'Source reappeared; refuse overwrite'}
        Move-Item -LiteralPath $destination -Destination $source
        if(Test-Path -LiteralPath ($destination+'.lock')){Move-Item -LiteralPath ($destination+'.lock') -Destination ($source+'.lock')}
        [IO.File]::WriteAllBytes($config,$originalConfig)
        & $python -B (Join-Path $root 'tools\checkpoint_virtual_store.py') $source (Join-Path $output 'K-AFTER-RETURN.json')
        if($LASTEXITCODE){throw 'Returned COW requires inspection'}
        $state.restored=$true;$state.phase='returned_to_K_original_config_restored';Save-State
    }
    Remove-Item Env:\CLASSIC7_WAKE_TRACE -ErrorAction SilentlyContinue
    Remove-Item Env:\CLASSIC7_USB_TIMING -ErrorAction SilentlyContinue
}
