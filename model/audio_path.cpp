// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// audio_path.cpp - f286::audio implementation. Every routine names the stock
// kernel.bin function it transcribes (VA = file_off + 0x80a05e00; all VAs
// below are true VAs re-checked with capstone).
#include "audio_path.h"

namespace f286 {
namespace audio {
namespace {

constexpr uint32_t kBaseAudioRel = 0x2A8000;  // BASE_AUDIO - BASE [I - UNPROVEN:
                                              // constant absent from kernel.bin,
                                              // readback-diff
                                              // on HW before trusting]

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}
inline void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stock access helpers.
//   read-until-two-equal-reads: 0x80EEE88E (u16) / 0x80EEEA92 (u32)
//   write-and-verify-retry:     0x80EFBD32 (u16) / 0x80EEEAAC (u32)
// The DSP and AP share plain RAM; these loops hide the async-update races.
// Stock caps both verify writers at 6 tries (`cmp r2,#6; bhi exit`) - the
// 100-try loop here is a safe superset.
// ---------------------------------------------------------------------------
uint16_t AudioPath::read16_stable(IAudioWindow::Win w, uint32_t off) {
    uint8_t buf[2];
    uint16_t a, b;
    do {
        hw_.read(w, off, buf, 2); a = rd16(buf);
        hw_.read(w, off, buf, 2); b = rd16(buf);
    } while (a != b);
    return b;
}

uint32_t AudioPath::read32_stable(IAudioWindow::Win w, uint32_t off) {
    uint8_t buf[4];
    uint32_t a, b;
    do {
        hw_.read(w, off, buf, 4); a = rd32(buf);
        hw_.read(w, off, buf, 4); b = rd32(buf);
    } while (a != b);
    return b;
}

bool AudioPath::write16_verify(IAudioWindow::Win w, uint32_t off, uint16_t v) {
    uint8_t buf[2]; wr16(buf, v);
    for (int i = 0; i < 100; ++i) {          // stock 0x80EFBD32 caps at 6 tries
        if (!hw_.write(w, off, buf, 2)) return false;
        if (read16_stable(w, off) == v) return true;
    }
    return false;
}

bool AudioPath::write32_verify(IAudioWindow::Win w, uint32_t off, uint32_t v) {
    uint8_t buf[4]; wr32(buf, v);
    for (int i = 0; i < 100; ++i) {          // stock 0x80EEEAAC caps at 6 tries
        if (!hw_.write(w, off, buf, 4)) return false;
        if (read32_stable(w, off) == v) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// share_init - zero-init 0x80EEE990 [V] + the aud_share_init sub-base stores
// (aud_share_init VA unlocated; 0x80EEF004, cited earlier, is not it). Stores BASE/baseB/BASE_AUDIO and the two sub-bases into
// the (port-local) device record and zeroes the shared regions like stock.
// ---------------------------------------------------------------------------
bool AudioPath::share_init(bool dsp_in_reset) {
    if (!dsp_in_reset) return false;   // 0x800E0000 head is the live DSP window

    // 0x80EEE990: memset([dev+0x28]=baseB, 0, 0xE00);
    //             memset([dev+0x24]=BASE,   0, 0x9CC);
    //             end-check (dev+0x18 pair, +2 slack vs 0xB2C) then
    //             memset([dev+0x18]=BASE+0x1F300, 0, 0xB2C)   [V]
    hw_.memset_win(IAudioWindow::Win::AudCmdQ, 0, 0, kZeroBaseBBytes);
    hw_.memset_win(IAudioWindow::Win::GsmDsp, 0, 0, kZeroBaseBytes);
    hw_.memset_win(IAudioWindow::Win::GsmDsp, kTdSharedOff, 0, kZeroTdBytes);

    // aud_share_init sub-bases [I: the 0x80EEF004 sub-base stores are
    // unsupported (0x1EC90 nowhere; no real str [dev],{0x2c,0x30,0x34} found);
    // zero-init 0x80EEE990 itself is verified]:
    //   dev+0x2C = BASE+0x1C000, dev+0x30 = BASE+0x1EC90,
    //   dev+0x34 = BASE_AUDIO. A port needs no RAM mirrors - every accessor
    //   below derives offsets from the k* constants.
    return true;
}

// ---------------------------------------------------------------------------
// put_msg - raw put 0x80B1957E [V re-verified ]:
//   mutex take 0x81099978; base = [pool]; r4 = base + 0x1000;
//   in = read32-stable([r4+0xC94]); idx = in + 1; if (idx >= 0x14) idx -= 0x14;
//   e = BASE_AUDIO + 0x1BA4 + idx*12 (stock: pool + idx*12 - 0x45C + 0x2000);
//   write w0 u32, then b@+6, a@+4, d@+0xA, c@+8 (the 5-arg scatter);
//   write-and-verify([r4+0xC94], new_in) in a tight loop;
//   mutex give 0x81099928; full check: new_in == out([r4+0xC98]).
// (Stock takes 5 args with scatter r0->w0, r1->b, r2->a, r3->d, stack->c;
// this port takes a pre-built Entry12 - set_volume applies the scatter.)
// Stock logs a level-3 debug message here (0x80B195E8-FC); the 0xA840/0xD840
// card-log traces belong to the cluster put, not to this raw put.
// ---------------------------------------------------------------------------
bool AudioPath::put_msg(const Entry12& e) {
    uint8_t buf[12];
    uint32_t in = read32_stable(IAudioWindow::Win::GsmDsp, kMsgRingInOff);
    uint32_t idx = in + 1;
    if (idx >= kMsgRingSlots) idx -= kMsgRingSlots;

    wr32(buf, e.w0); wr16(buf + 4, e.a); wr16(buf + 6, e.b);
    wr16(buf + 8, e.c); wr16(buf + 10, e.d);
    if (!hw_.write(IAudioWindow::Win::GsmDsp, kMsgRingOff + idx * 12, buf, 12))
        return false;
    if (!write32_verify(IAudioWindow::Win::GsmDsp, kMsgRingInOff, in + 1))
        return false;

    uint32_t out = read32_stable(IAudioWindow::Win::GsmDsp, kMsgRingOutOff);
    return (in + 1) != out;         // in==out means the ring was FULL
}

// ---------------------------------------------------------------------------
// cluster_put - THE command put 0x80EF14D6 [V]: every sender of the
// 0x80B3BB7C cluster calls this (not the baseB AudCmdQ guarded put).
//   gate 0x80B18204: [dev+0x10] == 1 -> silent bail (0x80EF15D4; the port's
//     Gates has no mirror of that flag, so this put is ungated here);
//   mutex take 0x81099978;
//   in = read32_stable([dev+0x34]+0x1C94); idx = (in+1) % 0x14 (udiv/mls);
//   out = read32_stable([dev+0x34]+0x1C98);
//   trace 0xA840 in / 0xD840 out (card-log 0x811750B2);
//   full (out == idx) => stock only logs a diagnostic (0x8109E71A) and
//   OVERWRITES the slot - no drop;
//   copy the 12-byte entry from [r0] VERBATIM to [dev+0x34] + 0x1BA4 + idx*12
//   (str w0; strh +6, +4, +0xA, +8);
//   write32_verify([dev+0x34]+0x1C94, idx); mutex give 0x81099928;
//   tail b.w 0x80DCCEE4 = event give(0,2) - a bare port polls.
// ---------------------------------------------------------------------------
bool AudioPath::cluster_put(const Entry12& e) {
    uint32_t in = read32_stable(IAudioWindow::Win::GsmDsp, kMsgRingInOff);
    uint32_t idx = (in + 1) % kMsgRingSlots;
    uint32_t out = read32_stable(IAudioWindow::Win::GsmDsp, kMsgRingOutOff);
    hw_.trace(0xA840, in, 0);
    hw_.trace(0xD840, out, 0);
    // (out == idx): ring full - stock logs and overwrites; nothing to do here.

    uint8_t buf[12];
    wr32(buf, e.w0); wr16(buf + 4, e.a); wr16(buf + 6, e.b);
    wr16(buf + 8, e.c); wr16(buf + 10, e.d);
    if (!hw_.write(IAudioWindow::Win::GsmDsp, kMsgRingOff + idx * 12, buf, 12))
        return false;
    return write32_verify(IAudioWindow::Win::GsmDsp, kMsgRingInOff, idx);
}

// ---------------------------------------------------------------------------
// get_resp - get+trace 0x80EF18DE [V]:
//   in = read32_stable(BASE_AUDIO+0x1D14); out = read32_stable(+0x1D18);
//   if (in == out) return false;
//   out = (out + 1) % 10 (udiv/mls);
//   entry @ BASE_AUDIO + 0x1C9C + out*12 copied to *out (stock copy order
//   str r2/[r4], strh +6, +4, +0xA, +8 preserves the layout);
//   write32_verify(+0x1D18, out); trace(w0, in) then trace 0xEAEA with
//   v1 = [dev+0x48] (running fetch index - not tracked in this port, pass 0).
// ---------------------------------------------------------------------------
bool AudioPath::get_resp(Entry12* out) {
    uint32_t in = read32_stable(IAudioWindow::Win::GsmDsp, kRespRingInOff);
    uint32_t o = read32_stable(IAudioWindow::Win::GsmDsp, kRespRingOutOff);
    if (in == o) return false;
    o = (o + 1) % kRespRingSlots;

    uint8_t buf[12];
    if (!hw_.read(IAudioWindow::Win::GsmDsp, kRespRingOff + o * 12, buf, 12))
        return false;
    out->w0 = rd32(buf);
    out->a = rd16(buf + 4); out->b = rd16(buf + 6);
    out->c = rd16(buf + 8); out->d = rd16(buf + 10);
    if (!write32_verify(IAudioWindow::Win::GsmDsp, kRespRingOutOff, o))
        return false;
    hw_.trace(0xEAEA, out->w0, 0);   // stock v1 = [dev+0x48]
    return true;
}

// ---------------------------------------------------------------------------
// guarded_put - baseB AudCmdQ put 0x80EF1418 [V]. KEPT but NOT used by the
// command senders below (stock senders call cluster_put into the
// BASE_AUDIO ring; this ring's stock callers include the id-0x25 sender).
//   gates: L1-state checks (0x80B18204 [dev+0x10]==1 -> bail; 0x81006FA8;
//   0x80B18048); mutex take; in = read16_stable(baseB+0x76E); idx = (in+1)&7;
//   trace 0xA830 in, 0xD830 out (read16_stable baseB+0x76C);
//   if (out == idx) -> full path 0x80EF139E (which also records the dropped
//   entry into a 30-slot diag ring, counters +0xC/+0xE %0x1E, pool
//   0x81974440) + flag [baseB+0xDD0] = 1 + 0x80EF12A2,
//   then mutex give (via tail 0x80DCCEE4: take; event give(0,2); give);
//   else copy the 12-byte entry VERBATIM to baseB + 0x70C + idx*12
//   (str w0 @0x70C; strh [+4]@0x710, [+6]@0x712, [+8]@0x714, [+0xA]@0x716),
//   write16_verify(baseB+0x76E, idx); the real notify is the event give(0,2)
//   - a bare port simply polls.
// ---------------------------------------------------------------------------
bool AudioPath::guarded_put(const Entry12& e) {
    if (gates_.l1_audio_active && !gates_.l1_audio_active()) return false;

    uint16_t in = read16_stable(IAudioWindow::Win::AudCmdQ, kQInOff);
    uint16_t idx = (uint16_t)((in + 1) & (kQSlots - 1));
    uint16_t out = read16_stable(IAudioWindow::Win::AudCmdQ, kQOutOff);
    hw_.trace(0xA830, in, 0);
    hw_.trace(0xD830, out, 0);
    if (out == idx) {   // ring full
        uint32_t one = 1;
        hw_.write(IAudioWindow::Win::AudCmdQ, kQNotReadyFlag, &one, 4);
        return false;
    }

    uint8_t buf[12];
    wr32(buf, e.w0); wr16(buf + 4, e.a); wr16(buf + 6, e.b);
    wr16(buf + 8, e.c); wr16(buf + 10, e.d);
    if (!hw_.write(IAudioWindow::Win::AudCmdQ, kQEntryOff + idx * 12, buf, 12))
        return false;
    return write16_verify(IAudioWindow::Win::AudCmdQ, kQInOff, idx);
}

// audcmdq_resp_get - baseB resp get 0x80EF1852 [V]: +0x7D2 is the PRODUCER
// (DSP) counter - read-only on our side; +0x7D0 is the CONSUMER (AP) counter
// we advance. Stable reads of +0x7D2 then +0x7D0; empty iff equal;
// idx = (consumer+1) & 7; entry @ baseB+0x770 + idx*12 copied to *out;
// write16_verify(+0x7D0, idx) (stock: `addw r0,r0,#0x7d0; bl 0x80EFBD32`);
// trace(w0, old-producer) then a second trace via 0x80EEF856 (0xEEEE/0xBBBB
// here).
bool AudioPath::audcmdq_resp_get(Entry12* out) {
    uint16_t prod = read16_stable(IAudioWindow::Win::AudCmdQ, kQRespProdOff);
    uint16_t cons = read16_stable(IAudioWindow::Win::AudCmdQ, kQRespConsOff);
    if (prod == cons) return false;
    uint16_t idx = (uint16_t)((cons + 1) & (kQSlots - 1));

    uint8_t buf[12];
    if (!hw_.read(IAudioWindow::Win::AudCmdQ, kQRespEntryOff + idx * 12, buf, 12))
        return false;
    out->w0 = rd32(buf);
    out->a = rd16(buf + 4); out->b = rd16(buf + 6);
    out->c = rd16(buf + 8); out->d = rd16(buf + 10);
    if (!write16_verify(IAudioWindow::Win::AudCmdQ, kQRespConsOff, idx))
        return false;
    hw_.trace(0xEEEE, out->w0, prod);
    hw_.trace(0xBBBB, 0, 0);
    return true;
}

// audcmdq_put - the baseB fast-command ring flavor. Kept for completeness;
// the command senders below do NOT use it (stock calls cluster_put).
bool AudioPath::audcmdq_put(const Entry12& e) { return guarded_put(e); }

// ---------------------------------------------------------------------------
// Command senders. All 13 route through cluster_put (0x80EF14D6), matching
// the stock BL sites; field scatter re-derived from each stock wrapper
// (entry layout: w0@+0, a@+4, b@+6, c@+8, d@+0xA relative to the entry base).
// ---------------------------------------------------------------------------
bool AudioPath::cmd_aud_enable(uint16_t p1, uint16_t p2) {
    // 0x80B3BB7C: the audio_param_transfered byte (0x8170609E) only triggers
    // a log (cbnz @0x80B3BB9E falls through) - stock ALWAYS builds+sends.
    // With (p1|p2)!=0 stock checks 0x80DCCD50 (a constant-1 thunk) and
    // would skip on 0; with (p1|p2)==0 it sends unconditionally. Entry:
    // {0, a=p2, b=p1, c=0, d=0x8108F4F0}.
    Entry12 e{0, p2, p1, 0,
              static_cast<uint16_t>(gates_.aud_enable_d ? gates_.aud_enable_d()
                                                        : 0)};
    return cluster_put(e);
}

bool AudioPath::cmd_codec_enable(uint16_t p1, uint16_t p2) {
    Entry12 e{1, p2, p1, 0, 0};                          // 0x80B3BC0C
    return cluster_put(e);
}

bool AudioPath::cmd_set_volume(uint16_t vol) {
    Entry12 e{2, 0, vol, 0, 0};                          // 0x80B3BDB2: vol in b
    return cluster_put(e);
}

bool AudioPath::cmd_stone(uint16_t a, uint16_t b, uint16_t c) {
    Entry12 e{4, b, a, 0, c};                            // 0x80B3BF92: d=arg2
    return cluster_put(e);
}

bool AudioPath::cmd_gtone(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    Entry12 e{5, b, a, d, c};                            // 0x80B3BFDA
    return cluster_put(e);
}

bool AudioPath::cmd_stop_tone() {
    Entry12 e{6, 0, 0, 0, 0};                            // 0x80B3C00C
    return cluster_put(e);
}

bool AudioPath::cmd_mute_enable(uint16_t a, uint16_t b) {
    Entry12 e{7, b, a, 0, 0};                            // 0x80B3BE56
    return cluster_put(e);
}

bool AudioPath::cmd_dlink_mute(uint16_t a, uint16_t b) {
    Entry12 e{8, b, a, 0, 0};                            // 0x80B3C594
    return cluster_put(e);
}

bool AudioPath::cmd_voice_ul(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    Entry12 e{9, b, a, d, c};                            // 0x80B3C5F0
    return cluster_put(e);
}

bool AudioPath::cmd_voice_lb(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    Entry12 e{0xA, b, a, d, c};                          // 0x80B3C55C
    return cluster_put(e);
}

bool AudioPath::cmd_voice_dl(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    Entry12 e{0xB, b, a, d, c};                          // 0x80B3C626 (4 args)
    return cluster_put(e);
}

bool AudioPath::cmd_aud_loop(uint16_t mode) {
    Entry12 e{0xC, 0, mode, 0, 0};                       // 0x80B3C4A4: mode in b
    return cluster_put(e);
}

bool AudioPath::cmd_echo_t(uint16_t a, uint16_t b) {
    Entry12 e{0xD, b, a, 0, 0};                          // 0x80B3C5C2
    return cluster_put(e);
}

// ---------------------------------------------------------------------------
// codec_analog_enable - SPRD_CODEC_TASK fns 0x80A5DA3A / 0x80A5DA12 [V]:
//   0x40608C08 |= 0x10 (codec module / clock enable)
//   0x40608C0C |= 3 on / &= ~3 off (codec power/enable pair)
// via ADI read/write helpers 0x81218546 / 0x81218598.
// ---------------------------------------------------------------------------
bool AudioPath::codec_analog_enable(bool on) {
    uint32_t v = 0;
    if (!hw_.adi_read(kAdiCodecEnableA, &v)) return false;
    if (!hw_.adi_write(kAdiCodecEnableA, v | 0x10)) return false;
    if (!hw_.adi_read(kAdiCodecEnableB, &v)) return false;
    v = on ? (v | 0x3) : (v & ~0x3u);
    return hw_.adi_write(kAdiCodecEnableB, v);
}

// ---------------------------------------------------------------------------
// Call sequence.  The DSP-side config itself rides the L1 CAL channel
// (cal signals 0x3D1 param / 0x3D2 mute; per-mode 0x408 B AMR arrays from
// HandleAMRConfigReqMsg 0x80B28E56 / ConfigAMRInitParam 0x80B2C7FA) and sets
// the audio_param_transfered gate - that exchange belongs to the L1 client's
// cal responder, not to this ring layer.
// ---------------------------------------------------------------------------
AudioPath::Step AudioPath::call_audio_start(uint16_t ul_a, uint16_t ul_b,
                                            uint16_t dl_a, uint16_t dl_b) {
    if (!gates_.audio_param_transfered) return Step::Gate;
    if (!codec_analog_enable(true)) return Step::HwError;
    if (!cmd_aud_enable(1, 1)) return Step::RingFull;      // 0x80B3BB7C
    if (!cmd_codec_enable(1, 1)) return Step::RingFull;    // 0x80B3BC0C
    if (!cmd_voice_ul(ul_a, ul_b, 0, 0)) return Step::RingFull;  // 0x80B3C5F0
    if (!cmd_voice_dl(dl_a, dl_b, 0, 0)) return Step::RingFull;  // 0x80B3C626
    return Step::Ok;
}

void AudioPath::call_audio_stop() {
    // hangup tail: CmdCodecEnable(0,*) then CmdAudEnable(0,*) - the disable
    // direction of 0x80B3BC0C/0x80B3BB7C (AT END -> VOICE_Stop -> ACM exit).
    // Stock sends both through cluster_put; CmdAudEnable(0,0) also sends
    // (the (p1|p2)==0 path skips only the 0x80DCCD50 gate, which is const-1).
    Entry12 c{1, 0, 0, 0, 0};
    (void)cluster_put(c);
    Entry12 a{0, 0, 0, 0, 0};
    (void)cluster_put(a);
    (void)codec_analog_enable(false);
}

// ---------------------------------------------------------------------------
// Volume / mute.  set_volume mirrors glue 0x80B2E632 [V]: clamp vol <= 9
// (both branches: `cmp r4,#9; bhi -> movs r4,#9`), then raw-put call at
// 0x80B2E728 with (r0=2, r1=vol, r2=vol, r3=vol, stack=0). The raw put's
// 5-arg scatter (r0->w0, r1->b, r2->a, r3->d, stack->c) makes the Entry12
// {2, a=vol, b=vol, c=0, d=vol}.  UL mute = cmd 7 (0x80B3BE56), DL mute =
// cmd 8 (0x80B3C594); stock's AUDMOD layer (0x8122E196 / 0x812FF66A) reaches
// these through the same put.
// ---------------------------------------------------------------------------
bool AudioPath::set_volume(uint16_t vol) {
    if (vol > kMaxVolume) vol = kMaxVolume;
    Entry12 e{2, vol, vol, 0, vol};
    return put_msg(e);
}

bool AudioPath::set_ulink_mute(bool on) {
    return cmd_mute_enable(on ? 1 : 0, 0);
}

bool AudioPath::set_downlink_mute(bool on) {
    return cmd_dlink_mute(on ? 1 : 0, 0);
}

// wait_tone_rsp - stock 0x80B182DC waits OSA event bit 0x80, timeout 0x3E8 ms
// (then asserts "L1 event not arrived! AUDIO_DSP_TG_ENABLE_RSP" @0x80B183D0);
// the rsp is set by the resp-ring pump, so a bare port polls the ring itself.
bool AudioPath::wait_tone_rsp(uint32_t timeout_ms) {
    uint32_t t0 = hw_.now_ms();
    Entry12 e{};
    while (hw_.now_ms() - t0 < timeout_ms) {
        if (get_resp(&e)) return true;   // caller classifies w0
        hw_.sleep_ms(5);
    }
    return false;
}

// ---------------------------------------------------------------------------
// PCM movement.
//   pcm_ul_read: one 188 B record from the baseB PCM ring (inline machinery in
//   0x80EF1738-0x80EF1850): in u16 @baseB+0xC66, out u16 @baseB+0xC64.
//   pcm_free_space: helper 0x80EEEBD8 [V consts]: counters @baseB+0x3A8/0x3AA,
//   quanta 0x1A / 0x17 / 0x52E halfwords; the 0x297000-wrap circ buffer is the
//   bulk 20 ms PCM stream. Exact free-space formula INFERRED (open gap).
// ---------------------------------------------------------------------------
bool AudioPath::pcm_ul_read(uint8_t* dst188) {
    uint16_t in = read16_stable(IAudioWindow::Win::AudCmdQ, kPcmInOff);
    uint16_t out = read16_stable(IAudioWindow::Win::AudCmdQ, kPcmOutOff);
    if (in == out) return false;   // no record available
    if (!hw_.read(IAudioWindow::Win::AudCmdQ, kPcmRingOff + out * kPcmRecord,
                  dst188, kPcmRecord))
        return false;
    return write16_verify(IAudioWindow::Win::AudCmdQ, kPcmOutOff,
                          (uint16_t)((out + 1) & 0xFFFF));
}

bool AudioPath::pcm_free_space(uint16_t* halfwords) {
    uint16_t in = read16_stable(IAudioWindow::Win::AudCmdQ, kFreeInOff);
    uint16_t out = read16_stable(IAudioWindow::Win::AudCmdQ, kFreeOutOff);
    uint32_t used = (uint32_t)(in - out);
    // 0x52E-halfword quantum ring [V consts; formula INFERRED]
    if (used > kCircQuantum) used = kCircQuantum;
    *halfwords = (uint16_t)(kCircQuantum - used);
    return true;
}

// ---------------------------------------------------------------------------
// midi_put - 0x80EF4840 [V]: gate waits while
// out(+0x1DA2) >= in(+0x1DA0)+7 (logs "Midi count is too great!" 0x80EF4A38 in
// the oversized case), then memcpy's (count<<1)&0xFFFF BYTES - the count
// halfwords only - into chunk idx (7 x 0x500 @ BASE_AUDIO+0x45B4, idx u16
// @+0x1D9E, wrap %7). The old "(count<<1) chunk-end marker" halfword was
// INVENTED: (count<<1) is the memcpy LENGTH argument (0x8109DAD8); the chunk
// tail keeps its previous contents. Hardening deviation: stock logs and still
// sends count > 0x280 (overrunning the 0x500 chunk); C++ refuses.
// ---------------------------------------------------------------------------
bool AudioPath::midi_put(const uint16_t* halfwords, uint16_t count) {
    if (halfwords == nullptr || count == 0 || count > kMidiChunkHw) return false;
    for (;;) {  // stock gate: block while out(+0x1DA2) >= in(+0x1DA0)+7
        const uint16_t in = read16_stable(IAudioWindow::Win::GsmDsp, kMidiCntOff);
        const uint16_t out = read16_stable(IAudioWindow::Win::GsmDsp, kMidiCntOff + 2);
        if (out < static_cast<uint16_t>(in + 7)) break;
        hw_.sleep_ms(1);
    }
    uint16_t idx = read16_stable(IAudioWindow::Win::GsmDsp, kMidiIdxOff);
    if (idx >= 7) return false;

    const uint32_t nbytes = static_cast<uint32_t>(count) << 1;  // memcpy length
    uint8_t buf[kMidiChunkBytes] = {0};
    for (uint32_t i = 0; i < count; ++i) wr16(buf + 2 * i, halfwords[i]);
    if (!hw_.write(IAudioWindow::Win::GsmDsp, kMidiRingOff + idx * kMidiChunkBytes,
                   buf, nbytes))
        return false;
    return write16_verify(IAudioWindow::Win::GsmDsp, kMidiIdxOff,
                          (uint16_t)((idx + 1) % 7));
}

}  // namespace audio
}  // namespace f286
