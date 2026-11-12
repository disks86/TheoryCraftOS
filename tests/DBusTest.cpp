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

// Host tests for the kernel's D-Bus (src/DBus): names and signatures, the marshalling (round trips in both byte orders, every truncation, bit flips),
// messages, authentication as libdbus, sd-bus and GDBus do it, match rules, the bus (Hello, the name queues, calls and replies, NoReply, signals and match
// rules, eavesdropping, monitors, descriptors, credentials, introspection, properties, protocol errors), the Notifications service, and the AF_UNIX
// transport with several client threads at once (descriptors passed through the system's sockets).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "DBus/Auth.hpp"
#include "DBus/Bus.hpp"
#include "DBus/Match.hpp"
#include "DBus/Message.hpp"
#include "DBus/Notifications.hpp"
#include "DBus/SocketServer.hpp"
#include "DBus/Wire.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/UnixSockets.hpp"

using namespace dbus;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t align) override {
    ++live;
    void* p = nullptr;
    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size ? size : 1) != 0) return nullptr;
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;

static std::atomic<int> g_failed{0};
static std::atomic<int> g_checks{0};
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace hardware {
uint64_t uptime_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace hardware

static std::string str(StringView v) { return std::string(v.data(), v.size()); }

// ---- names and signatures ------------------------------------------------------------------------------------------------------------------------

static void test_names() {
  CHECK(valid_object_path("/") && valid_object_path("/org/freedesktop/DBus") && valid_object_path("/a_b/c9"));
  CHECK(!valid_object_path("") && !valid_object_path("a") && !valid_object_path("/a/") && !valid_object_path("//") && !valid_object_path("/a//b") && !valid_object_path("/a-b"));
  CHECK(valid_interface_name("org.freedesktop.DBus") && valid_interface_name("a.b") && valid_interface_name("_a.b_9"));
  CHECK(!valid_interface_name("org") && !valid_interface_name("org.") && !valid_interface_name(".org.x") && !valid_interface_name("org.9x") && !valid_interface_name("a..b") &&
        !valid_interface_name("a.b-c"));
  CHECK(valid_member_name("Hello") && valid_member_name("_x9") && !valid_member_name("9x") && !valid_member_name("a.b") && !valid_member_name(""));
  CHECK(valid_bus_name(":1.5") && valid_bus_name(":1.0.2") && valid_bus_name("org.example.App-2") && valid_bus_name("a.b") && valid_bus_name(":a-b.9"));
  CHECK(!valid_bus_name(":1") && !valid_bus_name("org") && !valid_bus_name("org.9a") && !valid_bus_name("") && !valid_bus_name(".a.b") && !valid_bus_name("a.b."));
  std::string longname(254, 'a');
  longname[1] = '.';
  CHECK(valid_bus_name(StringView(longname.c_str())));
  longname += "aa";
  CHECK(!valid_bus_name(StringView(longname.c_str())));
  CHECK(valid_utf8(StringView("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80").bytes()));
  CHECK(!valid_utf8(StringView("\xC0\x80").bytes()) && !valid_utf8(StringView("\xED\xA0\x80").bytes()) && !valid_utf8(StringView("\xF4\x90\x80\x80").bytes()) &&
        !valid_utf8(StringView("\xE2\x82").bytes()) && !valid_utf8(StringView("\x80").bytes()));
}

static void test_signatures() {
  for (const char* good : {"", "y", "s", "a{sv}", "(ii)", "a(sa{sv})", "aay", "v", "susssasa{sv}i", "a{s(ii)}", "h", "(a{ss}v)"}) CHECK(valid_signature(good));
  for (const char* bad : {"a", "()", "{sv}", "a{vs}", "a{s}", "a{sss}", "(i", "i)", "z", "a{(i)s}", "(}", "aa"}) {
    if (valid_signature(bad)) printf("  wrongly valid: %s\n", bad);
    CHECK(!valid_signature(bad));
  }
  CHECK(single_complete_type("a{sv}") && !single_complete_type("ii") && !single_complete_type(""));
  std::string deep_arrays(32, 'a');
  deep_arrays += "i";
  CHECK(valid_signature(StringView(deep_arrays.c_str())));
  deep_arrays = "a" + deep_arrays;
  CHECK(!valid_signature(StringView(deep_arrays.c_str())));
  std::string deep_structs = std::string(32, '(') + "i" + std::string(32, ')');
  CHECK(valid_signature(StringView(deep_structs.c_str())));
  deep_structs = "(" + deep_structs + ")";
  CHECK(!valid_signature(StringView(deep_structs.c_str())));
  std::string too_long(256, 'i');
  CHECK(!valid_signature(StringView(too_long.c_str())));
}

// A body of every kind of value, in either byte order.
static const char* kRichSignature = "ybnqiuxtdsogva{sv}a(ii)ayh(s(ay))";
static void write_rich(Writer& w) {
  w.u8(7);
  w.boolean(true);
  w.u16(0xFFFE);
  w.u16(65000);
  w.u32(0x80000001u);
  w.u32(42);
  w.u64(0x8000000000000001ull);
  w.u64(~0ull);
  w.u64(0x400921FB54442D18ull);
  w.string("h\xC3\xA9llo");
  w.object_path("/org/x");
  w.signature("a{sv}");
  w.variant("(su)");
  w.begin_struct();
  w.string("in a variant");
  w.u32(5);
  Writer::ArrayMark dict = w.begin_array(8);
  for (int i = 0; i < 3; ++i) {
    w.begin_struct();
    w.string(i == 0 ? "a" : i == 1 ? "bb" : "ccc");
    w.variant(i == 2 ? "av" : "x");
    if (i == 2) {
      Writer::ArrayMark inner = w.begin_array(1);
      w.variant("y");
      w.u8(9);
      w.variant("s");
      w.string("deep");
      w.end_array(inner);
    } else {
      w.u64(uint64_t(i) * 1000);
    }
  }
  w.end_array(dict);
  Writer::ArrayMark pairs = w.begin_array(8);
  for (int i = 0; i < 2; ++i) {
    w.begin_struct();
    w.u32(uint32_t(i));
    w.u32(uint32_t(i * 2));
  }
  w.end_array(pairs);
  Writer::ArrayMark bytes = w.begin_array(1);
  for (int i = 0; i < 5; ++i) w.u8(uint8_t(i));
  w.end_array(bytes);
  w.u32(0);      // h: descriptor 0
  w.begin_struct();
  w.string("x");
  w.begin_struct();
  Writer::ArrayMark empty = w.begin_array(1);
  w.end_array(empty);
}

static void test_marshalling() {
  for (bool be : {false, true}) {
    ByteBuffer body(A);
    Writer w(body, be);
    write_rich(w);
    CHECK(w.ok());
    CHECK(validate_body(body.view(), kRichSignature, be, 1));
    CHECK(!validate_body(body.view(), kRichSignature, be, 0));      // (h 0 without a descriptor)
    CHECK(!validate_body(body.view(), kRichSignature, !be, 1));
    // read some back
    Reader r(body.view(), be);
    uint8_t y = 0;
    bool b = false;
    uint16_t n = 0, q = 0;
    uint32_t i = 0, u = 0;
    uint64_t x = 0;
    CHECK(r.u8(&y) && y == 7 && r.boolean(&b) && b && r.u16(&n) && n == 0xFFFE && r.u16(&q) && q == 65000 && r.u32(&i) && i == 0x80000001u && r.u32(&u) && u == 42 &&
          r.u64(&x) && x == 0x8000000000000001ull);
    // every truncation fails, and so does a byte too many
    int truncations_ok = 0;
    for (uint64_t len = 0; len < body.size(); ++len) truncations_ok += validate_body(body.view().first(len), kRichSignature, be, 1);
    CHECK(truncations_ok == 0);
    ByteBuffer longer(A);
    longer.append(body.view());
    longer.put8(0);
    CHECK(!validate_body(longer.view(), kRichSignature, be, 1));
    // bit flips: never a crash; a flip of a padding byte, a boolean or a length is refused
    std::mt19937 rng(be ? 7 : 3);
    int accepted = 0;
    for (uint64_t bit = 0; bit < body.size() * 8; ++bit) {
      ByteBuffer flipped(A);
      flipped.append(body.view());
      flipped.writable_view()[bit / 8] ^= uint8_t(1u << (bit % 8));
      accepted += validate_body(flipped.view(), kRichSignature, be, 1);
    }
    CHECK(accepted > 0 && uint64_t(accepted) < body.size() * 8);
    for (int round = 0; round < 3000; ++round) {
      ByteBuffer junk(A);
      const uint32_t len = rng() % 200;
      for (uint32_t k = 0; k < len; ++k) junk.put8(uint8_t(rng()));
      validate_body(junk.view(), kRichSignature, be, 1);
      validate_body(junk.view(), "a{sv}", be, 0);
      validate_body(junk.view(), "av", be, 0);
    }
  }
  // bad values
  {
    ByteBuffer b(A);
    Writer w(b);
    w.u32(2);
    CHECK(!validate_body(b.view(), "b", false, 0));
    b.clear();
    Writer w2(b);
    w2.string(StringView("a\0b", 3));
    CHECK(!validate_body(b.view(), "s", false, 0));
    b.clear();
    Writer w3(b);
    w3.string("\xC0\x80");
    CHECK(!validate_body(b.view(), "s", false, 0));
    b.clear();
    Writer w4(b);
    w4.object_path("not/a/path");
    CHECK(!validate_body(b.view(), "o", false, 0));
    b.clear();
    Writer w5(b);
    w5.signature("a{");
    CHECK(!validate_body(b.view(), "g", false, 0));
    b.clear();
    Writer w6(b);
    w6.variant("ii");      // (a variant holds one complete type)
    w6.u32(1);
    w6.u32(2);
    CHECK(!validate_body(b.view(), "v", false, 0));
    b.clear();
    Writer w7(b);      // a u32 array whose length is not a multiple of 4
    w7.u32(6);
    w7.u32(1);
    w7.u16(2);
    CHECK(!validate_body(b.view(), "au", false, 0));
    b.clear();
    Writer w8(b);      // nonzero padding
    w8.u8(1);
    w8.u8(1);
    w8.u8(0);
    w8.u8(0);
    w8.u32(5);
    CHECK(!validate_body(b.view(), "yu", false, 0));
    // variants nested past 64
    b.clear();
    Writer w9(b);
    for (int k = 0; k < 70; ++k) w9.variant("v");
    w9.variant("y");
    w9.u8(1);
    CHECK(!validate_body(b.view(), "v", false, 0));
    b.clear();
    Writer w10(b);      // an array longer than 64 MiB says so in its length alone
    w10.u32(kMaxArrayBytes + 4);
    CHECK(!validate_body(b.view(), "ay", false, 0));
  }
  // string args
  {
    ByteBuffer b(A);
    Writer w(b);
    w.string("first");
    w.u32(3);
    w.object_path("/p/q");
    Writer::ArrayMark m = w.begin_array(4);
    w.string("x");
    w.end_array(m);
    w.string("last");
    StringArgs args;
    string_args(b.view(), "suoass", false, 64, &args);
    CHECK(args.count == 5 && args.kind[0] == 's' && str(args.value[0]) == "first" && args.kind[1] == 0 && args.kind[2] == 'o' && str(args.value[2]) == "/p/q" &&
          args.kind[3] == 0 && str(args.value[4]) == "last");
  }
}

// ---- messages ------------------------------------------------------------------------------------------------------------------------------------

static void make_call(Message* m, bool be) {
  m->big_endian = be;
  m->type = kMethodCall;
  m->flags = 0;
  m->serial = 77;
  m->path.assign("/org/example/Obj");
  m->interface.assign("org.example.Iface");
  m->member.assign("DoIt");
  m->destination.assign("org.example.Service");
  m->signature.assign(kRichSignature);
  m->unix_fds = 1;
  m->body.clear();
  Writer w(m->body, be);
  write_rich(w);
}

static void test_messages() {
  for (bool be : {false, true}) {
    Message m(A), back(A);
    make_call(&m, be);
    ByteBuffer wire(A);
    CHECK(encode(m, &wire));
    CHECK(framed_size(wire.view()) == int64_t(wire.size()));
    CHECK(framed_size(wire.view().first(15)) == 0);
    CHECK(decode(wire.view(), &back) == DecodeStatus::Ok);
    CHECK(back.big_endian == be && back.type == kMethodCall && back.serial == 77 && back.path == "/org/example/Obj" && back.interface == "org.example.Iface" &&
          back.member == "DoIt" && back.destination == "org.example.Service" && back.signature == StringView(kRichSignature) && back.unix_fds == 1 &&
          back.body.size() == m.body.size());
    // every truncation of the whole message is refused
    int ok = 0;
    for (uint64_t len = 0; len < wire.size(); ++len) ok += decode(wire.view().first(len), &back) == DecodeStatus::Ok;
    CHECK(ok == 0);
    // every bit flip: no crash; whatever is still accepted encodes and decodes to the same thing again
    int accepted = 0, stable = 0;
    for (uint64_t bit = 0; bit < wire.size() * 8; ++bit) {
      ByteBuffer flipped(A);
      flipped.append(wire.view());
      flipped.writable_view()[bit / 8] ^= uint8_t(1u << (bit % 8));
      if (framed_size(flipped.view()) != int64_t(flipped.size())) continue;
      if (decode(flipped.view(), &back) != DecodeStatus::Ok) continue;
      ++accepted;
      ByteBuffer again(A);
      Message third(A);
      if (encode(back, &again) && decode(again.view(), &third) == DecodeStatus::Ok && third.serial == back.serial && third.member == back.member.view() &&
          third.body.size() == back.body.size())
        ++stable;
    }
    CHECK(accepted > 0 && accepted == stable);
  }
  // required fields, bad fixed headers, an unknown header field (dropped), a duplicate (refused)
  {
    Message m(A), back(A);
    make_call(&m, false);
    m.member.clear();
    ByteBuffer wire(A);
    CHECK(encode(m, &wire) && decode(wire.view(), &back) == DecodeStatus::Invalid);
    m.member.assign("X");
    m.type = kSignal;
    m.interface.clear();
    wire.clear();
    CHECK(encode(m, &wire) && decode(wire.view(), &back) == DecodeStatus::Invalid);
    m.type = kError;
    m.interface.assign("a.b");
    wire.clear();
    CHECK(encode(m, &wire) && decode(wire.view(), &back) == DecodeStatus::Invalid);      // (no error name, no reply serial)
    m.type = kMethodCall;
    m.serial = 0;
    wire.clear();
    CHECK(encode(m, &wire) && decode(wire.view(), &back) == DecodeStatus::Invalid);
    m.serial = 1;
    wire.clear();
    CHECK(encode(m, &wire));
    ByteBuffer bad(A);
    bad.append(wire.view());
    bad.writable_view()[3] = 2;      // protocol version
    CHECK(framed_size(bad.view()) == -1);
    bad.writable_view()[3] = 1;
    bad.writable_view()[0] = 'x';
    CHECK(framed_size(bad.view()) == -1);
    // hand-made header: PATH, MEMBER, an unknown field 200 (as), then the same MEMBER twice
    for (int duplicate = 0; duplicate < 2; ++duplicate) {
      ByteBuffer h(A);
      Writer w(h);
      w.u8('l'); w.u8(kMethodCall); w.u8(0); w.u8(1);
      w.u32(0);
      w.u32(9);
      Writer::ArrayMark f = w.begin_array(8);
      w.begin_struct(); w.u8(kFieldPath); w.variant("o"); w.object_path("/");
      w.begin_struct(); w.u8(kFieldMember); w.variant("s"); w.string("M");
      w.begin_struct(); w.u8(200); w.variant("as");
      Writer::ArrayMark arr = w.begin_array(4);
      w.string("ignored");
      w.end_array(arr);
      if (duplicate) { w.begin_struct(); w.u8(kFieldMember); w.variant("s"); w.string("N"); }
      w.end_array(f);
      w.pad(8);
      CHECK(w.ok());
      const DecodeStatus st = decode(h.view(), &back);
      CHECK(st == (duplicate ? DecodeStatus::Invalid : DecodeStatus::Ok));
      if (!duplicate) {
        ByteBuffer re(A);
        CHECK(encode(back, &re) && re.size() < h.size());      // (field 200 is gone)
      }
    }
    // a field of the wrong type
    ByteBuffer h(A);
    Writer w(h);
    w.u8('l'); w.u8(kMethodCall); w.u8(0); w.u8(1);
    w.u32(0);
    w.u32(9);
    Writer::ArrayMark f = w.begin_array(8);
    w.begin_struct(); w.u8(kFieldPath); w.variant("s"); w.string("/");
    w.begin_struct(); w.u8(kFieldMember); w.variant("s"); w.string("M");
    w.end_array(f);
    w.pad(8);
    CHECK(decode(h.view(), &back) == DecodeStatus::Invalid);
  }
  // random garbage framed as messages
  std::mt19937 rng(11);
  Message back(A);
  for (int round = 0; round < 5000; ++round) {
    ByteBuffer junk(A);
    const uint32_t len = 16 + rng() % 300;
    for (uint32_t k = 0; k < len; ++k) junk.put8(uint8_t(rng()));
    junk.writable_view()[0] = (rng() & 1) ? 'l' : 'B';
    junk.writable_view()[3] = 1;
    decode(junk.view(), &back);
  }
}

// ---- authentication ------------------------------------------------------------------------------------------------------------------------------

static std::string feed_all(AuthServer& auth, const std::string& in, uint64_t* used = nullptr) {
  ByteBuffer out(A);
  const uint64_t n = auth.feed(ConstByteView(reinterpret_cast<const uint8_t*>(in.data()), in.size()), &out);
  if (used) *used = n;
  return std::string(reinterpret_cast<const char*>(out.view().data()), out.size());
}

static void test_auth() {
  const char* guid = "0123456789abcdef0123456789abcdef";
  {      // libdbus: AUTH EXTERNAL <uid>, NEGOTIATE_UNIX_FD, BEGIN, then the first message in the same read
    AuthServer auth(A, 1000, guid, true);
    uint64_t used = 0;
    const std::string in = std::string("\0", 1) + "AUTH EXTERNAL 31303030\r\nNEGOTIATE_UNIX_FD\r\nBEGIN\r\nl\x01";
    const std::string out = feed_all(auth, in, &used);
    CHECK(out == std::string("OK ") + guid + "\r\nAGREE_UNIX_FD\r\n");
    CHECK(auth.done() && auth.unix_fds() && auth.uid() == 1000 && used == in.size() - 2);
  }
  {      // sd-bus: AUTH EXTERNAL, then an empty DATA
    AuthServer auth(A, 0, guid, true);
    const std::string out = feed_all(auth, std::string("\0", 1) + "AUTH EXTERNAL\r\nDATA\r\nNEGOTIATE_UNIX_FD\r\nBEGIN\r\n");
    CHECK(out == std::string("DATA\r\nOK ") + guid + "\r\nAGREE_UNIX_FD\r\n" && auth.done() && auth.uid() == 0);
  }
  {      // byte by byte; a mechanism list first; no descriptor passing on this transport
    AuthServer auth(A, 0, guid, false);
    const std::string in = std::string("\0", 1) + "AUTH\r\nAUTH EXTERNAL 30\r\nNEGOTIATE_UNIX_FD\r\nBEGIN\r\n";
    std::string out;
    for (char c : in) out += feed_all(auth, std::string(1, c));
    CHECK(out.rfind("REJECTED EXTERNAL\r\nOK ", 0) == 0 && out.find("ERROR") != std::string::npos && auth.done() && !auth.unix_fds());
  }
  {      // another user than the socket says: rejected; then the right one
    AuthServer auth(A, 1000, guid, true);
    const std::string out = feed_all(auth, std::string("\0", 1) + "AUTH EXTERNAL 30\r\nAUTH EXTERNAL 31303030\r\nBEGIN\r\n");
    CHECK(out.rfind("REJECTED EXTERNAL\r\nOK ", 0) == 0 && auth.done());
  }
  {
    AuthServer auth(A, 0, guid, true);
    feed_all(auth, "AUTH EXTERNAL 30\r\n");      // (no NUL byte first)
    CHECK(auth.failed());
  }
  {
    AuthServer auth(A, 0, guid, true);
    feed_all(auth, std::string("\0", 1) + "BEGIN\r\n");
    CHECK(auth.failed());
  }
  {
    AuthServer auth(A, 0, guid, true);
    feed_all(auth, std::string("\0", 1) + "AUTH EXTERNAL 30\n");      // (a bare LF)
    CHECK(auth.failed());
  }
  {
    AuthServer auth(A, 0, guid, true);
    const std::string out = feed_all(auth, std::string("\0", 1) + "FROB\r\nAUTH ANONYMOUS\r\nAUTH EXTERNAL zz\r\n");
    CHECK(out == "ERROR \"Unknown command\"\r\nREJECTED EXTERNAL\r\nREJECTED EXTERNAL\r\n" && !auth.failed() && !auth.done());
  }
  {
    AuthServer auth(A, 0, guid, true);
    feed_all(auth, std::string("\0", 1) + std::string(AuthServer::kMaxLine + 10, 'A'));
    CHECK(auth.failed());
  }
  {
    AuthServer auth(A, 0, guid, true);
    std::string many(1, '\0');
    for (int i = 0; i < 20; ++i) many += "AUTH FOO\r\n";
    feed_all(auth, many);
    CHECK(auth.failed());
  }
  // random bytes: never a crash
  std::mt19937 rng(5);
  for (int round = 0; round < 2000; ++round) {
    AuthServer auth(A, 0, guid, true);
    std::string junk(1, '\0');
    const int len = int(rng() % 80);
    for (int i = 0; i < len; ++i) junk += char("AUTHEXTERNAL BEGIN DATA30\r\n\x01\xff"[rng() % 29]);
    feed_all(auth, junk);
  }
}

// ---- match rules ---------------------------------------------------------------------------------------------------------------------------------

static void test_match_rules() {
  MatchRule r(A);
  CHECK(r.parse("type='signal',sender='org.example.S',interface='org.example.I',member='Changed',path_namespace='/org/example',arg0='hello',arg1path='/a/'"));
  CHECK(r.type == kSignal && r.sender == "org.example.S" && r.args.count() == 2 && r.args_wanted() == 2 && r.path_namespace == "/org/example");
  MatchRule same(A);
  CHECK(same.parse(" arg1path='/a/', arg0=hello,member=Changed,interface='org.example.I',path_namespace=/org/example,sender=org.example.S,type=signal"));
  CHECK(r.same_as(same));
  MatchRule other(A);
  CHECK(other.parse("type='signal',arg0='hello'") && !other.same_as(r));
  CHECK(MatchRule(A).parse(""));
  CHECK(MatchRule(A).parse("arg0='it'\\''s'"));
  {
    MatchRule q(A);
    CHECK(q.parse("arg0='it'\\''s'") && q.args[0].value == "it's");
    MatchRule q2(A);
    CHECK(q2.parse("arg0='a,b'") && q2.args[0].value == "a,b");
  }
  for (const char* bad : {"type='nope'", "type='signal',type='signal'", "path='/a',path_namespace='/b'", "arg64='x'", "arg01='x'", "interface='x'", "member='a.b'",
                          "sender='no'", "foo='bar'", "arg0='open", "eavesdrop='yes'", "path='a'", "arg0namespace='.x'", "arg0='a',arg0='b'", "=x"}) {
    if (MatchRule(A).parse(bad)) printf("  wrongly parsed: %s\n", bad);
    CHECK(!MatchRule(A).parse(bad));
  }
  // matching
  Message m(A);
  m.type = kSignal;
  m.sender.assign(":1.7");
  m.path.assign("/org/example/sub");
  m.interface.assign("org.example.I");
  m.member.assign("Changed");
  m.signature.assign("so");
  Writer w(m.body);
  w.string("hello");
  w.object_path("/a/b/c");
  StringArgs args;
  string_args(m.body.view(), m.signature.view(), false, 64, &args);
  auto owns = [](StringView name) { return name == "org.example.S"; };
  auto not_owns = [](StringView) { return false; };
  CHECK(r.matches(m, &args, owns));
  CHECK(!r.matches(m, &args, not_owns));
  MatchRule ns(A);
  CHECK(ns.parse("path_namespace='/org/example'") && ns.matches(m, nullptr, not_owns));
  MatchRule ns2(A);
  CHECK(ns2.parse("path_namespace='/org/ex'") && !ns2.matches(m, nullptr, not_owns));
  MatchRule all(A);
  CHECK(all.parse("path_namespace='/'") && all.matches(m, nullptr, not_owns));
  MatchRule a0ns(A);
  CHECK(a0ns.parse("arg0namespace='hel'") && !a0ns.matches(m, &args, not_owns));
  m.body.clear();
  Writer w2(m.body);
  w2.string("org.example.Foo");
  w2.object_path("/");
  string_args(m.body.view(), m.signature.view(), false, 64, &args);
  MatchRule a0ns2(A);
  CHECK(a0ns2.parse("arg0namespace='org.example'") && a0ns2.matches(m, &args, not_owns));
  MatchRule apath(A);
  CHECK(apath.parse("arg1path='/a/b/'") && apath.matches(m, &args, not_owns));      // (the argument "/" is a parent folder of the rule's)
  MatchRule apath2(A);
  CHECK(apath2.parse("arg0path='/x/'") && !apath2.matches(m, &args, not_owns));
  MatchRule a1(A);
  CHECK(a1.parse("arg1='/'") && !a1.matches(m, &args, not_owns));      // (argN matches strings only, not object paths)
  CHECK(arg_path_matches("/a/", "/a/b") && arg_path_matches("/a/b", "/a/") && !arg_path_matches("/a", "/a/b") && arg_path_matches("/x", "/x"));
  MatchRule unique(A);
  CHECK(unique.parse("sender=':1.7'") && unique.matches(m, nullptr, not_owns));
  MatchRule unique2(A);
  CHECK(unique2.parse("sender=':1.8'") && !unique2.matches(m, nullptr, owns));
}

// ---- the bus, driven as clients drive it ---------------------------------------------------------------------------------------------------------

// A descriptor of the test: counts how many are alive.
static std::atomic<int> g_fds_alive{0};
class TestFd final : public Fd {
 public:
  explicit TestFd(int tag) : tag_(tag) { ++g_fds_alive; }
  ~TestFd() { --g_fds_alive; }
  int tag() const { return tag_; }

 private:
  int tag_;
};
static int fd_tag(const FdRef& fd) { return static_cast<const TestFd&>(*fd).tag(); }

struct Client {
  Bus& bus;
  uint32_t id = 0;
  std::string unique;
  bool be;
  uint32_t serial = 1;
  std::string auth_out;
  bool authed = false;
  std::vector<uint8_t> rx;
  std::vector<std::pair<uint64_t, FdRef>> rx_fds;      // (offset in rx, descriptor)
  uint64_t rx_base = 0;
  std::vector<std::shared_ptr<Message>> got;
  bool closed = false;
  bool expect_negotiation = true;

  Client(Bus& b, bool fds = true, bool big_endian = false, uint32_t uid = 0, uint64_t pid = 4242) : bus(b), be(big_endian) {
    id = bus.open_client(pid, uid, true);
    std::string auth = std::string("\0", 1) + "AUTH EXTERNAL " + (uid == 1000 ? "31303030" : "30") + "\r\n" + (fds ? "NEGOTIATE_UNIX_FD\r\n" : "") + "BEGIN\r\n";
    bus.receive(id, ConstByteView(reinterpret_cast<const uint8_t*>(auth.data()), auth.size()), nullptr);
  }
  ~Client() {
    if (!closed) bus.close_client(id);
  }
  void close() {
    bus.close_client(id);
    closed = true;
  }
  void pump() {
    ByteBuffer out(A);
    List<Bus::OutFd> fds(A);
    if (bus.take_output(id, &out, &fds)) closed = true;
    const uint64_t base = rx_base + rx.size();
    for (const Bus::OutFd& f : fds.view()) rx_fds.push_back({base + f.at, f.fd});
    rx.insert(rx.end(), out.view().data(), out.view().data() + out.size());
    if (!authed) {
      for (;;) {
        auto at = std::find(rx.begin(), rx.end(), '\n');
        if (at == rx.end()) break;
        std::string line(rx.begin(), at + 1);
        rx.erase(rx.begin(), at + 1);
        rx_base += line.size();
        auth_out += line;
        const bool ok_seen = auth_out.find("OK ") != std::string::npos;
        if (ok_seen && (!expect_negotiation || line.rfind("AGREE", 0) == 0 || line.rfind("ERROR", 0) == 0)) {
          authed = true;
          break;
        }
      }
      if (!authed) return;
    }
    for (;;) {
      const int64_t size = framed_size(ConstByteView(rx.data(), rx.size()));
      if (size <= 0 || uint64_t(size) > rx.size()) break;
      auto m = std::make_shared<Message>(A);
      CHECK(decode(ConstByteView(rx.data(), uint64_t(size)), m.get()) == DecodeStatus::Ok);
      for (auto it = rx_fds.begin(); it != rx_fds.end();) {
        if (it->first == rx_base) { m->fds.push_back(it->second); it = rx_fds.erase(it); }
        else ++it;
      }
      got.push_back(m);
      rx.erase(rx.begin(), rx.begin() + size);
      rx_base += uint64_t(size);
    }
  }
  uint32_t send(uint8_t type, const char* dest, const char* path, const char* iface, const char* member, const char* sig, std::function<void(Writer&)> body = {},
                uint8_t flags = 0, std::vector<FdRef> fds = {}, uint32_t reply_serial = 0, const char* error_name = nullptr) {
    Message m(A);
    m.big_endian = be;
    m.type = type;
    m.flags = flags;
    m.serial = serial++;
    if (dest) m.destination.assign(dest);
    if (path) m.path.assign(path);
    if (iface) m.interface.assign(iface);
    if (member) m.member.assign(member);
    if (error_name) m.error_name.assign(error_name);
    if (reply_serial) { m.has_reply_serial = true; m.reply_serial = reply_serial; }
    m.signature.assign(sig ? sig : "");
    if (body) {
      Writer w(m.body, be);
      body(w);
    }
    m.unix_fds = uint32_t(fds.size());
    ByteBuffer wire(A);
    CHECK(encode(m, &wire));
    Queue<FdRef> q(A);
    for (FdRef& f : fds) q.enqueue(f);
    bus.receive(id, wire.view(), &q);
    return m.serial;
  }
  uint32_t call(const char* dest, const char* path, const char* iface, const char* member, const char* sig = "", std::function<void(Writer&)> body = {},
                uint8_t flags = 0, std::vector<FdRef> fds = {}) {
    return send(kMethodCall, dest, path, iface, member, sig, std::move(body), flags, std::move(fds));
  }
  uint32_t bus_call(const char* member, const char* sig = "", std::function<void(Writer&)> body = {}) {
    return call(kBusName, kBusPath, kBusName, member, sig, std::move(body));
  }
  std::shared_ptr<Message> reply(uint32_t to) {
    pump();
    for (auto& m : got)
      if ((m->type == kMethodReturn || m->type == kError) && m->reply_serial == to) return m;
    return nullptr;
  }
  std::shared_ptr<Message> take(uint8_t type, const char* member = nullptr) {
    pump();
    for (auto it = got.begin(); it != got.end(); ++it)
      if ((*it)->type == type && (!member || (*it)->member == member)) {
        auto m = *it;
        got.erase(it);
        return m;
      }
    return nullptr;
  }
  int count(uint8_t type, const char* member = nullptr) {
    pump();
    int n = 0;
    for (auto& m : got) n += m->type == type && (!member || m->member == member);
    return n;
  }
  void hello() {
    const uint32_t s = bus_call("Hello");
    auto r = reply(s);
    CHECK(r && r->type == kMethodReturn && r->signature == "s");
    if (!r) return;
    Reader rd(r->body.view(), r->big_endian);
    StringView name;
    rd.string(&name);
    unique = str(name);
  }
  uint32_t u_reply(uint32_t s) {
    auto r = reply(s);
    if (!r || r->type != kMethodReturn) return 0;
    Reader rd(r->body.view(), r->big_endian);
    uint32_t v = 0;
    rd.u32(&v);
    return v;
  }
  std::string s_reply(uint32_t s) {
    auto r = reply(s);
    if (!r || r->type != kMethodReturn) return "<error>";
    Reader rd(r->body.view(), r->big_endian);
    StringView v;
    rd.string(&v);
    return str(v);
  }
  std::string error_of(uint32_t s) {
    auto r = reply(s);
    return r && r->type == kError ? str(r->error_name.view()) : "";
  }
  uint32_t request(const char* name, uint32_t flags) {
    return u_reply(bus_call("RequestName", "su", [&](Writer& w) { w.string(name); w.u32(flags); }));
  }
  void add_match(const char* rule) {
    auto r = reply(bus_call("AddMatch", "s", [&](Writer& w) { w.string(rule); }));
    CHECK(r && r->type == kMethodReturn);
  }
  void clear() {
    pump();
    got.clear();
  }
};

static std::vector<std::string> strings_of(const Message& m) {
  std::vector<std::string> out;
  Reader r(m.body.view(), m.big_endian);
  uint64_t end = 0;
  if (!r.begin_array(4, &end)) return out;
  while (r.position() < end) {
    StringView s;
    if (!r.string(&s)) break;
    out.push_back(str(s));
  }
  return out;
}

static void test_bus_basics() {
  Bus bus(A, BusKind::Session, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  Client a(bus), b(bus, true, true);      // (b speaks big-endian)
  a.hello();
  b.hello();
  CHECK(a.unique == ":1.1" && b.unique == ":1.2");
  CHECK(a.count(kSignal, "NameAcquired") == 1);
  // a second Hello is an error
  CHECK(a.error_of(a.bus_call("Hello")) == "org.freedesktop.DBus.Error.Failed");
  // ListNames, GetId, NameHasOwner, GetNameOwner
  auto names = a.reply(a.bus_call("ListNames"));
  CHECK(names && strings_of(*names) == (std::vector<std::string>{"org.freedesktop.DBus", ":1.1", ":1.2"}));
  CHECK(a.s_reply(a.bus_call("GetId")) == "0123456789abcdef0123456789abcdef");
  CHECK(a.s_reply(a.call(kBusName, "/", "org.freedesktop.DBus.Peer", "GetMachineId")) == "fedcba9876543210fedcba9876543210");
  CHECK(a.reply(a.call(kBusName, "/", "org.freedesktop.DBus.Peer", "Ping"))->type == kMethodReturn);
  CHECK(a.s_reply(a.bus_call("GetNameOwner", "s", [](Writer& w) { w.string(":1.2"); })) == ":1.2");
  CHECK(a.error_of(a.bus_call("GetNameOwner", "s", [](Writer& w) { w.string("org.nobody.Here"); })) == "org.freedesktop.DBus.Error.NameHasNoOwner");
  CHECK(a.error_of(a.bus_call("GetNameOwner", "s", [](Writer& w) { w.string(":1.02"); })) == "org.freedesktop.DBus.Error.NameHasNoOwner");      // (not ":1.2")
  CHECK(a.error_of(a.call(":1.002", "/", "a.b", "M")) == "org.freedesktop.DBus.Error.NameHasNoOwner");
  CHECK(a.error_of(a.bus_call("GetNameOwner", "")) == "org.freedesktop.DBus.Error.InvalidArgs");
  CHECK(a.error_of(a.bus_call("NoSuchThing")) == "org.freedesktop.DBus.Error.UnknownMethod");
  CHECK(a.error_of(a.call(kBusName, "/", "org.example.Nope", "X")) == "org.freedesktop.DBus.Error.UnknownInterface");
  // a call with no interface finds the bus's method by name
  CHECK(a.s_reply(a.call(kBusName, "/", nullptr, "GetId")) == "0123456789abcdef0123456789abcdef");
  // credentials
  auto creds = a.reply(a.bus_call("GetConnectionCredentials", "s", [](Writer& w) { w.string(":1.2"); }));
  CHECK(creds && creds->type == kMethodReturn && creds->signature == "a{sv}" && creds->body.size() > 30);
  CHECK(a.u_reply(a.bus_call("GetConnectionUnixProcessID", "s", [](Writer& w) { w.string(":1.2"); })) == 4242);
  CHECK(a.reply(a.bus_call("GetConnectionUnixUser", "s", [](Writer& w) { w.string(":1.2"); }))->type == kMethodReturn);
  CHECK(a.error_of(a.bus_call("GetConnectionUnixProcessID", "s", [](Writer& w) { w.string(kBusName); })) == "org.freedesktop.DBus.Error.UnixProcessIdUnknown");
  CHECK(a.error_of(a.bus_call("GetAdtAuditSessionData", "s", [](Writer& w) { w.string(":1.2"); })) == "org.freedesktop.DBus.Error.AdtAuditDataUnknown");
  // properties and introspection
  auto features = a.reply(a.call(kBusName, kBusPath, "org.freedesktop.DBus.Properties", "Get", "ss", [](Writer& w) { w.string(kBusName); w.string("Features"); }));
  CHECK(features && features->type == kMethodReturn && features->signature == "v");
  auto all = a.reply(a.call(kBusName, kBusPath, "org.freedesktop.DBus.Properties", "GetAll", "s", [](Writer& w) { w.string(kBusName); }));
  CHECK(all && all->type == kMethodReturn && validate_body(all->body.view(), "a{sv}", false, 0));
  CHECK(a.error_of(a.call(kBusName, kBusPath, "org.freedesktop.DBus.Properties", "Set", "ssv", [](Writer& w) { w.string(kBusName); w.string("Features"); w.variant("u"); w.u32(1); })) ==
        "org.freedesktop.DBus.Error.PropertyReadOnly");
  const std::string xml = a.s_reply(a.call(kBusName, kBusPath, "org.freedesktop.DBus.Introspectable", "Introspect"));
  CHECK(xml.find("<method name=\"RequestName\">") != std::string::npos && xml.find("BecomeMonitor") != std::string::npos);
  CHECK(a.s_reply(a.call(kBusName, "/", "org.freedesktop.DBus.Introspectable", "Introspect")).find("<node name=\"org\"/>") != std::string::npos);
  // UpdateActivationEnvironment is fine on the session bus; StartServiceByName knows nothing to start
  CHECK(a.reply(a.bus_call("UpdateActivationEnvironment", "a{ss}", [](Writer& w) { Writer::ArrayMark m = w.begin_array(8); w.end_array(m); }))->type == kMethodReturn);
  CHECK(a.error_of(a.bus_call("StartServiceByName", "su", [](Writer& w) { w.string("org.nobody.X"); w.u32(0); })) == "org.freedesktop.DBus.Error.ServiceUnknown");
  CHECK(a.u_reply(a.bus_call("StartServiceByName", "su", [](Writer& w) { w.string(kBusName); w.u32(0); })) == kStartAlreadyRunning);
  // NO_REPLY_EXPECTED: no answer
  a.clear();
  a.call(kBusName, kBusPath, kBusName, "GetId", "", {}, kNoReplyExpected);
  CHECK(a.count(kMethodReturn) == 0);
}

static void test_names_and_queues() {
  Bus bus(A, BusKind::Session, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  Client watch(bus), a(bus), b(bus), c(bus);
  watch.hello();
  a.hello();
  b.hello();
  c.hello();
  watch.add_match("type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.example.N'");
  watch.clear();
  CHECK(a.request("org.example.N", 0) == kRequestPrimaryOwner);
  CHECK(a.count(kSignal, "NameAcquired") == 2);      // (its unique name, and this one)
  CHECK(watch.count(kSignal, "NameOwnerChanged") == 1);
  CHECK(a.request("org.example.N", 0) == kRequestAlreadyOwner);
  CHECK(b.request("org.example.N", 0) == kRequestInQueue);
  CHECK(c.request("org.example.N", kNameDoNotQueue) == kRequestExists);
  CHECK(c.request("org.example.N", kNameReplaceExisting) == kRequestInQueue);      // (a did not allow replacement)
  auto queue = a.reply(a.bus_call("ListQueuedOwners", "s", [](Writer& w) { w.string("org.example.N"); }));
  CHECK(queue && strings_of(*queue) == (std::vector<std::string>{a.unique, b.unique, c.unique}));
  // a now allows replacement; c replaces it, a goes back to second place
  CHECK(a.request("org.example.N", kNameAllowReplacement) == kRequestAlreadyOwner);
  a.clear();
  CHECK(c.request("org.example.N", kNameReplaceExisting) == kRequestPrimaryOwner);
  CHECK(a.count(kSignal, "NameLost") == 1);
  queue = a.reply(a.bus_call("ListQueuedOwners", "s", [](Writer& w) { w.string("org.example.N"); }));
  CHECK(queue && strings_of(*queue) == (std::vector<std::string>{c.unique, a.unique, b.unique}));
  CHECK(a.s_reply(a.bus_call("GetNameOwner", "s", [](Writer& w) { w.string("org.example.N"); })) == c.unique);
  // c releases: a is next
  auto rel = c.bus_call("ReleaseName", "s", [](Writer& w) { w.string("org.example.N"); });
  CHECK(c.u_reply(rel) == kReleaseReleased);
  CHECK(c.count(kSignal, "NameLost") == 1);
  CHECK(a.s_reply(a.bus_call("GetNameOwner", "s", [](Writer& w) { w.string("org.example.N"); })) == a.unique);
  CHECK(c.u_reply(c.bus_call("ReleaseName", "s", [](Writer& w) { w.string("org.example.N"); })) == kReleaseNotOwner);
  CHECK(c.u_reply(c.bus_call("ReleaseName", "s", [](Writer& w) { w.string("org.example.Gone"); })) == kReleaseNonExistent);
  // a goes away: b takes over; b goes away with DO_NOT_QUEUE replacement semantics
  watch.clear();
  a.close();
  CHECK(b.s_reply(b.bus_call("GetNameOwner", "s", [](Writer& w) { w.string("org.example.N"); })) == b.unique);
  CHECK(b.count(kSignal, "NameAcquired") >= 1);
  CHECK(watch.count(kSignal, "NameOwnerChanged") == 1);
  CHECK(b.request("org.example.N", kNameAllowReplacement | kNameDoNotQueue) == kRequestAlreadyOwner);
  CHECK(c.request("org.example.N", kNameReplaceExisting) == kRequestPrimaryOwner);
  queue = c.reply(c.bus_call("ListQueuedOwners", "s", [](Writer& w) { w.string("org.example.N"); }));
  CHECK(queue && strings_of(*queue) == (std::vector<std::string>{c.unique}));      // (b asked not to be queued)
  // names nobody may take
  CHECK(c.error_of(c.bus_call("RequestName", "su", [](Writer& w) { w.string(":1.99"); w.u32(0); })) == "org.freedesktop.DBus.Error.InvalidArgs");
  CHECK(c.error_of(c.bus_call("RequestName", "su", [](Writer& w) { w.string(kBusName); w.u32(0); })) == "org.freedesktop.DBus.Error.InvalidArgs");
  CHECK(c.error_of(c.bus_call("RequestName", "su", [](Writer& w) { w.string("bad"); w.u32(0); })) == "org.freedesktop.DBus.Error.InvalidArgs");
  // NameHasOwner follows
  auto has = c.reply(c.bus_call("NameHasOwner", "s", [](Writer& w) { w.string("org.example.N"); }));
  CHECK(has && has->body.size() == 4 && has->body.view()[0] == 1);
  c.close();
  has = b.reply(b.bus_call("NameHasOwner", "s", [](Writer& w) { w.string("org.example.N"); }));
  CHECK(has && has->body.size() == 4 && has->body.view()[0] == 0);
}

static void test_calls_and_signals() {
  Bus bus(A, BusKind::System, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  Client caller(bus), service(bus), listener(bus), spy(bus);
  caller.hello();
  service.hello();
  listener.hello();
  spy.hello();
  CHECK(service.request("org.example.Svc", 0) == kRequestPrimaryOwner);
  service.clear();
  // a call by the well-known name, answered by the service
  const uint32_t s = caller.call("org.example.Svc", "/obj", "org.example.I", "Echo", "s", [](Writer& w) { w.string("ping"); });
  auto in = service.take(kMethodCall, "Echo");
  CHECK(in && in->sender == StringView(caller.unique.c_str()) && in->destination == "org.example.Svc" && in->serial == s);
  service.send(kMethodReturn, caller.unique.c_str(), nullptr, nullptr, nullptr, "s", [](Writer& w) { w.string("pong"); }, 0, {}, s);
  CHECK(caller.s_reply(s) == "pong");
  // a second reply to the same call is not delivered (system bus: only requested replies), nor is one to a call never made
  caller.clear();
  service.send(kMethodReturn, caller.unique.c_str(), nullptr, nullptr, nullptr, "", {}, 0, {}, s);
  service.send(kMethodReturn, caller.unique.c_str(), nullptr, nullptr, nullptr, "", {}, 0, {}, 999);
  CHECK(caller.count(kMethodReturn) == 0);
  // an error reply
  const uint32_t s2 = caller.call("org.example.Svc", "/obj", "org.example.I", "Fail");
  service.take(kMethodCall, "Fail");
  service.send(kError, caller.unique.c_str(), nullptr, nullptr, nullptr, "s", [](Writer& w) { w.string("no"); }, 0, {}, s2, "org.example.Error.Nope");
  CHECK(caller.error_of(s2) == "org.example.Error.Nope");
  // nobody there
  CHECK(caller.error_of(caller.call("org.example.Nobody", "/", "a.b", "X")) == "org.freedesktop.DBus.Error.ServiceUnknown");
  CHECK(caller.error_of(caller.call(":1.99", "/", "a.b", "X")) == "org.freedesktop.DBus.Error.NameHasNoOwner");
  CHECK(caller.error_of(caller.call("org.example.Nobody", "/", "a.b", "X", "", {}, kNoAutoStart)) == "org.freedesktop.DBus.Error.NameHasNoOwner");
  // the callee goes away without answering: NoReply
  Client* flaky = new Client(bus);
  flaky->hello();
  const uint32_t s3 = caller.call(flaky->unique.c_str(), "/", "a.b", "Hang");
  delete flaky;
  CHECK(caller.error_of(s3) == "org.freedesktop.DBus.Error.NoReply");
  // signals: broadcast to match rules only
  listener.add_match("type='signal',interface='org.example.I',member='Tick'");
  listener.clear();
  spy.clear();
  service.send(kSignal, nullptr, "/obj", "org.example.I", "Tick", "u", [](Writer& w) { w.u32(1); });
  service.send(kSignal, nullptr, "/obj", "org.example.I", "Tock", "u", [](Writer& w) { w.u32(2); });
  CHECK(listener.count(kSignal, "Tick") == 1 && listener.count(kSignal, "Tock") == 0);
  CHECK(spy.count(kSignal) == 0);
  // the rule follows the well-known name's owner; arg0 rules; RemoveMatch
  listener.add_match("type='signal',sender='org.example.Svc',member='Named',arg0='yes'");
  listener.clear();
  service.send(kSignal, nullptr, "/obj", "org.example.I", "Named", "s", [](Writer& w) { w.string("yes"); });
  service.send(kSignal, nullptr, "/obj", "org.example.I", "Named", "s", [](Writer& w) { w.string("no"); });
  spy.send(kSignal, nullptr, "/obj", "org.example.I", "Named", "s", [](Writer& w) { w.string("yes"); });      // (spy does not own the name)
  CHECK(listener.count(kSignal, "Named") == 1);
  CHECK(listener.reply(listener.bus_call("RemoveMatch", "s", [](Writer& w) { w.string("type='signal',interface='org.example.I',member='Tick'"); }))->type == kMethodReturn);
  CHECK(listener.error_of(listener.bus_call("RemoveMatch", "s", [](Writer& w) { w.string("type='signal',interface='org.example.I',member='Tick'"); })) ==
        "org.freedesktop.DBus.Error.MatchRuleNotFound");
  CHECK(listener.error_of(listener.bus_call("AddMatch", "s", [](Writer& w) { w.string("type='bogus'"); })) == "org.freedesktop.DBus.Error.MatchRuleInvalid");
  listener.clear();
  service.send(kSignal, nullptr, "/obj", "org.example.I", "Tick", "u", [](Writer& w) { w.u32(1); });
  CHECK(listener.count(kSignal, "Tick") == 0);
  // a unicast signal reaches only its destination ... and an eavesdropper
  spy.add_match("eavesdrop='true',interface='org.example.I'");
  spy.clear();
  listener.clear();
  service.send(kSignal, caller.unique.c_str(), "/obj", "org.example.I", "ForYou", "");
  CHECK(caller.count(kSignal, "ForYou") == 1 && listener.count(kSignal, "ForYou") == 0 && spy.count(kSignal, "ForYou") == 1);
  // a match rule without eavesdrop does not see unicast traffic
  listener.add_match("interface='org.example.I'");
  listener.clear();
  service.send(kSignal, caller.unique.c_str(), "/obj", "org.example.I", "ForYou", "");
  CHECK(listener.count(kSignal, "ForYou") == 0);
  // the big-endian client's messages go through as they are
  Client be(bus, true, true);
  be.hello();
  CHECK(be.unique.size() > 3);
  const uint32_t s4 = be.call("org.example.Svc", "/obj", "org.example.I", "Echo", "s", [](Writer& w) { w.string("be"); });
  auto bein = service.take(kMethodCall, "Echo");
  CHECK(bein && bein->big_endian && bein->serial == s4);
  if (bein) {
    Reader r(bein->body.view(), true);
    StringView v;
    CHECK(r.string(&v) && v == "be");
  }
}

static void test_monitor_and_protocol_errors() {
  Bus bus(A, BusKind::Session, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  Client a(bus), b(bus), mon(bus);
  a.hello();
  b.hello();
  mon.hello();
  const uint32_t owed = a.call(mon.unique.c_str(), "/", "a.b", "WillNotAnswer");      // (the monitor-to-be owes a an answer: a gets NoReply)
  auto r = mon.reply(mon.call(kBusName, kBusPath, "org.freedesktop.DBus.Monitoring", "BecomeMonitor", "asu", [](Writer& w) {
    Writer::ArrayMark m = w.begin_array(4);
    w.end_array(m);
    w.u32(0);
  }));
  CHECK(r && r->type == kMethodReturn);
  CHECK(mon.count(kSignal, "NameLost") == 1);
  CHECK(a.error_of(owed) == "org.freedesktop.DBus.Error.NoReply");
  mon.clear();
  // it sees everything: a call, its reply, a signal
  const uint32_t s = a.call(b.unique.c_str(), "/", "a.b", "M");
  b.send(kMethodReturn, a.unique.c_str(), nullptr, nullptr, nullptr, "", {}, 0, {}, s);
  a.send(kSignal, nullptr, "/", "a.b", "Sig", "");
  CHECK(mon.count(kMethodCall, "M") == 1 && mon.count(kMethodReturn) >= 1 && mon.count(kSignal, "Sig") == 1);
  // its unique name is gone
  auto names = a.reply(a.bus_call("ListNames"));
  CHECK(names && strings_of(*names).size() == 3);
  // a monitor that sends is closed
  mon.bus_call("GetId");
  mon.pump();
  CHECK(mon.closed);
  // a monitor that does not read is closed once the bus holds kMaxOutgoingBytes for it; a listener that does not read just misses signals
  {
    Client slowmon(bus), deaf(bus);
    slowmon.hello();
    deaf.hello();
    deaf.add_match("type='signal'");
    slowmon.call(kBusName, kBusPath, "org.freedesktop.DBus.Monitoring", "BecomeMonitor", "asu", [](Writer& w) { Writer::ArrayMark m = w.begin_array(4); w.end_array(m); w.u32(0); });
    const std::string big(1 << 20, 'x');
    for (int i = 0; i < 70; ++i) a.send(kSignal, nullptr, "/", "a.b", "Big", "s", [&](Writer& w) { w.string(StringView(big.c_str())); });
    CHECK(bus.output_waiting(deaf.id) <= Bus::kMaxOutgoingBytes + (2u << 20));
    ByteBuffer out(A);
    List<Bus::OutFd> fds(A);
    CHECK(bus.take_output(slowmon.id, &out, &fds));      // (closing)
    a.clear();
    b.clear();
  }
  // a message before Hello closes the connection; so does garbage
  Client early(bus);
  early.call("org.example.X", "/", "a.b", "M");
  early.pump();
  CHECK(early.closed);
  Client garbage(bus);
  garbage.hello();
  const uint8_t junk[32] = {'l', 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
  bus.receive(garbage.id, ConstByteView(junk, 32), nullptr);
  garbage.pump();
  CHECK(garbage.closed);
  Client local(bus);
  local.hello();
  local.send(kSignal, nullptr, "/org/freedesktop/DBus/Local", "org.freedesktop.DBus.Local", "Disconnected", "");
  local.pump();
  CHECK(local.closed);
  // descriptors on a connection that did not negotiate them close it
  Client nofd(bus, false);
  nofd.expect_negotiation = false;
  nofd.hello();
  CHECK(!nofd.unique.empty());
  nofd.call(a.unique.c_str(), "/", "a.b", "M", "h", [](Writer& w) { w.u32(0); }, 0, {FdRef::make_as<TestFd>(A, 1)});
  nofd.pump();
  CHECK(nofd.closed);
  // a message split at every byte arrives whole
  Client slow(bus);
  slow.hello();
  Message m(A);
  m.type = kMethodCall;
  m.serial = 50;
  m.destination.assign(kBusName);
  m.path.assign(kBusPath);
  m.interface.assign(kBusName);
  m.member.assign("GetId");
  ByteBuffer wire(A);
  CHECK(encode(m, &wire));
  for (uint64_t i = 0; i < wire.size(); ++i) bus.receive(slow.id, wire.view().subview(i, 1), nullptr);
  CHECK(slow.s_reply(50) == "0123456789abcdef0123456789abcdef");
}

static void test_descriptors() {
  {
    Bus bus(A, BusKind::Session, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
    Client a(bus), b(bus), c(bus, false);
    c.expect_negotiation = false;
    a.hello();
    b.hello();
    c.hello();
    CHECK(g_fds_alive == 0);
    const uint32_t s = a.call(b.unique.c_str(), "/", "a.b", "Take", "hh", [](Writer& w) { w.u32(0); w.u32(1); }, 0,
                              {FdRef::make_as<TestFd>(A, 11), FdRef::make_as<TestFd>(A, 22)});
    auto in = b.take(kMethodCall, "Take");
    CHECK(in && in->fds.count() == 2 && in->unix_fds == 2 && fd_tag(in->fds[0]) == 11 && fd_tag(in->fds[1]) == 22);
    // to one that cannot take them: NotSupported
    const uint32_t s2 = a.call(c.unique.c_str(), "/", "a.b", "Take", "h", [](Writer& w) { w.u32(0); }, 0, {FdRef::make_as<TestFd>(A, 33)});
    CHECK(a.error_of(s2) == "org.freedesktop.DBus.Error.NotSupported");
    // UNIX_FDS more than were passed: closed
    Client d(bus);
    d.hello();
    {
      Message m(A);
      m.type = kMethodCall;
      m.serial = 9;
      m.destination.assign(b.unique.c_str());
      m.path.assign("/");
      m.member.assign("M");
      m.unix_fds = 1;
      ByteBuffer wire(A);
      CHECK(encode(m, &wire));
      bus.receive(d.id, wire.view(), nullptr);
      d.pump();
      CHECK(d.closed);
    }
    (void)s;
    in.reset();
    b.got.clear();
  }
  CHECK(g_fds_alive == 0);      // (every descriptor let go with its messages and connections)
}

// The Notifications service with a display that records.
struct FakeDisplay final : INotificationDisplay {
  std::vector<std::string> shown;
  std::vector<uint32_t> closed;
  uint32_t n = 0;
  uint32_t show(StringView app, StringView summary, StringView body, uint32_t timeout) override {
    shown.push_back(str(app) + "|" + str(summary) + "|" + str(body) + "|" + std::to_string(timeout));
    return ++n;
  }
  void close(uint32_t toast) override { closed.push_back(toast); }
};

static uint32_t notify(Client& c, const char* summary, uint32_t replaces, int32_t timeout) {
  return c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify", "susssasa{sv}i", [&](Writer& w) {
    w.string("app");
    w.u32(replaces);
    w.string("");
    w.string(summary);
    w.string("body text");
    Writer::ArrayMark actions = w.begin_array(4);
    w.string("default");
    w.string("Open");
    w.end_array(actions);
    Writer::ArrayMark hints = w.begin_array(8);
    w.begin_struct();
    w.string("urgency");
    w.variant("y");
    w.u8(1);
    w.end_array(hints);
    w.u32(uint32_t(timeout));
  });
}

static void test_notifications() {
  Bus bus(A, BusKind::Session, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  FakeDisplay display;
  NotificationService service(bus, display, A);
  CHECK(service.start());
  Client c(bus);
  c.hello();
  c.add_match("type='signal',interface='org.freedesktop.Notifications'");
  bus.tick(1000);
  const uint32_t id = c.u_reply(notify(c, "First", 0, -1));
  CHECK(id == 1 && display.shown.size() == 1 && display.shown[0] == "app|First|body text|8000");
  const uint32_t id2 = c.u_reply(notify(c, "Second", 0, 0));
  CHECK(id2 == 2 && service.showing() == 2);
  // replace the first
  CHECK(c.u_reply(notify(c, "First again", id, 500)) == id && display.closed.size() == 1 && service.showing() == 2);
  // it expires (500 ms); the second never does
  bus.tick(1000 + 400000);
  CHECK(c.count(kSignal, "NotificationClosed") == 0);
  bus.tick(1000 + 600000);
  auto closed = c.take(kSignal, "NotificationClosed");
  CHECK(closed && closed->signature == "uu" && service.showing() == 1);
  if (closed) {
    Reader r(closed->body.view(), false);
    uint32_t cid = 0, reason = 0;
    CHECK(r.u32(&cid) && r.u32(&reason) && cid == id && reason == 1);
  }
  // CloseNotification
  CHECK(c.reply(c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "CloseNotification", "u",
                       [&](Writer& w) { w.u32(id2); }))->type == kMethodReturn);
  CHECK(service.showing() == 0 && c.count(kSignal, "NotificationClosed") == 1);
  // the others
  auto caps = c.reply(c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "GetCapabilities"));
  CHECK(caps && strings_of(*caps) == (std::vector<std::string>{"body"}));
  auto info = c.reply(c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "GetServerInformation"));
  CHECK(info && info->signature == "ssss");
  CHECK(c.s_reply(c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.DBus.Introspectable", "Introspect")).find("Notify") !=
        std::string::npos);
  CHECK(c.error_of(c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Nope")) ==
        "org.freedesktop.DBus.Error.UnknownMethod");
  CHECK(c.error_of(c.call("org.freedesktop.Notifications", "/elsewhere", "org.freedesktop.Notifications", "GetCapabilities")) ==
        "org.freedesktop.DBus.Error.UnknownObject");
  // the service's credentials: the kernel (uid 0, no process)
  auto creds = c.reply(c.bus_call("GetConnectionUnixUser", "s", [](Writer& w) { w.string("org.freedesktop.Notifications"); }));
  CHECK(creds && creds->type == kMethodReturn);
  // text too long is cut at a character
  std::string long_summary(5000, 'x');
  long_summary[4095] = '\xC3';
  long_summary[4096] = '\xA9';
  display.shown.clear();
  c.u_reply(notify(c, long_summary.c_str(), 0, 100));
  CHECK(display.shown.size() == 1 && display.shown[0].size() == 4 + 4095 + 1 + 9 + 1 + 3);
}

// ---- the AF_UNIX transport, several threads ------------------------------------------------------------------------------------------------------

struct SystemSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

// A client on the system's sockets: blocking sends and receives done the way the Linux layer does them (under the lock, waiting on it).
struct SocketClient {
  SystemSync& sync;
  process::UnixSocketManager& sockets;
  uint32_t sock = 0;
  std::vector<uint8_t> rx;
  std::vector<process::UnixInFlight> rights;
  uint32_t serial = 1;
  std::string unique;

  SocketClient(SystemSync& s, process::UnixSocketManager& m) : sync(s), sockets(m) {}
  ~SocketClient() {
    for (auto& e : rights)
      if (e.token) sockets.release(uint32_t(e.token));
    if (sock) sockets.release(sock);
  }
  bool connect(const char* path, bool abstract) {
    if (sockets.create(process::UnixType::Stream, 77, &sock) != process::UnixStatus::Ok) return false;
    sync.lock();
    const auto st = sockets.connect_locked(sock, process::UnixName::from(abstract ? process::UnixName::Kind::Abstract : process::UnixName::Kind::Path, StringView(path).bytes()),
                                           abstract ? StringView() : StringView("/Linux/run/user/0/bus"), 77);
    sync.unlock();
    return st == process::UnixStatus::Ok;
  }
  bool send(ConstByteView bytes, Queue<process::UnixInFlight>* fds = nullptr) {
    uint64_t done = 0;
    sync.lock();
    while (done < bytes.size()) {
      uint32_t sent = 0;
      const auto st = sockets.send_locked(sock, bytes.subview(done), nullptr, 77, done == 0 ? fds : nullptr, &sent);
      if (st == process::UnixStatus::WouldBlock) { sync.wait(10000000); continue; }
      if (st != process::UnixStatus::Ok) { sync.unlock(); return false; }
      done += sent;
    }
    sync.unlock();
    return true;
  }
  // Waits for a whole message (up to `timeout_ms`).
  std::shared_ptr<Message> next(int timeout_ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      const int64_t size = framed_size(ConstByteView(rx.data(), rx.size()));
      if (size > 0 && uint64_t(size) <= rx.size()) {
        auto m = std::make_shared<Message>(A);
        if (decode(ConstByteView(rx.data(), uint64_t(size)), m.get()) != DecodeStatus::Ok) return nullptr;
        rx.erase(rx.begin(), rx.begin() + size);
        return m;
      }
      if (size < 0 || std::chrono::steady_clock::now() > until) return nullptr;
      uint8_t buf[4096];
      process::UnixSocketManager::Received got;
      Queue<process::UnixInFlight> in(A);
      sync.lock();
      auto st = sockets.receive_locked(sock, ByteView(buf, sizeof buf), false, &got, &in);
      if (st == process::UnixStatus::WouldBlock) { sync.wait(5000000); st = sockets.receive_locked(sock, ByteView(buf, sizeof buf), false, &got, &in); }
      sync.unlock();
      process::UnixInFlight e;
      while (in.dequeue(&e)) rights.push_back(e);
      if (st == process::UnixStatus::Ok) {
        if (got.end_of_file) return nullptr;
        rx.insert(rx.end(), buf, buf + got.got);
      }
    }
  }
  bool line(std::string* out) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      auto at = std::find(rx.begin(), rx.end(), '\n');
      if (at != rx.end()) {
        out->assign(rx.begin(), at + 1);
        rx.erase(rx.begin(), at + 1);
        return true;
      }
      if (std::chrono::steady_clock::now() > until) return false;
      uint8_t buf[256];
      process::UnixSocketManager::Received got;
      sync.lock();
      auto st = sockets.receive_locked(sock, ByteView(buf, sizeof buf), false, &got, nullptr);
      if (st == process::UnixStatus::WouldBlock) sync.wait(5000000);
      sync.unlock();
      if (st == process::UnixStatus::Ok) rx.insert(rx.end(), buf, buf + got.got);
    }
  }
  bool hello() {
    const std::string auth = std::string("\0", 1) + "AUTH EXTERNAL 30\r\nNEGOTIATE_UNIX_FD\r\nBEGIN\r\n";
    if (!send(ConstByteView(reinterpret_cast<const uint8_t*>(auth.data()), auth.size()))) return false;
    std::string l1, l2;
    if (!line(&l1) || l1.rfind("OK ", 0) != 0 || !line(&l2) || l2 != "AGREE_UNIX_FD\r\n") return false;
    const uint32_t s = call(kBusName, kBusPath, kBusName, "Hello", "", {});
    for (int i = 0; i < 4; ++i) {
      auto m = next();
      if (!m) return false;
      if (m->type == kMethodReturn && m->reply_serial == s) {
        Reader r(m->body.view(), m->big_endian);
        StringView name;
        r.string(&name);
        unique = str(name);
        return true;
      }
    }
    return false;
  }
  uint32_t call(const char* dest, const char* path, const char* iface, const char* member, const char* sig, std::function<void(Writer&)> body,
                Queue<process::UnixInFlight>* fds = nullptr, uint32_t nfds = 0, uint8_t type = kMethodCall, uint32_t reply_serial = 0) {
    Message m(A);
    m.type = type;
    m.serial = serial++;
    if (dest) m.destination.assign(dest);
    if (path) m.path.assign(path);
    if (iface) m.interface.assign(iface);
    if (member) m.member.assign(member);
    if (reply_serial) { m.has_reply_serial = true; m.reply_serial = reply_serial; }
    m.signature.assign(sig);
    if (body) { Writer w(m.body); body(w); }
    m.unix_fds = nfds;
    ByteBuffer wire(A);
    encode(m, &wire);
    return send(wire.view(), fds) ? m.serial : 0;
  }
};

static void test_transport_threads() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  filesystem::RamFileSystem fs(A);
  CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
  Bus system(A, BusKind::System, "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210");
  Bus session(A, BusKind::Session, "abcdef0123456789abcdef0123456789", "fedcba9876543210fedcba9876543210");
  FakeDisplay display;
  NotificationService notes(session, display, A);
  CHECK(notes.start());
  {
    SocketServer server(sockets, A);
    CHECK(listen_standard_sockets(server, system, session, &fs, A));
    UniquePointer<filesystem::IFile> file;
    CHECK(fs.open_file("/Linux/run/dbus/system_bus_socket", filesystem::OpenMode::Read, &file) == filesystem::Status::Ok);
    file.reset();
    CHECK(fs.open_file("/Linux/run/user/0/bus", filesystem::OpenMode::Read, &file) == filesystem::Status::Ok);
    file.reset();
    std::atomic<bool> stop{false};
    std::thread pumper([&] {
      while (!stop) {
        for (int i = 0; i < 16 && server.pump(hardware::uptime_us()); ++i) {}
        server.wait(2000000);
      }
    });
    // an echo service on the session bus (by path), descriptors passed to it
    SocketClient service(sync, sockets);
    CHECK(service.connect("/run/user/0/bus", false));
    CHECK(service.hello());
    const uint32_t rs = service.call(kBusName, kBusPath, kBusName, "RequestName", "su", [](Writer& w) { w.string("org.example.Echo"); w.u32(4); });
    bool owned = false;
    for (int i = 0; i < 4 && !owned; ++i) {
      auto m = service.next();
      owned = m && m->type == kMethodReturn && m->reply_serial == rs;
    }
    CHECK(owned);
    std::atomic<int> served{0};
    std::atomic<bool> service_stop{false};
    std::thread echo([&] {
      while (!service_stop) {
        auto m = service.next(200);
        if (!m || m->type != kMethodCall) continue;
        ++served;
        service.call(str(m->sender.view()).c_str(), nullptr, nullptr, nullptr, "s", [&](Writer& w) { w.string(m->sender.view()); }, nullptr, 0, kMethodReturn, m->serial);
      }
    });
    constexpr int kClients = 8, kCalls = 60;
    std::atomic<int> replies{0}, bus_replies{0}, signals{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kClients; ++t)
      threads.emplace_back([&, t] {
        SocketClient c(sync, sockets);
        const bool system_bus = t % 4 == 3;      // (some on the system bus, by its abstract name: they ping the bus only)
        if (!c.connect(system_bus ? "/run/dbus/system_bus_socket" : "/run/user/0/bus", system_bus) || !c.hello()) return;
        const uint32_t m = c.call(kBusName, kBusPath, kBusName, "AddMatch", "s", [](Writer& w) { w.string("type='signal',interface='org.example.Fan'"); });
        (void)m;
        for (int i = 0; i < kCalls; ++i) {
          const uint32_t s = system_bus ? c.call(kBusName, "/", "org.freedesktop.DBus.Peer", "Ping", "", {})
                                        : c.call("org.example.Echo", "/", "org.example.Echo", "Echo", "", {});
          for (int k = 0; k < 8; ++k) {
            auto r = c.next();
            if (!r) break;
            if (r->type == kSignal && r->member == "Fan") ++signals;
            if (r->type == kMethodReturn && r->reply_serial == s) {
              if (system_bus) ++bus_replies;
              else if (r->signature == "s") {
                Reader rd(r->body.view(), r->big_endian);
                StringView who;
                if (rd.string(&who) && str(who) == c.unique) ++replies;
              }
              break;
            }
          }
        }
        if (t == 0) c.call(nullptr, "/fan", "org.example.Fan", "Fan", "", {}, nullptr, 0, kSignal);
      });
    for (auto& th : threads) th.join();
    CHECK(replies == (kClients - kClients / 4) * kCalls);
    CHECK(bus_replies == (kClients / 4) * kCalls);
    CHECK(served == (kClients - kClients / 4) * kCalls);
    // a descriptor (an eventfd) passed through the sockets to the service and back with the reply's data
    {
      SocketClient c(sync, sockets);
      CHECK(c.connect("/run/user/0/bus", true) && c.hello());
      uint32_t ev = 0;
      CHECK(sockets.create_event(77, 5, false, &ev) == process::UnixStatus::Ok);
      Queue<process::UnixInFlight> fds(A);
      process::UnixInFlight e;
      e.kind = process::DescriptorKind::UnixSocket;
      e.token = ev;
      fds.enqueue(e);      // (the message takes this use)
      service_stop = true;
      echo.join();
      service.rights.clear();
      const uint32_t s = c.call("org.example.Echo", "/", "org.example.Echo", "TakeFd", "h", [](Writer& w) { w.u32(0); }, &fds, 1);
      CHECK(s != 0);
      std::shared_ptr<Message> in;
      for (int i = 0; i < 4 && !(in && in->member == "TakeFd"); ++i) in = service.next();
      CHECK(in && in->unix_fds == 1 && service.rights.size() == 1);
      if (!service.rights.empty()) {
        CHECK(service.rights[0].kind == process::DescriptorKind::UnixSocket && service.rights[0].token == ev);      // (the same eventfd)
        uint64_t value = 0;
        sync.lock();
        CHECK(sockets.event_take_locked(uint32_t(service.rights[0].token), &value) == process::UnixStatus::Ok && value == 5);
        sync.unlock();
      }
      // Notify through the sockets
      const uint32_t n = c.call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify", "susssasa{sv}i", [](Writer& w) {
        w.string("t"); w.u32(0); w.string(""); w.string("over the socket"); w.string("b");
        Writer::ArrayMark a1 = w.begin_array(4); w.end_array(a1);
        Writer::ArrayMark a2 = w.begin_array(8); w.end_array(a2);
        w.u32(uint32_t(-1));
      });
      bool replied = false;
      for (int i = 0; i < 6 && !replied; ++i) {
        auto m = c.next();
        replied = m && m->type == kMethodReturn && m->reply_serial == n;
      }
      CHECK(replied && !display.shown.empty() && display.shown.back() == "t|over the socket|b|8000");
    }
    stop = true;
    pumper.join();
    for (int i = 0; i < 20; ++i) server.pump(hardware::uptime_us());
    CHECK(session.connections() == 2);      // (the service's client and the Notifications service)
  }
  CHECK(session.connections() == 1 && system.connections() == 0);
}

