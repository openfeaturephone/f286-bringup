# f286-radio driver interface

`platforms/ums9117/linux/drivers/soc/ums9117/f286-radio.c` on the `ruio-f286` branch of
[openfeaturephone/fplinux](https://github.com/openfeaturephone/fplinux/tree/ruio-f286), compatible
`fplinux,ums9117-f286-radio`, built in with `CONFIG_F286_RADIO_UMS9117=y`. The DT
node is reg-less (`radio@0` under `soc`); the driver hard-codes its register
windows like the rest of the F286 port. It touches no hardware until you write
an attribute.

Device directory: `/sys/bus/platform/devices/soc:radio@0`. Probe creates only
`init`; the rest appear one at a time about 60 s after boot, each with a log line
before it, so a hang names the attribute that caused it.

## Attributes

| name | mode | what |
|---|---|---|
| `init` | W | map the MMIO blocks and both DSP AP windows, mask the unclaimed baseband IRQs in the GIC distributor |
| `radio_power` | W | whale-die power-on (runs in a kthread; the store returns at once). Result in dmesg and `status` |
| `load_lte` | W | LTE DSP boot and a 90 s poll (kthread). Starts the response pump |
| `load_gsm` | W | GSM DSP boot and a 90 s poll (kthread) |
| `status` | R | `state=`, PMU c0/c4/d4 decoded, both queue ctl words, loader/powerer state |
| `queue` | R bin | 0x3800 bytes: GSM window +0xe400 (0x1c00), then LTE window +0xe400 (0x1c00) |
| `window_gsm`, `window_lte` | R bin | the whole 0x12000-byte AP window; byte N = 0x800e0000 + N or 0x80400000 + N |
| `axidump` | R | PUB AXI debug latches (0x300e0000), PMU/AON DSP registers, whale AHB mirror |
| `gatetest` | W | one-shot: set AUTO_GATE_CTRL0 b7:6 (0x402e1140 = 0xc0) and retry the whale-APB clock writes |

Writing anything other than `1` to the W attributes returns -EINVAL; a second
load while one runs, or a load during `radio_power`, returns -EBUSY.

`status` fields: c0[4:0] WHu3g, [9:5] and [14:10] the two DSP power fields,
[19:15] CM4 (0 = on, 7 = off); c4[4:0] WTLCP (want 0); d4[7:4] whale FSM (want
6 = ready). A queue ctl of 0 means the DSP never touched the ring.

## Module parameters

All under `/sys/module/f286_radio/parameters/`; the 0644 ones can change between
loads.

| name | default | what |
|---|---|---|
| `stock_early` | 0 | add the remaining stock first-second writes to `radio_power` (needed for WTLCP to come up on my phone) |
| `xdsp_cores` | 0 | boot the LTE XDSP cores A/B from `f286-xdsp*.bin` before the LTE release |
| `unlock` | 1 | keyed 0x40310008 \|= 4 plus 0x402E1020/2020/1134 after step-0 |
| `boot_addr_mode` | 0 | 0 = entry >> 1 (stock), 1 = raw entry, 2 = (entry >> 1) + 4 |
| `send_cmd6` | 0 | send the cmdId 6 template after release (stock does not) |
| `hdr_layout` | 1 | headered message layout: 0 = {id, 0x5555, 0}, 1 = {id, 0x5555, id} |
| `xdsp_mux` | 2 | 0x402D02B4 selector (2 = TWPLL_51M2; > 3 skips the write) |
| `dieclk` | 0 | try the whale die-local clock release (0xD15D/E) in a separate kthread. Has hung the bus |
| `e010_light` | 0 | touch the 0xE010100C/1014 light path. Has hung the bus |
| `gsm_iram` | 0 | `load_gsm` also writes the stock IRAM cells at 0x10c00 and 0x11d30 |
| `stage` | 0 | (0444) loads stop right after mapping their window |
| `autoint` / `no_autoint` | 0 / 0 | (0444) run `init` from the deferred work at +60 s |

## Memory map the driver uses

Linux RAM starts at 0x80760000. Below it:

| range | what |
|---|---|
| 0x800e0000 + 0x12000 | GSM DSP AP window (queue at +0xe400) |
| 0x800f9670 | GSM param block (run word at +0x634, rf_open/rf_close at +0x770/+0x772) |
| 0x80100000 + 0x300000 | GSM DSP image (entry +0x80) |
| 0x80400000 + 0x12000 | LTE DSP AP window |
| 0x80447000 + 0x1000 | handshake cells: awake word +0xc40, nv9\|tick staging +0xe00, sleep sync +0xe14 |
| 0x80448000 + 0x2e4000 | LTE DSP image (entry 0x80448400) |
| 0x80678000 | where stock puts the LTE RF calibration (the driver does not) |
| 0x8073b000 + 0x25000 | 4 x 0x2800 split-message slots, then 10 x 0x2800 UL-data slots |

Offsets inside a window dump (LTE unless noted): command ctl 0xe400 (u16 in,
u16 out), 8 command entries at 0xe404 ({u16 cmdId, u16 len, u32 end_off}), L2
0xe444, response ctl 0xe478/0xe47a, 12 response entries at 0xe47c ({u16 type,
u16 len, u32 addr}, payload at 0xfbec + addr), command bodies 0xe4e0..0xf160,
sleep/resume sync 0xe14 (the DSP writes 0x22222222), alive-check dump word
0x10bf8 (stock writes 0x33 when its 30 s alive check times out).

Response type 5 is the DSP's version message (the handshake), type 7 an assert.
The DSP polls the command ring; there is no doorbell.
