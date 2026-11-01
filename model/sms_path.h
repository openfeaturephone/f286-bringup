// SPDX-License-Identifier: MIT
// Copyright (c) 2026 willisdylan
// sms_path.h - f286::sms - the SMS data path for FPLinux (Ruio F286 / Unisoc UMS9117,
// Mocor FM_BASE_20A_THIN_MODEM W20.46.4), reconstructed CLEAN for our Linux port.
//
// Grounding: the stock SMS stack is AP-side C in kernel.bin (MNSMS task prio
// 0xBE, MNM delivery engine, TL/RP/CP in the SMS task); the L1 data-plane
// binding is cmd 0x21 (TB block) on the generic data rings. oFono's smsutil.c
// served as a read-only behavioural reference (never copied).
//
// Stock VAs are kernel.bin space, file_off = VA - 0x80a05dc8 for the rodata tables
// below (anchor-verified: 0x8152C10E reads 40 a3 24 a5 e8 e9 ...). Code VAs cited
// per section. Tags:
//   [V]    byte-verified in the stock binary
//   [I]    inferred (offset pinned, semantics deduced)
//   [SPEC] from TS 23.040 / 23.038 / 24.011 / 24.301 / 51.011 - no binary evidence
//          exists (the air SUBMIT builder, the CP/RP byte packers and cause tables
//          could not be located in kernel.bin)
//   [SPEC-CAVEAT] SPEC, and the stock equivalent could not be located
//
// THE ONE-ANSWER SUMMARY (the question "which L1 cmdId carries SMS"):
//   None. There is NO dedicated SMS command on the DSP ring. SMS is an AP-side L3
//   service: by the time it reaches the L1 interface it is MAC transport-block
//   payload indistinguishable from any other DCCH traffic:
//     UL: MNSMS -> CP-DATA -> UL NAS TRANSPORT (EMM 0x65) -> ASM/RRC UL-DCCH ->
//         PDCP/RLC/MAC (AP-side) -> UL data slot win+0x345000 + L2 ring cmd 0x21.
//     DL: DSP -> DlDataMem win+0x3A9000 -> DL data-carrier message (id = the one
//         layout pinned) -> MAC/RLC/
//         PDCP -> RRC/ASM -> EMM DL NAS TRANSPORT (0x66) -> IND_NAM_MT_SMS_RCVD ->
//         MNM/MNSMS -> URC.
//   The DSP-visible commands 0x7a/0x7c are CPHY_PAGING_RECEIVE /
//   CPHY_MBSFN_AREA_INFO_CONFIG  - paging is only the MT wake-up
//   trigger (0x15 idle config arms 0x7a), never an SMS carrier.
//
// Target: little-endian host, no heap, no exceptions, C++17.
// Compiles with: g++ -std=c++17 -Wall -Wextra -c
#pragma once

#include <cstdint>
#include <cstddef>

