/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


// Host-side tests for WPA2-Personal: PBKDF2 and the PRF against reference values (computed with Python's hashlib / hmac and the 802.11i test passphrases), RFC 3394 key wrap, the
// EAPOL-Key frame format, and the supplicant against a test authenticator through the four-way handshake, retransmissions, replays, bad MICs, tampering and the group key handshake.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/WirelessInterface/Wpa2.hpp"

using namespace hardware;
using namespace hardware::wpa2;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

static Bytes hex(const char* s) {
  Bytes b;
  for (; s[0] && s[1]; s += 2) {
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    b.push_back(static_cast<uint8_t>(v(s[0]) * 16 + v(s[1])));
  }
  return b;
}
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

static const uint8_t kAa[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, kSpa[6] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5};

struct Authenticator {
  uint8_t pmk[kPmkBytes], anonce[kNonceBytes], snonce[kNonceBytes] = {}, ptk[kPtkBytes] = {};
  uint64_t counter = 100;
  Bytes rsn_ie;
  uint8_t gtk[16];
  uint8_t gtk_id = 2;

  static void put_counter(EapolKey& k, uint64_t c) {
    for (int i = 0; i < 8; ++i) k.replay_counter[i] = static_cast<uint8_t>(c >> (8 * (7 - i)));
  }
  Bytes message1() {
    EapolKey k;
    k.info = kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoAck;
    put_counter(k, ++counter);
    memcpy(k.nonce, anonce, 32);
    Bytes out(400);
    out.resize(build_eapol_key(k, ByteView(out.data(), out.size())));
    return out;
  }
  bool check_message2(const Bytes& m2) {
    EapolKey k;
    if (!parse_eapol_key(view(m2), &k)) return false;
    memcpy(snonce, k.nonce, 32);
    derive_ptk(pmk, kAa, kSpa, anonce, snonce, ptk);
    if (!verify_mic(ConstByteView(ptk, 16), view(m2))) return false;
    if (k.info != (kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoMic)) return false;
    return k.data.size() == rsn_ie.size() && memcmp(k.data.data(), rsn_ie.data(), rsn_ie.size()) == 0 && k.counter() == counter;
  }
  Bytes wrap_gtk_data(bool with_rsn) {
    Bytes plain;
    if (with_rsn) plain.insert(plain.end(), rsn_ie.begin(), rsn_ie.end());
    const uint8_t kde[8] = {0xDD, 22, 0x00, 0x0F, 0xAC, 1, gtk_id, 0};
    plain.insert(plain.end(), kde, kde + 8);
    plain.insert(plain.end(), gtk, gtk + 16);
    if (plain.size() % 8) { plain.push_back(0xDD); while (plain.size() % 8) plain.push_back(0); }
    Bytes wrapped(plain.size() + 8);
    aes_key_wrap(ConstByteView(ptk + 16, 16), view(plain), ByteView(wrapped.data(), wrapped.size()));
    return wrapped;
  }
  Bytes message3(bool with_rsn = true) {
    EapolKey k;
    k.info = kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoInstall | kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData;
    k.key_length = 16;
    put_counter(k, ++counter);
    memcpy(k.nonce, anonce, 32);
    Bytes data = wrap_gtk_data(with_rsn);
    k.data = view(data);
    Bytes out(400);
    out.resize(build_eapol_key(k, ByteView(out.data(), out.size())));
    set_mic(ConstByteView(ptk, 16), ByteView(out.data(), out.size()));
    return out;
  }
  bool check_message4(const Bytes& m4) {
    EapolKey k;
    return parse_eapol_key(view(m4), &k) && verify_mic(ConstByteView(ptk, 16), view(m4)) && k.info == (kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoMic | kInfoSecure) && k.data.size() == 0;
  }
  Bytes group_message1(uint8_t id) {
    EapolKey k;
    k.info = kInfoVersionHmacSha1Aes | kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData | (static_cast<uint16_t>(id) << 4);
    k.key_length = 16;
    put_counter(k, ++counter);
    gtk_id = id;
    Bytes data = wrap_gtk_data(false);
    k.data = view(data);
    Bytes out(400);
    out.resize(build_eapol_key(k, ByteView(out.data(), out.size())));
    set_mic(ConstByteView(ptk, 16), ByteView(out.data(), out.size()));
    return out;
  }
};

