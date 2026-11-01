// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sms_selftest - functional sanity for f286::sms (temporary; run from /tmp only)
#include "sms_path.h"
#include "sim_path.h"
#include <cstdio>
#include <cstring>
#include <vector>

// Run: g++ -std=c++17 -Wall -Wextra -I. sms_selftest.cpp sms_path.cpp sim_path.cpp -o sms_test

using namespace f286::sms;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

struct MockTx : ISmsTransport {
    std::vector<std::vector<uint8_t>> frames;
    bool send_nas_container(const uint8_t* p, size_t n) override {
        frames.emplace_back(p, p + n);
        return true;
    }
};

struct MockSt : ISmsStorage {
    SimSmsRecord recs[8] = {};
    bool used_[8] = {};
    int find_free(Storage) override { for (int i = 0; i < 8; ++i) if (!used_[i]) return i; return -1; }
    bool write(Storage, int i, const SimSmsRecord& r) override { if (i < 0 || i >= 8) return false; recs[i] = r; used_[i] = true; return true; }
    bool read(Storage, int i, SimSmsRecord* r) override { if (i < 0 || i >= 8 || !used_[i]) return false; *r = recs[i]; return true; }
    bool erase(Storage, int i) override { if (i < 0 || i >= 8) return false; used_[i] = false; return true; }
    int used(Storage) override { int n = 0; for (bool b : used_) n += b; return n; }
};

static StackEvent last_ev;
static bool ev_seen = false;
static void on_ev(void*, const StackEvent& ev) { last_ev = ev; ev_seen = true; }