namespace f286 {
namespace sms {

// ---------------------------------------------------------------------------
// 0. Constants
// ---------------------------------------------------------------------------

constexpr size_t   kUdMaxBytes     = 140;   // [V] TP-UD cap 0x8C, DL parser clamp
constexpr size_t   kAddrMaxDigits  = 20;    // [SPEC] TS 23.040 TP-DA max 12 BCD digits+..; stock SMS_ADDR_MAX_LEN assert
constexpr size_t   kSimRecordSize  = 176;   // [V]+[SPEC] TS 51.011 section 10.5.3 EF_SMS record
constexpr uint8_t  kSimMaxRecords  = 62;    // [V] MN_SMS_IN_SIM_MAX_NUM = 0x3E (EF_SMS row 0x8154B85C)
constexpr size_t   kContainerMin   = 2;     // [V] UL NAS TRANSPORT encoder 0x80bd5474 asserts
constexpr size_t   kContainerMax   = 251;   // [V]  "too long" line 0x49 (>0xFB)
constexpr uint8_t  kCpSmsPd        = 0x09;  // [SPEC] TS 24.011 protocol discriminator, SMS

// SIM EF ids. Rows VERIFIED at 0x8154B85C/0x8154B924/0x8154B9C4 (usim_al EF table);
// 6F42/6F43 names per TS 51.011 (my earlier notes labelled 6F42 "EF_SMSS"; the spec name is
// EF_SMSP = the SCA/parameters record - treat the binary row as SMSP, honestly tagged).
constexpr uint16_t kEfSms  = 0x6F3C;
constexpr uint16_t kEfSmsp = 0x6F42;
constexpr uint16_t kEfSmss = 0x6F43;  // [SPEC] TS 51.011 (not among the binary rows)
constexpr uint16_t kEfSmsr = 0x6F47;

// EMM NAS TRANSPORT message types [V]
constexpr uint8_t kEmmUlNasTransport = 0x65;
constexpr uint8_t kEmmDlNasTransport = 0x66;

// Timers [SPEC: values not extracted from the binary]
constexpr uint32_t kTr1mMs = 30000;  // MO SMS, RP-ACK wait (one outstanding MO)
constexpr uint32_t kTr2mMs = 15000;  // CP-ACK wait

// ---------------------------------------------------------------------------
// 1. GSM 7-bit alphabet - byte-exact stock tables
//
// All four arrays copied from kernel.bin (VERIFIED byte-for-byte ,
// file_off + 0x80a05dc8). Deviations from TS 23.038 that a byte-parity port must
// keep:
//   - byte map index 0x24 = '$' (spec says 0xA4 currency sign); the UCS2 map is
//     spec-correct there (0x00A4)
//   - byte map 0x10..0x1A identity/underscore-mapped (Greek not representable)
//   - the 0x1B extension tables are SEPARATE arrays: byte ext has NO form-feed at
//     0x0A and NO euro at 0x65; the UCS2 ext has the euro U+20AC at 0x65.
// ---------------------------------------------------------------------------

extern const uint8_t  kGsm7ToByte[128];      // stock @0x8152C10E
extern const uint8_t  kGsm7ExtToByte[128];   // stock @0x8152C08E (0x1B-escaped)
extern const uint16_t kGsm7ToUcs2[128];      // stock @0x8152C18E
extern const uint16_t kGsm7ExtToUcs2[128];   // stock @0x8152C28E (0x1B-escaped)
extern const uint8_t  kGsm7ByteToSeptet[256];// derived reverse map; 0xFF = not in GSM7

enum class Alphabet : uint8_t { Gsm7 = 0, Data8 = 1, Ucs2 = 2, Reserved = 3 }; // [I] Mocor MN_SMS_* order
enum class SmsClass : uint8_t { Class0 = 0, Class1, Class2, Class3, None = 0xFF };

// Septet packing - stock roles SPLIT, signatures mirrored so a
// byte-parity diff against the binary is possible.

// Septets (one per byte, GSM-mapped) -> packed bytes, in place. Stock 0x81246560.
// Rotation packing; partial last byte filled with 0x0D (CR) when fill_flag == 0
// and exactly 7 fill bits remain; *n in: septet count, out: packed byte length.
size_t gsm7_pack(uint8_t* septets, uint16_t* n, uint32_t fill_flag);

// Packed bytes -> septets (one per byte) into dest. Stock 0x81246666.
// *out_len = n*8/7. skip134: when != 0, a source septet at every position
// p % 134 == 0 is consumed but not emitted (stock quirk for the 23.038 <CR>
// page-break interval; used only by the NV codec / call-aux paths - our text
// decode uses 0).
void gsm7_expand(const uint8_t* src, uint16_t n_bytes, uint8_t* dest,
                 uint16_t* out_len, int skip134);

// Text <-> septets using the stock tables (extension handling via 0x1B).
// Returns septet count, or SIZE_MAX if any char is not representable in GSM7.
size_t ucs2_to_gsm7_septets(const uint16_t* ucs2, size_t nchars, uint8_t* septets,
                            size_t cap_septets);
// Septets (unpacked) -> UCS2, applying the 0x1B extension tables. Returns chars.
size_t gsm7_septets_to_ucs2(const uint8_t* septets, size_t nseptets,
                            uint16_t* ucs2, size_t cap_chars);

// Codec helpers for SMS-DELIVER TP-UD (7-bit): unpack respecting UDHL padding.
size_t gsm7_ud_decode(const uint8_t* ud, size_t ud_bytes, uint8_t udhl,
                      uint16_t* ucs2, size_t cap_chars);

// ---------------------------------------------------------------------------
// 2. Addresses (TP-DA / TP-OA / RP-DA) - BCD semi-octets
//
// Stock decode 0x80a673e8 + BCD convert 0x80a67344: {len semi-octets; nocts =
// (len+1)>>1; 0x0D -> '?'}; fail if nocts >= 13 (stock: nocts < 0xD required).
// Encode side is SPEC (23.040 section 9.1.2.5) - no binary BCD *builder* was found.
// ---------------------------------------------------------------------------

struct SmsAddress {
    uint8_t toa = 0x81;                    // TON<<4 | NPI (0x81 = national/unknown, 0x91 intl)
    uint8_t len = 0;                       // digit (semi-octet) count, air-format
    char    digits[kAddrMaxDigits + 1] = {0};
};

// Air bytes {len, toa, bcd...} -> SmsAddress. consumed = bytes read (2 + nocts).
bool addr_decode(const uint8_t* buf, size_t cap, size_t* consumed, SmsAddress* out);
// SmsAddress -> air bytes. Returns total byte count (2 + nocts) or 0 on error.
size_t addr_encode(const SmsAddress& a, uint8_t* out, size_t cap);

// ---------------------------------------------------------------------------
// 3. SCTS timestamp (TP-SCTS / TP-DT, TS 23.040 section 9.2.3.11) - semi-octet swizzle
// ---------------------------------------------------------------------------

struct Scts {
    uint16_t year = 0;      // 00..99 (century implicit)
    uint8_t  month = 1, day = 1, hour = 0, minute = 0, second = 0;
    int8_t   tz_quarters = 0;  // timezone in quarter-hour steps, +west/-east sign flipped vs air
};
void scts_encode(const Scts& t, uint8_t out[7]);           // [SPEC] standard layout
bool scts_decode(const uint8_t in[7], Scts* out);          // [SPEC] standard layout

// ---------------------------------------------------------------------------
// 4. TP-DCS - build [V 0x80a63030 expression] / parse [V whitelist squash]
// ---------------------------------------------------------------------------

uint8_t dcs_build(Alphabet alpha, SmsClass cls);
// Stock DL whitelist {0x00, 0x20-0x29, 0x31, 0x32, 0x40-0x47, 0x7F}: anything else
// is zeroed (unsupported-DCS squash). Returns false for whitelisted-but-classless
// encodings the caller should treat as Gsm7/ClassNone.
bool dcs_parse(uint8_t raw, Alphabet* alpha, SmsClass* cls);

// ---------------------------------------------------------------------------
// 5. TPDU codec (TS 23.040 air formats)
//
// Encode side: [SPEC] - a search of the binary found no air SUBMIT builder
// in kernel.bin (text composition lived in the opaque ATC dictionary VM).
// Decode side: constrained by the VERIFIED stock DL parser 0x80a99e20 (FO bits,
// DCS whitelist, UD cap 140, UDH concat 8/16-bit). VP packer semantics follow the
// VERIFIED internal format-tagged packer 0x80c2c4e0 (fmt0 emits nothing, fmt1 7
// octets, fmt2 1 octet, fmt3 14 octets) mapped onto TP-VPF 0/2/1 respectively -
// note the internal enum order differs from the air VPF order; the air mapping
// here is [SPEC].
// ---------------------------------------------------------------------------

enum class TpduType : uint8_t { Deliver = 0, Submit = 1, StatusReport = 2, Command = 3 };

struct TpVp {
    uint8_t format = 0;  // 0 = not present, 1 = relative (1 oct), 2 = absolute (7 oct), 3 = enhanced (7 oct) - [V packer] + [SPEC air mapping]
    uint8_t oct[14] = {0};
};

struct SmsSubmit {
    uint8_t  rd = 0;         // TP-RD: reject duplicates
    uint8_t  vpf = 0;        // TP-VPF: 0 none, 1 relative, 2 absolute, 3 enhanced
    bool     srr = false;    // TP-SRR: status report request
    bool     udhi = false;   // TP-UDHI
    bool     rp = false;     // TP-RP: reply path (stock never tested this bit - [V])
    uint8_t  mr = 0;         // TP-MR (from EF_SMSS last-MR + 1 in stock flow [I])
    SmsAddress da;
    uint8_t  pid = 0;        // TP-PID
    uint8_t  dcs = 0;
    TpVp     vp;
    uint8_t  ud[160] = {0};  // packed UD bytes (cap 140 [V])
    size_t   ud_len = 0;
    size_t   udl = 0;        // septets for Gsm7, bytes for Data8/Ucs2
};

struct SmsDeliver {
    bool     mms = false;    // TP-MMS: more messages to send (stock never reads it [V])
    bool     sri = false;    // TP-SRI
    bool     udhi = false;
    bool     rp = false;
    SmsAddress oa;
    uint8_t  pid = 0;
    uint8_t  dcs = 0;
    Scts     scts;
    uint8_t  ud[160] = {0};
    size_t   ud_len = 0;
    size_t   udl = 0;
};

struct SmsStatusReport {
    bool     udhi = false;
    bool     srq = false;    // TP-SRQ
    uint8_t  mr = 0;         // TP-MR of the submitted message
    SmsAddress ra;           // TP-RA recipient (digits)
    Scts     scts;           // TP-SCTS (SC timestamp of the original SMS-DELIVER)
    Scts     dt;             // TP-DT discharge time
    uint8_t  fcs = 0;        // TP-ST status (a.k.a. FCS; 0 = transaction done)
    uint8_t  pi = 0;         // TP-PI parameter indicator
    uint8_t  ud[160] = {0};
    size_t   ud_len = 0;
    size_t   udl = 0;
};

size_t submit_encode(const SmsSubmit& s, uint8_t* out, size_t cap);       // [SPEC]
bool   deliver_decode(const uint8_t* in, size_t len, SmsDeliver* d);      // [V-constrained]
bool   status_report_decode(const uint8_t* in, size_t len, SmsStatusReport* sr); // [SPEC]+[V FO bits]
TpduType tpdu_type(uint8_t first_octet);                                  // FO bits 1:0

// UDH concat (TS 23.040 section 9.2.3.24). Stock reassembly keys [V 0x80ccd300]:
//   8-bit ref (IEI 0x00): {ref, max, seq}; 16-bit (IEI 0x08): {ref BE u16, max, seq}.
size_t udh_concat_build(uint16_t ref, uint8_t max, uint8_t seq, bool ref16,
                        uint8_t* out, size_t cap, uint8_t* udhl);
bool   udh_concat_parse(const uint8_t* ud, size_t ud_len, uint8_t udhl,
                        uint16_t* ref, uint8_t* max, uint8_t* seq, bool* ref16);

// Segment budget for multipart sends [SPEC TS 23.040/23.038]:
// Gsm7: 160 single / 153+152 concat septets; Ucs2: 70 / 67 chars; Data8: 140 / 134 B.
size_t segment_septet_budget(Alphabet alpha, bool concatenated, bool ref16);

// ---------------------------------------------------------------------------
// 6. RP/CP layer (TS 24.011) - [SPEC-CAVEAT]
//
// NO CP/RP byte constants or packers were found anywhere in the
// binary (0 bitstream-primitive callers in the whole MNSMS + TL regions). The
// layouts below are per-spec; the only binary constraints are the NAS container
// bounds (2..251 [V]) and the TI-pool machinery. On the LTE NAS-transport path
// the correlation is 1:1 (one outstanding MO SMS) [V structure, SPEC mapping].
// ---------------------------------------------------------------------------

// CP message types (24.011 section 8.2 / section 10.3)
constexpr uint8_t kCpData  = 0x01;
constexpr uint8_t kCpAck   = 0x04;
constexpr uint8_t kCpError = 0x10;
// RP message type indicators (24.011 Table 10.4)
constexpr uint8_t kRpDataMsToN = 0x00;
constexpr uint8_t kRpDataNToMs = 0x01;
constexpr uint8_t kRpSmma      = 0x02;
constexpr uint8_t kRpAckMsToN  = 0x04;
constexpr uint8_t kRpAckNToMs  = 0x05;
constexpr uint8_t kRpErrMsToN  = 0x08;
constexpr uint8_t kRpErrNToMs  = 0x09;

struct CpHeader {
    uint8_t ti = 0;    // transaction id (0 on the NAS path - no real TI [SPEC])
    uint8_t type = 0;  // kCpData / kCpAck / kCpError
    uint8_t reference = 0;   // CP-Reference (CpData/CpAck)
    uint8_t cause = 0;       // CpError only
};

// CP-DATA: {TI<<4|0x09, 0x01, CP-Reference, RPDU...}
size_t cp_data_build(uint8_t ti, uint8_t reference, const uint8_t* rpdu, size_t rpdu_len,
                     uint8_t* out, size_t cap);
size_t cp_ack_build(uint8_t ti, uint8_t* out, size_t cap);
size_t cp_error_build(uint8_t ti, uint8_t cause, uint8_t* out, size_t cap);
bool   cp_parse(const uint8_t* in, size_t len, CpHeader* hdr,
                const uint8_t** body, size_t* body_len);

// RP-DATA (MS->N): {0x00, RP-MR, RP-O {0x00,0x00}, RP-D {len,toa,bcd}, RP-UD {len,tpdu}}
size_t rp_data_mo_build(uint8_t mr, const SmsAddress& sca,
                        const uint8_t* tpdu, size_t tpdu_len,
                        uint8_t* out, size_t cap);
// RP-DATA (N->MS): {0x01, RP-MR, RP-O {len,toa,bcd}(SMSC), RP-D {0x00,0x00}, RP-UD}
bool   rp_data_mt_parse(const uint8_t* in, size_t len, uint8_t* mr,
                        SmsAddress* sca, const uint8_t** tpdu, size_t* tpdu_len);
// RP-ACK both directions; RP-ERROR both directions (cause byte).
size_t rp_ack_mo_build(uint8_t mr, uint8_t* out, size_t cap);   // MS->N, for an MT RP-DATA
size_t rp_ack_mt_build(uint8_t mr, uint8_t* out, size_t cap);   // N->MS, for an MO RP-DATA
bool   rp_error_parse(const uint8_t* in, size_t len, uint8_t* mr, uint8_t* cause);

// ---------------------------------------------------------------------------
// 7. NAS-transport envelope (TS 24.301) - the SMS carrier on LTE
//
// UL: plain EMM header {0x70 (PD=7, sec-hdr 0), 0x65, container-LV}. Binary encoder
// 0x80bd5474 verifies: 1 length octet at struct+0x107, container bytes from +0xC,
// bounds 2..251 [V]. DL: type 0x66; the stock decoder was never located - we
// mirror the UL, which per 24.301 section 9.9.3.22 is the LV container.
// ---------------------------------------------------------------------------

size_t nas_transport_ul_build(const uint8_t* container, size_t len,
                              uint8_t* out, size_t cap);
// Strips the EMM header + LV; returns the container pointer into `in`.
bool   nas_transport_dl_parse(const uint8_t* in, size_t len,
                              const uint8_t** container, size_t* container_len);

// One-call MO helper: TPDU -> RP-DATA -> CP-DATA -> NAS TRANSPORT, checking the
// 2..251 container bound at each stage. Returns total byte count or 0.
size_t sms_ul_frame_build(uint8_t cp_ref, uint8_t rp_mr, const SmsAddress& sca,
                          const uint8_t* tpdu, size_t tpdu_len,
                          uint8_t* out, size_t cap);

// ---------------------------------------------------------------------------
// 8. L1/L2 transport binding (all offsets [V])
//
// SMS rides the generic DCCH data path. Constants + ring mechanics replicated
// from the stock senders so our L2 layer can frame MAC TBs exactly like stock:
//   - L2 cmd ring: ctl @win+0xE444 {u16 in; u16 out;}, 6 slots, bodies 6 x 0x1B8
//     @win+0xF160, entry {u16 0x000C, u16 0x01B8, u32 off}, in = (in+1)%6.
//     cmd 0x21 = "transmit UL TBs" (0x1A4-B payload, body[0]=card, body[4]=0x21);
//     cmd 0x100 = "DL bursts free" (0x20-B payload).
//   - UL TB slots: 10 x 0x2800 @win+0x345000, blind round-robin (no free fn [V]).
//   - DL: DlDataMem 0x330 x 0x150 @win+0x3A9000; slot indices arrive in the DL
//     data-carrier message (id not pinned yet, needs a live capture); PHY_DataBufferHandle
//     records 816 x 0x10 @win+0x3E9240.
//   - Gates: send blocked while [0x8175DDC8] (IsDspLoadingCode) == 1 (assert) and
//     silently skipped while *[0x8175E17C] == 0 (opposite polarity, [V Guard B]).
// ---------------------------------------------------------------------------

namespace l2ring {

constexpr uint32_t kCtlOff        = 0x0E444;
constexpr uint32_t kBodyOff       = 0x0F160;
constexpr uint32_t kBodySize      = 0x01B8;
constexpr uint32_t kSlots         = 6;
constexpr uint16_t kEnvId         = 0x000C;  // ring-entry envelope id (not the command)
constexpr uint32_t kCmdUlGrant    = 0x21;
constexpr uint32_t kCmdDlFree     = 0x100;
constexpr uint32_t kUlSlotOff     = 0x345000;
constexpr uint32_t kUlSlotSize    = 0x2800;
constexpr uint32_t kUlSlots       = 10;
constexpr uint32_t kDlDataOff     = 0x3A9000;
constexpr uint32_t kDlDataSize    = 0x150;
constexpr uint32_t kDlDataSlots   = 0x330;
constexpr uint32_t kHandleArrOff  = 0x3E9240;  // 816 x 0x10 PHY_DataBufferHandle

struct RingCtl { uint16_t in; uint16_t out; };

inline bool can_put(const volatile RingCtl* ctl) {
    return ((uint32_t)(ctl->in + 1) % kSlots) != ctl->out;   // stock asserts instead of blocking
}

// Generic L2 command put, stock 0x80C0D608/0x80C0D6B0 mechanics:
// body[0]=card u16, body[4]=cmd u32, payload at body+8 (len 0x1A4 for 0x21,
// 0x20 for 0x100); ring entry {kEnvId, kBodySize, off}; in=(in+1)%6.
// `win` = DSP window base (0x80400000-aperture). Returns false when full.
bool put_cmd(void* win, uint32_t cmd, uint16_t card, const void* payload, size_t len);

// DL data-carrier message layout (, pinned [V]):
struct DlDataInd {
    uint16_t card;
    uint32_t id;        // the carrier id itself (not pinned yet)
    uint32_t matcher;   // == card+0x2B8 snapshot
    uint16_t list_id;   // vs registered-id array card+0x2000+0x344
    uint32_t selector;  // 0 = status variant, != 0 = data variant B
    uint32_t len[2];    // per slot, <= 0x150
    uint16_t count;     // <= 2 (stock clamps + error log)
    uint16_t idx[2];    // DlDataMem slot indices
};
bool parse_dl_data_msg(const uint8_t* msg, size_t msg_len, DlDataInd* out);
inline uint8_t* dl_slot_ptr(void* win, uint16_t idx) {
    return static_cast<uint8_t*>(win) + kDlDataOff + (uint32_t)idx * kDlDataSize;
}

// L2 cmd 0x100 payload (0x20 B, pinned [V]; semantics need one live capture):
struct DlFreePayload {
    uint16_t pad0;       // stale in stock (never written) - replicate or zero
    uint16_t pad2;
    uint16_t mode_flag;  // collector arg2 (1 = pool reset first)
    uint16_t burst_count;
    uint16_t enc_id[4];  // burstIdx<<5, 0xFFFF = empty
    uint32_t mask[4];    // freed-index bitmap per burst
};
static_assert(sizeof(DlFreePayload) == 0x20, "wire size");

} // namespace l2ring

// ---------------------------------------------------------------------------
// 9. SIM storage records (TS 51.011; caps [V])
// ---------------------------------------------------------------------------

// EF_SMS status byte (4-bit classic convention, upper nibble vendor) [SPEC];
// the stock status enum (MN_SMS_MO_TO_BE_SEND .. SR_RECEIVED_AND_STORED, [V
// asserts @0x8158D2AC-0x8158D418]) maps onto these but no binary byte layout
// was extracted.
constexpr uint8_t kRecFree = 0x00;
constexpr uint8_t kRecMtUnread = 0x01;
constexpr uint8_t kRecMtRead = 0x03;
constexpr uint8_t kRecMoSent = 0x05;
constexpr uint8_t kRecMoToSend = 0x07;
constexpr uint8_t kRecSrStored = 0x0D;

struct SimSmsRecord {
    uint8_t status = kRecFree;
    SmsAddress addr;      // TP-OA (MT) / TP-DA (MO), stock stores the decoded form [V]
    uint8_t  pid = 0;
    uint8_t  dcs = 0;
    Scts     scts;
    uint8_t  ud[153] = {0};  // UD bytes; record trailing blob (PID+DCS+SCTS+UDL+UD) <= 163 B [V]
    size_t   ud_len = 0;
    size_t   udl = 0;
};

bool sms_record_encode(const SimSmsRecord& r, uint8_t out[kSimRecordSize]); // [SPEC layout]
bool sms_record_decode(const uint8_t in[kSimRecordSize], SimSmsRecord* r);

// EF_SMSP (28 B): SMSC addr 12 B {len, toa, bcd} + DCS + VP + alpha 14 B [SPEC].
bool smsp_parse(const uint8_t* rec, size_t len, SmsAddress* sca, uint8_t* dcs, uint8_t* vp);
bool smsp_build(const SmsAddress& sca, uint8_t dcs, uint8_t vp, uint8_t* out, size_t cap);

// EF_SMSS (2 B): last-used TP-MR + spare [SPEC].
struct SmssState { uint8_t last_mr; uint8_t spare; };
bool smss_parse(const uint8_t* rec, size_t len, SmssState* out);
bool smss_build(const SmssState& s, uint8_t* out, size_t cap);

// ---------------------------------------------------------------------------
// 10. The task flow - MNSMS-lite
//
// Stock flow, with our facade replacing the
// opaque ATC dictionary VM (every +C* string is verified absent from kernel.bin):
//
//   MO: +CMGS -> submit_pdu/submit_text -> domain select (PS first; stock
//       keeps a CS<->PS retry-fallback flip [V 0x80a68672]) -> EMM conn
//       (CMD_NAM_EST_SMS_CONN equiv) -> CMD_NAM_SEND_SMS_MSG equiv ->
//       UL NAS TRANSPORT -> [L2/L1: slot + cmd 0x21] -> RP-ACK in DL NAS
//       TRANSPORT -> CP-ACK -> EV_MN_APP_SEND_SMS_CNF_F -> "+CMGS: <mr>".
//   MT: paging (0x7a) -> RRC conn -> DL NAS TRANSPORT 0x66 -> CP-DATA(RP-DATA)
//       -> DELIVER decode (+concat) -> store (SIM EF_SMS / ME) -> CP-ACK +
//       CP-DATA(RP-ACK) -> EV_MN_APP_SMS_IND_F -> "+CMT"/"+CMTI".
// ---------------------------------------------------------------------------

enum class EventId : uint8_t {
    SmsReady = 0,
    SendCnf,            // RP-ACK received for our MO  -> "+CMGS: <mr>"
    SendErr,            // RP-ERROR / TR1M expiry      -> "+CMS ERROR"
    SmsInd,             // MT delivered in-band        -> "+CMT"
    MemFullInd,
    MemOverFlowInd,
    ReadCnf,            // +CMGR
    WriteCnf,           // +CMGW
    UpdateStatusCnf,    // +CMGD
    StatusReportInd,    // +CDS
    ReadSmsParamCnf,    // +CSCA?/+CSMP?
    WriteSmsParamCnf,
    SetReceiveFuncCnf,  // +CNMI
};

struct StackEvent {
    EventId  id;
    uint8_t  mr = 0;              // TP-MR of the affected message
    uint8_t  rp_cause = 0;        // RP-ERROR cause (0 = none)
    int      storage = -1;        // Storage enum value for record events
    int      record = -1;         // record index
    bool     concatenated = false;
    SmsDeliver deliver{};         // valid for SmsInd (concat parts carry their own)
};

enum class Storage : int { None = 0, Sim = 1, Me = 2 };

// Ports implement these two against the EMM/NAS client and the storage backend.
class ISmsTransport {
public:
    virtual ~ISmsTransport() = default;
    // Hand a CP-DATA/CP-ACK/CP-ERROR PDU to EMM UL NAS TRANSPORT (f286 NAS port).
    // Return false if not attached / RRC conn cannot be established.
    virtual bool send_nas_container(const uint8_t* cpdu, size_t len) = 0;
};

class ISmsStorage {
public:
    virtual ~ISmsStorage() = default;
    virtual int  find_free(Storage where) = 0;
    virtual bool write(Storage where, int index, const SimSmsRecord& rec) = 0;
    virtual bool read(Storage where, int index, SimSmsRecord* rec) = 0;
    virtual bool erase(Storage where, int index) = 0;
    virtual int  used(Storage where) = 0;
};

// Concat reassembly pool (stock keeps a 124-B snapshot store with 8/16-bit refs,
// [V 0x80ccd300]; ours is a fixed 4-slot pool, no heap).
struct ConcatSlot {
    bool     active = false;
    uint16_t ref = 0;
    uint8_t  total = 0;
    uint8_t  have = 0;
    uint8_t  seen = 0;          // bitmask of arrived seq (total <= 8 supported)
    size_t   ud_bytes = 0;      // accumulated
    Alphabet alpha = Alphabet::Gsm7;
    uint8_t  buf[8 * kUdMaxBytes] = {0};
};

class SmsStack {
public:
    SmsStack(ISmsTransport& tx, ISmsStorage* st);
    using EventFn = void(*)(void* user, const StackEvent& ev);
    void set_event_fn(EventFn fn, void* user) { fn_ = fn; user_ = user; }

