# Test rig

How I run radio experiments on a Ruio F286 with FPLinux. Everything here is done
at runtime over SSH; nothing is written to the phone's flash. Each hang costs a
battery pull and a fresh RAM boot, so the rig is built around keeping the last
log lines.

## Booting

Build and boot FPLinux for the target as usual (see the top-level README). The
runner prints the phone's address and leaves an SSH key for the session; the
rig scripts take them from the environment:

```
export PHONE=root@<address the runner printed>
export SSH_OPTS="-i /path/to/the/session/id_ed25519 -o StrictHostKeyChecking=no"
```

The radio driver creates its attributes about 60 s after boot (the probe only
schedules deferred work), so wait for `/sys/bus/platform/devices/soc:radio@0/radio_power`
to appear. `rig/stock_early_test.sh` does that wait for you.

## DSP firmware: copy it in on every boot

The DSP firmware is not in this repository and never will be. Cut it out of a
NAND backup of your own phone:

```
python3 tools/extract_dsp_fw.py --gsm p_10000006.bin --lte p_10000005.bin \
    --kernel kernel.bin -o fw/
```

The device-data path in the FPLinux build is supposed to deliver these files,
but on the RAM-booted image I use, `/lib/firmware` comes up empty. Its root is a
writable overlay in RAM, so I push the files after each boot:

```
bash rig/push_firmware.sh fw/
```

They land in `/lib/firmware/ums9117/`, where the driver reads them
(`f286-lte-dsp.bin`, `f286-gsm-dsp.bin`, `f286-xdsp{A,B}-{boot,main}.bin`).
The two big images are read with `filp_open` in 128 KB chunks, not with
`request_firmware`: from the load kthread `request_firmware` pended for good, and
a whole-file allocation on a 36 MB phone with zram swap can deadlock in the zram
compressor. If a load ever sits in D state in `zcomp_stream_get`, only a reboot
helps.

## Kernel log: stream /dev/kmsg

BusyBox `dmesg` has no `-w`, and a hard hang leaves nothing to read afterwards.
Stream `/dev/kmsg` to the PC in a second terminal before poking anything:

```
bash rig/kmsg_stream.sh lte_try1
```

The log goes to `logs/stream_<time>_lte_try1.log`. The phone's screen shows
`fplinux-terminal` (a user-space terminal on DRM, `/dev/pts/0`), not the kernel
console. `MIRROR=1 bash rig/kmsg_stream.sh` copies every line to `/dev/pts/0` as
well, which helps when the USB link dies first.

## Running a sequence

```
bash rig/stock_early_test.sh        # stock_early=1 + radio_power, prints status
bash rig/boot_sequence.sh           # radio_power, load_lte, status, dmesg snapshots
```

Or by hand on the phone:

```
R=/sys/bus/platform/devices/soc:radio@0
P=/sys/module/f286_radio/parameters
echo 1 > $P/stock_early
echo 1 > $P/xdsp_cores
echo 1 > $R/radio_power; sleep 5; cat $R/status
echo 1 > $R/init
echo 1 > $R/load_lte
```

See `docs/radio-driver.md` for every attribute and parameter.

To watch what the DSP writes after release, copy `tools/phone/memsample` to the
phone and start it before `load_lte`; it arms itself on the boot-address write
and prints every changed word:

```
make -C tools/phone
ssh $SSH_OPTS $PHONE "cat > /tmp/memsample && chmod +x /tmp/memsample" < tools/phone/memsample
ssh $SSH_OPTS $PHONE /tmp/memsample > logs/memsample.log
```

## Gotchas

- Git Bash path rewriting. Git Bash turns a lone argument like `/tmp/x` into
  `C:/Program Files/Git/tmp/x` before ssh sees it. Set `MSYS_NO_PATHCONV=1`
  (`rig/phone.sh` does).
- Battery pull. After a hang the phone does not reset by itself. Pull the
  battery, then RAM-boot again. Reading DRAM after a hang needs the 7 s
  power-key reset path in `ramboot/ramdump_after_hang.cmd`.
- /dev/mem pointers. When reading memory on a running phone, check every
  address against the current kernel and range-check pointers you follow. A
  stale address once reset the phone. On stock firmware, reading a clock-gated
  peripheral reboots the phone and any read in the CM4 range (0x5082_0000 and
  up) hangs it.
- Bus hangs. The whale die-local pages (0xD15D/E0000) and the 0xE010xxxx
  light-path page have hung the bus from Linux. The driver keeps them behind
  `dieclk` and `e010_light` (default off). Try them one access per boot.
