// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// audio_path.h - f286::audio: the voice/audio data path between the A7 and the
// GSM DSP ("8800g") of the F286 (Unisoc UMS9117, Mocor FM_BASE_20A_THIN_MODEM).
//
// Ground truth: kernel.bin, VA = file_off + 0x80a05e00 (payload code starts at
// file 0x200 = VA 0x80A06000). Mapping settled via
// absolute payload pointers (u32 0x8152C1C6 = the UCS2-table VA appears twice
// as a link-time pointer; the 0x38-lower value appears nowhere). Every VA below
// is a true kernel.bin VA, re-disassembled at the cited address (earlier
// notes quoted VAs 0x38 too low).
//
// The picture for a Linux port:
//   * The DSP runs the vocoder (AMR-NB/AMR-WB, 20 ms frames), tones and the
//     ADC/DAC front end. The AP (Linux) is only manager + speech FX (LVVE on
//     stock) + RTP. There is NO separate audio DSP ("AGDSP": 0 string hits).
//   * All AP<->DSP audio traffic is shared-memory rings with 12-byte entries
//     {u32 w0; u16 a,b,c,d} - no doorbell IRQ: stock notifies with an RTOS
//     event give after every put; a bare port polls the ring counters.
//   * Windows (AP VAs):
//       BASE       = 0x800E0000  GSM DSP window   (getter 0x811A76CA -> b.w
//                    0x811D0CA0 -> ldr [0x811D0E78] = 0x800E0000) [V]
//       baseB      = 0x10C00     AudCmdQ window   (getter 0x811A76CE -> b.w
//                    0x811D0CA8 -> mov.w r0,#0x10C00) [V] - physical alias TBD
//       BASE_AUDIO = BASE + 0x2A8000 = 0x80388000 [I - UNPROVEN: the constant
//                    0x2A8000 appears NOWHERE in kernel.bin; the cited
//                    "aud_share_init 0x80EEF004 add.w r2,r0,#0x2A8000" does not
//                    exist (0x80EEF004 is a wrap-6 chunk-put loop). Ring offsets
//                    stay instruction-verified RELATIVE to [dev+0x34]; confirm
//                    the absolute base by readback-diff on HW before trusting]
//   * Device struct (stock RAM 0x81712BAC, pool literal 0x80EEEC88) [V]:
//       +0x18 td_sharedmem = BASE+0x1F300   +0x24 BASE  +0x28 baseB
//       +0x2C BASE+0x1C000   +0x30 BASE+0x1EC90   +0x34 BASE_AUDIO
//
// FPLinux note: current ruio-f286 DT keeps RAM at 0x80760000-0x82dfffff
// (fix: base moved off 0x80740000, which collided with the LTE
// UL-data slots [0x80745000,0x8075E000)), so
// BASE_AUDIO 0x80388000 is device memory now (the earlier "inside System RAM"
// warning applies to the older 0x80200000 RAM start). Map it with ioremap.

#pragma once

#include <cstdint>
#include <cstddef>

