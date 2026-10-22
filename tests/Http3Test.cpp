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

// Host-side tests for the HTTP/3 pieces that need no connection: QPACK (field sections, integers, Huffman coding) against the vectors of RFC 7541 and RFC
// 9204, and the Alt-Svc cache. The exchange itself is covered by tests/http3_live.sh (a real HTTP/3 server) and the QUIC pieces by QuicTest.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "Network/Http/AltSvc.hpp"
#include "Network/Quic/Qpack.hpp"

using namespace network;
using namespace network::h3;

static int g_failed = 0;
static long g_live = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { ++g_live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --g_live; free(p); } }
};

static std::string hex(ConstByteView v) {
  std::string out;
  char b[4];
  for (uint64_t i = 0; i < v.size(); ++i) {
    snprintf(b, sizeof b, "%02x", v[i]);
    out += b;
  }
  return out;
}
static std::vector<uint8_t> unhex(const char* s) {
  std::vector<uint8_t> out;
  for (; s[0] && s[1]; s += 2) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    out.push_back(static_cast<uint8_t>(nib(s[0]) * 16 + nib(s[1])));
  }
  return out;
}
static ConstByteView view(const std::vector<uint8_t>& v) { return ConstByteView(v.data(), v.size()); }

static bool huffman_is(const char* plain, const char* coded) {
  TestAllocator a;
  ByteBuffer out(a);
  if (!huffman_encode(plain, &out) || hex(out.view()) != coded || huffman_encoded_size(plain) != out.size()) return false;
  String back(a);
  const std::vector<uint8_t> raw = unhex(coded);
  return huffman_decode(view(raw), &back, 1000) && back.view().equals(plain);
}

static void test_huffman() {
  // RFC 7541 appendix C.4 and C.6
  CHECK(huffman_is("www.example.com", "f1e3c2e5f23a6ba0ab90f4ff"));
  CHECK(huffman_is("no-cache", "a8eb10649cbf"));
  CHECK(huffman_is("custom-key", "25a849e95ba97d7f"));
  CHECK(huffman_is("custom-value", "25a849e95bb8e8b4bf"));
  CHECK(huffman_is("302", "6402"));
  CHECK(huffman_is("private", "aec3771a4b"));
  CHECK(huffman_is("Mon, 21 Oct 2013 20:13:21 GMT", "d07abe941054d444a8200595040b8166e082a62d1bff"));
  CHECK(huffman_is("https://www.example.com", "9d29ad171863c78f0b97c8e9ae82ae43d3"));
  CHECK(huffman_is("", ""));
  TestAllocator a;
  // every byte value survives a round trip
  std::string all;
  for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
  {
    ByteBuffer out(a);
    CHECK(huffman_encode(StringView(all.data(), all.size()), &out));
    String back(a);
    CHECK(huffman_decode(out.view(), &back, 1000) && back.size() == 256 && back.view().equals(StringView(all.data(), all.size())));
    String small(a);
    CHECK(!huffman_decode(out.view(), &small, 10));   // over the limit
  }
  // bad data: padding that is not ones, padding of a whole byte or more, the EOS code inside
  String back(a);
  {
    const auto bad_padding = unhex("f1e3c2e5f23a6ba0ab90f4fe");
    CHECK(!huffman_decode(view(bad_padding), &back, 100));
    back.clear();
    const auto long_padding = unhex("f1e3c2e5f23a6ba0ab90f4ffff");
    CHECK(!huffman_decode(view(long_padding), &back, 100));
    back.clear();
    const auto eos = unhex("fffffffc");   // 30 one bits are EOS
    CHECK(!huffman_decode(view(eos), &back, 100));
  }
}

static std::string encoded(const HeaderList& fields) {
  TestAllocator a;
  ByteBuffer out(a);
  if (!qpack_encode(fields, &out)) return "";
  return hex(out.view());
}

static void test_qpack_encode() {
  TestAllocator a;
  {
    HeaderList f(a);
    f.add(":method", "GET");
    CHECK(encoded(f) == "0000d1");   // the static table entry 17
  }
  {
    HeaderList f(a);
    f.add(":path", "/");
    f.add(":scheme", "https");
    f.add(":status", "200");
    CHECK(encoded(f) == "0000c1d7d9");
  }
  {
    HeaderList f(a);
    f.add(":authority", "example.com");   // name from the table (0), value as is
    CHECK(encoded(f) == "000050" "0b" "6578616d706c652e636f6d");
  }
  {
    HeaderList f(a);
    f.add("x-a", "b");   // neither in the table
    CHECK(encoded(f) == "000023" "782d61" "01" "62");
  }
  {
    HeaderList f(a);
    f.add("accept-encoding", "identity");   // the name is entry 31: more than a 4 bit prefix holds
    CHECK(encoded(f) == "00005f10" "08" "6964656e74697479");
  }
  CHECK(g_live == 0);
}

