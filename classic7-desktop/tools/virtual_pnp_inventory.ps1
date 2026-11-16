param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
function Get-ParentChain([string]$Instance) {
    $chain=[System.Collections.Generic.List[string]]::new()
    $node=$Instance
    for($i=0;$i -lt 24 -and $node;$i++) {
        if($chain.Contains($node)){break}
        $chain.Add($node)
        $prop=Get-PnpDeviceProperty -InstanceId $node -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue
        if(-not $prop){break}
        $node=[string]$prop.Data
    }
    return ,($chain.ToArray())
}
$virtual=@()
$devices=Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_05AC&PID_1261\*' }
foreach($device in $devices) {
    $chain=Get-ParentChain $device.InstanceId
    if($chain -contains 'ROOT\USB\0000') {
        $virtual+=@{instance=$device.InstanceId;name=$device.FriendlyName;status=[string]$device.Status;class=$device.Class;parent_chain=$chain;ude_parent_proven=$true}
    }
}
$disks=@()
function Save-Witness([string]$Stage) {
    @{utc=[DateTime]::UtcNow.ToString('o');stage=$Stage;virtual_usb=$virtual;virtual_disks=$disks;physical_devices_touched=$false;file_writes=$false} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding utf8
}
Save-Witness 'actual_USB_identity_before_storage_query' 
if($virtual.Count -gt 0) {
    $candidates=Get-CimInstance Win32_DiskDrive | Where-Object { $_.PNPDeviceID -like 'USBSTOR\*' }
    foreach($disk in $candidates) {
        $chain=Get-ParentChain $disk.PNPDeviceID
        $sameUsb=$false
        foreach($item in $virtual){if($chain -contains $item.instance){$sameUsb=$true}}
        if(($chain -contains 'ROOT\USB\0000') -and $sameUsb) {
            $parts=@()
            $entry=@{disk_number=$disk.Index;model=$disk.Model;size=$disk.Size;bytes_per_sector=$disk.BytesPerSector;pnp=$disk.PNPDeviceID;parent_chain=$chain;partitions=@();ude_parent_proven=$true}
            $disks+=,$entry
            Save-Witness 'actual_virtual_disk_before_partition_query' 
            foreach($part in @(Get-Partition -DiskNumber $disk.Index -ErrorAction SilentlyContinue)) {
                $vol=Get-Volume -Partition $part -ErrorAction SilentlyContinue
                $parts+=@{partition_number=$part.PartitionNumber;drive_letter=[string]$part.DriveLetter;offset=$part.Offset;size=$part.Size;volume=@($vol | Select-Object DriveLetter,FileSystem,FileSystemLabel,HealthStatus,Size,SizeRemaining,UniqueId)}
            }
            $entry.partitions=$parts
            Save-Witness 'actual_partitions_and_volume_query_completed' 
        }
    }
}
Save-Witness 'inventory_completed'