namespace f286 {
namespace audio {

// ---------------------------------------------------------------------------
// Geometry (all [V] unless tagged [I])
// ---------------------------------------------------------------------------
constexpr uint32_t kGsmDspBase   = 0x800E0000;  // BASE   (GSM DSP AP window)
constexpr uint32_t kAudCmdQBase  = 0x00010C00;  // baseB  (phys alias [I])
constexpr uint32_t kAudioBase    = 0x80388000;  // BASE_AUDIO = BASE+0x2A8000 [I]

// AUD msg/cmd ring - AP -> DSP, 20 x 12 B. THE command ring: every sender of
// the 0x80B3BB7C cluster calls the gated+mutexed put 0x80EF14D6 [V], and the
// raw put 0x80B1957E [V] (used by the set_volume glue) hits the same ring:
//   r4 = [pool] (= BASE_AUDIO holder) + 0x1000; in-count u32 @ [r4+0xC94],
//   entry[i] @ BASE_AUDIO + 0x1BA4 + i*12, wrap `if (++i >= 0x14) i -= 0x14`,
//   write-and-verify on the counter, mutex 0x81099978/0x81099928 around it,
//   full check in==out at [r4+0xC94] vs [r4+0xC98].
constexpr uint32_t kMsgRingOff      = 0x1BA4;   // entry[20], stride 12 [V]
constexpr uint32_t kMsgRingSlots    = 20;       // wrap 0x14 [V]
constexpr uint32_t kMsgRingInOff    = 0x1C94;   // u32 AP write index [V]
constexpr uint32_t kMsgRingOutOff   = 0x1C98;   // u32 DSP read index [V]

// DSP resp/notify ring - DSP -> AP, 10 x 12 B.  get+trace 0x80EF18DE [V]:
//   in u32 @ BASE_AUDIO+0x1D14, out u32 @ +0x1D18 (read32-stable helper
//   0x80EEEA92, write32-verify 0x80EEEAAC), entry base +0x1C9C, wrap %10
//   (udiv/mls). Empty => out==in; the get advances +0x1D18 (consumer).
constexpr uint32_t kRespRingOff     = 0x1C9C;   // entry[10], stride 12 [V]
constexpr uint32_t kRespRingSlots   = 10;       // % 10 [V]
constexpr uint32_t kRespRingInOff   = 0x1D14;   // u32 DSP write index [V]
constexpr uint32_t kRespRingOutOff  = 0x1D18;   // u32 AP read index [V]

// baseB AudCmdQ - AP -> DSP fast command ring, 8 x 12 B, guarded put
// 0x80EF1418 [V]. NOT the ring the 13 L1 command senders use (they all
// call 0x80EF14D6 into the BASE_AUDIO ring above); stock callers of this ring
// include the id-0x25 sender (0x80B3C672). Shape:
//   entry[i] @ baseB + 0x70C + i*12 (12 bytes copied verbatim), in u16 @ +0x76E
//   (read16-stable 0x80EEE88E), out u16 @ +0x76C, idx = (in+1) & 7, counter
//   written with write16-verify 0x80EFBD32. Not-ready/full paths 0x80EF139E /
//   0x80EF12A2 set flag u32 @ baseB+0xDD0. After every put the stock tail is
//   0x80DCCEE4 = {mutex take 0x81099978; OSA event give(0,2) 0x811BEFDE; give}.
constexpr uint32_t kQEntryOff       = 0x070C;   // entry[8], stride 12 [V]
constexpr uint32_t kQSlots          = 8;        // & 7 [V]
constexpr uint32_t kQOutOff         = 0x076C;   // u16 [V]
constexpr uint32_t kQInOff          = 0x076E;   // u16 [V]
constexpr uint32_t kQNotReadyFlag   = 0x0DD0;   // u32, set on full/not-ready [V]

// baseB AudCmdQ resp ring - DSP -> AP, 8 x 12 B, get 0x80EF1852 [V]:
//   entry[i] @ baseB + 0x770 + i*12; PRODUCER (DSP) u16 @ +0x7D2 - read-only
//   on the AP side; CONSUMER (AP) u16 @ +0x7D0 - the get advances it:
//   empty iff consumer == producer, idx = (consumer+1) & 7, copy entry,
//   write16-verify(+0x7D0, idx) (0x80EF1852: addw r0,r0,#0x7d0; bl 0x80EFBD32).
constexpr uint32_t kQRespEntryOff   = 0x0770;   // entry[8], stride 12 [V]
constexpr uint32_t kQRespConsOff    = 0x07D0;   // u16 AP/consumer (we advance) [V]
constexpr uint32_t kQRespProdOff    = 0x07D2;   // u16 DSP/producer (read-only) [V]

// PCM burst ring next to the AudCmdQ - 188 B records (47 halfwords), inline in
// 0x80EF1738-0x80EF1850 (memcpy 0x8109DAD8) [V offsets, semantics partly [I]].
constexpr uint32_t kPcmRingOff      = 0x08B8;   // records, stride 188 [V]
constexpr uint32_t kPcmRecordBytes  = 188;      // 47 halfwords [V]
constexpr uint32_t kPcmOutOff       = 0x0C64;   // u16 [V]
constexpr uint32_t kPcmInOff        = 0x0C66;   // u16 [V]

// "AudioPara" ring on the BASE window - 5 x 188 B @ BASE+0x604, in @ +0x9B0
// (alloc-error string "AUD_NEW:AudioParaBuffIndex return value is err!"
// 0x80EF530C); DSP-parameter block download (mode NV -> DSP).
constexpr uint32_t kAudioParaOff    = 0x0604;   // 5 x 188 B [I - abs-base claim
                                                // unproven; offset verified
                                                // rel. dev+0x34 window]
constexpr uint32_t kAudioParaSlots  = 5;        // [I]
constexpr uint32_t kAudioParaInOff  = 0x09B0;   // [V]
// Free-space helper 0x80EEEBD8 [V]: counters u16 @ baseB+0x3A8 / +0x3AA,
// quantum constants 0x1A / 0x17 / 0x52E (0x52E = halfwords in 0xA5C bytes =
// the large PCM circ buffer quantum; meaning otherwise [I]).
constexpr uint32_t kFreeInOff       = 0x03A8;   // u16 [V]
constexpr uint32_t kFreeOutOff      = 0x03AA;   // u16 [V]
constexpr uint32_t kCircQuantum     = 0x52E;    // halfwords [V const]
constexpr uint32_t kCircWrapBytes   = 0x297000; // large PCM circ buffer wrap [V]

// MIDI/tone chunk ring - AP -> DSP tone generator, 7 x 0x500 B @
// BASE_AUDIO+0x45B4; put 0x80EF4840 [V]: <= 0x280 halfwords per chunk
// ("Midi count is too great!" 0x80EF4A38), len stored as (len<<1)&0xFFFF.
constexpr uint32_t kMidiRingOff     = 0x45B4;   // 7 x 0x500 B [V]
constexpr uint32_t kMidiChunkBytes  = 0x500;    // [V]
constexpr uint32_t kMidiChunkHw     = 0x280;    // max halfwords per put [V]
constexpr uint32_t kMidiIdxOff      = 0x1D9E;   // u16 idx @ BASE_AUDIO [V]
constexpr uint32_t kMidiCntOff      = 0x1DA0;   // u16 cnt pair [V]

// Zero-init ranges performed by 0x80EEE990 [V] - only with the DSP held in
// reset (0x800E0000 head is the live GSM DSP window).
constexpr uint32_t kZeroBaseBBytes  = 0xE00;    // memset(baseB, 0, 0xE00)
constexpr uint32_t kZeroBaseBytes   = 0x9CC;    // memset(BASE, 0, 0x9CC)
constexpr uint32_t kZeroTdBytes     = 0xB2C;    // memset(BASE+0x1F300, 0, 0xB2C)
constexpr uint32_t kTdSharedOff     = 0x1F300;  // dev+0x18 [V]
// GSM/TD reconfig 0x80B1E2D4 mode 0: additionally zeroes 0xB8C @ BASE+0x1A800.
constexpr uint32_t kReconfZeroOff   = 0x1A800;  // 0x801BA800 [V]
constexpr uint32_t kReconfZeroBytes = 0xB8C;    // [V]

// Analog codec + audio-clock reach (ADI bus; SPRD_CODEC_TASK driver) [V]:
//   fn 0x80A5DA3A: ADI RMW  0x40608C08 |= 0x10   (pool 0x40608C14 @0x80A5DB5C,
//                  `subs #0xC`; no-op when arg==0; ADI helpers
//                  read 0x81218546 / write 0x81218598)
//   fn 0x80A5DA12: ADI RMW  0x40608C0C |= 3 (enable) / &= ~3 (disable)
constexpr uint32_t kAdiCodecEnableA = 0x40608C08;  // |= 0x10
constexpr uint32_t kAdiCodecEnableB = 0x40608C0C;  // |= 3 / &= ~3
// Audio-clock/PLL block: runtime base pointer RAM 0x8170DB54, fields +0x30
// (mux 0x70), +0x34/+0x38 dividers, +0x3C mux, +0x78 bit 0x200 ready poll
// [V disasm 0x80B5F884-0x80B5FBC8]. Correction: the init
// data at 0x8170DB54 holds 0x40608700, so the slave registers are the
// 0x40608730-0x40608778 family (the old "0x40608030-style" reading was
// wrong; literal 0x40608000 has 0 word hits in kernel.bin). Live
// readback-diff of [0x8170DB54] and each +0x30/+0x34/+0x38/+0x3C/+0x78 is
// still mandatory before ANY write (the stock runtime may rewrite the
// pointer; PLL writes here can glitch the analog audio clock).

// ---------------------------------------------------------------------------
// Ring entry - the 12-byte record used by every ring.  w0 = command id /
// status family, then four u16 args.  No timestamps, no stream id, no sample
// header: sequencing is the shared in/out counters themselves.  Bulk PCM and
// coded frames travel in the 188 B records / circ buffers, not in this entry.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct Entry12 {                  // 12 bytes [V - copied verbatim by put]
    uint32_t w0;                  // +0x00 cmd id (AP->DSP) / status family
    uint16_t a;                   // +0x04
    uint16_t b;                   // +0x06
    uint16_t c;                   // +0x08
    uint16_t d;                   // +0x0A
};
static_assert(sizeof(Entry12) == 12, "audio ring entry must be 12 bytes");
#pragma pack(pop)

// Stock raw put 0x80B1957E takes 5 args and scatters them (r0->w0, r1->b,
// r2->a, r3->d, stack->c); the guarded put 0x80EF1418 and the cluster put
// 0x80EF14D6 copy an Entry12 verbatim from [r0]. This port always builds an
// Entry12 and applies the scatter only where stock used the raw put
// (set_volume: {2, a=vol, b=vol, c=0, d=vol}).

// Command ids - w0 of the 12-B entry; senders in 0x80B3BB7C-0x80B3C8E8 [V]:
// each sender builds the entry on the stack and calls the cluster put
// 0x80EF14D6 (BASE_AUDIO ring; BL sites 0x80B3BC04/0x80B3BC34/0x80B3BE7E/
// 0x80B3BFD2/0x80B3C02C/0x80B3C4C8/0x80B3C58C/0x80B3C5EA/0x80B3C620/0x80B3C652
// ...). The live id space is >= 17 ids (0xE/0xF/0x27 also appear) - expect
// unknown-id responses.
enum class CmdId : uint16_t {
    AudEnable    = 0x0,  // CmdAudEnable 0x80B3BB7C: {0, a=p2, b=p1, c=0,
                         // d=0x8108F4F0}; the audio_param_transfered byte
                         // (RAM 0x8170609E, ldrb [pool+2] @0x80B3BB9C) only
                         // triggers a log - stock always sends [V]
    CodecEnable  = 0x1,  // 0x80B3BC0C: {1, a=p2, b=p1} [V]
    SetVolume    = 0x2,  // 0x80B3BDB2: {2, a=0, b=vol}; glue 0x80B2E632 sends
                         // the raw-put form {2, vol, vol, 0, vol} instead [V]
    STone        = 0x4,  // 0x80B3BF92: {4, a=arg1, b=arg0, d=arg2} [V]
    GTone        = 0x5,  // 0x80B3BFDA: {5, a=arg1, b=arg0, c=arg3, d=arg2} [V]
    StopTone     = 0x6,  // 0x80B3C00C: {6,0,0,0,0} [V]
    MuteEnable   = 0x7,  // 0x80B3BE56: {7, a=arg1, b=arg0} [V]
    DlinkMute    = 0x8,  // 0x80B3C594: {8, a=arg1, b=arg0} [V]
    VoiceUL      = 0x9,  // 0x80B3C5F0: {9, a=arg1, b=arg0, c=arg3, d=arg2} [V]
    VoiceLB      = 0xA,  // 0x80B3C55C: {A, a=arg1, b=arg0, c=arg3, d=arg2} [V]
    VoiceDL      = 0xB,  // 0x80B3C626: {B, a=arg1, b=arg0, c=arg3, d=arg2} [V]
    AudLoop      = 0xC,  // 0x80B3C4A4: {C, a=0, b=mode} [V]
    EchoT        = 0xD,  // 0x80B3C5C2: {D, a=arg1, b=arg0} [V]
    Unknown13    = 0x13, // 0x80B3BDDC [V]
    Unknown22    = 0x22, // 0x80B3C47A [V]
    Unknown33    = 0x33, // 0x80B3BD48 [V]
    Unknown34    = 0x34, // 0x80B3BD66 [V] (candidate)
};

// AMR-NB / AMR-WB frame sizes (bytes, one 20 ms frame; RTP octet-aligned).
// Stock keeps the RTP packetizer tables in init data (AMR-WB @0x817567B4,
// octet-aligned @0x817567D4) [V]; the codec itself is DSP-side (no AMR
// codebook or NB size table in ANY partition).
constexpr uint8_t kAmrNbSize[9] = {12, 13, 15, 17, 19, 20, 26, 31, 5};  // +SID
// Correction: stock init-data AMR-WB table @0x817567B4 is
// {17,23,32,36,40,46,50,58,60} - FT8 = 23.85 kbps = 60 B, not 4. RFC 4867
// AMR-WB SID is ~6 B; handle SID separately, do not ship the old 4.
constexpr uint8_t kAmrWbSize[9] = {17, 23, 32, 36, 40, 46, 50, 58, 60};  // FT0-8; SID separate

// Hardware abstraction - same idiom as f286::l1::IL1Window. The two windows
// are addressed window-relative: the GSM DSP window is mapped from
// 0x800E0000 (BASE_AUDIO 0x80388000 = +0x2A8000 inside it), the AudCmdQ
// window from 0x10C00 (physical identity to be established on HW).
class IAudioWindow {
public:
    virtual ~IAudioWindow() = default;
    enum class Win : uint8_t { GsmDsp = 0, AudCmdQ = 1 };

