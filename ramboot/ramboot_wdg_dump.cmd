@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2026 willisdylan
rem
rem RAM-boot an image whose shim arms the PMIC watchdog (45 s, reset mode), then, after the
rem watchdog resets the hung phone, read its DRAM back through FDL1. No flash writes. The only
rem memory write in part 2 puts back the kernel header word 'DHTB' (0x42544844) at 0x80a05e00,
rem which makes FDL1 switch DRAM on.
rem
rem   ramboot_wdg_dump.cmd IMAGE OUTPREFIX
rem     IMAGE      kernel_wdg_ram.bin (stock kernel, hangs, watchdog resets it) or
rem                kernel_wdgspin_ram.bin (test: never enters the kernel)
rem     OUTPREFIX  e.g. dumps\ram_hang -> dumps\ram_hang_kcode.bin, _kdata.bin, _dram.bin
rem
rem Needs SPD_DUMP and FDL1 like ramboot.cmd.
rem HOW: run this, battery out, then hold the left soft key and KEEP HOLDING IT the whole time,
rem put the battery in and plug USB. Part 1 ends with "timeout reached" or LIBUSB_ERROR_PIPE
rem (expected). About 45-60 s later the watchdog should reset the phone and part 2 starts reading
rem (several minutes). Release the key once part 2 prints progress. If nothing happens within
rem ~2 min, pull the battery: the kernel turned the watchdog off, or the arming did not work.
setlocal
if "%~2"=="" (echo usage: ramboot_wdg_dump.cmd IMAGE OUTPREFIX & exit /b 2)
if not defined SPD_DUMP set SPD_DUMP=spd_dump.exe
if not defined FDL1 (echo set FDL1 to the FDL1 image & exit /b 2)
if not exist "%~1" (echo missing %~1 & exit /b 1)
set OUT=%~2
echo === part 1: RAM load %~1 ===
"%SPD_DUMP%" --verbose 0 --wait 300 keep_charge 1 fdl "%FDL1%" 0x6200 fdl "%~1" 0x80a05e00
echo === part 1 done (exit %errorlevel%; an error here is expected). Keep holding the key. ===
echo === part 2: waiting for the watchdog reset, then reading DRAM ===
rem The first boot-ROM handshake after the reset failed once ("ver expected"): retry quickly
rem while the key is still held (the boot ROM keeps waiting), verbose so a failure shows why.
set WAIT=300
set N=0
:retry
"%SPD_DUMP%" --verbose 1 --wait %WAIT% keep_charge 1 fdl "%FDL1%" 0x6200 write_word 0x80a05e00 0x42544844 read_mem 0x80a06000 0x100000 "%OUT%_kcode.bin" read_mem 0x81700000 0x100000 "%OUT%_kdata.bin" read_mem 0x80000000 0x3000000 "%OUT%_dram.bin"
if %errorlevel%==0 goto done
set /a N+=1
set WAIT=20
echo --- attempt %N% failed, retrying ---
if %N% lss 8 goto retry
:done
echo === part 2 done (exit %errorlevel%, retries %N%). Pull the battery to boot normally. ===
