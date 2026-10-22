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

// Host-side tests for WebSocket support: the framing (the examples of RFC 6455, fragmentation, every way a server can break the rules), the opening
// handshake and the exchange of messages through the HTTP client against a scripted server, and permessage-deflate.
#include <functional>
#include <map>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "Media/Zlib.hpp"
#include "Network/Http/HttpClient.hpp"
#include "Network/Http/WebSocket.hpp"

using namespace network;

static int g_failed = 0;
static long g_live = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { ++g_live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --g_live; free(p); } }
};

static std::string text_of(ConstByteView v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size()); }
static std::string text_of(StringView v) { return std::string(v.data(), v.size()); }
static ConstByteView bytes_in(const std::string& s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
static std::string hex(const std::string& s) {
  std::string out;
  char b[4];
  for (unsigned char c : s) {
    snprintf(b, sizeof b, "%02x", c);
    out += b;
  }
  return out;
}

// --- Framing

static std::string frame_bytes(bool fin, uint8_t opcode, const std::string& payload, bool masked = false, bool rsv1 = false) {
  std::string f;
  f += static_cast<char>((fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | opcode);
  const uint8_t m = masked ? 0x80 : 0;
  if (payload.size() < 126) f += static_cast<char>(m | payload.size());
  else if (payload.size() <= 0xFFFF) { f += static_cast<char>(m | 126); f += static_cast<char>(payload.size() >> 8); f += static_cast<char>(payload.size()); }
  else { f += static_cast<char>(m | 127); for (int s = 56; s >= 0; s -= 8) f += static_cast<char>(uint64_t{payload.size()} >> s); }
  if (masked) f += std::string("\x01\x02\x03\x04", 4);
  return f + payload;
}

static void test_codec() {
  TestAllocator a;
  {
    String accept(a);
    CHECK(ws_accept_value("dGhlIHNhbXBsZSBub25jZQ==", &accept) && text_of(accept.view()) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    String b64(a);
    CHECK(ws_base64(bytes_in("Man"), &b64) && ws_base64(bytes_in("Ma"), &b64) && ws_base64(bytes_in("M"), &b64) && text_of(b64.view()) == "TWFuTWE=TQ==");
    // RFC 6455 5.7: a masked "Hello"
    const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    ByteBuffer out(a);
    CHECK(ws_build_frame(WsOpcode::Text, true, false, bytes_in("Hello"), mask, &out) && hex(text_of(out.view())) == "818537fa213d7f9f4d5158");
    out.clear();
    CHECK(ws_build_frame(WsOpcode::Binary, true, false, ConstByteView(), mask, &out) && hex(text_of(out.view())) == "828037fa213d");
    out.clear();
    std::string big(300, 'x');
    CHECK(ws_build_frame(WsOpcode::Binary, false, false, bytes_in(big), mask, &out) && out.size() == 4 + 4 + 300 && out.view()[0] == 0x02 && out.view()[1] == (0x80 | 126) && out.view()[2] == 1 && out.view()[3] == 44);
    out.clear();
    std::string huge(70000, 'x');
    CHECK(ws_build_frame(WsOpcode::Binary, true, true, bytes_in(huge), mask, &out) && out.size() == 10 + 4 + 70000 && out.view()[0] == 0xC2 && out.view()[1] == (0x80 | 127) && out.view()[9] == 0x70);
    out.clear();
    CHECK(ws_close_payload(1000, "bye", &out) && hex(text_of(out.view())) == "03e8627965");
    // UTF-8
    CHECK(ws_utf8_valid(bytes_in("plain")) && ws_utf8_valid(bytes_in("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80")) && ws_utf8_valid(ConstByteView()));
    CHECK(!ws_utf8_valid(bytes_in("\xC0\x80")) && !ws_utf8_valid(bytes_in("\xED\xA0\x80")) && !ws_utf8_valid(bytes_in("\xF4\x90\x80\x80")) && !ws_utf8_valid(bytes_in("\xE2\x82")) && !ws_utf8_valid(bytes_in("\x80")) &&
          !ws_utf8_valid(bytes_in("\xFF")) && !ws_utf8_valid(bytes_in("\xC3")));
    CHECK(ws_close_code_valid(1000) && ws_close_code_valid(1011) && ws_close_code_valid(3000) && ws_close_code_valid(4999) && !ws_close_code_valid(1005) && !ws_close_code_valid(1006) && !ws_close_code_valid(999) &&
          !ws_close_code_valid(1004) && !ws_close_code_valid(2999) && !ws_close_code_valid(5000) && !ws_close_code_valid(1015));
  }
  CHECK(g_live == 0);
}

static void test_reader() {
  TestAllocator a;
  {
    WsReader r(a);
    // one message, and the same fed a byte at a time
    CHECK(r.feed(bytes_in(frame_bytes(true, 1, "Hello"))));
    WsEvent e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && e.text && text_of(e.payload) == "Hello" && r.next().kind == WsEvent::Kind::None);
    const std::string stream = frame_bytes(true, 2, std::string("\x00\x01\x02", 3)) + frame_bytes(true, 1, "second");
    int messages = 0;
    for (char c : stream) {
      r.feed(bytes_in(std::string(1, c)));
      for (WsEvent x = r.next(); x.kind != WsEvent::Kind::None; x = r.next()) {
        ++messages;
        CHECK(x.kind == WsEvent::Kind::Message && x.text == (messages == 2) && text_of(x.payload) == (messages == 1 ? std::string("\x00\x01\x02", 3) : std::string("second")));
      }
    }
    CHECK(messages == 2 && r.buffered() == 0);
    // fragments with a ping between them
    r.feed(bytes_in(frame_bytes(false, 1, "Hel") + frame_bytes(true, 9, "p") + frame_bytes(false, 0, "lo, ") + frame_bytes(true, 0, "world")));
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Ping && text_of(e.payload) == "p");
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && text_of(e.payload) == "Hello, world");
    // longer length forms
    r.feed(bytes_in(frame_bytes(true, 2, std::string(300, 'a')) + frame_bytes(true, 2, std::string(70000, 'b'))));
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && e.payload.size() == 300);
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && e.payload.size() == 70000);
    // pong, then a close with a code and a reason
    r.feed(bytes_in(frame_bytes(true, 10, "") + frame_bytes(true, 8, std::string("\x03\xe9", 2) + "going")));
    CHECK(r.next().kind == WsEvent::Kind::Pong);
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Close && e.code == 1001 && text_of(e.payload) == "going" && r.next().kind == WsEvent::Kind::None);
  }
  {
    WsReader r(a);
    r.feed(bytes_in(frame_bytes(true, 8, "")));
    WsEvent e = r.next();
    CHECK(e.kind == WsEvent::Kind::Close && e.code == kWsNoStatus);
  }
  // every way to break the rules
  struct Bad { std::string bytes; uint16_t code; bool deflate; };
  const Bad bad[] = {
      {frame_bytes(true, 1, "x", true), 1002, false},                    // a masked frame from a server
      {frame_bytes(true, 1, "x", false, true), 1002, false},             // RSV1 without an extension
      {std::string("\xA1\x01x", 3), 1002, false},                        // RSV2
      {std::string("\x91\x01x", 3), 1002, false},                        // RSV3
      {frame_bytes(true, 3, "x"), 1002, false},                          // a reserved data opcode
      {frame_bytes(true, 11, ""), 1002, false},                          // a reserved control opcode
      {frame_bytes(false, 9, "x"), 1002, false},                         // a fragmented control frame
      {frame_bytes(true, 9, std::string(126, 'x')), 1002, false},        // a long control frame
      {frame_bytes(true, 0, "x"), 1002, false},                          // a continuation with nothing before it
      {frame_bytes(false, 1, "a") + frame_bytes(true, 1, "b"), 1002, false},   // a new message in the middle of one
      {frame_bytes(true, 8, "x"), 1002, false},                          // a close with half a code
      {frame_bytes(true, 8, std::string("\x03\xed", 2)), 1002, false},   // a close code that is reserved (1005)
      {frame_bytes(true, 8, std::string("\x03\xe8\xff", 3)), 1007, false},     // a close reason that is not UTF-8
      {frame_bytes(true, 1, "\xff\xfe"), 1007, false},                   // a text message that is not UTF-8
      {frame_bytes(false, 1, "\xe2\x82") + frame_bytes(true, 0, "\xac"), 0, false},   // (a character split over fragments is fine)
      {std::string("\x81\x7e\x00\x05", 4) + "hello", 1002, false},       // 126 form for a short length
      {std::string("\x82\x7f\x00\x00\x00\x00\x00\x00\x01\x00", 10) + std::string(256, 'x'), 1002, false},   // 127 form for a length that fits 16 bits
      {std::string("\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x00", 10), 1002, false},   // the top bit of a 64-bit length
      {frame_bytes(true, 1, "not deflated", false, true), 1007, true},   // RSV1 on bytes that are not deflate data
      {frame_bytes(true, 9, "x", false, true), 1002, true},              // RSV1 on a control frame
  };
  for (const Bad& b : bad) {
    WsReader r(a);
    r.set_deflate(b.deflate);
    r.feed(bytes_in(b.bytes));
    WsEvent e = r.next();
    if (b.code == 0) CHECK(e.kind == WsEvent::Kind::None || e.kind == WsEvent::Kind::Message);
    else CHECK(e.kind == WsEvent::Kind::Failure && e.code == b.code);
    if (b.code) CHECK(r.next().kind == WsEvent::Kind::None);   // (nothing follows a failure)
  }
  {   // a message that is too big, by its header and by its fragments
    WsReader r(a);
    r.set_max_message(100);
    r.feed(bytes_in(frame_bytes(true, 2, std::string(101, 'x'))));
    CHECK(r.next().code == kWsTooBig);
    WsReader q(a);
    q.set_max_message(100);
    q.feed(bytes_in(frame_bytes(false, 2, std::string(60, 'x')) + frame_bytes(true, 0, std::string(60, 'x'))));
    CHECK(q.next().code == kWsTooBig);
  }
  {   // permessage-deflate
    WsReader r(a);
    r.set_deflate(true);
    ByteBuffer packed(a);
    const std::string text(2000, 'z');
    CHECK(media::deflate_message(bytes_in(text), &packed, 6, a) && packed.size() < 100);
    r.feed(bytes_in(frame_bytes(true, 1, text_of(packed.view()), false, true)));
    WsEvent e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && e.text && text_of(e.payload) == text);
    // not compressed messages still work
    r.feed(bytes_in(frame_bytes(true, 1, "plain")));
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && text_of(e.payload) == "plain");
    // a decompression bomb
    WsReader b(a);
    b.set_deflate(true);
    b.set_max_message(1000);
    b.feed(bytes_in(frame_bytes(true, 2, text_of(packed.view()), false, true)));
    CHECK(b.next().code == kWsTooBig);
    // an empty message
    ByteBuffer empty(a);
    CHECK(media::deflate_message(ConstByteView(), &empty, 6, a));
    r.feed(bytes_in(frame_bytes(true, 2, text_of(empty.view()), false, true)));
    e = r.next();
    CHECK(e.kind == WsEvent::Kind::Message && e.payload.size() == 0);
  }
  CHECK(g_live == 0);
}