int main() {
    // 1. 3GPP classic vector: "hellohello" -> E8 32 9B FD 46 97 D9 EC 37
    {
        const uint16_t t[] = {'h','e','l','l','o','h','e','l','l','o'};
        uint8_t sept[64];
        size_t n = ucs2_to_gsm7_septets(t, 10, sept, sizeof(sept));
        CHECK(n == 10, "hellohello septet count");
        uint16_t pn = (uint16_t)n;
        gsm7_pack(sept, &pn, 0);
        const uint8_t want[] = {0xE8,0x32,0x9B,0xFD,0x46,0x97,0xD9,0xEC,0x37};
        CHECK(pn == 9, "hellohello packed len");
        CHECK(memcmp(sept, want, 9) == 0, "hellohello packed bytes");
        // unpack back
        uint8_t se2[64]; uint16_t ol = 0;
        gsm7_expand(want, 9, se2, &ol, 0);
        CHECK(ol == 10, "expand count 9*8/7 = 10");
        uint16_t back[16];
        size_t nb = gsm7_septets_to_ucs2(se2, ol, back, 16);
        CHECK(nb >= 10 && memcmp(back, t, 10 * 2) == 0, "hellohello roundtrip");
    }
    // 2. fill cases: 7 septets = 49 bits -> 7 spare bits -> CR (0x0D) continuation;
    //    9 septets = 63 bits -> 1 spare bit -> zero fill.
    {
        const uint16_t t7[] = {'0','1','2','3','4','5','6'};
        uint8_t s7[16];
        size_t n7 = ucs2_to_gsm7_septets(t7, 7, s7, sizeof(s7));
        uint16_t p7 = (uint16_t)n7;
        gsm7_pack(s7, &p7, 0);
        CHECK(p7 == 7 && s7[6] == 0x1A, "7 septets -> CR fill (0x0D<<1)");
        const uint16_t t9[] = {'0','1','2','3','4','5','6','7','8'};
        uint8_t s9[16];
        size_t n9 = ucs2_to_gsm7_septets(t9, 9, s9, sizeof(s9));
        uint16_t p9 = (uint16_t)n9;
        gsm7_pack(s9, &p9, 0);
        CHECK(p9 == 8 && s9[7] == 0x38, "9 septets -> zero fill last byte 0x38");
    }
    // 3. extension chars {}[]^~\| and euro(->UCS2 fallback)
    {
        const uint16_t t[] = {'{','}','^','~','\\','[',']','|'};
        uint8_t sept[32];
        size_t n = ucs2_to_gsm7_septets(t, 8, sept, sizeof(sept));
        CHECK(n == 16, "ext chars -> 1B pairs");
        CHECK(sept[0] == 0x1B && sept[1] == 0x28 && sept[2] == 0x1B && sept[3] == 0x29,
              "ext brace mapping");
        const uint16_t e = 0x20AC;
        // stock UCS2 ext table HAS the euro at 0x65 (byte ext does not)
        CHECK(ucs2_to_gsm7_septets(&e, 1, sept, 8) == 2 && sept[0] == 0x1B && sept[1] == 0x65,
              "euro -> 0x1B 0x65 via stock UCS2 ext table");
    }
    // 4. address + scts roundtrip
    {
        SmsAddress a; a.toa = 0x91;
        strcpy(a.digits, "447700900123");
        uint8_t b[16]; size_t n = addr_encode(a, b, sizeof(b));
        CHECK(n == 8 && b[0] == 12 && b[1] == 0x91, "addr encode len/toa");
        CHECK(b[2] == 0x44 && b[3] == 0x77, "addr bcd semi-octets");
        size_t used; SmsAddress back;
        CHECK(addr_decode(b, n, &used, &back) && strcmp(back.digits, "447700900123") == 0, "addr roundtrip");
        Scts t{26, 6, 24, 14, 30, 15, -8};
        uint8_t s[7]; scts_encode(t, s);
        Scts t2; scts_decode(s, &t2);
        CHECK(t2.year == 26 && t2.month == 6 && t2.day == 24 && t2.hour == 14 &&
              t2.minute == 30 && t2.second == 15 && t2.tz_quarters == -8, "scts roundtrip");
    }
    // 5. SUBMIT encode structural check + full MO frame via SmsStack
    {
        MockTx tx; MockSt st;
        SmsStack stack(tx, &st);
        stack.set_event_fn(on_ev, nullptr);
        SmsAddress sca; sca.toa = 0x91; strcpy(sca.digits, "15555550100");
        stack.set_sca(sca);
        const uint16_t t[] = {'h','e','l','l','o',' ','w','o','r','l','d'};
        CHECK(stack.submit_text(t, 11, "15555550123", 0x91), "submit_text accepted");
        CHECK(tx.frames.size() == 1, "one NAS frame");
        const std::vector<uint8_t>& f = tx.frames[0];
        // the stack hands the transport the bare CP PDU; the EMM port adds
        // the {0x70, 0x65, LV} NAS-transport envelope
        CHECK(f[0] == 0x09, "CP PD/TI octet (PD=9 SMS)");
        CHECK(f[1] == kCpData, "CP-DATA");
        CHECK(f.size() - 3 >= 2 && f.size() - 3 <= 251, "container bound [V]");
        CpHeader h; const uint8_t* body = nullptr; size_t bl = 0;
        CHECK(cp_parse(f.data(), f.size(), &h, &body, &bl) && h.type == kCpData && body != nullptr,
              "cp_parse CpData");
        if (!body) { printf("SELFTEST: abort, no CP body\n"); return 1; }
        CHECK(body[0] == kRpDataMsToN, "RP-DATA MS->N");
        CHECK(body[2] == 0x00 && body[3] == 0x00, "RP-O empty");
        CHECK(body[4] == 6 + 1, "RP-D contents-len includes TOA");
        CHECK(body[5] == 0x91, "RP-D toa");
        const uint8_t rp_mr = body[1];
        const size_t rpud_off = 4 + 1 + body[4] + 0;
        const size_t ud_len = body[rpud_off];
        const uint8_t* tpdu = body + rpud_off + 1;
        CHECK(tpdu[0] == 0x01 || (tpdu[0] & 3) == 1, "TP-MTI submit");
        CHECK(tpdu[1] >= 1, "TP-MR set");
        (void)rp_mr; (void)ud_len;
        // SCA inside RP-D decodes back
        CHECK(body[5] == 0x91 && body[6] == 0x51 && body[7] == 0x55 && body[8] == 0x55,
              "RP-D sca toa+bcd");
    }
    // 6. MT: DELIVER in CP-DATA over NAS -> SmsInd + storage write + RP-ACK echo
    {
        // build DELIVER TPDU: FO=04 (MMS=1), OA +447700900123, PID 0, DCS 0, SCTS, UDL, UD
        uint8_t tpdu[128];
        size_t o = 0;
        tpdu[o++] = 0x04;
        SmsAddress oa; oa.toa = 0x91; strcpy(oa.digits, "447700900123");
        o += addr_encode(oa, tpdu + o, 12);
        tpdu[o++] = 0x00;  // PID
        tpdu[o++] = 0x00;  // DCS gsm7 class0
        Scts ts{26, 10, 21, 12, 0, 0, 8};
        scts_encode(ts, tpdu + o); o += 7;
        const uint16_t msg[] = {'H','i',' ','f','r','o','m',' ','n','e','t'};
        uint8_t sept[32];
        size_t ns = ucs2_to_gsm7_septets(msg, 11, sept, sizeof(sept));
        tpdu[o++] = (uint8_t)ns;
        uint16_t pn = (uint16_t)ns;
                gsm7_pack(sept, &pn, 0);
        memcpy(tpdu + o, sept, pn); o += pn;

        uint8_t rp[64];
        size_t rl = rp_data_mo_build(0x42, SmsAddress{}, tpdu, o, rp, sizeof(rp)); // sca empty for brevity
        (void)rl;
        // manual N->MS RP-DATA: {01, MR, RP-O(SCA), RP-D empty, RP-UD}
        uint8_t rpmt[64];
        size_t ro = 0;
        rpmt[ro++] = kRpDataNToMs;
        rpmt[ro++] = 0x77;  // network MR
        rpmt[ro++] = 0x00; rpmt[ro++] = 0x00;  // RP-O empty (capture used SCA-less)
        rpmt[ro++] = 0x00; rpmt[ro++] = 0x00;  // RP-D empty
        rpmt[ro++] = (uint8_t)o;
        memcpy(rpmt + ro, tpdu, o); ro += o;
        uint8_t cp[80];
        size_t cl2 = cp_data_build(0, 0x55, rpmt, ro, cp, sizeof(cp));

        MockTx tx; MockSt st;
        SmsStack stack(tx, &st);
        stack.set_event_fn(on_ev, nullptr);
        ev_seen = false;
        stack.on_nas_container(cp, cl2);
        CHECK(ev_seen && last_ev.id == EventId::SmsInd, "MT SmsInd fired");
        CHECK(last_ev.deliver.ud_len == pn && memcmp(last_ev.deliver.ud, sept, pn) == 0, "MT UD bytes");
        uint16_t back[32];
        size_t nb2 = gsm7_ud_decode(last_ev.deliver.ud, last_ev.deliver.ud_len, 0, back, 32);
        if (nb2 != 11 || memcmp(back, msg, 11 * 2) != 0) {
            printf("  debug: nb2=%zu chars:", nb2);
            for (size_t i = 0; i < nb2 && i < 16; ++i) printf(" %04x", back[i]);
            printf("\n  ud_len=%zu udl=%zu\n", last_ev.deliver.ud_len, last_ev.deliver.udl);
        }
        CHECK(nb2 == 11 && memcmp(back, msg, 11 * 2) == 0, "MT text roundtrip");
        CHECK(st.used_[0] && st.recs[0].status == kRecMtUnread, "MT stored unread");
        // RP-ACK must have gone out: CP-DATA with RP-ACK MS->N echoing MR 0x77
        CHECK(tx.frames.size() == 2, "CP-ACK + RP-ACK frames out");
        bool saw_rp_ack = false;
        for (auto& fr : tx.frames) {
            // bare CP PDU {09 01 ref RPDU}; EMM adds the {70,65,LV} envelope
            if (fr.size() >= 5 && fr[1] == kCpData) {
                if (fr[3] == kRpAckMsToN && fr[4] == 0x77) saw_rp_ack = true;
            }
        }
        CHECK(saw_rp_ack, "RP-ACK echoes network MR");
    }
    // 7. MO PDU mode + RP-ACK completion + TR1M timeout path
    {
        MockTx tx; MockSt st;
        SmsStack stack(tx, &st);
        stack.set_event_fn(on_ev, nullptr);
        const uint8_t tpdu[] = {0x11, 0x05, 0x0B, 0x91, 0x51, 0x55, 0x55, 0x05, 0x21, 0xF3,
                                0x00, 0x00, 0x0A, 0xE8, 0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
        CHECK(stack.submit_pdu(tpdu, sizeof(tpdu)), "submit_pdu accepted");
        ev_seen = false;
        // feed RP-ACK N->MS {05, MR=05}
        const uint8_t rp[] = {kRpAckNToMs, 0x05};
        uint8_t cp[16];
        size_t cl3 = cp_data_build(0, 1, rp, 2, cp, sizeof(cp));
        stack.on_nas_container(cp, cl3);
        CHECK(ev_seen && last_ev.id == EventId::SendCnf && last_ev.mr == 0x05, "SendCnf on RP-ACK");
        CHECK(stack.idle(), "back to idle");
        // timeout path
        const uint8_t tpdu2[] = {0x11, 0x06, 0x0B, 0x91, 0x51, 0x55, 0x55, 0x05, 0x21, 0xF3,
                                 0x00, 0x00, 0x0A, 0xE8, 0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
        CHECK(stack.submit_pdu(tpdu2, sizeof(tpdu2)), "submit_pdu 2 accepted");
        ev_seen = false;
        stack.tick(kTr1mMs + 1);
        CHECK(ev_seen && last_ev.id == EventId::SendErr, "TR1M expiry -> SendErr");
    }
    // 8. record encode/decode roundtrip
    {
        SimSmsRecord r{};
        r.status = kRecMtUnread;
        SmsAddress oa; oa.toa = 0x91; strcpy(oa.digits, "447700900123"); r.addr = oa;
        r.pid = 0; r.dcs = 0x00;
        Scts ts{26, 10, 21, 12, 0, 0, 8}; r.scts = ts;
        const uint8_t ud[] = {0xC8, 0x32, 0x9B, 0xFD, 0x66, 0x81, 0x36, 0x90, 0xF5};
        memcpy(r.ud, ud, sizeof(ud)); r.ud_len = sizeof(ud); r.udl = 10;
        uint8_t raw[kSimRecordSize];
        CHECK(sms_record_encode(r, raw), "record encode");
        CHECK(raw[0] == kRecMtUnread, "record status byte");
        SimSmsRecord r2;
        CHECK(sms_record_decode(raw, &r2) && r2.status == kRecMtUnread &&
              strcmp(r2.addr.digits, "447700900123") == 0 && r2.ud_len == sizeof(ud) &&
              memcmp(r2.ud, ud, sizeof(ud)) == 0, "record roundtrip");
        // EF_SMSP
        uint8_t sp[28];
        SmsAddress sca; sca.toa = 0x91; strcpy(sca.digits, "15555550100");
        CHECK(smsp_build(sca, 0x00, 0xA7, sp, sizeof(sp)), "smsp build");
        SmsAddress s2; uint8_t dcs, vp;
        CHECK(smsp_parse(sp, sizeof(sp), &s2, &dcs, &vp) && strcmp(s2.digits, "15555550100") == 0 &&
              dcs == 0x00 && vp == 0xA7, "smsp roundtrip");
    }
    // 9. STATUS-REPORT decode (TS 23.040 section 9.2.3.3: FO,MR,RA,SCTS,DT,ST,PI[,UD])
    {
        uint8_t tp[64]; size_t o = 0;
        tp[o++] = 0x06;                        // FO: TP-MTI = 2 (status report)
        tp[o++] = 0x42;                        // TP-MR
        SmsAddress ra; ra.toa = 0x91; strcpy(ra.digits, "447700900123");
        o += addr_encode(ra, tp + o, 16);      // TP-RA
        Scts scts{26, 10, 21, 9, 30, 0, 8};
        scts_encode(scts, tp + o); o += 7;     // TP-SCTS
        Scts dt{26, 10, 21, 12, 0, 15, 8};
        scts_encode(dt, tp + o); o += 7;       // TP-DT
        tp[o++] = 0x00;                        // TP-ST
        tp[o++] = 0x04;                        // TP-PI: TP-UD present
        tp[o++] = 3;                           // TP-UDL
        tp[o++] = 0x61; tp[o++] = 0x62; tp[o++] = 0x63;
        SmsStatusReport sr;
        CHECK(status_report_decode(tp, o, &sr), "SR decode");
        CHECK(sr.mr == 0x42 && strcmp(sr.ra.digits, "447700900123") == 0,
              "SR MR/RA");
        CHECK(sr.scts.hour == 9 && sr.scts.minute == 30,
              "SR TP-SCTS read after RA (spec order)");
        CHECK(sr.dt.hour == 12 && sr.dt.second == 15, "SR TP-DT after SCTS");
        CHECK(sr.fcs == 0x00 && sr.pi == 0x04 && sr.udl == 3 && sr.ud_len == 3 &&
              memcmp(sr.ud, "abc", 3) == 0, "SR ST/PI/UD");
        SmsStatusReport srx;
        CHECK(!status_report_decode(tp, 10, &srx), "SR truncated rejected");
        // minimal PI=0 frame
        uint8_t tp2[24]; size_t p = 0;
        tp2[p++] = 0x06; tp2[p++] = 0x07;
        SmsAddress r2; r2.toa = 0x81; strcpy(r2.digits, "1234");
        p += addr_encode(r2, tp2 + p, 8);
        scts_encode(scts, tp2 + p); p += 7;
        scts_encode(dt, tp2 + p); p += 7;
        tp2[p++] = 0x40; tp2[p++] = 0x00;      // TP-ST, TP-PI=0
        SmsStatusReport sr3;
        CHECK(status_report_decode(tp2, p, &sr3) && sr3.mr == 0x07 &&
              strcmp(sr3.ra.digits, "1234") == 0 && sr3.pi == 0,
              "SR minimal PI=0");
    }
    // 10. L2 ring mechanics (in-memory window)
    {
        static uint8_t win[0x400000];
        memset(win, 0, sizeof(win));
        uint8_t p21[0x1A4];
        memset(p21, 0xAB, sizeof(p21));
        CHECK(l2ring::put_cmd(win, l2ring::kCmdUlGrant, 0, p21, sizeof(p21)), "cmd 0x21 put");
        const uint8_t* body = win + l2ring::kBodyOff;
        CHECK(body[0] == 0x00 && body[4] == 0x21 && body[8] == 0xAB, "cmd body framing");
        const uint16_t* entry = (const uint16_t*)(win + l2ring::kCtlOff + 4);
        CHECK(entry[0] == l2ring::kEnvId && entry[1] == l2ring::kBodySize && entry[2] == 0,
              "ring entry {0x0C, 0x1B8, 0}");
        const l2ring::RingCtl* ctl = (const l2ring::RingCtl*)(win + l2ring::kCtlOff);
        CHECK(ctl->in == 1 && ctl->out == 0, "in=(in+1)%6");
        // DL data message parse
        uint8_t msg[0x50];
        memset(msg, 0, sizeof(msg));
        msg[0x04] = 0x99; msg[0x05] = 0x05;  // id 0x5599 (any; the real id is not pinned yet)
        msg[0x14] = 0x01;                    // selector != 0 -> variant B
        msg[0x2C] = 0x40;                    // len[0] = 0x40
        msg[0x48] = 0x01;                    // count
        msg[0x4C] = 0x12; msg[0x4D] = 0x03;  // idx[0] = 0x0312
        l2ring::DlDataInd ind;
        CHECK(l2ring::parse_dl_data_msg(msg, sizeof(msg), &ind) && ind.count == 1 &&
              ind.len[0] == 0x40 && ind.idx[0] == 0x0312, "DL data msg parse");
        CHECK(l2ring::dl_slot_ptr(win, 0x0312) == win + l2ring::kDlDataOff + 0x0312 * l2ring::kDlDataSize,
              "DlDataMem ptr");
    }
    // 11. EF_IMSI semi-octet decode - pySim test vector :
    //     body 08 29 82 60 82 00 00 20 80 -> IMSI 228062800000208
    //     (parity digit = buf[1] lo nibble, skipped; digit 1 = hi nibble).
    {
        const uint8_t efimsi[] = {0x08, 0x29, 0x82, 0x60, 0x82, 0x00, 0x00, 0x20, 0x80};
        char imsi[16];
        f286::sim::decode_imsi_digits(efimsi, sizeof(efimsi), imsi, sizeof(imsi));
        CHECK(strcmp(imsi, "228062800000208") == 0, "pySim IMSI vector 228062800000208");
    }
    printf(fails ? "SELFTEST: %d FAILURES\n" : "SELFTEST: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