    virtual bool read (Win w, uint32_t off, void* dst, size_t len) = 0;
    virtual bool write(Win w, uint32_t off, const void* src, size_t len) = 0;
    // Analog-die ADI bus (0x4060xxxx): read-modify-write happens above.
    virtual bool adi_read (uint32_t addr, uint32_t* val) = 0;
    virtual bool adi_write(uint32_t addr, uint32_t val) = 0;
    virtual void memset_win(Win w, uint32_t off, uint8_t v, size_t len) = 0;
    virtual void sleep_ms(uint32_t ms) = 0;
    virtual uint32_t now_ms() = 0;
    // Stock card-log trace 0x81174FC0 (global 16-B records {type,u32,u32,tick},
    // index wraps 0x2710, ctl 0x81702074) - 0xEEEE AudCmdQ fetch, 0xBBBB
    // counter, 0xEAEA voice-data fetch, 0xA840/0xD840 cmd-ring in/out.
    virtual void trace(uint16_t type, uint32_t v0, uint32_t v1) { (void)type; (void)v0; (void)v1; }
};

// Optional gate mirror of the stock AP-side L1-state checks (0x80B18204 /
// 0x80B18048 / 0x81006FA8 before every guarded put) and the
// audio_param_transfered byte (RAM 0x8170609E - log-only for CmdAudEnable).
// aud_enable_d mirrors the CmdAudEnable d-field getter 0x8108F4F0 (returns
// [entity_tbl[cur]+0x434] == 1); a bare port can return 0.
struct Gates {
    bool (*l1_audio_active)() = nullptr;   // guarded-put preamble
    bool audio_param_transfered = false;   // sequence-level guard (call_audio_start)
    uint16_t (*aud_enable_d)() = nullptr;  // CmdAudEnable d field (0x8108F4F0)
};

class AudioPath {
public:
    explicit AudioPath(IAudioWindow& hw, Gates gates = {}) : hw_(hw), gates_(gates) {}