static QpackResult decode(const char* hexed, HeaderList* out, uint64_t max = 100000) {
  const std::vector<uint8_t> raw = unhex(hexed);
  return qpack_decode(view(raw), out, max);
}

static void test_qpack_decode() {
  TestAllocator a;
  {
    HeaderList f(a);
    CHECK(decode("0000d1c1d9", &f) == QpackResult::Ok && f.count() == 3);
    CHECK(f.name(0).equals(":method") && f.value(0).equals("GET") && f.name(1).equals(":path") && f.value(1).equals("/") && f.name(2).equals(":status") && f.value(2).equals("200"));
  }
  {
    HeaderList f(a);   // a value in Huffman coding
    CHECK(decode("000050" "8c" "f1e3c2e5f23a6ba0ab90f4ff", &f) == QpackResult::Ok && f.count() == 1 && f.name(0).equals(":authority") && f.value(0).equals("www.example.com"));
  }
  {
    HeaderList f(a);   // a literal name in Huffman coding: 001 N=0 H=1 length 8
    CHECK(decode("00002e" "a8eb10649cbf" "01" "31", &f) == QpackResult::Ok && f.count() == 0 + 1 && f.name(0).equals("no-cache") && f.value(0).equals("1"));
  }
  {
    HeaderList f(a);   // the "never index" bit changes nothing here
    CHECK(decode("000070" "01" "61", &f) == QpackResult::Ok && f.name(0).equals(":authority") && f.value(0).equals("a"));
  }
  {
    HeaderList f(a);   // an empty section
    CHECK(decode("0000", &f) == QpackResult::Ok && f.count() == 0);
  }
  // the dynamic table is never used (capacity 0): any reference to it, and any non-zero Required Insert Count, is refused
  const char* dynamic[] = {"0100", "0000" "80", "0000" "40" "00", "0000" "10", "0000" "01" "01" "61", "0000" "c0" "10"};
  for (const char* d : dynamic) {
    HeaderList f(a);
    const QpackResult r = decode(d, &f);
    CHECK(r == QpackResult::DynamicTable || (std::string(d) == "0000c010" && r != QpackResult::Ok));
  }
  // damaged sections
  const char* bad[] = {"00", "0000" "50" "0b" "6578", "0000" "ff", "0000" "50" "ff", "0000" "ff" "24" /* index 99: past the table */, "0000" "23" "782d" /* name cut short */, "0000" "4f" "ff" "ff" "ff" "ff" "ff" "ff" "ff" "ff" "ff" "ff" "7f"};
  for (const char* b : bad) {
    HeaderList f(a);
    CHECK(decode(b, &f) == QpackResult::Malformed);
  }
  {
    HeaderList f(a);
    CHECK(decode("0000d1" "000050" , &f) != QpackResult::Ok);   // (a later byte that is not a field line)
  }
  {
    HeaderList f(a);   // limits: names, values and 32 bytes of overhead per field count
    CHECK(decode("0000d1d1d1d1", &f, 100) == QpackResult::TooLarge);
    HeaderList g(a);
    CHECK(decode("0000d1d1", &g, 2 * (7 + 3 + 32)) == QpackResult::Ok && g.count() == 2);
  }
  CHECK(g_live == 0);
}

static void test_qpack_roundtrip() {
  TestAllocator a;
  {
  HeaderList in(a);
  const std::string long_value(3000, 'v');
  in.add(":method", "POST");
  in.add(":scheme", "https");
  in.add(":authority", "example.com:8443");
  in.add(":path", "/a/b?c=d&e=f");
  in.add("user-agent", "TheoryCraftOS");
  in.add("accept", "*/*");
  in.add("accept-encoding", "gzip, deflate");
  in.add("content-type", "application/x-www-form-urlencoded");
  in.add("content-length", "12345");
  in.add("x-empty", "");
  in.add("x-long", StringView(long_value.data(), long_value.size()));
  in.add("cookie", "a=1; b=2");
  in.add("x-bytes", "\x01\x7f\xff");
  ByteBuffer wire(a);
  CHECK(qpack_encode(in, &wire));
  HeaderList out(a);
  CHECK(qpack_decode(wire.view(), &out, 100000) == QpackResult::Ok && out.count() == in.count());
  for (uint64_t i = 0; i < in.count() && i < out.count(); ++i) CHECK(in.name(i).equals(out.name(i)) && in.value(i).equals(out.value(i)));
  CHECK(out.find("content-length").equals("12345") && out.find("nope").empty());
  // every cut of the wire form is refused or yields fewer fields, never a crash
  for (uint64_t n = 0; n < wire.size(); n += 7) {
    HeaderList partial(a);
    qpack_decode(wire.view().first(n), &partial, 100000);
  }
  }
  CHECK(g_live == 0);
}

