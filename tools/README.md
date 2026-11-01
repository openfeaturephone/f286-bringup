# Tools

`extract_dsp_fw.py` cuts the six DSP firmware files the radio driver loads out of
your own stock dump (partitions carved one per file). See its docstring and
`docs/test-rig.md`.

`setup_fplinux_wsl.sh` sets up an Ubuntu WSL distribution for FPLinux builds:
packages, an `fplinux` build user with subordinate ids for rootless containers,
and a clone of the `ruio-f286` branch of openfeaturephone/fplinux in
`/home/fplinux/fplinux`. Run it as root inside WSL:

```
sudo bash tools/setup_fplinux_wsl.sh
```

## phone/

Small static ARM programs and scripts that run on the phone under FPLinux, all
through `/dev/mem`, which only the debug build has (`CONFIG_DEVMEM=y` in
FPLinux's debug.config; use `--build-type debug`):

- `memsample` streams every 32-bit word that changes in the DSP windows, the
  handshake cells and the PMU/AON pages. It waits for the driver to write the
  LTE boot address (or the GSM one with `memsample g`), so start it before
  `load_lte`. A one-line summary of each burst also goes to `/dev/kmsg`.
- `poke ADDR [VALUE]` reads or writes one 32-bit register. BusyBox on the phone
  has no `devmem`.
- `devmem ADDR [VAL]` / `devmem -d ADDR COUNT` is the older peek/poke with a dump
  mode.
- `axidump.sh` dumps the PUB AXI debug registers with `dd`; the driver's
  `axidump` attribute does the same and more.

Build with `make -C tools/phone` (zig through pip, or set `CC`).

## legacy/

`whale.c` and `dspload.c` are the user-space experiments that came before the
kernel driver. `whale` runs each MMIO access in a forked child so an external
abort or a hung read kills the child, not the shell. They are kept for that
technique and for the record; the sequences they run are out of date. Both use
the 0x515d/0x515e die addresses, which are wrong (the real ones are
0xD15D/E0000), and `dspload` assumes Linux RAM starts at 0x80200000, which is no
longer true. Do not run them against the current FPLinux image.
