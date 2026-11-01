# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
<#
.SYNOPSIS
  usb-watch.ps1 - stream all USB device arrival/removal events on Windows.

.DESCRIPTION
  Subscribes to WMI __InstanceCreationEvent / __InstanceDeletionEvent indications on
  Win32_PnPEntity (about 1 s resolution - plenty for the Unisoc boot ROM, which stays
  visible for several seconds at power-on) and prints every USB plug/unplug with a
  timestamp, device ID, VID/PID, name, class and service. Everything is also appended
  to usb-watch.log next to this script.

  Built for the Ruio F286 / Unisoc T107 download-mode hunt: the boot ROM enumerates as
  USB VID_1782 (Spreadtrum/Unisoc) PID_4D00 only while a boot key is held during
  power-on. Any 1782 device is highlighted, and 1782:4D00 gets a loud banner + beep.

.USAGE
  # from Git Bash or cmd:
  powershell -NoProfile -ExecutionPolicy Bypass -File usb-watch.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File usb-watch.ps1 -VidFilter 1782
  powershell -NoProfile -ExecutionPolicy Bypass -File usb-watch.ps1 -VidFilter 1782,0e8d
  powershell -NoProfile -ExecutionPolicy Bypass -File usb-watch.ps1 -SnapshotOnly
  powershell -NoProfile -ExecutionPolicy Bypass -File usb-watch.ps1 -NoLog

  Ctrl+C to stop. No admin rights required.
#>
param(
    # Only report devices whose VID hex matches one of these, e.g. -VidFilter 1782,0e8d
    [string[]]$VidFilter = @(),
    # Print currently present USB devices and exit
    [switch]$SnapshotOnly,
    # Do not write usb-watch.log
    [switch]$NoLog
)

$ErrorActionPreference = 'Stop'

function Test-UsbId([string]$id) {
    $id -like 'USB\*' -or $id -like 'USBSTOR\*' -or $id -like 'HID\VID_*' -or $id -like 'WPD\*'
}

function Get-VidPid([string]$id) {
    if ($id -match '(?i)VID_([0-9A-F]{4})&PID_([0-9A-F]{4})') {
        [pscustomobject]@{ Vid = $Matches[1].ToUpper(); PidHex = $Matches[2].ToUpper() }
    } else { $null }
}

function Get-UsbSnapshot {
    Get-CimInstance Win32_PnPEntity -Filter 'Present = True' |
        Where-Object { Test-UsbId $_.DeviceID } |
        Sort-Object DeviceID |
        ForEach-Object {
            [pscustomobject]@{
                DeviceID = $_.DeviceID
                Name     = $_.Name
                Class    = $_.PNPClass
                Service  = $_.Service
            }
        }
}

$script:LogPath = Join-Path $PSScriptRoot 'usb-watch.log'

function Emit([string]$kind, $dev, $vp) {
    $ts  = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    $vid = if ($vp) { $vp.Vid }   else { '--' }
    $pdx = if ($vp) { $vp.PidHex } else { '--' }
    $name = $dev.Name
    if (-not $name) { $name = $dev.Description }
    $line = '[{0}] {1,-7} {2}  [{3}] class={4} svc={5} vid={6} pid={7}' -f `
        $ts, $kind, $dev.DeviceID, $name, $dev.PNPClass, $dev.Service, $vid, $pdx

    $color = 'DarkGray'
    if     ($vid -eq '1782')            { $color = 'Cyan' }
    elseif ($kind -eq 'ARRIVE')         { $color = 'Green' }
    elseif ($kind -eq 'REMOVE')         { $color = 'DarkYellow' }
    Write-Host $line -ForegroundColor $color
    if (-not $script:NoLog) { Add-Content -Path $script:LogPath -Value $line }

    if ($vid -eq '1782' -and $pdx -eq '4D00' -and $kind -eq 'ARRIVE') {
        Write-Host ''
        Write-Host '************************************************************************' -ForegroundColor Magenta
        Write-Host '**  SPREADTRUM BOOT ROM / DOWNLOAD MODE DETECTED  (VID_1782 PID_4D00) **' -ForegroundColor Magenta
        Write-Host '**  Connect spd_dump NOW - this window lasts only a few seconds.      **' -ForegroundColor Magenta
        Write-Host '************************************************************************' -ForegroundColor Magenta
        Write-Host ''
        try { [console]::Beep(880,150); [console]::Beep(1200,250) } catch { }
    }
}

# ---------------------------------------------------------------- snapshot mode
if ($SnapshotOnly) {
    $snap = Get-UsbSnapshot
    Write-Host ("Currently present USB devices: {0}" -f $snap.Count) -ForegroundColor Gray
    $snap | Format-Table -AutoSize | Out-String -Width 240 | Write-Host
    exit 0
}

# ---------------------------------------------------------------- live watching
Write-Host 'usb-watch: streaming USB PnP events (~1 s resolution). Ctrl+C to stop.' -ForegroundColor Gray
if (-not $script:NoLog) {
    Write-Host ("usb-watch: log file: {0}" -f $script:LogPath) -ForegroundColor Gray
}
if ($VidFilter.Count -gt 0) {
    Write-Host ("usb-watch: filtering to VID(s): {0}" -f ($VidFilter -join ', ')) -ForegroundColor Gray
}

Write-Host ('usb-watch: currently present:') -ForegroundColor Gray
Get-UsbSnapshot | ForEach-Object {
    Emit 'present' ([pscustomobject]@{
        DeviceID = $_.DeviceID; Name = $_.Name; Description = $null;
        PNPClass = $_.Class; Service = $_.Service
    }) (Get-VidPid $_.DeviceID)
}
Write-Host 'usb-watch: waiting for changes...' -ForegroundColor Gray

Register-CimIndicationEvent `
    -Query "SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_PnPEntity'" `
    -SourceIdentifier UsbArrive | Out-Null
Register-CimIndicationEvent `
    -Query "SELECT * FROM __InstanceDeletionEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_PnPEntity'" `
    -SourceIdentifier UsbLeave | Out-Null

try {
    while ($true) {
        $ev = Wait-Event -ErrorAction SilentlyContinue
        if (-not $ev) { continue }
        while ($ev) {
            $inst = $ev.SourceEventArgs.NewEvent.TargetInstance
            if ($null -ne $inst -and (Test-UsbId $inst.DeviceID)) {
                $vp = Get-VidPid $inst.DeviceID
                $show = $true
                if ($VidFilter.Count -gt 0) {
                    $show = ($null -ne $vp) -and ($VidFilter -contains $vp.Vid)
                }
                if ($show) {
                    $kind = if ($ev.SourceIdentifier -eq 'UsbArrive') { 'ARRIVE' } else { 'REMOVE' }
                    Emit $kind ([pscustomobject]@{
                        DeviceID = $inst.DeviceID; Name = $inst.Name;
                        Description = $inst.Description; PNPClass = $inst.PNPClass;
                        Service = $inst.Service
                    }) $vp
                }
            }
            Remove-Event -EventIdentifier $ev.EventIdentifier -ErrorAction SilentlyContinue
            $ev = Get-Event -ErrorAction SilentlyContinue | Select-Object -First 1
        }
    }
}
finally {
    Get-Event -ErrorAction SilentlyContinue | Remove-Event -ErrorAction SilentlyContinue
    Unregister-Event -SourceIdentifier UsbArrive -ErrorAction SilentlyContinue
    Unregister-Event -SourceIdentifier UsbLeave -ErrorAction SilentlyContinue
    Write-Host 'usb-watch: stopped.' -ForegroundColor Gray
}
