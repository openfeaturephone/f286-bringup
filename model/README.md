# C++ model of the stock F286 modem paths

Host-compilable C++17 transcriptions of what the stock Ruio F286 (UMS9117) kernel
does to bring up and talk to its baseband: PMU power domains, the whale die,
clocks, DSP firmware load and boot, the AP<->DSP command rings, and the SIM, SMS
and audio paths. I wrote these while reading the stock kernel's disassembly, as a
checkable reference for the f286-radio Linux driver. They are not a driver and
nothing here talks to hardware: every unit goes through a small bus interface
(`f286_bus.h`, `IL1Window`, `DspHw`) that a test double or a real driver can
implement.

Every function names the stock kernel VA it mirrors (VA = kernel.bin file offset
+ 0x80a05e00). Tags in the comments: `[V]` checked against the disassembly at
that address, `[I]` inferred (offsets real, meaning deduced), `[SPEC]` taken from
the 3GPP/ISO spec because no stock code for it could be found.

## Files

| files | namespace | what |
|---|---|---|
| `f286_bus.h` | `f286::power` | MMIO bus abstraction and the `Confidence` result type |
| `pmu.*` | `f286::power` | PMU_APB domains: LTE pre-boot release 0x80D0FA7C (the four CLEAR writes incl. 0x402b2034 = 0x03000000), GSM twin, full sweep 0x80D0FAA0, DSP power-release HAL 0x811D0D96, WHu3g/WTLCP polls |
| `whale.*` | `f286::power` | whale-die power: entry 0x80D0F4C2, ensure-on 0x80D0F2FA, light path, die clock block (marked AP-unreachable: it hung the bus on hardware), full init 0x80D0F186 |
| `clocks.*` | `f286::power` | AON gate pairs, 0x402e006c \|= 0x9620, XDSP mux 0x402D02B4, TWPLL ops, RF-gate helpers |
| `efuse.*` | `f286::power` | chip-id read and CM4 image select |
| `fw_loader.*` | `f286::dsp` | stock firmware loader 0x80c2eee6: CM4 0x80020000/0xc0000, GSM 0x80100000/0x300000, LTE 0x80448000/0x2e4000 |
| `boot_hal.*` | `f286::dsp` | DSP boot HAL 0x811d0c98..0x811d0e4f: resets, boot address (written verbatim, the caller shifts), start control, power release |
| `boot_flow.*` | `f286::dsp` | LTE boot 0x80e2d9e6 and GSM boot 0x810b33fe step by step |
| `l1ring.*` | `f286::dsp` | command ring at win+0xe400, response ring at win+0xe478, split slots |
| `messages.*`, `codec.*` | `f286::l1` | typed command catalogue with the stock wire sizes, packers and decoders |
| `l1_client.*`, `sequences.*` | `f286::l1` | ring client, handshake, cell search and other request/response sequences |
| `sim_path.*` | `f286::sim` | USIM: AP-direct controller, activation, ATR parse, T=0 transport |
| `sms_path.*` | `f286::sms` | GSM7 tables, TPDU codec, CP/RP layers, transport binding |
| `audio_path.*` | `f286::audio` | voice path command rings and senders |

## Building

```
g++ -std=c++17 -Wall -Wextra -c *.cpp
g++ -std=c++17 -o sms_selftest sms_selftest.cpp sms_path.cpp sim_path.cpp && ./sms_selftest
g++ -std=c++17 -o audio_selftest audio_selftest.cpp audio_path.cpp && ./audio_selftest
```

`python -m ziglang c++` works in place of `g++` on Windows. Both self-tests print
`ALL PASS` on my machine; they cover the codecs and ring bookkeeping only, not
anything that needs a phone. The phone numbers in `sms_selftest.cpp` are from
fictional ranges.

## Known problems

- The audio and SMS layers are the least checked. `[I]` marks are frequent there.
- Some older comments quote VAs 0x38 lower than kernel.bin VAs (a disassembler
  base mismatch). Where it matters both are given; everything else should be in
  kernel.bin space.
- RRC/NAS and everything above L1 is not modelled.
