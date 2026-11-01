@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2026 willisdylan
rem
rem Read the DRAM of a hung RAM-booted kernel. No flash writes.
rem Why it can work: boot1's "7 s reset" path jumps back into the kernel without reloading it, so
rem the stock design keeps DRAM contents across a 7 s power-key reset. The only write is one word
rem put back to its own value: 'DHTB' (0x42544844) at 0x80a05e00, the kernel image header, needed
rem because FDL1 only switches DRAM on at the first DRAM data load.
rem
rem   ramdump_after_hang.cmd OUTPREFIX      e.g. dumps\ram_hang
rem
rem Needs SPD_DUMP and FDL1 like ramboot.cmd.
rem HOW: phone hung, battery in, cable in. Start this script, then hold the left soft key and keep
rem holding it, and hold the end (red, power) key for 7-10 s until the screen resets. Release the
rem end key, keep the soft key held until spd_dump starts talking. If the screen never resets
rem within ~15 s the 7 s reset is not armed: pull the battery instead.
setlocal
if "%~1"=="" (echo usage: ramdump_after_hang.cmd OUTPREFIX & exit /b 2)
if not defined SPD_DUMP set SPD_DUMP=spd_dump.exe
if not defined FDL1 (echo set FDL1 to the FDL1 image & exit /b 2)
set OUT=%~1
echo === 1 MB of kernel code, 1 MB of kernel data, then all 48 MB of DRAM ===
"%SPD_DUMP%" --verbose 0 --wait 300 keep_charge 1 fdl "%FDL1%" 0x6200 write_word 0x80a05e00 0x42544844 read_mem 0x80a06000 0x100000 "%OUT%_kcode.bin" read_mem 0x81700000 0x100000 "%OUT%_kdata.bin" read_mem 0x80000000 0x3000000 "%OUT%_dram.bin"
echo === spd_dump exit code %errorlevel%. Pull the battery to boot normally. ===