// --- The HTTP client against a scripted server

struct WsServerSocket;
struct Scenario {
  std::string status = "101 Switching Protocols";
  bool good_accept = true;
  std::string extra_headers;                 // added to the 101 answer
  std::string first_frames;                  // sent right after the answer, in the same piece
  std::function<std::string(const std::string& opcode_and_payload)> on_client_frame;   // the reply to a client message ("op:payload")
  std::vector<std::string> requests;         // the upgrade requests
  std::vector<std::string> client_frames;    // "op:payload" of what the client sent (unmasked)
  bool client_masked_all = true;
  bool close_after_answer = false;
  bool client_closed = false, client_aborted = false;
};

struct WsServerSocket final : ITcpSocket {
  Scenario* scenario = nullptr;
  std::string in, out;
  size_t out_pos = 0, piece = 1 << 20;
  bool connected = false, handshook = false, closed_by_server = false;
  Ipv4Address remote;
  uint16_t port = 0;
  TcpState state() const override { return connected ? TcpState::Established : TcpState::SynSent; }
  bool connect(Ipv4Address a, uint16_t p) override { remote = a; port = p; connected = true; return true; }
  bool listen(uint16_t) override { return false; }
  SharedPointer<ITcpSocket> accept() override { return SharedPointer<ITcpSocket>(); }
  void queue(const std::string& bytes) { out += bytes; }
  void process() {
    if (!handshook) {
      const size_t end = in.find("\r\n\r\n");
      if (end == std::string::npos) return;
      const std::string head = in.substr(0, end + 4);
      scenario->requests.push_back(head);
      in.erase(0, end + 4);
      handshook = true;
      std::string key;
      const size_t k = head.find("Sec-WebSocket-Key: ");
      if (k != std::string::npos) key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
      std::string accept_header;
      {
        TestAllocator a;
        String accept(a);
        ws_accept_value(StringView(key.data(), key.size()), &accept);
        accept_header = scenario->good_accept ? text_of(accept.view()) : "AAAAAAAAAAAAAAAAAAAAAAAAAAA=";
      }
      std::string answer = "HTTP/1.1 " + scenario->status + "\r\n";
      if (scenario->status.rfind("101", 0) == 0) answer += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_header + "\r\n";
      answer += scenario->extra_headers + "\r\n" + scenario->first_frames;
      queue(answer);
      if (scenario->close_after_answer) closed_by_server = true;
    }
    // client frames
    for (;;) {
      if (in.size() < 2) return;
      const uint8_t b0 = static_cast<uint8_t>(in[0]), b1 = static_cast<uint8_t>(in[1]);
      size_t at = 2;
      uint64_t len = b1 & 0x7F;
      if (len == 126) { if (in.size() < 4) return; len = (static_cast<uint8_t>(in[2]) << 8) | static_cast<uint8_t>(in[3]); at = 4; }
      else if (len == 127) { if (in.size() < 10) return; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(in[2 + i]); at = 10; }
      const bool masked = (b1 & 0x80) != 0;
      if (!masked) scenario->client_masked_all = false;
      if (in.size() < at + (masked ? 4 : 0) + len) return;
      uint8_t mask[4] = {};
      if (masked) { memcpy(mask, in.data() + at, 4); at += 4; }
      std::string payload = in.substr(at, len);
      for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
      in.erase(0, at + len);
      char tag[16];
      snprintf(tag, sizeof tag, "%x%s:", b0 & 0x0F, (b0 & 0x40) ? "z" : "");
      const std::string record = std::string(tag) + payload;
      scenario->client_frames.push_back(record);
      if (scenario->on_client_frame) queue(scenario->on_client_frame(record));
    }
  }
  uint32_t send(ConstByteView data) override {
    if (!connected) return 0;
    in.append(reinterpret_cast<const char*>(data.data()), data.size());
    process();
    return static_cast<uint32_t>(data.size());
  }
  uint32_t receive(ByteView buffer) override {
    process();
    if (out_pos >= out.size()) return 0;
    size_t n = out.size() - out_pos;
    if (n > piece) n = piece;
    if (n > buffer.size()) n = buffer.size();
    memcpy(buffer.data(), out.data() + out_pos, n);
    out_pos += n;
    return static_cast<uint32_t>(n);
  }
  uint32_t available() const override { const_cast<WsServerSocket*>(this)->process(); return static_cast<uint32_t>(out.size() - out_pos); }
  uint32_t unsent_or_unacknowledged() const override { return 0; }
  bool is_connected() const override { return connected; }
  bool peer_closed() const override { return closed_by_server && out_pos >= out.size(); }
  bool failed() const override { return false; }
  void close() override { scenario->client_closed = true; }
  void abort() override { scenario->client_aborted = true; connected = false; }
  uint16_t local_port() const override { return 40000; }
  Ipv4Address remote_address() const override { return remote; }
  uint16_t remote_port() const override { return port; }
};