static Bytes rsn_element() {
  return hex("30140100000fac040100000fac040100000fac020000");
}

int main() {
  // PBKDF2 / PMK (the 802.11i H.4 test passphrases)
  uint8_t pmk[32];
  CHECK(derive_pmk(StringView("password"), ConstByteView(reinterpret_cast<const uint8_t*>("IEEE"), 4), pmk) && Bytes(pmk, pmk + 32) == hex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
  CHECK(derive_pmk(StringView("ThisIsAPassword"), ConstByteView(reinterpret_cast<const uint8_t*>("ThisIsASSID"), 11), pmk) && Bytes(pmk, pmk + 32) == hex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
  const uint8_t ssid[] = {'a'};
  CHECK(!derive_pmk(StringView("short"), ConstByteView(ssid, 1), pmk) && !derive_pmk(StringView("good passphrase"), ConstByteView(), pmk));
  CHECK(!derive_pmk(StringView("bad\x01passphrase"), ConstByteView(ssid, 1), pmk));
  char sixty_four[65];
  memset(sixty_four, 'x', 64); sixty_four[64] = 0;
  CHECK(!derive_pmk(StringView(sixty_four), ConstByteView(ssid, 1), pmk));
  // the PTK and a 64 byte PRF
  Bytes pmk2 = hex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
  uint8_t an[32], sn[32], ptk[48];
  for (int i = 0; i < 32; ++i) { an[i] = static_cast<uint8_t>((i * 7 + 1) % 256); sn[i] = static_cast<uint8_t>((i * 5 + 3) % 256); }
  derive_ptk(pmk2.data(), kAa, kSpa, an, sn, ptk);
  CHECK(Bytes(ptk, ptk + 48) == hex("b0b74fc43780a8cb25e32cd763768ffc432756d20fb879e9ecb62ca04dd5fbde242ce76e3f25365bc3c462d618f36ac9"));
  uint8_t ptk_swapped[48];
  derive_ptk(pmk2.data(), kSpa, kAa, sn, an, ptk_swapped);                       // the order of the addresses and nonces does not matter
  CHECK(memcmp(ptk, ptk_swapped, 48) == 0);
  uint8_t long_prf[64];
  prf_sha1(view(pmk2), StringView("Test"), ConstByteView(reinterpret_cast<const uint8_t*>("xyz"), 3), ByteView(long_prf, 64));
  CHECK(Bytes(long_prf, long_prf + 64) == hex("69fbf98eaecb30b03400c856c1395ed9cd334203c9bec34faeb37d2fa951fcdcac09c0d353fbfa92e012effc99ea417b56c47babb649f858ac216c98ad259d7d"));
  // RFC 3394 4.1
  Bytes kek = hex("000102030405060708090A0B0C0D0E0F"), key = hex("00112233445566778899AABBCCDDEEFF"), wrapped(24), unwrapped(16);
  CHECK(aes_key_wrap(view(kek), view(key), ByteView(wrapped.data(), 24)) && wrapped == hex("1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5"));
  CHECK(aes_key_unwrap(view(kek), view(wrapped), ByteView(unwrapped.data(), 16)) && unwrapped == key);
  wrapped[5] ^= 1;
  CHECK(!aes_key_unwrap(view(kek), view(wrapped), ByteView(unwrapped.data(), 16)));
  CHECK(!aes_key_wrap(view(kek), ConstByteView(key.data(), 15), ByteView(wrapped.data(), 24)) && !aes_key_unwrap(view(kek), ConstByteView(wrapped.data(), 16), ByteView(unwrapped.data(), 16)));

  // frames
  EapolKey k;
  k.info = 0x13CA; k.key_length = 16; k.replay_counter[7] = 9; for (int i = 0; i < 32; ++i) k.nonce[i] = static_cast<uint8_t>(i);
  Bytes data{1, 2, 3, 4, 5};
  k.data = view(data);
  Bytes buf(400);
  const uint32_t len = build_eapol_key(k, ByteView(buf.data(), buf.size()));
  CHECK(len == 99 + 5 && buf[0] == 2 && buf[1] == 3 && buf[2] == 0 && buf[3] == 100 && buf[4] == 2);
  EapolKey back;
  CHECK(parse_eapol_key(ConstByteView(buf.data(), len), &back) && back.info == 0x13CA && back.key_length == 16 && back.counter() == 9 && back.nonce[31] == 31 && back.data.size() == 5);
  CHECK(!parse_eapol_key(ConstByteView(buf.data(), len - 1), &back) && !parse_eapol_key(ConstByteView(buf.data(), 98), &back));
  Bytes not_key(buf.begin(), buf.begin() + len); not_key[1] = 0;
  CHECK(!parse_eapol_key(view(not_key), &back));
  Bytes wpa_desc(buf.begin(), buf.begin() + len); wpa_desc[4] = 254;
  CHECK(!parse_eapol_key(view(wpa_desc), &back));
  Bytes lying(buf.begin(), buf.begin() + len); lying[3] = 250;
  CHECK(!parse_eapol_key(view(lying), &back));
  k.data = ConstByteView(buf.data(), 300);
  CHECK(build_eapol_key(k, ByteView(buf.data() + 100, 300)) == 0);

  // the handshake
  const Bytes rsn = rsn_element();
  Authenticator ap;
  derive_pmk(StringView("hunter2hunter2"), ConstByteView(reinterpret_cast<const uint8_t*>("Home"), 4), ap.pmk);
  memcpy(ap.anonce, an, 32);
  ap.rsn_ie = rsn;
  for (int i = 0; i < 16; ++i) ap.gtk[i] = static_cast<uint8_t>(0x40 + i);
  Supplicant sup;
  sup.start(ap.pmk, kAa, kSpa, view(rsn), sn);
  uint8_t reply[400];
  Supplicant::Result res;
  Bytes m1 = ap.message1();
  CHECK(sup.on_eapol(view(m1), ByteView(reply, sizeof reply), &res) && res.reply_length > 0 && !res.install_ptk && sup.state() == Supplicant::State::WaitMessage3);
  CHECK(ap.check_message2(Bytes(reply, reply + res.reply_length)));
  // message 3 before... a message 3 with a bad MIC, a wrong nonce, a stale counter, lacking flags: all dropped
  Bytes m3 = ap.message3();
  for (size_t i = 0; i < m3.size(); ++i) {                                     // every single flipped bit is refused (the MIC, the flags, the data, the nonce)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes c = m3; c[i] ^= static_cast<uint8_t>(1 << bit);
      Supplicant copy = sup;
      Supplicant::Result r;
      const bool accepted = copy.on_eapol(view(c), ByteView(reply, sizeof reply), &r);
      if (accepted && !(i < 4)) { printf("  accepted flip at byte %zu bit %d\n", i, bit); CHECK(false); break; }                      // (the frame's version / length header bytes are outside the MIC; a length flip is caught by the parser)
    }
  }
  Supplicant::Result r3;
  CHECK(sup.on_eapol(view(m3), ByteView(reply, sizeof reply), &r3) && r3.install_ptk && r3.install_gtk && r3.handshake_complete && r3.reply_length > 0 && sup.state() == Supplicant::State::Established);
  CHECK(ap.check_message4(Bytes(reply, reply + r3.reply_length)));
  CHECK(sup.gtk().id == 2 && sup.gtk().length == 16 && sup.gtk().key[0] == 0x40 && sup.gtk().key[15] == 0x4F);
  CHECK(memcmp(sup.tk(), ap.ptk + 32, 16) == 0);
  // a replayed message 3 (same counter) is dropped; a retransmission with a new counter is answered but installs nothing again
  CHECK(!sup.on_eapol(view(m3), ByteView(reply, sizeof reply), &res));
  Bytes m3b = ap.message3();
  CHECK(sup.on_eapol(view(m3b), ByteView(reply, sizeof reply), &res) && res.reply_length > 0 && !res.install_ptk && !res.install_gtk && !res.handshake_complete);
  // a message 1 after the handshake is ignored (no reinstallation)
  Bytes m1b = ap.message1();
  CHECK(!sup.on_eapol(view(m1b), ByteView(reply, sizeof reply), &res));
  // the group key handshake: a new key and id
  for (int i = 0; i < 16; ++i) ap.gtk[i] = static_cast<uint8_t>(0x90 + i);
  Bytes g1 = ap.group_message1(3);
  CHECK(sup.on_eapol(view(g1), ByteView(reply, sizeof reply), &res) && res.install_gtk && !res.install_ptk && res.reply_length > 0 && sup.gtk().id == 3 && sup.gtk().key[0] == 0x90);
  { EapolKey gk; CHECK(parse_eapol_key(ConstByteView(reply, res.reply_length), &gk) && verify_mic(ConstByteView(ap.ptk, 16), ConstByteView(reply, res.reply_length)) && gk.info == (kInfoVersionHmacSha1Aes | kInfoMic | kInfoSecure | 0x30)); }
  CHECK(!sup.on_eapol(view(g1), ByteView(reply, sizeof reply), &res));         // replay
  Bytes g_bad = ap.group_message1(1); g_bad[g_bad.size() - 1] ^= 1;
  CHECK(!sup.on_eapol(view(g_bad), ByteView(reply, sizeof reply), &res) && sup.gtk().id == 3);

  // without message 1 first, nothing is answered
  Supplicant fresh;
  fresh.start(ap.pmk, kAa, kSpa, view(rsn), sn);
  Authenticator ap2 = ap;
  CHECK(!fresh.on_eapol(view(ap2.message3()), ByteView(reply, sizeof reply), &res) && !fresh.on_eapol(view(g1), ByteView(reply, sizeof reply), &res));
  // a message 3 with a wrong nonce (a new ANonce that message 1 never announced)
  Supplicant s3;
  s3.start(ap.pmk, kAa, kSpa, view(rsn), sn);
  Authenticator ap3 = ap;
  Bytes a1 = ap3.message1();
  CHECK(s3.on_eapol(view(a1), ByteView(reply, sizeof reply), &res) && ap3.check_message2(Bytes(reply, reply + res.reply_length)));
  ap3.anonce[0] ^= 1;
  CHECK(!s3.on_eapol(view(ap3.message3()), ByteView(reply, sizeof reply), &res));
  // a message 3 whose key data has no group key
  ap3.anonce[0] ^= 1;
  { Supplicant s4; s4.start(ap.pmk, kAa, kSpa, view(rsn), sn); Authenticator ap4 = ap; Bytes b1 = ap4.message1(); s4.on_eapol(view(b1), ByteView(reply, sizeof reply), &res); ap4.check_message2(Bytes(reply, reply + res.reply_length));
    ap4.gtk_id = 0;                                                                                // key id 0 is not a group key
    CHECK(!s4.on_eapol(view(ap4.message3()), ByteView(reply, sizeof reply), &res) && s4.state() == Supplicant::State::Failed && s4.failure()[0]); }
  // a wrong passphrase on our side: message 3's MIC cannot be right
  { uint8_t wrong[32]; derive_pmk(StringView("not the password"), ConstByteView(reinterpret_cast<const uint8_t*>("Home"), 4), wrong);
    Supplicant s5; s5.start(wrong, kAa, kSpa, view(rsn), sn); Authenticator ap5 = ap; Bytes b1 = ap5.message1();
    CHECK(s5.on_eapol(view(b1), ByteView(reply, sizeof reply), &res));                                // message 2 goes out (it cannot be checked here)...
    CHECK(!ap5.check_message2(Bytes(reply, reply + res.reply_length)));                                // ...and the access point's check of its MIC fails: the AP sends no message 3
  }
  // too small a reply buffer: no reply, no crash
  { Supplicant s6; s6.start(ap.pmk, kAa, kSpa, view(rsn), sn); Authenticator ap6 = ap; Bytes b1 = ap6.message1(); uint8_t tiny[20]; CHECK(!s6.on_eapol(view(b1), ByteView(tiny, sizeof tiny), &res)); }
  // random bytes
  uint32_t seed = 5;
  for (int round = 0; round < 2000; ++round) {
    Bytes junk(60 + round % 300);
    for (auto& b : junk) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
    if (round % 2) { junk[0] = 2; junk[1] = 3; junk[2] = 0; junk[3] = static_cast<uint8_t>(junk.size() - 4); junk[4] = 2; }
    Supplicant::Result jr;
    Supplicant s7; s7.start(ap.pmk, kAa, kSpa, view(rsn), sn);
    (void)s7.on_eapol(view(junk), ByteView(reply, sizeof reply), &jr);
  }
  printf(g_failed ? "Wpa2Test: %d FAILED\n" : "Wpa2Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
