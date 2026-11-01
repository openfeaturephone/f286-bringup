# f286-bringup

Research tools and notes from getting the Ruio F286's hardware (Unisoc UMS9117 / T107) working
under [FPLinux](https://github.com/fplinux/fplinux), mostly the baseband radio. The port itself
is not here: the target, the radio driver and the changes to FPLinux are on the `ruio-f286`
branch of [openfeaturephone/fplinux](https://github.com/openfeaturephone/fplinux/tree/ruio-f286),
a fork of FPLinux, so they can go upstream as ordinary commits.

This repo has the parts upstream won't want: the test rig, RAM-boot loaders, phone-side
`/dev/mem` tools, a C++ model of the stock bring-up code, and the write-ups.

- `docs/` - [bring-up status](docs/bringup-status.md) (what works, what's ruled out, open
  questions), the [test rig](docs/test-rig.md), and the [radio driver's sysfs
  interface](docs/radio-driver.md).
- `rig/` - the SSH scripts I drive experiments with: stream `/dev/kmsg`, push the DSP firmware,
  run the power-on sequence.
- `ramboot/` - RAM-booting the stock kernel through download mode, and reading DRAM back after
  a hang.
- `tools/` - DSP firmware extraction from your own dump, a WSL build setup, and small static ARM
  programs for poking registers on the phone.
- `model/` - a C++ model of the stock bring-up paths (PMU, whale die, clocks, DSP boot, L1 rings,
  SIM, SMS, audio), written while reading the stock kernel's disassembly, as a reference for the
  driver. Host-only, nothing in it touches hardware.

No firmware is included. The DSP images, the CM4 image and the Bluetooth calibration all come
from your own phone.

## Building the port

```
git clone -b ruio-f286 https://github.com/openfeaturephone/fplinux
cd fplinux
./fplinux doctor
./fplinux build ruio-f286 --build-type debug
./fplinux run ruio-f286 --build-type debug
```

The branch sits on FPLinux 7c32451, which is what my phone boots. Upstream has since reorganised
how targets are laid out, so it still needs rebasing before it can be sent upstream.

On Windows I build in WSL. `tools/setup_fplinux_wsl.sh` sets up an Ubuntu WSL distribution with
a build user and a clone of that branch.

The BootROM key is the left soft key: power off, hold it, connect USB. The device data (Bluetooth
NV records, DSP firmware) comes from a NAND backup of the phone through FPLinux's normal
device-data step.

## Status

See [docs/bringup-status.md](docs/bringup-status.md). Short version, on one phone: the whale die
powers on (with `stock_early=1`), both LTE XDSP cores load and report ready, and releasing the
LTE DSP hangs the whole AP about 1.1 s later with no panic and no sign that the DSP ever ran.

## Known problems

- The radio driver is a research driver, full of switches and log lines, and `load_lte` will
  hang the phone.
- The 48 MB bootstrap layout commit on the branch changes the layout for every UMS9117 target.
  Upstream needs a per-target layout first.
- On my RAM-boot setup `/lib/firmware` comes up empty, so I copy the DSP firmware in by hand
  after each boot (`rig/push_firmware.sh`).
- The NAND OOB size (64) in the storage commit is an assumption.
- Only tested on one F286.

## Licence

MIT, see [LICENSE](LICENSE). The kernel and FPLinux-side code is GPL-2.0-only and lives in the
fork.