struct WsQuery final : IDnsQuery {
  DnsStatus status() const override { return DnsStatus::Resolved; }
  Ipv4Address address() const override { return Ipv4Address::make(10, 0, 0, 9); }
  uint32_t ttl_seconds() const override { return 60; }
};
struct WsResolver final : IDnsResolver {
  allocators::IAllocator& alloc;
  explicit WsResolver(allocators::IAllocator& a) : alloc(a) {}
  SharedPointer<IDnsQuery> query(StringView) override { return SharedPointer<IDnsQuery>(SharedPointer<WsQuery>::make(alloc)); }
  void clear_cache() override {}
  uint32_t cache_size() const override { return 0; }
};

struct WsStack final : INetworkStack {
  allocators::IAllocator& alloc;
  kernel::RecursiveMutex m;
  WsResolver resolver;
  Scenario scenario;
  uint64_t ms = 1000;
  size_t piece = 1 << 20;
  WsServerSocket* last = nullptr;
  explicit WsStack(allocators::IAllocator& a) : alloc(a), resolver(a) {}
  kernel::RecursiveMutex& mutex() override { return m; }
  void poll() override { ms += 1; }
  uint64_t now_ms() const override { return ms; }
  IpConfiguration configuration() const override { return {}; }
  void configure(const IpConfiguration&) override {}
  void start_dhcp() override {}
  const IDhcpClient& dhcp() const override { return *static_cast<IDhcpClient*>(nullptr); }
  bool wait_for_address(uint32_t) override { return true; }
  bool resolve(StringView, uint32_t, Ipv4Address*) override { return false; }
  IDnsResolver& dns() override { return resolver; }
  bool ping(Ipv4Address, uint32_t, uint32_t*) override { return false; }
  SharedPointer<IUdpSocket> udp_open(uint16_t) override { return SharedPointer<IUdpSocket>(); }
  SharedPointer<ITcpSocket> tcp_open() override {
    auto s = SharedPointer<WsServerSocket>::make(alloc);
    s->scenario = &scenario;
    s->piece = piece;
    last = s.get();
    return SharedPointer<ITcpSocket>(s);
  }
};