// Connections that come and go while others talk, every one of them from a thread of its own.
static void test_transport_churn() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Bus session(A, BusKind::Session, "abcdef0123456789abcdef0123456789", "fedcba9876543210fedcba9876543210");
  SocketServer server(sockets, A);
  CHECK(server.listen(session, "/run/user/0/bus", StringView(), nullptr));
  std::atomic<bool> stop{false};
  std::thread pumper([&] {
    while (!stop) {
      for (int i = 0; i < 16 && server.pump(hardware::uptime_us()); ++i) {}
      server.wait(2000000);
    }
  });
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t)
    threads.emplace_back([&] {
      for (int round = 0; round < 15; ++round) {
        SocketClient c(sync, sockets);
        if (!c.connect("/run/user/0/bus", true) || !c.hello()) continue;
        const uint32_t s = c.call(kBusName, kBusPath, kBusName, "RequestName", "su", [](Writer& w) { w.string("org.example.Churn"); w.u32(0); });
        for (int k = 0; k < 6; ++k) {
          auto m = c.next();
          if (m && m->type == kMethodReturn && m->reply_serial == s) { ++ok; break; }
        }
        if (round % 3 == 0) {      // half a message, then gone
          const uint8_t half[8] = {'l', 1, 0, 1, 0, 0, 0, 0};
          c.send(ConstByteView(half, 8));
        }
      }
    });
  for (auto& th : threads) th.join();
  stop = true;
  pumper.join();
  for (int i = 0; i < 20; ++i) server.pump(hardware::uptime_us());
  CHECK(ok == 6 * 15);
  CHECK(server.connections() == 0 && session.connections() == 0 && session.owner_of("org.example.Churn") == 0);
}

