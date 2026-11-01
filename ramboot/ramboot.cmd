@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2026 willisdylan
rem
rem RAM-boot a kernel image through the UMS9117 boot ROM's download mode, the way FPLinux boots.
rem Nothing is written to flash: FDL1 copies the image into DRAM at 0x80a05e00 (where boot1
rem normally puts it) and jumps to +0x200.
rem
rem   ramboot.cmd IMAGE        e.g. kernel_stock_ram.bin or kernel_shim_ram.bin
rem
rem Needs SPD_DUMP (path to spd_dump.exe, default: spd_dump.exe on PATH) and FDL1 (path to the
rem FDL1 image). Expected end: spd_dump prints "timeout reached" after the second fdl (the kernel
rem never answers FDL1), then the phone boots from RAM. A battery pull returns to the normal boot.
rem Run this first, then: battery out, hold the left soft key, plug USB while holding it.
setlocal
if "%~1"=="" (echo usage: ramboot.cmd IMAGE & exit /b 2)
if not defined SPD_DUMP set SPD_DUMP=spd_dump.exe
if not defined FDL1 (echo set FDL1 to the FDL1 image & exit /b 2)
if not exist "%~1" (echo missing %~1 & exit /b 1)
echo === RAM load %~1 (no flash writes) ===
"%SPD_DUMP%" --verbose 0 --wait 300 keep_charge 1 fdl "%FDL1%" 0x6200 fdl "%~1" 0x80a05e00
echo === spd_dump exit code %errorlevel% ("timeout reached" here is expected) ===
echo Leave the cable in and release the key.