static void test_alt_svc() {
  TestAllocator a;
  AltSvcCache cache(a);
  uint16_t port = 0;
  CHECK(!cache.find("example.com", 443, 0, &port));
  cache.learn("example.com", 443, "h3=\":443\"; ma=3600", 1000);
  CHECK(cache.find("example.com", 443, 1000, &port) && port == 443);
  CHECK(cache.find("EXAMPLE.com", 443, 1000 + 3600 * 1000 - 1, &port));
  CHECK(!cache.find("example.com", 443, 1000 + 3600 * 1000, &port));          // expired
  CHECK(!cache.find("example.com", 8443, 1000, &port) && !cache.find("other.com", 443, 1000, &port));
  // several alternatives: the h3 one is taken, odd spacing and the default age
  cache.learn("a.test", 443, "h3-29=\":443\",   h3=\":8443\" ;ma=60 , h2=\":443\"", 0);
  CHECK(cache.find("a.test", 443, 59000, &port) && port == 8443 && !cache.find("a.test", 443, 60000, &port));
  cache.learn("b.test", 443, "h3=\":443\"", 0);
  CHECK(cache.find("b.test", 443, 24ull * 3600 * 1000 - 1, &port) && !cache.find("b.test", 443, 24ull * 3600 * 1000, &port));
  // an age over a week is cut to a week
  cache.learn("c.test", 443, "h3=\":443\"; ma=99999999", 0);
  CHECK(cache.find("c.test", 443, 7ull * 24 * 3600 * 1000 - 1, &port) && !cache.find("c.test", 443, 7ull * 24 * 3600 * 1000, &port));
  // alternatives on another host, other protocols and damaged values are ignored
  cache.learn("d.test", 443, "h3=\"elsewhere.test:443\"", 0);
  cache.learn("d.test", 443, "h2=\":443\"", 0);
  cache.learn("d.test", 443, "h3=:443", 0);
  cache.learn("d.test", 443, "h3=\":0\"", 0);
  cache.learn("d.test", 443, "h3=\":99999\"", 0);
  cache.learn("d.test", 443, "h3=\"d.test\"", 0);
  cache.learn("d.test", 443, "", 0);
  CHECK(!cache.find("d.test", 443, 0, &port));
  cache.learn("d.test", 443, "h3=\"D.TEST:4433\"; ma=\"30\"", 0);   // the same host, any case, a quoted age
  CHECK(cache.find("d.test", 443, 29000, &port) && port == 4433 && !cache.find("d.test", 443, 30000, &port));
  // clear and ma=0 forget
  cache.learn("example.com", 443, "clear", 2000);
  CHECK(!cache.find("example.com", 443, 2000, &port));
  cache.learn("b.test", 443, "h3=\":443\"; ma=0", 0);
  CHECK(!cache.find("b.test", 443, 0, &port));
  // a failed attempt: left alone for ten minutes
  cache.learn("e.test", 443, "h3=\":443\"; ma=86400", 0);
  cache.mark_broken("e.test", 443, 5000);
  CHECK(cache.is_broken("e.test", 443, 5000) && !cache.find("e.test", 443, 5000, &port));
  CHECK(!cache.find("e.test", 443, 5000 + 600000 - 1, &port) && cache.find("e.test", 443, 5000 + 600000, &port) && !cache.is_broken("e.test", 443, 5000 + 600000));
  cache.mark_broken("f.test", 443, 0);   // never announced: only the failure is kept
  CHECK(cache.is_broken("f.test", 443, 1) && !cache.find("f.test", 443, 1, &port) && !cache.is_broken("f.test", 443, 600001));
  // the cache is bounded
  for (int i = 0; i < 200; ++i) {
    String host(a);
    host.assign("host");
    host.append_uint(static_cast<uint64_t>(i));
    cache.learn(host.view(), 443, "h3=\":443\"", 0);
  }
  CHECK(cache.count() <= AltSvcCache::kMaxEntries);
  CHECK(cache.find("host199", 443, 0, &port) && !cache.find("host0", 443, 0, &port));
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_huffman();
  test_qpack_encode();
  test_qpack_decode();
  test_qpack_roundtrip();
  test_alt_svc();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("http3 tests passed\n");
  return 0;
}