// A client that reads as fast as signals come: the transport's buffer for it must not keep what went.
static void test_transport_stream() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  Bus session(A, BusKind::Session, "abcdef0123456789abcdef0123456789", "fedcba9876543210fedcba9876543210");
  SocketServer server(sockets, A);
  CHECK(server.listen(session, "/run/user/0/bus", StringView(), nullptr));
  std::atomic<bool> stop{false};
  std::thread pumper([&] {
    while (!stop) {
      for (int i = 0; i < 16 && server.pump(hardware::uptime_us()); ++i) {}
      server.wait(1000000);
    }
  });
  SocketClient listener(sync, sockets), sender(sync, sockets);
  CHECK(listener.connect("/run/user/0/bus", true) && listener.hello() && sender.connect("/run/user/0/bus", true) && sender.hello());
  const uint32_t m = listener.call(kBusName, kBusPath, kBusName, "AddMatch", "s", [](Writer& w) { w.string("type='signal',member='Tick'"); });
  bool matched = false;
  for (int i = 0; i < 4 && !matched; ++i) { auto r = listener.next(); matched = r && r->reply_serial == m; }
  const std::string text(4000, 'y');
  int got = 0;
  std::thread reader([&] { for (int i = 0; i < 2000; ++i) { auto r = listener.next(); if (!r) break; got += r->member == "Tick"; } });
  for (int i = 0; i < 2000; ++i) sender.call(nullptr, "/t", "org.example.T", "Tick", "s", [&](Writer& w) { w.string(StringView(text.c_str())); }, nullptr, 0, kSignal);
  reader.join();
  stop = true;
  pumper.join();
  CHECK(matched && got == 2000);
}

int main() {
  test_names();
  test_signatures();
  test_marshalling();
  test_messages();
  test_auth();
  test_match_rules();
  test_bus_basics();
  test_names_and_queues();
  test_calls_and_signals();
  test_monitor_and_protocol_errors();
  test_descriptors();
  test_notifications();
  test_transport_threads();
  test_transport_churn();
  test_transport_stream();
  printf("%d checks, %d failed\n", g_checks.load(), g_failed.load());
  return g_failed ? 1 : 0;
}
