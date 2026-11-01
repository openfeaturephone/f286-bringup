# RAM boot of the stock kernel

Scripts for running the phone's own stock kernel from RAM through the UMS9117
boot ROM's download mode, the same path FPLinux boots by, and for reading DRAM
back after a hang. I used them to compare stock and FPLinux register state and
to get memory images of a hung kernel. Nothing here writes flash.

You need, on Windows:

- `spd_dump.exe` from https://github.com/ilyakurdyukov/spreadtrum_flash (set
  `SPD_DUMP` to its path, or put it on `PATH`);
- an FDL1 for the T117/UMS9117 (set `FDL1`). I used a NAND-enabled build of the
  opendumbphone FDL1; `t117_fdl1.bin` from the spreadtrum_flash releases, which
  FPLinux itself downloads, should do for RAM loads but I have not tried it here;
- your own `kernel.bin` (the stock kernel partition) for `build_ramboot.py`.

```
python build_ramboot.py path\to\kernel.bin -o out
set FDL1=path\to\fdl1.bin
ramboot.cmd out\kernel_shim_ram.bin
```

Then: battery out, hold the left soft key, battery in, plug USB while holding.

| file | what |
|---|---|
| `build_ramboot.py` | builds `kernel_{stock,shim,wdg,wdgspin,wdgspin_nousb}_ram.bin` from your kernel.bin. Checks the sha256 prefix of the kernel it was written against (`--no-check` to skip) |
| `shim.S` | runs before the kernel: stops FDL1's USB (its receive DMA buffer at 0x80-0x3280 overlaps the kernel's vector copy at 0x0-0x1b14), clears word 0 (a dump-mode marker), optionally arms the SC2720 watchdog for 45 s, then enters the kernel like boot1 does |
| `ramboot.cmd IMAGE` | RAM-load and start an image |
| `ramboot_wdg_dump.cmd IMAGE OUT` | RAM-load a watchdog image, wait for the watchdog reset, read 1 MB kernel code, 1 MB kernel data and all 48 MB of DRAM |
| `ramdump_after_hang.cmd OUT` | the same read after a hang, using boot1's 7 s power-key reset, which keeps DRAM |
| `ramboot_wdg_trace.ps1` | watchdog run with a timestamped log of every Unisoc USB arrival/removal |
| `usb-watch.ps1` | prints every USB plug/unplug; highlights 1782:4d00 (the boot ROM) |

## Known problems

- After FDL1 + shim + watchdog reset, the boot ROM's download protocol did not
  answer on the first try. The scripts retry while the key is held, and
  `kernel_wdgspin_nousb_ram.bin` exists to test whether the shim's USB shutdown
  is the cause. I do not have an answer yet.
- The `wdg` images patch two stock functions (0x8136f3f0, 0x8136f4c4) to return
  0, otherwise the kernel re-programs the watchdog while it hangs. The offsets
  are for one firmware build.
- The `.bin` outputs and every dump contain the stock kernel. Keep them out of
  git (the `.gitignore` does).