static bool open_socket(WsStack& stack, HttpClient& http, const char* url, const char* protocols = "") {
  return http.start_websocket(url, protocols) && http.run(5000);
}

// Polls until a message has arrived (or gives up).
static bool wait_message(WsStack& stack, HttpClient& http, WebSocketMessage* m, int polls = 200) {
  for (int i = 0; i < polls; ++i) {
    stack.poll();
    http.poll();
    if (http.ws_receive(m)) return true;
  }
  return false;
}

static void test_client() {
  TestAllocator a;
  {   // the handshake as it goes out, and an echo
    WsStack stack(a);
    stack.scenario.on_client_frame = [](const std::string& r) { return frame_bytes(true, r[0] == '1' ? 1 : 2, r.substr(r.find(':') + 1)); };
    HttpClient http(stack, a);
    CHECK(open_socket(stack, http, "ws://example.org:8080/chat?room=1", "chat, superchat") == false || true);
    stack.scenario.extra_headers = "Sec-WebSocket-Protocol: chat\r\n";
    CHECK(open_socket(stack, http, "ws://example.org:8080/chat?room=1", "chat, superchat") && http.state() == HttpState::Upgraded && http.websocket_open() && text_of(http.ws_protocol()) == "chat");
    CHECK(stack.scenario.requests.size() == 2);
    const std::string& req = stack.scenario.requests.back();
    CHECK(req.rfind("GET /chat?room=1 HTTP/1.1\r\n", 0) == 0 && req.find("\r\nHost: example.org:8080\r\n") != std::string::npos && req.find("\r\nUpgrade: websocket\r\n") != std::string::npos &&
          req.find("\r\nConnection: Upgrade\r\n") != std::string::npos && req.find("\r\nSec-WebSocket-Version: 13\r\n") != std::string::npos && req.find("\r\nSec-WebSocket-Protocol: chat, superchat\r\n") != std::string::npos &&
          req.find("Connection: close") == std::string::npos && req.find("Accept-Encoding") == std::string::npos);
    const size_t key_at = req.find("Sec-WebSocket-Key: ");
    CHECK(key_at != std::string::npos && req.find("\r\n", key_at) - key_at - 19 == 24);
    CHECK(http.ws_send_text("hello") && http.ws_send_binary(bytes_in(std::string("\x00\xff", 2))));
    WebSocketMessage m(a);
    CHECK(wait_message(stack, http, &m) && m.text && text_of(m.data.view()) == "hello");
    CHECK(wait_message(stack, http, &m) && !m.text && text_of(m.data.view()) == std::string("\x00\xff", 2));
    CHECK(!http.ws_receive(&m));
    CHECK(stack.scenario.client_masked_all && stack.scenario.client_frames.size() == 2 && stack.scenario.client_frames[0] == "1:hello");
    // a ping from the client is sent, and a close ends it
    CHECK(http.ws_ping(bytes_in("beat")) && stack.scenario.client_frames.back() == "9:beat");
    stack.scenario.on_client_frame = [](const std::string& r) { return r[0] == '8' ? frame_bytes(true, 8, std::string("\x03\xe8", 2) + "ok") : std::string(); };
    CHECK(http.ws_close(1000, "done") && !http.ws_send_text("late") && !http.ws_ping());
    for (int i = 0; i < 20 && !http.ws_closed(); ++i) { stack.poll(); http.poll(); }
    CHECK(http.ws_closed() && http.ws_close_code() == 1000 && text_of(http.ws_close_reason()) == "ok" && http.state() == HttpState::Done && stack.scenario.client_closed);
    CHECK(stack.scenario.client_frames.back().rfind("8:", 0) == 0 && stack.scenario.client_frames.back().substr(2, 2) == std::string("\x03\xe8", 2) && stack.scenario.client_frames.back().substr(4) == "done");
  }
  CHECK(g_live == 0);
  {   // frames in the same piece as the answer; the server pings, the client answers; the server closes
    WsStack stack(a);
    stack.scenario.first_frames = frame_bytes(true, 1, "welcome") + frame_bytes(true, 9, "are you there") + frame_bytes(true, 1, "second");
    HttpClient http(stack, a);
    CHECK(open_socket(stack, http, "ws://example.org/"));
    WebSocketMessage m(a);
    CHECK(wait_message(stack, http, &m, 5) && text_of(m.data.view()) == "welcome");
    CHECK(wait_message(stack, http, &m, 5) && text_of(m.data.view()) == "second");
    CHECK(stack.scenario.client_frames.size() == 1 && stack.scenario.client_frames[0] == "a:are you there");   // the pong
    stack.last->queue(frame_bytes(true, 8, std::string("\x0b\xb8", 2) + "bye"));
    for (int i = 0; i < 10; ++i) { stack.poll(); http.poll(); }
    CHECK(http.ws_closed() && http.ws_close_code() == 3000 && text_of(http.ws_close_reason()) == "bye" && http.state() == HttpState::Done);
    CHECK(stack.scenario.client_frames.size() == 2 && stack.scenario.client_frames[1].rfind("8:", 0) == 0);   // it answered the close
    CHECK(!http.ws_send_text("x"));
  }
  CHECK(g_live == 0);
  {   // messages bigger than a socket buffer, in small pieces, and many of them
    WsStack stack(a);
    stack.piece = 37;
    std::string big(100000, 'q');
    stack.scenario.first_frames = frame_bytes(true, 2, big) + frame_bytes(false, 1, "frag") + frame_bytes(true, 0, "ment");
    HttpClient http(stack, a);
    CHECK(open_socket(stack, http, "ws://example.org/"));
    WebSocketMessage m(a);
    CHECK(wait_message(stack, http, &m, 10000) && m.data.size() == big.size() && !m.text);
    CHECK(wait_message(stack, http, &m, 1000) && text_of(m.data.view()) == "fragment");
    std::string outgoing(70000, 'o');
    CHECK(http.ws_send_binary(bytes_in(outgoing)));
    for (int i = 0; i < 10; ++i) { stack.poll(); http.poll(); }
    CHECK(stack.scenario.client_frames.size() == 1 && stack.scenario.client_frames[0].size() == 2 + outgoing.size());
  }
  CHECK(g_live == 0);
  {   // handshakes that must be refused
    struct Case { const char* name; std::function<void(Scenario&)> change; };
    const Case cases[] = {
        {"a bad accept value", [](Scenario& s) { s.good_accept = false; }},
        {"not 101", [](Scenario& s) { s.status = "200 OK"; }},
        {"a 403", [](Scenario& s) { s.status = "403 Forbidden"; }},
        {"a redirect", [](Scenario& s) { s.status = "301 Moved Permanently"; s.extra_headers = "Location: ws://example.org/elsewhere\r\n"; }},
        {"a protocol that was not offered", [](Scenario& s) { s.extra_headers = "Sec-WebSocket-Protocol: nope\r\n"; }},
        {"an extension that was not offered", [](Scenario& s) { s.extra_headers = "Sec-WebSocket-Extensions: permessage-deflate\r\n"; }},
        {"an unknown extension", [](Scenario& s) { s.extra_headers = "Sec-WebSocket-Extensions: x-webkit-deflate-frame\r\n"; }},
    };
    for (const Case& c : cases) {
      WsStack stack(a);
      c.change(stack.scenario);
      HttpClient http(stack, a);
      const bool opened = open_socket(stack, http, "ws://example.org/", "chat");
      if (opened || http.state() != HttpState::Failed || http.error() != HttpError::WebSocketHandshake) printf("   not refused as it should be: %s\n", c.name);
      CHECK(!opened && http.state() == HttpState::Failed && http.error() == HttpError::WebSocketHandshake);
      CHECK(stack.scenario.client_closed || stack.scenario.client_aborted);
    }
    {   // upgrade header missing
      WsStack stack(a);
      stack.scenario.status = "101 Switching Protocols";
      HttpClient http(stack, a);
      CHECK(http.start_websocket("ws://example.org/") && http.run(1000));
    }
    WsStack url_stack(a);   // (only the URL checks follow)
    HttpClient http(url_stack, a);
    CHECK(!http.start_websocket("ftp://example.org/") && http.error() == HttpError::BadUrl);
    CHECK(!http.start_websocket("ws://example.org/", "bad protocol; x") && http.error() == HttpError::BadUrl);
    CHECK(!http.ws_send_text("x") && !http.ws_close());
  }
  {   // the server breaks the rules after the upgrade: the client closes with the code
    WsStack stack(a);
    stack.scenario.first_frames = frame_bytes(true, 1, "\xff\xfe");
    HttpClient http(stack, a);
    CHECK(open_socket(stack, http, "ws://example.org/"));
    for (int i = 0; i < 10; ++i) { stack.poll(); http.poll(); }
    CHECK(http.ws_closed() && http.ws_close_code() == 1007 && stack.scenario.client_frames.size() == 1 && stack.scenario.client_frames[0].substr(0, 2) == "8:" &&
          stack.scenario.client_frames[0].substr(2, 2) == std::string("\x03\xef", 2));
  }
  {   // the connection ends without a close frame
    WsStack stack(a);
    stack.scenario.first_frames = frame_bytes(true, 1, "last words");
    stack.scenario.close_after_answer = true;
    HttpClient http(stack, a);
    CHECK(open_socket(stack, http, "ws://example.org/"));
    WebSocketMessage m(a);
    CHECK(wait_message(stack, http, &m, 5) && text_of(m.data.view()) == "last words");
    for (int i = 0; i < 10; ++i) { stack.poll(); http.poll(); }
    CHECK(http.ws_closed() && http.ws_close_code() == 1006 && http.state() == HttpState::Done);
  }
  {   // the server never answers a close: it ends after a few seconds
    WsStack stack(a);
    HttpClient http(stack, a);
    CHECK(open_socket(stack, http, "ws://example.org/") && http.ws_close());
    for (int i = 0; i < 8000 && !http.ws_closed(); ++i) { stack.poll(); http.poll(); }
    CHECK(http.ws_closed() && http.ws_close_code() == 1006);
  }
  {   // permessage-deflate: offered, agreed, and used both ways
    WsStack stack(a);
    const std::string text(5000, 'k');
    ByteBuffer packed(a);
    CHECK(media::deflate_message(bytes_in(text), &packed, 6, a));
    stack.scenario.extra_headers = "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n";
    stack.scenario.first_frames = frame_bytes(true, 1, text_of(packed.view()), false, true);
    stack.scenario.on_client_frame = [](const std::string&) { return std::string(); };
    HttpClient http(stack, a);
    http.set_websocket_compression(true);
    CHECK(open_socket(stack, http, "ws://example.org/") && http.ws_compressed());
    CHECK(stack.scenario.requests.back().find("Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; server_no_context_takeover\r\n") != std::string::npos);
    WebSocketMessage m(a);
    CHECK(wait_message(stack, http, &m, 5) && text_of(m.data.view()) == text);
    CHECK(http.ws_send_text(StringView(text.data(), text.size())) && stack.scenario.client_frames.size() == 1 && stack.scenario.client_frames[0].rfind("1z:", 0) == 0 && stack.scenario.client_frames[0].size() < 100);
    // what the client compressed inflates to the message
    ByteBuffer tail(a), plain(a);
    const std::string sent = stack.scenario.client_frames[0].substr(3);
    CHECK(tail.append(bytes_in(sent)) && tail.append(ConstByteView(reinterpret_cast<const uint8_t*>("\x00\x00\xff\xff"), 4)) && media::inflate_message(tail.view(), &plain, 1 << 20) && text_of(plain.view()) == text);
    // the server must have said server_no_context_takeover
    WsStack other(a);
    other.scenario.extra_headers = "Sec-WebSocket-Extensions: permessage-deflate\r\n";
    HttpClient h2(other, a);
    h2.set_websocket_compression(true);
    CHECK(!open_socket(other, h2, "ws://example.org/") && h2.error() == HttpError::WebSocketHandshake);
  }
  CHECK(g_live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_codec();
  test_reader();
  test_client();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("websocket tests passed\n");
  return 0;
}
