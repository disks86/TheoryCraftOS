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

// Host tests for WebRTC's building blocks: SDP, STUN, RTP/RTCP, SRTP, ICE (over an in-memory network), and later DTLS, SCTP and the peer connection.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Rtc/Dtls.hpp"
#include "FakeRtcNetwork.hpp"
#include "Rtc/Ice.hpp"
#include "Rtc/G711.hpp"
#include "Rtc/PeerConnection.hpp"
#include "Rtc/Rtp.hpp"
#include "Rtc/Sctp.hpp"
#include "Rtc/Sdp.hpp"
#include "Rtc/Srtp.hpp"
#include "Rtc/Stun.hpp"
#include "Crypto/X509.hpp"

using namespace rtc;
using crypto::Certificate;
using crypto::CertStatus;
using crypto::KeyType;
using crypto::parse_certificate;
using crypto::verify_signature;
using network::Ipv4Address;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::string hex_of(ConstByteView v) {
  static const char k[] = "0123456789abcdef";
  std::string s;
  for (uint8_t b : v) { s.push_back(k[b >> 4]); s.push_back(k[b & 15]); }
  return s;
}
static void unhex(const char* text, uint8_t* out) {
  for (size_t i = 0; text[i * 2]; ++i) {
    auto d = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    out[i] = static_cast<uint8_t>(d(text[i * 2]) * 16 + d(text[i * 2 + 1]));
  }
}
static ConstByteView text_view(const std::string& s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// ---------------------------------------------------------------------------------------------------------------------------------------- SDP
static const char* const kOffer =
    "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\na=fingerprint:sha-256 AB:CD:EF:01\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=ice-ufrag:abcd\r\na=ice-pwd:0123456789012345678901\r\na=rtpmap:111 opus/48000/2\r\na=sendrecv\r\n"
    "a=candidate:1 1 udp 2113937151 192.168.1.5 50000 typ host\r\na=candidate:2 1 udp 1677729535 203.0.113.9 50001 typ srflx raddr 192.168.1.5 rport 50000\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=sctp-port:5000\r\n";

static void test_sdp() {
  TestAllocator a;
  {
    Sdp sdp(a);
    CHECK(sdp.parse(kOffer) && sdp.media.count() == 2 && sdp.session.count() == 5);
    CHECK(sdp.media[0]->kind.view().equals("audio") && sdp.media[0]->formats.count() == 2 && sdp.media[1]->formats[0].view().equals("webrtc-datachannel"));
    StringView v;
    CHECK(sdp.media[0]->attribute("ice-ufrag", &v) && v.equals("abcd") && sdp.media[0]->has_flag("sendrecv") && sdp.media[0]->count("candidate") == 2);
    CHECK(sdp.media[1]->attribute("sctp-port", &v) && v.equals("5000") && !sdp.media[1]->attribute("ice-ufrag", &v));
    CHECK(sdp_inherited(sdp, *sdp.media[1], "fingerprint", &v) && v.starts_with("sha-256"));
    String out(a);
    CHECK(sdp.write(&out) && out.view().equals(kOffer));
    IceCandidateInfo c(a);
    StringView cand;
    CHECK(sdp.media[0]->nth("candidate", 1, &cand) && parse_candidate(cand, &c) && c.type.view().equals("srflx") && c.port == 50001 && c.related_port == 50000 && c.related_address.view().equals("192.168.1.5"));
    String text(a);
    write_candidate(c, &text);
    CHECK(text.view().equals("candidate:2 1 udp 1677729535 203.0.113.9 50001 typ srflx raddr 192.168.1.5 rport 50000"));
    String alg(a), hex(a);
    CHECK(parse_fingerprint("sha-256 AB:CD:EF:01", &alg, &hex) && alg.view().equals("sha-256") && !parse_fingerprint("sha-256 zz", &alg, &hex));
  }
  // not session descriptions
  const char* bad[] = {"", "hello", "v=1\r\n", "v=0\r\nx\r\n", "v=0\r\nm=audio\r\n", "v=0\r\nm=audio x UDP 0\r\n", "v=0\r\nm=audio 70000 UDP 0\r\n", "v=0\r\n=oops\r\n"};
  for (const char* b : bad) {
    Sdp sdp(a);
    CHECK(!sdp.parse(b));
  }
  // truncation and bit flips never crash and never leak
  const std::string offer = kOffer;
  for (size_t cut = 0; cut < offer.size(); ++cut) {
    Sdp sdp(a);
    sdp.parse(StringView(offer.data(), cut));
  }
  for (size_t at = 0; at < offer.size(); ++at)
    for (int bit = 0; bit < 8; bit += 3) {
      std::string f = offer;
      f[at] = static_cast<char>(f[at] ^ (1 << bit));
      Sdp sdp(a);
      sdp.parse(StringView(f.data(), f.size()));
    }
  IceCandidateInfo c(a);
  CHECK(!parse_candidate("1 1 udp 5 1.2.3.4 70000 typ host", &c) && !parse_candidate("1 1 udp 5 1.2.3.4 80 host", &c) && !parse_candidate("", &c));
  CHECK(a.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- STUN
static void test_stun() {
  TestAllocator a;
  {
  uint8_t tx[12];
  for (int i = 0; i < 12; ++i) tx[i] = static_cast<uint8_t>(i + 1);
  const std::string key = "VOkJxbRl1RmTxUk/WvJxBt";
  ByteBuffer msg(a);
  CHECK(stun_begin(&msg, kStunBindingRequest, tx) && stun_text_attribute(&msg, kStunUsername, "evtj:h6vY") && stun_text_attribute(&msg, kStunSoftware, "x"));
  uint8_t prio[4] = {0x6e, 0x00, 0x01, 0xff}, tie[8] = {0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36};
  CHECK(stun_attribute(&msg, kStunPriority, ConstByteView(prio, 4)) && stun_attribute(&msg, kStunIceControlled, ConstByteView(tie, 8)) && stun_attribute(&msg, kStunUseCandidate, ConstByteView()));
  CHECK(stun_finish(&msg, text_view(key), true));
  CHECK(stun_looks_like(msg.view()) && demux(msg.view()) == Demux::Stun && msg.size() % 4 == 0);
  StunMessage m(a);
  CHECK(stun_parse(msg.view(), &m) && m.type == kStunBindingRequest && m.username.view().equals("evtj:h6vY") && m.software.view().equals("x") && m.priority == 0x6e0001ff && m.has_priority);
  CHECK(m.has_controlled && !m.has_controlling && m.tie_breaker == 0x932ff9b151263b36ull && m.use_candidate && m.fingerprint_ok && m.has_integrity);
  CHECK(stun_check_integrity(msg.view(), m, text_view(key)) && !stun_check_integrity(msg.view(), m, text_view("wrong")));
  // an answer with the mapped address
  ByteBuffer ans(a);
  CHECK(stun_begin(&ans, kStunBindingSuccess, tx) && stun_xor_address(&ans, kStunXorMapped, Ipv4Address::make(192, 0, 2, 1), 32853, tx) && stun_finish(&ans, text_view(key), true));
  CHECK(stun_parse(ans.view(), &m) && m.has_mapped && m.mapped == Ipv4Address::make(192, 0, 2, 1) && m.mapped_port == 32853 && m.fingerprint_ok && stun_check_integrity(ans.view(), m, text_view(key)));
  ByteBuffer err(a);
  CHECK(stun_begin(&err, kStunBindingError, tx) && stun_error_attribute(&err, 487, "Role Conflict") && stun_finish(&err, ConstByteView(), true));
  CHECK(stun_parse(err.view(), &m) && m.has_error && m.error_code == 487 && !m.has_integrity && m.fingerprint_ok);
  // changed or cut, it is refused or at least checked: nothing crashes, and integrity never passes for a changed message
  for (uint64_t cut = 0; cut < msg.size(); ++cut) {
    StunMessage t(a);
    CHECK(!stun_parse(msg.view().first(cut), &t));
  }
  for (uint64_t at = 0; at < msg.size(); ++at)
    for (int bit = 0; bit < 8; ++bit) {
      ByteBuffer f(a);
      f.append(msg.view());
      f.writable_view()[at] ^= static_cast<uint8_t>(1 << bit);
      StunMessage t(a);
      if (stun_parse(f.view(), &t)) CHECK(!(t.fingerprint_ok && t.has_integrity && stun_check_integrity(f.view(), t, text_view(key))));
    }
  CHECK(demux(ConstByteView()) == Demux::Unknown);
  }
  CHECK(a.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- RTP / RTCP
static void test_rtp() {
  TestAllocator a;
  {
  ByteBuffer ext(a), p(a);
  CHECK(rtp_add_extension_element(&ext, 1, text_view("ab")) && rtp_add_extension_element(&ext, 3, text_view("0123456789ABCDEF")) && !rtp_add_extension_element(&ext, 15, text_view("x")));
  RtpHeader h;
  h.payload_type = 111;
  h.sequence = 65535;
  h.timestamp = 0x01020304;
  h.ssrc = 0xA1B2C3D4;
  h.marker = true;
  CHECK(rtp_write_header(&p, h, ext.view()) && p.append(text_view("payload")));
  RtpHeader r;
  CHECK(rtp_parse(p.view(), &r) && r.marker && r.payload_type == 111 && r.sequence == 65535 && r.timestamp == 0x01020304 && r.ssrc == 0xA1B2C3D4 && r.has_extension && r.payload_size == 7);
  CHECK(r.header_size == p.size() - 7);
  ConstByteView v;
  CHECK(rtp_extension(p.view(), r, 1, &v) && v.size() == 2 && v[0] == 'a' && rtp_extension(p.view(), r, 3, &v) && v.size() == 16 && !rtp_extension(p.view(), r, 2, &v));
  CHECK(demux(p.view()) == Demux::Rtp);
  for (uint64_t cut = 0; cut < p.size(); ++cut) {
    RtpHeader t;
    if (cut < r.header_size) CHECK(!rtp_parse(p.view().first(cut), &t));
  }
  // RTCP
  ByteBuffer c(a);
  RtcpReportBlock b;
  b.ssrc = 7; b.fraction_lost = 12; b.cumulative_lost = -3; b.highest_sequence = 0x10002; b.jitter = 9; b.last_sr = 5; b.delay_since_sr = 6;
  CHECK(rtcp_write_sender_report(&c, 1234, 1, 2, 3, 4, 5, &b, 1) && rtcp_write_sdes(&c, 1234, "cname@host") && rtcp_write_receiver_report(&c, 99, &b, 1));
  const uint16_t lost[] = {100, 101, 105, 116, 117, 400};
  CHECK(rtcp_write_nack(&c, 99, 1234, lost, 6) && rtcp_write_pli(&c, 99, 1234) && rtcp_write_fir(&c, 99, 1234, 3) && rtcp_write_bye(&c, 1234));
  const uint32_t ssrcs[] = {1234};
  CHECK(rtcp_write_remb(&c, 99, 2500000, ssrcs, 1));
  CHECK(demux(c.view()) == Demux::Rtcp);
  int packets = 0, nacks = 0, plis = 0, rembs = 0, byes = 0, srs = 0;
  uint16_t got[16];
  uint32_t got_n = 0;
  uint64_t bitrate = 0;
  CHECK(rtcp_walk(c.view(), [&](const RtcpPacket& pk, ConstByteView whole) {
    ++packets;
    if (pk.type == kRtcpSr) { ++srs; RtcpReportBlock x; CHECK(pk.ntp_hi == 1 && pk.packet_count == 4 && pk.octet_count == 5 && rtcp_report_block(whole, pk, 0, &x) && x.cumulative_lost == -3 && x.highest_sequence == 0x10002 && x.fraction_lost == 12); }
    if (pk.type == kRtcpRtpFb) { ++nacks; got_n = rtcp_nack_list(whole, pk, got, 16); }
    if (pk.type == kRtcpPsFb && pk.count_or_format == 1) ++plis;
    if (pk.type == kRtcpPsFb && pk.count_or_format == 15) { ++rembs; CHECK(rtcp_remb_bitrate(whole, pk, &bitrate)); }
    if (pk.type == kRtcpBye) { ++byes; uint32_t s; CHECK(rtcp_bye_ssrc(whole, pk, 0, &s) && s == 1234); }
  }));
  CHECK(packets == 8 && srs == 1 && nacks == 1 && plis == 1 && rembs == 1 && byes == 1);
  CHECK(got_n == 6 && got[0] == 100 && got[1] == 101 && got[2] == 105 && got[3] == 116 && got[4] == 117 && got[5] == 400);
  CHECK(bitrate > 2400000 && bitrate < 2600000);
  for (uint64_t cut = 0; cut < c.size(); ++cut) rtcp_walk(c.view().first(cut), [](const RtcpPacket&, ConstByteView) {});
  }
  CHECK(a.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- SRTP
static void test_srtp() {
  TestAllocator a;
  {
  uint8_t key[16], salt[14], out[20];
  unhex("E1F97A0D3E018BE0D64FA32C06DE4139", key);
  unhex("0EC675AD498AFEEBB6960B3AABE6", salt);
  // RFC 3711 appendix B.3
  CHECK(Srtp::derive_key(ConstByteView(key, 16), ConstByteView(salt, 14), 0, ByteView(out, 16)) && hex_of(ConstByteView(out, 16)) == "c61e7a93744f39ee10734afe3ff7a087");
  CHECK(Srtp::derive_key(ConstByteView(key, 16), ConstByteView(salt, 14), 2, ByteView(out, 14)) && hex_of(ConstByteView(out, 14)) == "30cbbc08863d8c85d49db34a9ae1");
  CHECK(Srtp::derive_key(ConstByteView(key, 16), ConstByteView(salt, 14), 1, ByteView(out, 20)) && hex_of(ConstByteView(out, 20)) == "cebe321f6ff7716b6fd4ab49af256a156d38baa4");

  auto packet = [&](ByteBuffer* b, uint16_t seq, uint32_t ssrc, const char* text) {
    RtpHeader h;
    h.payload_type = 96; h.sequence = seq; h.timestamp = seq * 960u; h.ssrc = ssrc;
    b->clear();
    return rtp_write_header(b, h) && b->append(text_view(text));
  };
  const SrtpProfile profiles[] = {SrtpProfile::Aes128CmSha1_80, SrtpProfile::Aes128CmSha1_32, SrtpProfile::AeadAes128Gcm, SrtpProfile::AeadAes256Gcm};
  for (const SrtpProfile profile : profiles) {
    uint8_t mk[32], ms[14];
    for (int i = 0; i < 32; ++i) mk[i] = static_cast<uint8_t>(i * 7 + 1);
    for (int i = 0; i < 14; ++i) ms[i] = static_cast<uint8_t>(i * 5 + 3);
    Srtp tx(a), rx(a);
    CHECK(tx.init(profile, ConstByteView(mk, srtp_key_size(profile)), ConstByteView(ms, srtp_salt_size(profile))) && rx.init(profile, ConstByteView(mk, srtp_key_size(profile)), ConstByteView(ms, srtp_salt_size(profile))));
    CHECK(!Srtp(a).init(profile, ConstByteView(mk, 3), ConstByteView(ms, 3)));
    // a stream that crosses the wrap of the sequence number: the roll-over counter follows on both sides
    ByteBuffer p(a), copy(a);
    uint16_t seq = 65530;
    for (int i = 0; i < 12; ++i, ++seq) {
      CHECK(packet(&p, seq, 0x11223344, "hello srtp payload"));
      copy.clear();
      copy.append(p.view());
      CHECK(tx.protect_rtp(&p) && p.size() > copy.size());
      CHECK(memcmp(p.view().data() + 12, copy.view().data() + 12, copy.size() - 12) != 0);   // (encrypted)
      ByteBuffer wire(a);
      wire.append(p.view());
      CHECK(rx.unprotect_rtp(&p) && p.size() == copy.size() && memcmp(p.view().data(), copy.view().data(), copy.size()) == 0);
      CHECK(!rx.unprotect_rtp(&wire));   // a replay
    }
    // tamper
    packet(&p, seq, 0x11223344, "tamper me");
    CHECK(tx.protect_rtp(&p));
    for (uint64_t at = 0; at < p.size(); ++at) {
      ByteBuffer f(a);
      f.append(p.view());
      f.writable_view()[at] ^= 1;
      CHECK(!rx.unprotect_rtp(&f));
    }
    for (uint64_t cut = 0; cut < p.size(); ++cut) {
      ByteBuffer f(a);
      f.append(p.view().first(cut));
      CHECK(!rx.unprotect_rtp(&f));
    }
    CHECK(rx.unprotect_rtp(&p));
    // out of order within the window is fine once
    ByteBuffer late(a), later(a);
    packet(&late, seq + 1, 0x11223344, "one"); packet(&later, seq + 2, 0x11223344, "two");
    CHECK(tx.protect_rtp(&late) && tx.protect_rtp(&later) && rx.unprotect_rtp(&later) && rx.unprotect_rtp(&late));
    // another SSRC has its own counters
    packet(&p, 5, 0x99999999, "other");
    CHECK(tx.protect_rtp(&p) && rx.unprotect_rtp(&p));
    // RTCP
    ByteBuffer c(a), cc(a);
    rtcp_write_sender_report(&c, 0x11223344, 1, 2, 3, 4, 5, nullptr, 0);
    cc.append(c.view());
    CHECK(tx.protect_rtcp(&c) && c.size() > cc.size() && memcmp(c.view().data() + 8, cc.view().data() + 8, 8) != 0);
    ByteBuffer wire(a);
    wire.append(c.view());
    CHECK(rx.unprotect_rtcp(&c) && c.size() == cc.size() && memcmp(c.view().data(), cc.view().data(), cc.size()) == 0 && !rx.unprotect_rtcp(&wire));
    rtcp_write_pli(&c, 0x11223344, 0x55);
    CHECK(tx.protect_rtcp(&c));
    for (uint64_t at = 0; at < c.size(); ++at) {
      ByteBuffer f(a);
      f.append(c.view());
      f.writable_view()[at] ^= 0x40;
      CHECK(!rx.unprotect_rtcp(&f));
    }
    CHECK(rx.unprotect_rtcp(&c));
  }
  }
  CHECK(a.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- ICE
// Runs two agents against each other until `done` or `ms` pass; local candidates are handed over as they are found (trickle).
template <class Done>
static void run_pair(FakeWire& wire, IceAgent& a, IceAgent& b, uint32_t ms, Done done, bool skip_host_of_a = false) {
  for (uint32_t t = 0; t < ms && !done(); t += 5) {
    wire.now += 5;
    a.poll();
    b.poll();
    IceCandidate c;
    while (a.take_local_candidate(&c))
      if (!(skip_host_of_a && c.type == CandidateType::Host)) b.add_remote_candidate(c);
    while (b.take_local_candidate(&c)) a.add_remote_candidate(c);
  }
}

static void test_ice() {
  TestAllocator alloc;
  {   // two peers on one network: host candidates, checks, nomination, data both ways
    FakeWire wire;
    wire.alloc = &alloc;
    FakeNode na(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1), nb(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    IceAgent a(alloc, na), b(alloc, nb);
    a.set_local_credentials("aaaa", "pppppppppppppppppppppp");
    b.set_local_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    a.set_remote_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    b.set_remote_credentials("aaaa", "pppppppppppppppppppppp");
    a.set_controlling(true);
    b.set_controlling(false);
    CHECK(a.start_gathering(nullptr, 0) && b.start_gathering(nullptr, 0) && a.gathering() == IceGathering::Complete);
    a.start_checks();
    b.start_checks();
    run_pair(wire, a, b, 3000, [&] { return a.state() == IceState::Completed && b.state() == IceState::Completed; });
    CHECK(a.state() == IceState::Completed && b.state() == IceState::Completed);
    IceCandidate l, r;
    CHECK(a.selected(&l, &r) && r.address == nb.addr && r.port == b.local_port());
    uint8_t data[4] = {0x80, 1, 2, 3}, in[100];
    CHECK(a.send(ConstByteView(data, 4)));
    run_pair(wire, a, b, 50, [] { return false; });
    CHECK(b.receive(ByteView(in, 100)) == 4 && in[1] == 1 && b.receive(ByteView(in, 100)) == 0);
    CHECK(b.send(ConstByteView(data, 4)));
    run_pair(wire, a, b, 50, [] { return false; });
    CHECK(a.receive(ByteView(in, 100)) == 4);
    IceState st;
    int changes = 0;
    while (a.state_changed(&st)) ++changes;
    CHECK(changes >= 1);
    // consent: the path stays alive while it is quiet, and goes down when the other side is gone
    run_pair(wire, a, b, 20000, [] { return false; });
    CHECK(a.state() == IceState::Completed && b.state() == IceState::Completed);
    wire.drop_all = true;
    run_pair(wire, a, b, 8000, [&] { return a.state() == IceState::Disconnected; });
    CHECK(a.state() == IceState::Disconnected);
    wire.drop_all = false;
    run_pair(wire, a, b, 6000, [&] { return a.state() == IceState::Completed; });
    CHECK(a.state() == IceState::Completed);
    wire.drop_all = true;
    run_pair(wire, a, b, 40000, [&] { return a.state() == IceState::Failed; });
    CHECK(a.state() == IceState::Failed);
    a.close();
    CHECK(a.state() == IceState::Closed && !a.send(ConstByteView(data, 4)));
  }
  {   // behind a NAT: the STUN server tells the address the world sees, and that candidate alone is enough
    FakeWire wire;
    wire.alloc = &alloc;
    FakeNode na(wire, alloc, Ipv4Address::make(10, 0, 0, 5), 3), nb(wire, alloc, Ipv4Address::make(203, 0, 113, 7), 4);
    na.nat = Ipv4Address::make(203, 0, 113, 99);
    IceAgent a(alloc, na), b(alloc, nb);
    a.set_local_credentials("aaaa", "pppppppppppppppppppppp");
    b.set_local_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    a.set_remote_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    b.set_remote_credentials("aaaa", "pppppppppppppppppppppp");
    a.set_controlling(false);
    b.set_controlling(true);
    const StringView servers[1] = {"stun.test:3478"};
    CHECK(a.start_gathering(servers, 1) && b.start_gathering(nullptr, 0));
    a.start_checks();
    b.start_checks();
    bool saw_srflx = false;
    for (uint32_t t = 0; t < 4000 && !(a.state() == IceState::Completed && b.state() == IceState::Completed); t += 5) {
      wire.now += 5;
      a.poll();
      b.poll();
      IceCandidate c;
      while (a.take_local_candidate(&c)) {
        if (c.type == CandidateType::ServerReflexive) {
          saw_srflx = c.address == na.nat && c.related_address == na.addr;
          b.add_remote_candidate(c);   // (only this one: the private address is of no use to B)
        }
      }
      while (b.take_local_candidate(&c)) a.add_remote_candidate(c);
    }
    CHECK(saw_srflx && a.gathering() == IceGathering::Complete);
    CHECK(a.state() == IceState::Completed && b.state() == IceState::Completed);
    IceCandidate l, r;
    CHECK(b.selected(&l, &r) && r.address == na.nat);
  }
  {   // both sides think they are controlling: the tie-breaker settles it (487 and a switch)
    FakeWire wire;
    wire.alloc = &alloc;
    FakeNode na(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1), nb(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    IceAgent a(alloc, na), b(alloc, nb);
    a.set_local_credentials("aaaa", "pppppppppppppppppppppp");
    b.set_local_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    a.set_remote_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    b.set_remote_credentials("aaaa", "pppppppppppppppppppppp");
    CHECK(a.start_gathering(nullptr, 0) && b.start_gathering(nullptr, 0));
    a.start_checks();
    b.start_checks();
    run_pair(wire, a, b, 5000, [&] { return a.state() == IceState::Completed && b.state() == IceState::Completed; });
    CHECK(a.state() == IceState::Completed && b.state() == IceState::Completed && a.controlling() != b.controlling());
  }
  {   // wrong credentials: the requests are answered 401 and the pair fails
    FakeWire wire;
    wire.alloc = &alloc;
    FakeNode na(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1), nb(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    IceAgent a(alloc, na), b(alloc, nb);
    a.set_local_credentials("aaaa", "pppppppppppppppppppppp");
    b.set_local_credentials("bbbb", "qqqqqqqqqqqqqqqqqqqqqq");
    a.set_remote_credentials("bbbb", "WRONGWRONGWRONGWRONGWR");
    b.set_remote_credentials("aaaa", "pppppppppppppppppppppp");
    a.set_controlling(true);
    b.set_controlling(false);
    a.start_gathering(nullptr, 0);
    b.start_gathering(nullptr, 0);
    a.start_checks();
    b.start_checks();
    a.remote_candidates_done();
    run_pair(wire, a, b, 10000, [&] { return a.state() == IceState::Failed; });
    CHECK(a.state() != IceState::Completed && a.state() != IceState::Connected && b.state() != IceState::Completed);
  }
  CHECK(alloc.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- DTLS
struct CountRandom final : crypto::IRandom {
  uint32_t n;
  explicit CountRandom(uint32_t seed) : n(seed) {}
  bool fill(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) { n = n * 1664525u + 1013904223u; out[i] = static_cast<uint8_t>(n >> 24); }
    return true;
  }
};

// Passes datagrams between two transports until both are quiet; `lose(n)` says whether the n-th datagram is lost, `tamper` may change one.
template <class Lose, class Tamper>
static void pump_dtls(DtlsTransport& a, DtlsTransport& b, uint64_t* now, uint32_t rounds, Lose lose, Tamper tamper) {
  static uint32_t counter = 0;
  TestAllocator t;
  ByteBuffer d(t);
  for (uint32_t r = 0; r < rounds; ++r) {
    *now += 100;
    a.tick(*now);
    b.tick(*now);
    bool moved = false;
    while (a.next_datagram(&d)) { moved = true; if (!lose(counter++)) { tamper(&d); b.receive(d.view(), *now); } }
    while (b.next_datagram(&d)) { moved = true; if (!lose(counter++)) { tamper(&d); a.receive(d.view(), *now); } }
    if (!moved && a.state() == DtlsTransport::State::Connected && b.state() == DtlsTransport::State::Connected) break;
  }
}

static void test_dtls() {
  TestAllocator alloc;
  {
    CountRandom ra(1), rb(2), rc(3), rd(4);
    DtlsIdentity ida(alloc), idb(alloc);
    CHECK(ida.generate(ra, "WebRTC") && idb.generate(rb, "WebRTC") && ida.ready());
    String fa(alloc), fb(alloc);
    ida.fingerprint(&fa);
    idb.fingerprint(&fb);
    CHECK(fa.size() == 95 && !fa.view().equals(fb.view()));
    uint8_t fp[32];
    CHECK(fingerprint_bytes(fa.view(), fp) && !fingerprint_bytes("AB:CD", fp));
    // the certificate is a certificate: it parses, carries a P-256 key and the fingerprint is of its bytes
    Certificate cert;
    CHECK(parse_certificate(ida.certificate(), &cert) == CertStatus::Ok && cert.key_type == KeyType::EcP256 && cert.ec_point.size() == 65 && cert.self_issued());
    CHECK(verify_signature(cert, cert) == CertStatus::Ok);
    {   // a handshake between a client and a server, and application data both ways
      DtlsTransport client(alloc, ida, rc), server(alloc, idb, rd);
      client.set_role(DtlsTransport::Role::Client);
      server.set_role(DtlsTransport::Role::Server);
      CHECK(client.set_expected_fingerprint("sha-256", fb.view()) && server.set_expected_fingerprint("SHA-256", fa.view()));
      uint64_t now = 1000;
      server.start(now);
      client.start(now);
      pump_dtls(client, server, &now, 100, [](uint32_t) { return false; }, [](ByteBuffer*) {});
      CHECK(client.state() == DtlsTransport::State::Connected && server.state() == DtlsTransport::State::Connected);
      SrtpProfile pc, ps;
      uint8_t ck1[32], sk1[32], cs1[14], ss1[14], ck2[32], sk2[32], cs2[14], ss2[14];
      CHECK(client.srtp_keys(&pc, ck1, sk1, cs1, ss1) && server.srtp_keys(&ps, ck2, sk2, cs2, ss2) && pc == ps && pc == SrtpProfile::AeadAes128Gcm);
      CHECK(memcmp(ck1, ck2, 16) == 0 && memcmp(sk1, sk2, 16) == 0 && memcmp(cs1, cs2, 12) == 0 && memcmp(ss1, ss2, 12) == 0 && memcmp(ck1, sk1, 16) != 0);
      CHECK(client.send_application(text_view("hello from the client")) && server.send_application(text_view("and from the server")));
      pump_dtls(client, server, &now, 5, [](uint32_t) { return false; }, [](ByteBuffer*) {});
      ByteBuffer got(alloc);
      CHECK(server.receive_application(&got) && got.size() == 21 && memcmp(got.view().data(), "hello from the client", 21) == 0 && !server.receive_application(&got));
      CHECK(client.receive_application(&got) && got.size() == 19);
      // the keys work for SRTP
      Srtp tx(alloc), rx(alloc);
      CHECK(tx.init(pc, ConstByteView(ck1, 16), ConstByteView(cs1, 12)) && rx.init(ps, ConstByteView(ck2, 16), ConstByteView(cs2, 12)));
      ByteBuffer p(alloc);
      RtpHeader h;
      h.payload_type = 96; h.sequence = 1; h.ssrc = 5;
      rtp_write_header(&p, h);
      p.append(text_view("media"));
      CHECK(tx.protect_rtp(&p) && rx.unprotect_rtp(&p) && p.size() == 17);
      // a replayed datagram is ignored, a closed connection says so
      client.close();
      pump_dtls(client, server, &now, 5, [](uint32_t) { return false; }, [](ByteBuffer*) {});
      CHECK(client.state() == DtlsTransport::State::Closed && server.state() == DtlsTransport::State::Closed && !client.send_application(text_view("x")));
    }
    {   // lost datagrams: the flights are sent again
      CountRandom r1(11), r2(12);
      DtlsTransport client(alloc, ida, r1), server(alloc, idb, r2);
      server.set_role(DtlsTransport::Role::Server);
      client.set_expected_fingerprint("sha-256", fb.view());
      server.set_expected_fingerprint("sha-256", fa.view());
      uint64_t now = 1000;
      server.start(now);
      client.start(now);
      pump_dtls(client, server, &now, 400, [](uint32_t n) { return ((n * 2654435761u) >> 13) % 3 == 0; }, [](ByteBuffer*) {});
      CHECK(client.state() == DtlsTransport::State::Connected && server.state() == DtlsTransport::State::Connected);
    }
    {   // the other certificate is not the one in the fingerprint: both sides refuse
      CountRandom r1(21), r2(22);
      DtlsTransport client(alloc, ida, r1), server(alloc, idb, r2);
      server.set_role(DtlsTransport::Role::Server);
      client.set_expected_fingerprint("sha-256", fa.view());   // (wrong: that is the client's own)
      server.set_expected_fingerprint("sha-256", fa.view());
      uint64_t now = 1000;
      server.start(now);
      client.start(now);
      pump_dtls(client, server, &now, 100, [](uint32_t) { return false; }, [](ByteBuffer*) {});
      CHECK(client.state() == DtlsTransport::State::Failed && server.state() != DtlsTransport::State::Connected && !client.send_application(text_view("x")));
    }
    {   // nobody answers: the handshake gives up
      CountRandom r1(31);
      DtlsTransport client(alloc, ida, r1);
      client.set_expected_fingerprint("sha-256", fb.view());
      uint64_t now = 1000;
      client.start(now);
      ByteBuffer d(alloc);
      for (int i = 0; i < 400; ++i) { now += 500; client.tick(now); while (client.next_datagram(&d)) {} }
      CHECK(client.state() == DtlsTransport::State::Failed);
    }
    // damage: every datagram of a handshake in turn is cut or has a bit flipped; nothing crashes, and a connection that comes up has working keys
    for (uint32_t victim = 0; victim < 12; ++victim)
      for (int mode = 0; mode < 3; ++mode) {
        CountRandom r1(41), r2(42);
        DtlsTransport client(alloc, ida, r1), server(alloc, idb, r2);
        server.set_role(DtlsTransport::Role::Server);
        client.set_expected_fingerprint("sha-256", fb.view());
        server.set_expected_fingerprint("sha-256", fa.view());
        uint64_t now = 1000;
        server.start(now);
        client.start(now);
        uint32_t seen = 0;
        pump_dtls(client, server, &now, 120, [](uint32_t) { return false; }, [&](ByteBuffer* d) {
          if (seen++ != victim || d->size() < 14) return;
          if (mode == 0) d->truncate(d->size() / 2);
          else if (mode == 1) d->writable_view()[d->size() - 3] ^= 0x10;
          else d->writable_view()[14 % d->size()] ^= 0x01;
        });
        SrtpProfile pc, ps;
        uint8_t ck1[32], sk1[32], cs1[14], ss1[14], ck2[32], sk2[32], cs2[14], ss2[14];
        if (client.state() == DtlsTransport::State::Connected && server.state() == DtlsTransport::State::Connected) {
          CHECK(client.srtp_keys(&pc, ck1, sk1, cs1, ss1) && server.srtp_keys(&ps, ck2, sk2, cs2, ss2) && memcmp(ck1, ck2, 16) == 0 && memcmp(ss1, ss2, 12) == 0);
        }
      }
  }
  CHECK(alloc.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- SCTP
template <class Lose, class Tamper>
static void pump_sctp(SctpAssociation& a, SctpAssociation& b, uint64_t* now, uint32_t rounds, Lose lose, Tamper tamper, bool stop_when_quiet = true) {
  static uint32_t counter = 0;
  TestAllocator t;
  ByteBuffer p(t);
  for (uint32_t r = 0; r < rounds; ++r) {
    *now += 10;
    a.tick(*now);
    b.tick(*now);
    bool moved = false;
    for (int k = 0; k < 64 && a.next_packet(&p); ++k) { moved = true; if (!lose(counter++)) { tamper(&p); b.receive(p.view(), *now); } }
    for (int k = 0; k < 64 && b.next_packet(&p); ++k) { moved = true; if (!lose(counter++)) { tamper(&p); a.receive(p.view(), *now); } }
    if (stop_when_quiet && !moved && r > 5 && a.buffered(1) == 0 && b.buffered(1) == 0) break;
  }
}

static void test_sctp() {
  TestAllocator alloc;
  CHECK(crc32c(text_view("123456789")) == 0xE3069283u);
  {
    CountRandom ra(7), rb(8);
    SctpAssociation a(alloc, ra), b(alloc, rb);
    uint64_t now = 1000;
    auto none = [](uint32_t) { return false; };
    auto same = [](ByteBuffer*) {};
    CHECK(!a.send(1, 51, text_view("x"), SctpOptions()));   // (not up yet)
    a.connect(now);
    pump_sctp(a, b, &now, 50, none, same);
    CHECK(a.established() && b.established());
    SctpOptions ordered, unordered;
    unordered.ordered = false;
    CHECK(a.send(1, 51, text_view("first"), ordered) && a.send(1, 51, text_view("second"), ordered) && a.send(3, 53, text_view("unordered"), unordered) && b.send(1, 51, text_view("from b"), ordered));
    CHECK(!a.send(1, 51, ConstByteView(), ordered));
    pump_sctp(a, b, &now, 100, none, same);
    UniquePointer<SctpMessage> m;
    std::vector<std::string> got;
    while (b.receive_message(&m)) got.push_back(std::to_string(m->stream) + ":" + std::to_string(m->ppid) + ":" + std::string(reinterpret_cast<const char*>(m->data.view().data()), m->data.size()));
    CHECK(got.size() == 3 && got[0] == "1:51:first" && got[1] == "1:51:second" && got[2] == "3:53:unordered");
    CHECK(a.receive_message(&m) && m->data.size() == 6 && !a.receive_message(&m));
    // a big message, fragmented, across a lossy path, both ways at once
    std::string big(150000, 'a');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 7) % 26);
    CHECK(a.send(5, 53, text_view(big), ordered) && b.send(6, 53, text_view(big), ordered) && a.send(5, 53, text_view("after the big one"), ordered));
    pump_sctp(a, b, &now, 6000, [](uint32_t n) { return ((n * 2654435761u) >> 11) % 10 == 0; }, same, false);
    bool big_ok = false, small_ok = false;
    while (b.receive_message(&m)) {
      if (m->data.size() == big.size()) big_ok = memcmp(m->data.view().data(), big.data(), big.size()) == 0;
      else small_ok = m->data.size() == 17;
    }
    CHECK(big_ok && small_ok);
    CHECK(a.receive_message(&m) && m->data.size() == big.size() && memcmp(m->data.view().data(), big.data(), big.size()) == 0);
    CHECK(a.retransmissions() > 0 && a.buffered(5) == 0);
    // a message that is given up on: the path loses everything for a while, a message with max-retransmits 0 is dropped, the one after it arrives
    SctpOptions partial;
    partial.max_retransmits = 0;
    CHECK(a.send(7, 53, text_view("lossy-1"), partial));
    pump_sctp(a, b, &now, 3, [](uint32_t) { return true; }, same, false);
    for (int k = 0; k < 300; ++k) { a.tick(now += 10); }
    CHECK(a.send(7, 53, text_view("lossy-2"), ordered));
    pump_sctp(a, b, &now, 1500, none, same);
    got.clear();
    while (b.receive_message(&m)) got.push_back(std::string(reinterpret_cast<const char*>(m->data.view().data()), m->data.size()));
    CHECK(!got.empty() && got.back() == "lossy-2");
    // a stream reset: the peer hears of it
    CHECK(a.reset_stream(1));
    pump_sctp(a, b, &now, 300, none, same, false);
    uint16_t sid = 99;
    CHECK(b.take_reset(&sid) && sid == 1 && !b.take_reset(&sid));
    CHECK(a.send(1, 51, text_view("after reset"), ordered));
    pump_sctp(a, b, &now, 100, none, same);
    CHECK(b.receive_message(&m) && m->data.size() == 11);
    // an orderly shutdown
    a.shutdown();
    pump_sctp(a, b, &now, 500, none, same, false);
    CHECK(a.state() == SctpAssociation::State::Closed && b.state() == SctpAssociation::State::Closed);
  }
  {   // nobody answers: the association gives up
    CountRandom ra(9);
    SctpAssociation a(alloc, ra);
    uint64_t now = 1000;
    a.connect(now);
    ByteBuffer p(alloc);
    for (int i = 0; i < 800; ++i) { now += 100; a.tick(now); while (a.next_packet(&p)) {} }
    CHECK(a.state() == SctpAssociation::State::Closed);
  }
  {   // damage: truncated and flipped packets never crash it, and a packet with a wrong checksum or tag changes nothing
    for (int mode = 0; mode < 3; ++mode)
      for (uint32_t victim = 0; victim < 40; ++victim) {
        CountRandom ra(7), rb(8);
        SctpAssociation a(alloc, ra), b(alloc, rb);
        uint64_t now = 1000;
        a.connect(now);
        uint32_t seen = 0;
        pump_sctp(a, b, &now, 30, [](uint32_t) { return false; }, [&](ByteBuffer* d) {
          if (seen++ != victim) return;
          if (mode == 0) d->truncate(d->size() / 2);
          else if (mode == 1) d->writable_view()[d->size() / 2] ^= 0x20;
          else d->writable_view()[d->size() - 1] ^= 0x01;
        });
        SctpOptions o;
        if (a.established()) a.send(1, 51, text_view("hello"), o);
        pump_sctp(a, b, &now, 300, [](uint32_t) { return false; }, [](ByteBuffer*) {});
      }
  }
  CHECK(alloc.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- Peer connection
struct Peer {
  Peer(FakeWire& wire, TestAllocator& a, Ipv4Address address, uint8_t seed) : node(wire, a, address, seed), random(seed * 77u), identity(a) {
    identity.generate(random, "WebRTC");
    pc = UniquePointer<PeerConnection>::make(a, a, node, random, identity);
    this->alloc = &a;
  }
  FakeNode node;
  CountRandom random;
  DtlsIdentity identity;
  UniquePointer<PeerConnection> pc;
  TestAllocator* alloc = nullptr;
  std::vector<std::string> events;
  std::vector<std::string> candidates;   // candidate lines found
  std::string take_all() {
    String e(*alloc);
    std::string all;
    while (pc->take_event(&e)) {
      std::string s(e.view().data(), e.size());
      events.push_back(s);
      all += s + "\n";
    }
    return all;
  }
  bool saw(const std::string& needle) const {
    for (const std::string& e : events)
      if (e.find(needle) != std::string::npos) return true;
    return false;
  }
  size_t count(const std::string& needle) const {
    size_t n = 0;
    for (const std::string& e : events) n += e.find(needle) != std::string::npos;
    return n;
  }
};

// Hands each side's new candidates to the other (trickle) and runs both for `ms`.
static void run_peers(FakeWire& wire, Peer& a, Peer& b, uint32_t ms, bool trickle = true) {
  for (uint32_t t = 0; t < ms; t += 5) {
    wire.now += 5;
    a.pc->poll();
    b.pc->poll();
    for (Peer* from : {&a, &b}) {
      Peer* to = from == &a ? &b : &a;
      std::string all = from->take_all();
      size_t at = 0;
      while ((at = all.find("{\"t\":\"icecandidate\",\"candidate\":\"", at)) != std::string::npos) {
        const size_t start = at + strlen("{\"t\":\"icecandidate\",\"candidate\":\"");
        const size_t end = all.find('"', start);
        const std::string line = all.substr(start, end - start);
        at = end;
        from->candidates.push_back(line);
        String err(*from->alloc);
        if (trickle) to->pc->add_ice_candidate(StringView(line.data(), line.size()), "0", 0, &err);
      }
    }
  }
}

static void test_peer_connection() {
  TestAllocator alloc;
  {
    FakeWire wire;
    wire.alloc = &alloc;
    Peer a(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1), b(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    String err(alloc), offer(alloc), answer(alloc);
    // the script's side of it: a channel, an offer, set it, the answer, set it
    uint32_t ch = a.pc->create_channel("chat", "v1", true, -1, -1, false, -1, &err);
    CHECK(ch != 0 && a.take_all().find("negotiationneeded") != std::string::npos);
    CHECK(!a.pc->create_answer(&answer, &err) && err.view().starts_with("InvalidStateError"));
    CHECK(a.pc->create_offer(&offer, &err));
    CHECK(offer.view().contains("m=application 9 UDP/DTLS/SCTP webrtc-datachannel") && offer.view().contains("a=setup:actpass") && offer.view().contains("a=group:BUNDLE 0") && offer.view().contains("a=sctp-port:5000"));
    CHECK(!offer.view().contains("a=candidate"));
    CHECK(a.pc->set_local_description("offer", "", &err) && a.pc->signaling_state() == PeerConnection::Signaling::HaveLocalOffer);
    CHECK(!a.pc->set_local_description("answer", "", &err));
    CHECK(b.pc->set_remote_description("offer", offer.view(), &err) && b.pc->signaling_state() == PeerConnection::Signaling::HaveRemoteOffer);
    CHECK(b.pc->create_answer(&answer, &err) && answer.view().contains("a=setup:active") && answer.view().contains("a=fingerprint:sha-256"));
    CHECK(b.pc->set_local_description("answer", "", &err) && b.pc->signaling_state() == PeerConnection::Signaling::Stable);
    CHECK(a.pc->set_remote_description("answer", answer.view(), &err) && a.pc->signaling_state() == PeerConnection::Signaling::Stable);
    run_peers(wire, a, b, 5000);
    CHECK(a.pc->connection_state() == PeerConnection::Connection::Connected && b.pc->connection_state() == PeerConnection::Connection::Connected);
    CHECK(a.saw("\"connectionstatechange\",\"state\":\"connected\"") && a.saw("\"iceconnectionstatechange\",\"state\":\"completed\"") && a.saw("\"icegatheringstatechange\",\"state\":\"complete\""));
    CHECK(a.saw("\"candidate\":null"));
    CHECK(b.saw("\"t\":\"datachannel\"") && b.saw("\"label\":\"chat\"") && b.saw("\"protocol\":\"v1\"") && a.saw("\"t\":\"dcopen\""));
    // the local description now has the candidates
    String type(alloc), sdp(alloc);
    CHECK(a.pc->local_description(&type, &sdp) && type.view().equals("offer") && sdp.view().contains("a=candidate:") && sdp.view().contains("a=end-of-candidates"));
    CHECK(b.pc->remote_description(&type, &sdp) && type.view().equals("offer") && sdp.view().equals(offer.view()));
    // messages: text, binary, empty, large, both ways
    const uint8_t bin[5] = {0, 1, 2, 254, 255};
    CHECK(a.pc->channel_send(ch, false, text_view("hello \"world\"\n"), &err) && a.pc->channel_send(ch, true, ConstByteView(bin, 5), &err) && a.pc->channel_send(ch, false, ConstByteView(), &err));
    std::string big(100000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 3) % 26);
    CHECK(a.pc->channel_send(ch, false, text_view(big), &err));
    CHECK(!a.pc->channel_send(ch, false, ConstByteView(reinterpret_cast<const uint8_t*>(big.data()), PeerConnection::kMaxMessage + 1), &err));
    run_peers(wire, a, b, 3000);
    CHECK(b.saw("\"dcmessage\"") && b.saw("hello \\\"world\\\"\\n") && b.saw("\"binary\":true,\"data\":\"00010 2feff") == false);
    CHECK(b.saw("\"binary\":true,\"data\":\"000102feff\"") && b.saw("\"binary\":false,\"data\":\"\""));
    CHECK(b.saw(big.substr(0, 50)) && b.count("\"dcmessage\"") == 4);
    uint32_t remote_ch = 1;   // (the first channel b knows)
    CHECK(b.pc->channel_send(remote_ch, false, text_view("reply"), &err));
    run_peers(wire, a, b, 500);
    CHECK(a.saw("\"data\":\"reply\""));
    CHECK(a.pc->channel_buffered(ch) == 0);
    // another channel, unordered with no retransmits, opened later; a negotiated one on both sides
    uint32_t ch2 = a.pc->create_channel("fast", "", false, 0, -1, false, -1, &err);
    uint32_t n1 = a.pc->create_channel("fixed", "", true, -1, -1, true, 7, &err), n2 = b.pc->create_channel("fixed", "", true, -1, -1, true, 7, &err);
    CHECK(ch2 != 0 && n1 != 0 && n2 != 0 && !a.pc->create_channel("again", "", true, -1, -1, true, 7, &err) && !a.pc->create_channel("bad", "", true, 1, 1, false, -1, &err));
    run_peers(wire, a, b, 1000);
    CHECK(b.saw("\"label\":\"fast\"") && b.saw("\"ordered\":false") && b.saw("\"maxRetransmits\":0"));
    CHECK(a.pc->channel_send(n1, false, text_view("on 7"), &err) && b.pc->channel_send(n2, false, text_view("back on 7"), &err));
    run_peers(wire, a, b, 500);
    CHECK(b.saw("\"data\":\"on 7\"") && a.saw("\"data\":\"back on 7\""));
    // closing a channel is seen on both sides
    a.pc->channel_close(ch2);
    run_peers(wire, a, b, 1500);
    CHECK(a.count("\"dcclosing\"") == 1 && a.saw("\"dcclose\",\"id\":" + std::to_string(ch2)) && b.count("\"dcclose\"") >= 1);
    CHECK(!a.pc->channel_send(ch2, false, text_view("x"), &err) && err.view().starts_with("InvalidStateError"));
    String stats(alloc);
    a.pc->stats(&stats);
    CHECK(stats.view().contains("\"type\":\"data-channel\"") && stats.view().contains("\"type\":\"candidate-pair\"") && stats.view().contains("\"dtlsState\":\"connected\""));
    a.pc->close();
    run_peers(wire, a, b, 500);
    CHECK(a.pc->connection_state() == PeerConnection::Connection::Closed && a.pc->signaling_state() == PeerConnection::Signaling::Closed && a.saw("\"state\":\"closed\""));
    CHECK(!a.pc->create_offer(&offer, &err) && !a.pc->channel_send(ch, false, text_view("x"), &err));
  }
  {   // the answer for a description that is not ours; a broken answer
    FakeWire wire;
    wire.alloc = &alloc;
    Peer a(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1);
    String err(alloc), offer(alloc);
    a.pc->create_channel("x", "", true, -1, -1, false, -1, &err);
    CHECK(a.pc->create_offer(&offer, &err) && a.pc->set_local_description("offer", "", &err));
    CHECK(!a.pc->set_remote_description("answer", "garbage", &err) && err.view().starts_with("InvalidAccessError"));
    CHECK(!a.pc->set_remote_description("answer", "v=0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:5\r\n", &err));
    CHECK(!a.pc->set_remote_description("offer", offer.view(), &err) && !a.pc->set_remote_description("bogus", offer.view(), &err));
    CHECK(a.pc->set_local_description("rollback", "", &err) && a.pc->signaling_state() == PeerConnection::Signaling::Stable);
    CHECK(!a.pc->add_ice_candidate("candidate:1 1 udp 1 1.2.3.4 5 typ host", "0", 0, &err));   // (no remote description)
  }
  {   // peers that cannot reach each other fail
    FakeWire wire;
    wire.alloc = &alloc;
    Peer a(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1), b(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    String err(alloc), offer(alloc), answer(alloc);
    a.pc->create_channel("x", "", true, -1, -1, false, -1, &err);
    a.pc->create_offer(&offer, &err);
    a.pc->set_local_description("offer", "", &err);
    b.pc->set_remote_description("offer", offer.view(), &err);
    b.pc->create_answer(&answer, &err);
    b.pc->set_local_description("answer", "", &err);
    a.pc->set_remote_description("answer", answer.view(), &err);
    wire.drop_all = true;
    run_peers(wire, a, b, 12000);
    a.pc->add_ice_candidate("", "0", 0, &err);
    run_peers(wire, a, b, 12000);
    CHECK(a.pc->connection_state() != PeerConnection::Connection::Connected);
  }
  CHECK(alloc.live == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- Audio
static double zero_crossings_per_second(const std::vector<int16_t>& v, size_t from, size_t count) {
  size_t n = 0;
  for (size_t i = from + 1; i < from + count && i < v.size(); ++i) n += (v[i - 1] < 0) != (v[i] < 0);
  return static_cast<double>(n) * 8000.0 / static_cast<double>(count);
}

static void test_audio() {
  TestAllocator alloc;
  // the codec
  CHECK(g711_ulaw_decode(0xFF) == 0 && g711_ulaw_decode(0x00) == -32124 && g711_ulaw_decode(0x80) == 32124 && g711_ulaw_encode(0) == 0xFF);
  CHECK(g711_alaw_decode(0xD5) == 8 && g711_alaw_decode(0x55) == -8 && g711_alaw_encode(8) == 0xD5);
  for (int v = -32768; v <= 32767; v += 13) {
    const int u = g711_ulaw_decode(g711_ulaw_encode(static_cast<int16_t>(v))), a = g711_alaw_decode(g711_alaw_encode(static_cast<int16_t>(v)));
    const int tolerance = (v < 0 ? -v : v) / 16 + 40;   // (the steps are about 1/16 of the level)
    CHECK(u - v < tolerance && v - u < tolerance && a - v < tolerance + 20 && v - a < tolerance + 20);
  }
  {
    FakeWire wire;
    wire.alloc = &alloc;
    Peer a(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1), b(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    SineSource mic_a(wire, 440), mic_b(wire, 880);
    TestDevices dev_a(wire, alloc), dev_b(wire, alloc);
    a.pc->set_devices(&dev_a);
    b.pc->set_devices(&dev_b);
    String err(alloc), offer(alloc), answer(alloc), info(alloc);
    // a data channel and a track on one connection; the other side adds its own after it has the offer
    CHECK(a.pc->create_channel("chat", "", true, -1, -1, false, -1, &err) != 0);
    CHECK(a.pc->add_track(&mic_a, "track-a", "stream-a", &err) == 0);   // (the first section)
    CHECK(a.pc->add_transceiver("video", 0, &err) < 0 && err.view().starts_with("NotSupportedError"));
    CHECK(a.pc->create_offer(&offer, &err));
    CHECK(offer.view().contains("m=audio 9 UDP/TLS/RTP/SAVPF 0 8") && offer.view().contains("a=sendrecv") && offer.view().contains("a=rtpmap:0 PCMU/8000") && offer.view().contains("a=msid:stream-a track-a") &&
          offer.view().contains("m=application") && offer.view().contains("a=group:BUNDLE 0 1") && offer.view().contains(" msid:stream-a track-a"));
    CHECK(a.pc->set_local_description("offer", "", &err));
    CHECK(b.pc->set_remote_description("offer", offer.view(), &err));
    CHECK(b.take_all().find("\"t\":\"track\",\"index\":0,\"mid\":\"0\",\"trackId\":\"track-a\",\"streamId\":\"stream-a\"") != std::string::npos);
    b.pc->take_event(&info);
    CHECK(b.pc->transceiver_info(0, &info) && info.view().contains("\"direction\":\"recvonly\"") && info.view().contains("\"currentDirection\":null") && info.view().contains("\"remoteTrackId\":\"track-a\""));
    CHECK(b.pc->add_track(&mic_b, "track-b", "stream-b", &err) == 0);   // (reuses the transceiver)
    CHECK(b.pc->transceiver_count() == 2 && b.pc->create_answer(&answer, &err));
    CHECK(answer.view().contains("m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n") && answer.view().contains("a=sendrecv") && answer.view().contains("a=msid:stream-b track-b") && !answer.view().contains("rtpmap:8"));
    CHECK(b.pc->set_local_description("answer", "", &err) && a.pc->set_remote_description("answer", answer.view(), &err));
    // the receivers play into recording sinks
    CHECK(a.pc->set_receiver_playing(0, true) == true && b.pc->set_receiver_playing(0, true) == true);
    run_peers(wire, a, b, 4000);
    CHECK(dev_a.heard.size() > 24000 && dev_b.heard.size() > 24000);   // (about four seconds at 8 kHz)
    {
      const double fa = zero_crossings_per_second(dev_a.heard, 8000, 8000) / 2, fb = zero_crossings_per_second(dev_b.heard, 8000, 8000) / 2;
      CHECK(fa > 860 && fa < 900 && fb > 430 && fb < 450);   // (each hears the other's tone: 880 Hz and 440 Hz)
    }
    CHECK(a.pc->connection_state() == PeerConnection::Connection::Connected && b.pc->connection_state() == PeerConnection::Connection::Connected);
    CHECK(a.saw("\"t\":\"track\",\"index\":0") && a.saw("\"t\":\"trackunmute\",\"index\":0") && b.saw("\"t\":\"trackunmute\",\"index\":0"));
    CHECK(a.saw("\"t\":\"dcopen\""));   // (the channel works beside the audio)
    String stats(alloc);
    a.pc->stats(&stats);
    CHECK(stats.view().contains("\"type\":\"outbound-rtp\"") && stats.view().contains("\"type\":\"inbound-rtp\"") && stats.view().contains("\"packetsLost\":0"));
    b.pc->stats(&stats);
    // about 50 packets a second each way
    const std::string s(stats.view().data(), stats.size());
    const size_t at = s.find("\"packetsReceived\":");
    CHECK(at != std::string::npos && atoi(s.c_str() + at + 18) > 100);
    CHECK(b.pc->transceiver_info(0, &info) && info.view().contains("\"currentDirection\":\"sendrecv\"") && info.view().contains("\"muted\":false"));
    // stopping a transceiver closes its section in the next offer
    String err2(alloc), second(alloc);
    CHECK(a.pc->set_direction(0, 2, &err2) && a.pc->create_offer(&second, &err2) && second.view().contains("a=recvonly"));
    CHECK(a.pc->stop_transceiver(0, &err2) && a.pc->create_offer(&second, &err2) && second.view().contains("m=audio 0 UDP/TLS/RTP/SAVPF") && second.view().contains("a=inactive"));
    a.pc->drop_source(&mic_a);
    CHECK(a.pc->set_sender_enabled(0, false) && !a.pc->set_sender_enabled(7, false));
  }
  {   // a track on a connection whose offer has only one side sending, and a section the other side cannot use
    FakeWire wire;
    wire.alloc = &alloc;
    Peer a(wire, alloc, Ipv4Address::make(192, 168, 1, 10), 1);
    String err(alloc), offer(alloc), answer(alloc);
    CHECK(a.pc->add_transceiver("audio", 2, &err) == 0 && a.pc->create_offer(&offer, &err) && offer.view().contains("a=recvonly") && !offer.view().contains("a=ssrc:"));
    Peer b(wire, alloc, Ipv4Address::make(192, 168, 1, 20), 2);
    // an offer with a codec we do not have, and a video section
    const std::string odd = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n"
                            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=ice-ufrag:abcd\r\na=ice-pwd:0123456789012345678901\r\na=fingerprint:sha-256 " + std::string(95, 'A') + "\r\na=setup:actpass\r\na=sendrecv\r\na=rtpmap:111 opus/48000/2\r\n"
                            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\na=mid:1\r\na=sendrecv\r\na=rtpmap:96 VP8/90000\r\n";
    String fp(alloc);
    std::string fixed = odd;
    b.identity.fingerprint(&fp);
    fixed.replace(fixed.find(std::string(95, 'A')), 95, std::string(fp.view().data(), fp.size()));
    CHECK(b.pc->set_remote_description("offer", StringView(fixed.data(), fixed.size()), &err) && b.pc->create_answer(&answer, &err));
    CHECK(answer.view().contains("m=audio 0 UDP/TLS/RTP/SAVPF 111") && answer.view().contains("m=video 0 UDP/TLS/RTP/SAVPF 96") && !answer.view().contains("a=group:BUNDLE 0"));
    CHECK(!b.saw("\"t\":\"track\""));
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_sdp();
  test_stun();
  test_rtp();
  test_srtp();
  test_ice();
  test_dtls();
  test_sctp();
  test_peer_connection();
  test_audio();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("rtc tests passed\n");
  return 0;
}