    // One-time share-memory init, stock aud_share_init 0x80EEF004 + zero-init
    // 0x80EEE990. `dsp_in_reset` must be true: the ranges overlap the live DSP
    // window head.
    bool share_init(bool dsp_in_reset);

    // --- low-level ring ops (stock semantics: stable reads / verify writes,
    //     counters wrapped exactly like stock) ---
    bool cluster_put(const Entry12& e);    // 0x80EF14D6: THE command put
                                           // (20-slot BASE_AUDIO ring)
    bool put_msg(const Entry12& e);        // raw put 0x80B1957E (same ring,
                                           // 5-arg scatter already applied)
    bool get_resp(Entry12* out);           // 10-slot DSP->AP, 0x80EF18DE flow
    bool audcmdq_put(const Entry12& e);    // 8-slot baseB, 0x80EF1418 flow
                                           // (kept; NOT used by the senders)
    bool audcmdq_resp_get(Entry12* out);   // 8-slot, 0x80EF1852 flow

    // --- command senders (fields re-derived from each stock wrapper;
    //     all route through cluster_put 0x80EF14D6 like stock) ---
    bool cmd_aud_enable(uint16_t p1, uint16_t p2);     // {0, p2, p1, 0, d}
    bool cmd_codec_enable(uint16_t p1, uint16_t p2);   // {1, p2, p1, 0, 0}
    bool cmd_set_volume(uint16_t vol);                 // {2, 0, vol, 0, 0}
    bool cmd_stone(uint16_t a, uint16_t b, uint16_t c);// {4, b, a, 0, c}
    bool cmd_gtone(uint16_t a, uint16_t b, uint16_t c, uint16_t d); // {5, b, a, d, c}
    bool cmd_stop_tone();                              // {6, 0, 0, 0, 0}
    bool cmd_mute_enable(uint16_t a, uint16_t b);      // {7, b, a, 0, 0}
    bool cmd_dlink_mute(uint16_t a, uint16_t b);       // {8, b, a, 0, 0}
    bool cmd_voice_ul(uint16_t a, uint16_t b, uint16_t c, uint16_t d); // {9, b, a, d, c}
    bool cmd_voice_lb(uint16_t a, uint16_t b, uint16_t c, uint16_t d); // {A, b, a, d, c}
    bool cmd_voice_dl(uint16_t a, uint16_t b, uint16_t c, uint16_t d); // {B, b, a, d, c}
    bool cmd_aud_loop(uint16_t mode);                  // {C, 0, mode, 0, 0}
    bool cmd_echo_t(uint16_t a, uint16_t b);           // {D, b, a, 0, 0}

