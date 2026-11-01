# SPDX-License-Identifier: MIT
# Copyright (c) 2026 willisdylan
#
# Watchdog-reset dump with a USB timeline. Part 1 RAM-loads an image through FDL1 (no flash
# writes). Then every Unisoc USB arrival/removal is logged with a timestamp, and spd_dump is tried
# only on a fresh 1782:4d00 arrival (never on the device left over from part 1), at most 3 times,
# for up to -Seconds. Part 2 only puts back 'DHTB' at 0x80a05e00 (switches DRAM on) and reads 64 KB
# of kernel code twice. Log and dumps go to -OutDir.
#   .\ramboot_wdg_trace.ps1 -Fdl1 fdl1.bin -Image kernel_wdgspin_ram.bin   # arm, spin, USB off
#   .\ramboot_wdg_trace.ps1 -Fdl1 fdl1.bin -Image kernel_wdg_ram.bin       # stock kernel hangs, watchdog resets it
# HOW: run it, battery out, hold the left soft key and KEEP HOLDING IT, battery in, plug USB.
# Release the key once a read is in progress (or when the script ends).
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Fdl1,
    [string]$SpdDump = "spd_dump.exe",
    [string]$OutDir = "dumps",
    [int]$Seconds = 240
)
$img = (Resolve-Path $Image).Path
$fdl1Path = (Resolve-Path $Fdl1).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$log = Join-Path $OutDir "ramboot_trace_$stamp.log"
$out = Join-Path $OutDir "trace_$stamp"
$t0 = Get-Date
function Log($m) { $l = "{0,7:F1}s  {1}" -f ((Get-Date) - $t0).TotalSeconds, $m; Write-Host $l; Add-Content $log $l }
function Devs { @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^USB\\VID_1782&PID_[0-9A-F]{4}\\' } | ForEach-Object { $_.InstanceId }) }

Log "image $Image  sha256 $((Get-FileHash $img).Hash.Substring(0,8))"
Log "part 1: waiting for the boot ROM, then RAM load"
& $SpdDump --verbose 0 --wait 300 keep_charge 1 fdl $fdl1Path 0x6200 fdl $img 0x80a05e00 2>&1 | ForEach-Object { Log "  spd1: $_" }
Log "part 1 exit $LASTEXITCODE (an error here is expected). Keep holding the key."
$prev = Devs
Log "present now: $($prev -join ', ')"
$tries = 0
$t1 = Get-Date
while (((Get-Date) - $t1).TotalSeconds -lt $Seconds -and $tries -lt 3) {
    $now = Devs
    foreach ($d in $now) { if ($prev -notcontains $d) { Log "ARRIVED $d" } }
    foreach ($d in $prev) { if ($now -notcontains $d) { Log "REMOVED $d" } }
    $fresh = $now | Where-Object { $_ -match 'PID_4D00' -and $prev -notcontains $_ }
    if (-not $fresh -and ($prev | Where-Object { $_ -match 'PID_4D00' }).Count -eq 0 -and ($now | Where-Object { $_ -match 'PID_4D00' })) { $fresh = $true }
    $prev = $now
    if ($fresh) {
        $tries++
        Log "try $tries on the fresh boot-ROM device"
        & $SpdDump --verbose 1 --wait 10 keep_charge 1 fdl $fdl1Path 0x6200 write_word 0x80a05e00 0x42544844 read_mem 0x80a06000 0x10000 "${out}_kcode.bin" read_mem 0x80a06008 0x10000 "${out}_kcode8.bin" 2>&1 | ForEach-Object { Log "  spd2: $_" }
        Log "try $tries exit $LASTEXITCODE"
        if ($LASTEXITCODE -eq 0) { break }
        $prev = Devs
    }
    Start-Sleep -Milliseconds 200
}
Log "done (tries $tries). Log: $log"
