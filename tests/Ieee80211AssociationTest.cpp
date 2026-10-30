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


// Host-side tests for joining a network: the 802.11 frame builders and parsers, the RSN negotiation, and the client state machine against a simulated access point (open and
// WPA2-Personal networks, refusals, silence and retries, a wrong passphrase, deauthentication, frames from strangers).
#include <stdio.h>
#include <string.h>
#include <functional>
#include <vector>
#include "Hardware/WirelessInterface/Ieee80211Association.hpp"

using namespace hardware;
using namespace hardware::ieee80211;
using namespace hardware::wpa2;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }
static Bytes hex(const char* s) {
  Bytes b;
  for (; s[0] && s[1]; s += 2) {
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    b.push_back(static_cast<uint8_t>(v(s[0]) * 16 + v(s[1])));
  }
  return b;
}

static const uint8_t kBssid[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, kUs[6] = {0x28, 0xD2, 0x44, 0x01, 0x02, 0x03};

struct FakeLink final : IWifiLink {
  std::vector<Bytes> sent;
  std::function<void(const Bytes&)> on_send;
  uint64_t now = 1000;
  Bytes ptk_installed;
  GroupKey gtk_installed;
  bool gtk_seen = false, opened = false, refuse_keys = false;
  uint16_t aid = 0;
  bool transmit(ConstByteView f) override {
    sent.push_back(Bytes(f.data(), f.data() + f.size()));
    if (on_send) on_send(sent.back());
    return true;
  }
  bool install_pairwise_key(const uint8_t tk[kTkBytes]) override { if (refuse_keys) return false; ptk_installed = Bytes(tk, tk + kTkBytes); return true; }
  bool install_group_key(const GroupKey& k) override { gtk_installed = k; gtk_seen = true; return true; }
  void port_opened(uint16_t a) override { opened = true; aid = a; }
  uint64_t now_ms() override { return now; }
};

// frames from the access point to us
static Bytes ap_header(uint16_t fc, const uint8_t* from = kBssid) {
  Bytes f{static_cast<uint8_t>(fc), static_cast<uint8_t>(fc >> 8), 0, 0};
  f.insert(f.end(), kUs, kUs + 6);
  f.insert(f.end(), from, from + 6);
  f.insert(f.end(), from, from + 6);
  f.insert(f.end(), {0, 0});
  return f;
}
static Bytes auth_response(uint16_t status, const uint8_t* from = kBssid) { Bytes f = ap_header(0x00B0, from); f.insert(f.end(), {0, 0, 2, 0, static_cast<uint8_t>(status), static_cast<uint8_t>(status >> 8)}); return f; }
static Bytes assoc_response(uint16_t status, uint16_t aid) {
  Bytes f = ap_header(0x0010);
  f.insert(f.end(), {0x11, 0x04, static_cast<uint8_t>(status), static_cast<uint8_t>(status >> 8), static_cast<uint8_t>(aid), static_cast<uint8_t>((aid >> 8) | 0xC0)});
  return f;
}
static Bytes deauth(const uint8_t* from = kBssid) { Bytes f = ap_header(0x00C0, from); f.insert(f.end(), {7, 0}); return f; }
static Bytes eapol_to_us(const Bytes& eapol, bool qos = false) {
  Bytes f = ap_header(qos ? 0x0288 : 0x0208);
  if (qos) f.insert(f.end(), {0, 0});
  f.insert(f.end(), {0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E});
  f.insert(f.end(), eapol.begin(), eapol.end());
  return f;
}

// the test authenticator of the handshake (see Wpa2Test)
struct Authenticator {
  uint8_t pmk[kPmkBytes], anonce[kNonceBytes], snonce[kNonceBytes] = {}, ptk[kPtkBytes] = {};
  uint64_t counter = 100;
  Bytes rsn_ie;
  uint8_t gtk[16];
  static void put_counter(EapolKey& k, uint64_t c) { for (int i = 0; i < 8; ++i) k.replay_counter[i] = static_cast<uint8_t>(c >> (8 * (7 - i))); }
  Bytes message1() {
    EapolKey k; k.info = kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoAck; put_counter(k, ++counter); memcpy(k.nonce, anonce, 32);
    Bytes out(400); out.resize(build_eapol_key(k, ByteView(out.data(), out.size()))); return out;
  }
  bool check_message2(const Bytes& m2) {
    EapolKey k;
    if (!parse_eapol_key(view(m2), &k)) return false;
    memcpy(snonce, k.nonce, 32);
    derive_ptk(pmk, kBssid, kUs, anonce, snonce, ptk);
    return verify_mic(ConstByteView(ptk, 16), view(m2));
  }
  Bytes message3() {
    EapolKey k;
    k.info = kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoInstall | kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData;
    k.key_length = 16; put_counter(k, ++counter); memcpy(k.nonce, anonce, 32);
    Bytes plain(rsn_ie);
    const uint8_t kde[8] = {0xDD, 22, 0x00, 0x0F, 0xAC, 1, 1, 0};
    plain.insert(plain.end(), kde, kde + 8); plain.insert(plain.end(), gtk, gtk + 16);
    if (plain.size() % 8) { plain.push_back(0xDD); while (plain.size() % 8) plain.push_back(0); }
    Bytes wrapped(plain.size() + 8);
    aes_key_wrap(ConstByteView(ptk + 16, 16), view(plain), ByteView(wrapped.data(), wrapped.size()));
    k.data = view(wrapped);
    Bytes out(400); out.resize(build_eapol_key(k, ByteView(out.data(), out.size())));
    set_mic(ConstByteView(ptk, 16), ByteView(out.data(), out.size()));
    return out;
  }
};

static const char* kOurRsn = "30140100000fac040100000fac040100000fac020000";

static WifiNetwork network(WifiSecurity sec, uint8_t channel = 6) {
  WifiNetwork n;
  strcpy(n.ssid, "Home");
  memcpy(n.bssid.bytes, kBssid, 6);
  n.security = sec;
  n.channel = channel;
  return n;
}
static uint8_t g_snonce[32];

int main() {
  for (int i = 0; i < 32; ++i) g_snonce[i] = static_cast<uint8_t>(0x55 + i);
  // ---- frames
  uint8_t buf[300];
  CHECK(build_authentication(kBssid, kUs, ByteView(buf, sizeof buf)) == 30 && buf[0] == 0xB0 && buf[4] == 0xA0 && buf[10] == 0x28 && buf[16] == 0xA0 && buf[24] == 0 && buf[26] == 1 && buf[28] == 0);
  CHECK(build_authentication(kBssid, kUs, ByteView(buf, 29)) == 0);
  const uint8_t ssid[] = {'H', 'o', 'm', 'e'};
  const Bytes rsn = hex(kOurRsn);
  uint32_t n = build_association_request(kBssid, kUs, ConstByteView(ssid, 4), false, view(rsn), ByteView(buf, sizeof buf));
  CHECK(n == 24 + 4 + 6 + 10 + 6 + 22 && buf[0] == 0x00 && buf[24] == 0x11 && buf[25] == 0x00 && buf[28] == 0 && buf[29] == 4 && buf[30] == 'H' && buf[n - 22] == 48);
  n = build_association_request(kBssid, kUs, ConstByteView(ssid, 4), true, ConstByteView(), ByteView(buf, sizeof buf));
  CHECK(n == 24 + 4 + 6 + 10 && buf[24] == 0x01 && buf[34] == 1 && buf[36] == 0x8C);
  CHECK(build_association_request(kBssid, kUs, ConstByteView(), false, ConstByteView(), ByteView(buf, sizeof buf)) == 0);
  CHECK(build_deauthentication(kBssid, kUs, 3, ByteView(buf, sizeof buf)) == 26 && buf[0] == 0xC0 && buf[24] == 3);
  const Bytes eapol{2, 3, 0, 1, 0xEE};
  n = build_eapol_data(kBssid, kUs, view(eapol), ByteView(buf, sizeof buf));
  CHECK(n == 24 + 8 + 5 && buf[0] == 0x08 && buf[1] == 0x01 && buf[24] == 0xAA && buf[30] == 0x88 && buf[31] == 0x8E && buf[32] == 2);
  // parsing
  MgmtFrame m;
  Bytes a = auth_response(0);
  CHECK(parse_management(view(a), kBssid, kUs, &m) && m.subtype == kSubtypeAuthentication && m.algorithm == 0 && m.sequence == 2 && m.status == 0);
  Bytes as = assoc_response(0, 5);
  CHECK(parse_management(view(as), kBssid, kUs, &m) && m.subtype == kSubtypeAssocResponse && m.association_id == 5);
  Bytes de = deauth();
  CHECK(parse_management(view(de), kBssid, kUs, &m) && m.subtype == kSubtypeDeauthentication && m.reason == 7);
  const uint8_t stranger[6] = {1, 2, 3, 4, 5, 6};
  Bytes other = auth_response(0, stranger);
  CHECK(!parse_management(view(other), kBssid, kUs, &m));
  CHECK(!parse_management(ConstByteView(a.data(), 29), kBssid, kUs, &m));
  Bytes not_us = a; not_us[4] ^= 0xFF;
  CHECK(!parse_management(view(not_us), kBssid, kUs, &m));
  Bytes data_frame = eapol_to_us(eapol);
  CHECK(!parse_management(view(data_frame), kBssid, kUs, &m));
  ConstByteView e = parse_eapol_data(view(data_frame), kBssid, kUs);
  CHECK(e.size() == 5 && e[4] == 0xEE);
  Bytes qos = eapol_to_us(eapol, true);
  e = parse_eapol_data(view(qos), kBssid, kUs);
  CHECK(e.size() == 5);
  Bytes protected_frame = data_frame; protected_frame[1] |= 0x40;
  CHECK(parse_eapol_data(view(protected_frame), kBssid, kUs).size() == 0);
  Bytes to_ds = data_frame; to_ds[1] = 0x01;
  CHECK(parse_eapol_data(view(to_ds), kBssid, kUs).size() == 0);
  Bytes wrong_type = data_frame; wrong_type[30] = 0x08;
  CHECK(parse_eapol_data(view(wrong_type), kBssid, kUs).size() == 0 && parse_eapol_data(ConstByteView(data_frame.data(), 20), kBssid, kUs).size() == 0);
  // RSN
  Bytes beacon = ap_header(0x0080);
  beacon.insert(beacon.end(), 12, 0);
  const Bytes ap_rsn = hex("30140100000fac040100000fac040100000fac020000");
  beacon.insert(beacon.end(), {0, 4, 'H', 'o', 'm', 'e'});
  beacon.insert(beacon.end(), ap_rsn.begin(), ap_rsn.end());
  ConstByteView found = find_rsn_element(view(beacon));
  CHECK(found.size() == 22 && found[0] == 48);
  uint8_t ours[40];
  const char* why = nullptr;
  CHECK(negotiate_rsn(found, ByteView(ours, sizeof ours), &why) == 22 && memcmp(ours, rsn.data(), 22) == 0);
  CHECK(find_rsn_element(ConstByteView(beacon.data(), beacon.size() - 3)).size() == 0);
  Bytes tkip_group = ap_rsn; tkip_group[7] = 2;
  CHECK(negotiate_rsn(view(tkip_group), ByteView(ours, sizeof ours), &why) == 0 && why);
  Bytes no_ccmp = ap_rsn; no_ccmp[13] = 2;
  CHECK(negotiate_rsn(view(no_ccmp), ByteView(ours, sizeof ours), &why) == 0);
  Bytes enterprise = ap_rsn; enterprise[19] = 1;
  CHECK(negotiate_rsn(view(enterprise), ByteView(ours, sizeof ours), &why) == 0);
  Bytes mfpr = ap_rsn; mfpr[20] = 0x40;
  CHECK(negotiate_rsn(view(mfpr), ByteView(ours, sizeof ours), &why) == 0);
  CHECK(negotiate_rsn(ConstByteView(ap_rsn.data(), 10), ByteView(ours, sizeof ours), &why) == 0 && negotiate_rsn(ConstByteView(), ByteView(ours, sizeof ours), &why) == 0);
  CHECK(negotiate_rsn(view(ap_rsn), ByteView(ours, 10), &why) == 0);

  // ---- an open network
  {
    FakeLink link;
    Ieee80211Association assoc(link);
    link.on_send = [&](const Bytes& f) {
      if (f[0] == 0xB0) assoc.on_frame(view(auth_response(0)));
      else if (f[0] == 0x00) assoc.on_frame(view(assoc_response(0, 7)));
    };
    CHECK(assoc.start(network(WifiSecurity::Open), ConstByteView(), StringView(), g_snonce, kUs) == WifiResult::Ok);
    CHECK(assoc.state() == Ieee80211Association::State::Connected && link.opened && link.aid == 7 && link.sent.size() == 2 && !assoc.secure());
    assoc.leave();
    CHECK(link.sent.back()[0] == 0xC0 && assoc.state() == Ieee80211Association::State::Idle);
  }
  // ---- WPA2
  const Bytes ssid_bytes{'H', 'o', 'm', 'e'};
  auto run_wpa2 = [&](const char* ap_pass, const char* our_pass, bool deauth_midway, bool drop_msg2, bool qos_frames, Ieee80211Association::State want, bool want_silent) {
    FakeLink link;
    Ieee80211Association assoc(link);
    Authenticator ap;
    derive_pmk(StringView(ap_pass), view(ssid_bytes), ap.pmk);
    for (int i = 0; i < 32; ++i) ap.anonce[i] = static_cast<uint8_t>(i * 3 + 1);
    ap.rsn_ie = rsn;
    for (int i = 0; i < 16; ++i) ap.gtk[i] = static_cast<uint8_t>(0x30 + i);
    bool m4_seen = false;
    link.on_send = [&](const Bytes& f) {
      if (f[0] == 0xB0) assoc.on_frame(view(auth_response(0)));
      else if (f[0] == 0x00) {
        assoc.on_frame(view(assoc_response(0, 3)));
        assoc.on_frame(view(eapol_to_us(ap.message1(), qos_frames)));                            // the access point starts the handshake
      } else if (f[0] == 0x08 && f[1] == 0x01) {
        ConstByteView p(f.data() + 32, f.size() - 32);
        EapolKey k;
        if (!parse_eapol_key(p, &k)) return;
        if (!(k.info & kInfoSecure)) {                                                           // message 2
          if (deauth_midway) { assoc.on_frame(view(deauth())); return; }
          if (drop_msg2) return;
          if (ap.check_message2(Bytes(p.data(), p.data() + p.size()))) assoc.on_frame(view(eapol_to_us(ap.message3(), qos_frames)));
        } else {
          m4_seen = true;
        }
      }
    };
    const WifiResult r = assoc.start(network(WifiSecurity::Wpa2), view(ap_rsn), StringView(our_pass), g_snonce, kUs);
    CHECK(r == WifiResult::Ok);
    if (want == Ieee80211Association::State::Handshaking) {
      CHECK(assoc.state() == Ieee80211Association::State::Handshaking);
      link.now += 6000;
      assoc.poll();
    }
    CHECK(assoc.state() == want || (want == Ieee80211Association::State::Handshaking && assoc.state() == Ieee80211Association::State::Failed));
    CHECK(assoc.bad_passphrase_suspected() == want_silent);
    if (want == Ieee80211Association::State::Connected) {
      CHECK(link.opened && link.aid == 3 && m4_seen && link.ptk_installed.size() == 16 && memcmp(link.ptk_installed.data(), ap.ptk + 32, 16) == 0);
      CHECK(link.gtk_seen && link.gtk_installed.id == 1 && link.gtk_installed.key[0] == 0x30 && assoc.secure());
    } else {
      CHECK(!link.opened && assoc.failure()[0]);
    }
  };
  run_wpa2("hunter2hunter2", "hunter2hunter2", false, false, false, Ieee80211Association::State::Connected, false);
  run_wpa2("hunter2hunter2", "hunter2hunter2", false, false, true, Ieee80211Association::State::Connected, false);       // QoS data frames
  run_wpa2("hunter2hunter2", "wrong passphrase", false, false, false, Ieee80211Association::State::Handshaking, true);    // message 2 does not verify: silence
  run_wpa2("hunter2hunter2", "hunter2hunter2", false, true, false, Ieee80211Association::State::Handshaking, true);       // message 2 lost
  run_wpa2("hunter2hunter2", "hunter2hunter2", true, false, false, Ieee80211Association::State::Failed, false);           // deauthenticated during the handshake
  // a passphrase that is not one
  { FakeLink link; Ieee80211Association assoc(link);
    CHECK(assoc.start(network(WifiSecurity::Wpa2), view(ap_rsn), StringView("short"), g_snonce, kUs) == WifiResult::BadPassphrase && link.sent.empty()); }
  // an RSN element we cannot speak, and networks we cannot join
  { FakeLink link; Ieee80211Association assoc(link);
    CHECK(assoc.start(network(WifiSecurity::Wpa2), view(tkip_group), StringView("hunter2hunter2"), g_snonce, kUs) == WifiResult::Unsupported && assoc.failure()[0]);
    CHECK(assoc.start(network(WifiSecurity::Enterprise), ConstByteView(), StringView(), g_snonce, kUs) == WifiResult::Unsupported);
    CHECK(assoc.start(network(WifiSecurity::Wpa3), ConstByteView(), StringView(), g_snonce, kUs) == WifiResult::Unsupported);
    WifiNetwork nameless = network(WifiSecurity::Open); nameless.ssid[0] = 0;
    CHECK(assoc.start(nameless, ConstByteView(), StringView(), g_snonce, kUs) == WifiResult::InvalidArgument); }
  // refusals
  { FakeLink link; Ieee80211Association assoc(link);
    link.on_send = [&](const Bytes& f) { if (f[0] == 0xB0) assoc.on_frame(view(auth_response(17))); };
    assoc.start(network(WifiSecurity::Open), ConstByteView(), StringView(), g_snonce, kUs);
    CHECK(assoc.state() == Ieee80211Association::State::Failed && !link.opened); }
  { FakeLink link; Ieee80211Association assoc(link);
    link.on_send = [&](const Bytes& f) { if (f[0] == 0xB0) assoc.on_frame(view(auth_response(0))); else if (f[0] == 0x00) assoc.on_frame(view(assoc_response(17, 0))); };
    assoc.start(network(WifiSecurity::Open), ConstByteView(), StringView(), g_snonce, kUs);
    CHECK(assoc.state() == Ieee80211Association::State::Failed && !link.opened); }
  // silence: three tries, a second apart, then failure
  { FakeLink link; Ieee80211Association assoc(link);
    assoc.start(network(WifiSecurity::Open), ConstByteView(), StringView(), g_snonce, kUs);
    CHECK(link.sent.size() == 1);
    link.now += 500; assoc.poll(); CHECK(link.sent.size() == 1);
    link.now += 600; assoc.poll(); CHECK(link.sent.size() == 2);
    link.now += 1000; assoc.poll(); CHECK(link.sent.size() == 3 && assoc.state() == Ieee80211Association::State::Authenticating);
    link.now += 1000; assoc.poll(); CHECK(assoc.state() == Ieee80211Association::State::Failed && link.sent.size() == 3); }
  // frames from a stranger, an answer in the wrong state, a deauthentication after connecting
  { FakeLink link; Ieee80211Association assoc(link);
    assoc.start(network(WifiSecurity::Open), ConstByteView(), StringView(), g_snonce, kUs);
    assoc.on_frame(view(auth_response(0, stranger)));
    CHECK(assoc.state() == Ieee80211Association::State::Authenticating);
    assoc.on_frame(view(assoc_response(0, 1)));
    CHECK(assoc.state() == Ieee80211Association::State::Authenticating);
    assoc.on_frame(view(auth_response(0)));
    CHECK(assoc.state() == Ieee80211Association::State::Associating);
    assoc.on_frame(view(assoc_response(0, 9)));
    CHECK(assoc.state() == Ieee80211Association::State::Connected);
    assoc.on_frame(view(deauth(stranger)));
    CHECK(assoc.state() == Ieee80211Association::State::Connected);
    assoc.on_frame(view(deauth()));
    CHECK(assoc.state() == Ieee80211Association::State::Failed); }
  // a radio that refuses the keys
  { FakeLink link; link.refuse_keys = true; Ieee80211Association assoc(link); Authenticator ap;
    derive_pmk(StringView("hunter2hunter2"), view(ssid_bytes), ap.pmk);
    for (int i = 0; i < 32; ++i) ap.anonce[i] = static_cast<uint8_t>(i);
    ap.rsn_ie = rsn; for (int i = 0; i < 16; ++i) ap.gtk[i] = 1;
    link.on_send = [&](const Bytes& f) {
      if (f[0] == 0xB0) assoc.on_frame(view(auth_response(0)));
      else if (f[0] == 0x00) { assoc.on_frame(view(assoc_response(0, 3))); assoc.on_frame(view(eapol_to_us(ap.message1()))); }
      else if (f[0] == 0x08 && f[1] == 0x01) { EapolKey k; ConstByteView p(f.data() + 32, f.size() - 32); if (parse_eapol_key(p, &k) && !(k.info & kInfoSecure) && ap.check_message2(Bytes(p.data(), p.data() + p.size()))) assoc.on_frame(view(eapol_to_us(ap.message3()))); }
    };
    assoc.start(network(WifiSecurity::Wpa2), view(ap_rsn), StringView("hunter2hunter2"), g_snonce, kUs);
    CHECK(assoc.state() == Ieee80211Association::State::Failed && !link.opened); }
  // random frames never crash
  { uint32_t seed = 11;
    for (int round = 0; round < 3000; ++round) {
      FakeLink link; Ieee80211Association assoc(link);
      assoc.start(network(round % 2 ? WifiSecurity::Open : WifiSecurity::Wpa2), view(ap_rsn), StringView("hunter2hunter2"), g_snonce, kUs);
      Bytes junk(30 + round % 120);
      for (auto& b : junk) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
      if (round % 3 == 0) { memcpy(junk.data() + 4, kUs, 6); memcpy(junk.data() + 10, kBssid, 6); memcpy(junk.data() + 16, kBssid, 6); }
      assoc.on_frame(view(junk));
    } }
  printf(g_failed ? "Ieee80211AssociationTest: %d FAILED\n" : "Ieee80211AssociationTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
