// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// audio_selftest - ring-direction + sender-routing sanity for f286::audio
// (guards against regressions: wrong ring for the command
// senders, inverted resp-ring counter roles). Run from the model/ dir:
//   g++ -std=c++17 -Wall -Wextra -I. audio_selftest.cpp audio_path.cpp -o audio_test
#include "audio_path.h"
#include <cstdio>
#include <cstring>

using namespace f286::audio;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

struct MockWin : IAudioWindow {
    uint8_t gsm[0x8000] = {0};   // BASE window (BASE_AUDIO ring lives here)
    uint8_t qb[0x1000] = {0};    // baseB AudCmdQ window
    uint32_t ticks = 0;
    uint8_t* buf(Win w) { return w == Win::GsmDsp ? gsm : qb; }
    bool read(Win w, uint32_t off, void* dst, size_t len) override {
        memcpy(dst, buf(w) + off, len); return true;
    }
    bool write(Win w, uint32_t off, const void* src, size_t len) override {
        memcpy(buf(w) + off, src, len); return true;
    }
    bool adi_read(uint32_t, uint32_t* v) override { *v = 0; return true; }
    bool adi_write(uint32_t, uint32_t) override { return true; }
    void memset_win(Win w, uint32_t off, uint8_t v, size_t len) override {
        memset(buf(w) + off, v, len);
    }
    void sleep_ms(uint32_t) override {}
    uint32_t now_ms() override { return ++ticks; }
};

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr_local(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

static void chk_entry(MockWin& hw, uint32_t slot, uint32_t w0, uint16_t a,
                      uint16_t b, uint16_t c, uint16_t d, const char* msg) {
    const uint8_t* e = hw.gsm + kMsgRingOff + slot * 12;
    if (rd32(e) != w0 || rd16(e + 4) != a || rd16(e + 6) != b ||
        rd16(e + 8) != c || rd16(e + 10) != d) {
        printf("FAIL: %s - entry {%04x, %04x, %04x, %04x, %04x} want {%04x, %04x, %04x, %04x, %04x}\n",
               msg, rd32(e), rd16(e + 4), rd16(e + 6), rd16(e + 8), rd16(e + 10),
               w0, a, b, c, d);
        ++fails;
    }
}

int main() {
    MockWin hw;
    Gates gates;
    static uint16_t d_val = 7;
    gates.aud_enable_d = []() { return d_val; };  // 0x8108F4F0 stand-in
    AudioPath ap(hw, gates);

    // 1. every command sender routes through the 20-slot BASE_AUDIO
    //    msg ring (in-counter u32 @0x1C94 advances, entry @0x1BA4+slot*12),
    //    NOT the baseB 8-slot AudCmdQ (0x70C/0x76E stay zero).
    ap.cmd_aud_enable(0x11, 0x22);   chk_entry(hw, 1, 0x0, 0x22, 0x11, 0, 7, "aud_enable {0,p2,p1,0,d}");
    ap.cmd_codec_enable(0x11, 0x22); chk_entry(hw, 2, 0x1, 0x22, 0x11, 0, 0, "codec_enable {1,p2,p1}");
    ap.cmd_set_volume(3);            chk_entry(hw, 3, 0x2, 0, 3, 0, 0, "set_volume sender {2,0,vol}");
    ap.cmd_stone(1, 2, 3);           chk_entry(hw, 4, 0x4, 2, 1, 0, 3, "stone {4,b,a,0,c}");
    ap.cmd_gtone(1, 2, 3, 4);        chk_entry(hw, 5, 0x5, 2, 1, 4, 3, "gtone {5,b,a,d,c}");
    ap.cmd_stop_tone();              chk_entry(hw, 6, 0x6, 0, 0, 0, 0, "stop_tone {6,0,0,0,0}");
    ap.cmd_mute_enable(1, 0);        chk_entry(hw, 7, 0x7, 0, 1, 0, 0, "mute_enable {7,b,a}");
    ap.cmd_dlink_mute(1, 0);         chk_entry(hw, 8, 0x8, 0, 1, 0, 0, "dlink_mute {8,b,a}");
    ap.cmd_voice_ul(1, 2, 3, 4);     chk_entry(hw, 9, 0x9, 2, 1, 4, 3, "voice_ul {9,b,a,d,c}");
    ap.cmd_voice_lb(1, 2, 3, 4);     chk_entry(hw, 10, 0xA, 2, 1, 4, 3, "voice_lb {A,b,a,d,c}");
    ap.cmd_voice_dl(1, 2, 3, 4);     chk_entry(hw, 11, 0xB, 2, 1, 4, 3, "voice_dl {B,b,a,d,c}");
    ap.cmd_aud_loop(5);              chk_entry(hw, 12, 0xC, 0, 5, 0, 0, "aud_loop {C,0,mode}");
    ap.cmd_echo_t(1, 2);             chk_entry(hw, 13, 0xD, 2, 1, 0, 0, "echo_t {D,b,a}");
    CHECK(rd32(hw.gsm + kMsgRingInOff) == 13, "13 sends -> in-counter 0x1C94 == 13");
    CHECK(rd16(hw.qb + kQInOff) == 0 && rd16(hw.qb + kQOutOff) == 0,
          "baseB AudCmdQ counters untouched by senders");
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t* e = hw.qb + kQEntryOff + i * 12;
        CHECK(rd32(e) == 0, "baseB AudCmdQ entries untouched");
    }

    // 2. set_volume uses the raw put - same ring, scatter
    //    {2, a=vol, b=vol, c=0, d=vol}.
    CHECK(ap.set_volume(9), "set_volume ok");
    chk_entry(hw, 14, 0x2, 9, 9, 0, 9, "set_volume glue {2,vol,vol,0,vol}");
    CHECK(rd32(hw.gsm + kMsgRingInOff) == 14, "raw-put counter advanced");

    // 3. audcmdq_resp_get advances the AP/consumer counter (+0x7D0)
    //    and never writes the DSP/producer counter (+0x7D2).
    hw.qb[kQRespProdOff] = 1; hw.qb[kQRespProdOff + 1] = 0;   // producer = 1
    const uint8_t* ent = hw.qb + kQRespEntryOff + 1 * 12;
    wr_local(const_cast<uint8_t*>(ent), 0xDEAD);
    Entry12 r{};
    CHECK(ap.audcmdq_resp_get(&r), "resp_get has an entry");
    CHECK(r.w0 == 0xDEAD, "resp_get copied entry idx=cons+1");
    CHECK(rd16(hw.qb + kQRespConsOff) == 1, "consumer +0x7D0 advanced to 1");
    CHECK(rd16(hw.qb + kQRespProdOff) == 1, "producer +0x7D2 untouched");
    CHECK(!ap.audcmdq_resp_get(&r), "empty when consumer == producer");
    CHECK(rd16(hw.qb + kQRespConsOff) == 1, "consumer not advanced when empty");

    // 4. 10-slot resp ring keeps stock direction: producer +0x1D14 read-only,
    //    consumer +0x1D18 advanced by get_resp.
    hw.gsm[kRespRingInOff] = 1;
    const uint8_t* re = hw.gsm + kRespRingOff + 1 * 12;
    wr_local(const_cast<uint8_t*>(re), 0xC0DE);
    CHECK(ap.get_resp(&r), "get_resp has an entry");
    CHECK(r.w0 == 0xC0DE, "get_resp copied entry");
    CHECK(rd32(hw.gsm + kRespRingOutOff) == 1, "consumer +0x1D18 advanced");
    CHECK(rd32(hw.gsm + kRespRingInOff) == 1, "producer +0x1D14 untouched");

    // 5. wrap %0x14 on the command ring.
    for (int i = 0; i < 21; ++i) CHECK(ap.cmd_stop_tone(), "stop_tone send");
    CHECK(rd32(hw.gsm + kMsgRingInOff) == 15, "counter wraps mod 0x14 (14+21 -> 15)");

    printf(fails ? "AUDIO SELFTEST: %d FAILURES\n" : "AUDIO SELFTEST: ALL PASS\n",
           fails);
    return fails ? 1 : 0;
}