    // --- analog front end (SPRD_CODEC_TASK equivalents) ---
    bool codec_analog_enable(bool on);
    // Audio-clock block programming is intentionally NOT wrapped here: its ADI
    // absolute base is UNPROVEN. Bring-up order:
    // readback-diff the base pointer value first, then mirror +0x30..+0x78.

    // --- call sequence ---
    // Stock order when a call starts:
    //   1. codec/AMR mode parameters reach the DSP via the CAL CHANNEL of the
    //      L1 client (cal signals 0x3D1 param / 0x3D2 mute; per-mode 0x408 B
      //    arrays, HandleAMRConfigReqMsg/ConfigAMRInitParam); this sets the
    //      audio_param_transfered gate.
    //   2. codec_analog_enable(true)
    //   3. cmd_aud_enable(1,1) -> cmd_codec_enable(...) - starts the DSP
    //      audio path (tone rsp is event bit 0x80; wait_tone_rsp polls here).
    //   4. cmd_voice_ul(...) + cmd_voice_dl(...) - start encode/decode.
    // UL frames then arrive in the resp ring / PCM records; DL frames are
    // pushed with audcmdq/PCM bursts.
    enum class Step { Ok, Gate, RingFull, HwError };
    Step call_audio_start(uint16_t ul_a, uint16_t ul_b, uint16_t dl_a,
                          uint16_t dl_b);
    void   call_audio_stop();                          // disable both cmd 1/0