    // Configuration (+CSCA / +CSMP equivalents). sca comes from EF_SMSP or the TE.
    void set_sca(const SmsAddress& sca) { sca_ = sca; }
    void set_profile(Alphabet pref, SmsClass cls, uint8_t vp_rel, bool srr);

    // MO, text mode (+CMGS text): chooses alphabet, packs, segments if needed.
    // Returns false (and fires nothing) if the text is empty or unencodable.
    bool submit_text(const uint16_t* ucs2, size_t nchars, const char* da_digits,
                     uint8_t da_toa);
    // MO, PDU mode (+CMGS <len>) - the RECOMMENDED path: the TE supplies the
    // air TPDU; stock has no SUBMIT builder to mirror.
    bool submit_pdu(const uint8_t* tpdu, size_t len);
    // MO, programmatic SUBMIT build (uses sca_, profile, EF_SMSS-style MR counter).
    bool submit_build(const char* da_digits, uint8_t da_toa,
                      const uint16_t* ucs2, size_t nchars);

    // DL side: feed the NAS-transport container from the EMM port (type 0x66).
    void on_nas_container(const uint8_t* cpdu, size_t len);

    // Timers: call periodically with a monotonic millisecond clock.
    void tick(uint32_t now_ms);

    bool     idle() const { return mo_state_ == MoState::Idle; }
    uint8_t  next_mr() { return ++mr_; }   // EF_SMSS semantics [I]

private:
    enum class MoState : uint8_t { Idle, WaitRpAck };
    void fire(const StackEvent& ev);
    bool send_cp(const uint8_t* p, size_t n) { return tx_.send_nas_container(p, n); }
    void handle_cp_data(const uint8_t* body, size_t len);
    void handle_cp_ack();
    void handle_cp_error(uint8_t cause);
    void start_mo_timer() { timer_deadline_ = now_ms_ + kTr1mMs; timer_armed_ = true; }

    ISmsTransport& tx_;
    ISmsStorage*   st_;
    EventFn        fn_ = nullptr;
    void*          user_ = nullptr;

    SmsAddress sca_{};
    Alphabet   pref_alpha_ = Alphabet::Gsm7;
    SmsClass   cls_ = SmsClass::Class1;
    uint8_t    vp_rel_ = 0xA7;              // 24 h, TS 23.040 section 9.2.3.12.1 [SPEC]
    bool       srr_ = false;
    uint8_t    mr_ = 0;                     // last TP-MR (EF_SMSS byte [SPEC])
    uint8_t    cp_ref_ = 0;

    MoState    mo_state_ = MoState::Idle;
    bool       timer_armed_ = false;
    uint32_t   timer_deadline_ = 0;
    uint32_t   now_ms_ = 0;
    uint16_t   concat_ref_ = 0;             // our MO multipart counter

    ConcatSlot concat_[4];
};

} // namespace sms
} // namespace f286