    // Volume/mute (AT+CLVL chain; AUDMOD_SetVolume 0x8122E17A -> glue
    // 0x80B2E632 [V re-verified ]: clamps vol to <= 9, then
    // raw-put call at 0x80B2E728 with (r0=2, r1=vol, r2=vol, r3=vol,
    // stack=0) => Entry12 {2, a=vol, b=vol, c=0, d=vol} via the scatter.
    // The cal-channel alternative (signal 0x3D1 sub 0x5D, u16 vol @+0x10) is
    // used when the DSP-audio flag is clear and is NOT reproduced here).
    static constexpr uint16_t kMaxVolume = 9;
    bool set_volume(uint16_t vol);
    bool set_ulink_mute(bool on);    // cmd 7
    bool set_downlink_mute(bool on); // cmd 8

    // Tone rsp: stock waits on OSA event bit 0x80 with 0x3E8 ms timeout
    // (0x80B182DC, then asserts "L1 event not arrived!" @0x80B183D0); a bare
    // port polls the resp ring instead.
    bool wait_tone_rsp(uint32_t timeout_ms);

    // PCM movement. UL: copy one 188 B record out of the baseB PCM ring.
    // DL: push <= 0x280 halfwords as a MIDI-style burst is for tones only;
    // coded-voice DL frames go through the same resp/pcm machinery - the
    // per-frame field semantics are the open gap.
    bool pcm_ul_read(uint8_t* dst188);
    bool pcm_free_space(uint16_t* halfwords);

    // Tone/melody download to the DSP tone generator (0x80EF4840 semantics).
    bool midi_put(const uint16_t* halfwords, uint16_t count);

    static constexpr size_t kEntryBytes = 12;
    static constexpr size_t kPcmRecord  = 188;

private:
    // read-until-two-equal-reads (0x80EEE88E u16 / 0x80EEEA92 u32; stock caps
    // the verify writers at 6 tries)
    uint16_t read16_stable(IAudioWindow::Win w, uint32_t off);
    uint32_t read32_stable(IAudioWindow::Win w, uint32_t off);
    // write-and-verify-retry (0x80EFBD32 u16 / 0x80EEEAAC u32, cap 6)
    bool write16_verify(IAudioWindow::Win w, uint32_t off, uint16_t v);
    bool write32_verify(IAudioWindow::Win w, uint32_t off, uint32_t v);

    bool guarded_put(const Entry12& e);   // 0x80EF1418 shape (baseB ring)
    IAudioWindow& hw_;
    Gates gates_;
};

}  // namespace audio
}  // namespace f286
