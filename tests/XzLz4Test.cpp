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

// Host test of the .xz / .lzma, LZ4 and LZO1X decoders (src/Media/Xz, Lz4, Lzo). Fixtures are made at test time with
// the host's reference implementations: /usr/bin/xz and /usr/bin/lz4, and a small helper linked against liblzo2 and liblz4
// for LZO1X and raw LZ4 blocks (built here with the host C compiler; without it only hand-made vectors are checked).
// Each fixture must decode to its input; then every truncation (strided for big inputs) must fail, random bit flips must
// fail or decode correctly where a checksum covers the data (and must not crash or trip a sanitizer anywhere), and
// max_output must cut off exactly at the output size.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Media/Lz4.hpp"
#include "Media/Lzo.hpp"
#include "Media/Xz.hpp"

using namespace media;
using Bytes = std::vector<uint8_t>;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static const std::string kDir = "/tmp/claude-1000/xztest";

static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

static bool read_file(const std::string& path, Bytes* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) out->insert(out->end(), chunk, chunk + n);
  fclose(f);
  return true;
}
static bool write_file(const std::string& path, const Bytes& data) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
  return fclose(f) == 0 && ok;
}
static bool run(const std::string& command) { return system(command.c_str()) == 0; }

struct Rng {
  uint64_t s;
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

enum class Kind { Xz, Lzma, Lz4Block, Lz4Frame, Lzo };

static bool decode(Kind kind, ConstByteView in, ByteBuffer* out, uint64_t max) {
  switch (kind) {
    case Kind::Xz: return xz_decompress(in, out, max);
    case Kind::Lzma: return lzma_decompress(in, out, max);
    case Kind::Lz4Block: return lz4_block_decompress(in, out, max);
    case Kind::Lz4Frame: return lz4_frame_decompress(in, out, max);
    case Kind::Lzo: return lzo1x_decompress(in, out, max);
  }
  return false;
}

// Decodes `in` after a prefix already in the buffer; the prefix must survive, and a failure must leave only it.
static bool decode_to(Kind kind, const Bytes& in, Bytes* result, uint64_t max = UINT64_MAX) {
  TestAllocator a;
  bool ok;
  {
    ByteBuffer out(a);
    static const uint8_t kPrefix[3] = {'p', 'r', 'e'};
    if (!out.append(ConstByteView(kPrefix))) return false;
    ok = decode(kind, view(in), &out, max);
    const ConstByteView v = out.view();
    if (v.size() < 3 || v[0] != 'p' || v[1] != 'r' || v[2] != 'e') {
      printf("  prefix damaged\n");
      ++g_failed;
    }
    if (!ok && v.size() != 3) {
      printf("  failure left %llu bytes behind\n", (unsigned long long)v.size() - 3);
      ++g_failed;
    }
    result->assign(v.begin() + 3, v.end());
  }
  if (a.live != 0) {
    printf("  allocator leak: %d blocks\n", a.live);
    ++g_failed;
  }
  return ok;
}

// The full battery on one fixture. `checked`: a checksum covers the output, so a corrupted input must fail or decode
// to the right data. `strict_truncation`: every proper prefix must fail.
static void exercise(const char* name, Kind kind, const Bytes& compressed, const Bytes& original, bool checked, bool strict_truncation) {
  const int failed_before = g_failed;
  Bytes result;
  const bool ok = decode_to(kind, compressed, &result);
  CHECK(ok);
  CHECK(result == original);
  if (!ok || result != original) {
    printf("  %s: decode failed (ok=%d, got %zu bytes, want %zu)\n", name, ok, result.size(), original.size());
    return;
  }
  // max_output: exactly the size works, one less does not.
  CHECK(decode_to(kind, compressed, &result, original.size()) && result == original);
  if (!original.empty()) CHECK(!decode_to(kind, compressed, &result, original.size() - 1));
  if (original.size() > 1000) CHECK(!decode_to(kind, compressed, &result, original.size() / 2));

  // Truncation.
  const uint64_t n = compressed.size();
  const uint64_t step = n <= 3000 ? 1 : n / 97 + 1;
  for (uint64_t cut = 0; cut < n; cut += step) {
    Bytes prefix(compressed.begin(), compressed.begin() + cut);
    const bool cut_ok = decode_to(kind, prefix, &result);
    if (strict_truncation) {
      CHECK(!cut_ok);
      if (cut_ok) { printf("  %s: truncation at %llu decoded\n", name, (unsigned long long)cut); break; }
    } else if (checked && cut_ok) {
      CHECK(result == original);
    }
  }
  // Bit flips.
  Rng rng{0x9E3779B97F4A7C15ull ^ n};
  const int flips = original.size() > 1000000 ? 12 : n < 4000 ? 300 : 80;
  for (int i = 0; i < flips && n; ++i) {
    Bytes corrupt = compressed;
    const uint64_t at = rng.next() % n;
    corrupt[at] ^= static_cast<uint8_t>(1u << (rng.next() % 8));
    const bool flip_ok = decode_to(kind, corrupt, &result);
    if (checked && flip_ok && result != original) {
      CHECK(!"corruption decoded to wrong data");
      printf("  %s: flip at %llu decoded to wrong data\n", name, (unsigned long long)at);
      break;
    }
  }
  if (g_failed != failed_before) printf("  ^ in %s\n", name);
}

static Bytes make_text(uint64_t size, uint64_t seed) {
  static const char* kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "squashfs", "kernel", "block",
                                 "inode", "directory", "fragment", "compression", "theory", "craft", "\n", ", ", "."};
  Rng rng{seed};
  Bytes out;
  while (out.size() < size) {
    const char* w = kWords[rng.next() % 20];
    out.insert(out.end(), w, w + strlen(w));
    out.push_back(' ');
  }
  out.resize(size);
  return out;
}
static Bytes make_random(uint64_t size, uint64_t seed) {
  Rng rng{seed};
  Bytes out(size);
  for (auto& b : out) b = static_cast<uint8_t>(rng.next() >> 24);
  return out;
}
static Bytes make_repetitive(uint64_t size) {
  Bytes out;
  while (out.size() < size) {
    for (int i = 0; i < 300; ++i) out.push_back(static_cast<uint8_t>("ABCD"[i % 4]));
    out.insert(out.end(), 700, 0);
    for (int i = 0; i < 50; ++i) out.push_back(static_cast<uint8_t>(i));
  }
  out.resize(size);
  return out;
}

struct Input {
  std::string name;
  Bytes data;
  bool big() const { return data.size() > 1000000; }
};

// Compresses `input` with a shell command reading kDir/in and writing kDir/out.
static bool compress_with(const std::string& command, const Bytes& input, Bytes* out) {
  if (!write_file(kDir + "/in", input)) return false;
  if (!run(command + " < " + kDir + "/in > " + kDir + "/out 2>/dev/null")) return false;
  return read_file(kDir + "/out", out);
}

static void test_xz(const std::vector<Input>& inputs) {
  printf("xz\n");
  struct Variant {
    const char* options;
    bool checked;
    bool exe_only;
  };
  const Variant variants[] = {
      {"-0", true, false}, {"-1", true, false}, {"-2", true, false}, {"-3", true, false}, {"-4", true, false},
      {"-5", true, false}, {"-6", true, false}, {"-7", true, false}, {"-8", true, false}, {"-9", true, false},
      {"-6e", true, false}, {"--check=crc32", true, false}, {"--check=crc64", true, false}, {"--check=sha256", true, false},
      {"--check=none", false, false}, {"-T4 --block-size=65536", true, false}, {"--lzma2=preset=6,lc=4,lp=0,pb=0", true, false},
      {"--x86 --lzma2", true, true}, {"--arm --lzma2", true, true}, {"--armthumb --lzma2", true, true}, {"--arm64 --lzma2", true, true},
      {"--powerpc --lzma2", true, true}, {"--sparc --lzma2", true, true}, {"--ia64 --lzma2", true, true}, {"--riscv --lzma2", true, true},
      {"--delta=dist=4 --lzma2", true, false}, {"--x86=start=4096 --delta=dist=2 --lzma2=preset=1", true, true},
      {"--arm64=start=8192 --lzma2", true, true},
  };
  for (const Input& input : inputs) {
    for (const Variant& v : variants) {
      if (v.exe_only && input.name.rfind("exe", 0) != 0 && input.name != "random") continue;
      if (input.big() && !v.exe_only && strcmp(v.options, "-0") && strcmp(v.options, "-6") && strcmp(v.options, "--check=sha256") &&
          strcmp(v.options, "-T4 --block-size=65536"))
        continue;
      Bytes compressed;
      const std::string name = "xz " + std::string(v.options) + " " + input.name;
      if (!compress_with(std::string("xz -c -q ") + v.options, input.data, &compressed)) {
        printf("  (xz %s not available, skipped)\n", v.options);
        continue;
      }
      exercise(name.c_str(), Kind::Xz, compressed, input.data, v.checked, true);
    }
  }
  // Concatenated streams, with stream padding between and after them.
  const Bytes& a = inputs[2].data;
  const Bytes& b = inputs[3].data;
  Bytes ca, cb;
  if (compress_with("xz -c -q --check=crc32", a, &ca) && compress_with("xz -c -q -1 --check=sha256", b, &cb)) {
    Bytes joined = ca;
    joined.insert(joined.end(), cb.begin(), cb.end());
    Bytes expect = a;
    expect.insert(expect.end(), b.begin(), b.end());
    exercise("xz concatenated", Kind::Xz, joined, expect, true, false);
    Bytes padded = ca;
    padded.insert(padded.end(), 8, 0);
    padded.insert(padded.end(), cb.begin(), cb.end());
    padded.insert(padded.end(), 4, 0);
    Bytes result;
    CHECK(decode_to(Kind::Xz, padded, &result) && result == expect);
    Bytes bad_padding = ca;
    bad_padding.insert(bad_padding.end(), 3, 0);
    CHECK(!decode_to(Kind::Xz, bad_padding, &result));
    Bytes garbage = ca;
    garbage.push_back(1);
    CHECK(!decode_to(Kind::Xz, garbage, &result));
  }
  Bytes result;
  CHECK(!decode_to(Kind::Xz, Bytes{}, &result));
  CHECK(!decode_to(Kind::Xz, Bytes(12, 0), &result));
}

static void test_lzma(const std::vector<Input>& inputs) {
  printf("lzma (alone)\n");
  const char* presets[] = {"-0", "-1", "-6", "-9", "-6e", "--lzma1=preset=6,lc=4,lp=0,pb=4", "--lzma1=preset=6,lc=0,lp=4,pb=2", "--lzma1=preset=2,lc=0,lp=0,pb=0"};
  for (const Input& input : inputs) {
    for (const char* p : presets) {
      if (input.big() && strcmp(p, "-6") != 0) continue;
      Bytes compressed;
      if (!compress_with(std::string("xz -c -q --format=lzma ") + p, input.data, &compressed)) {
        printf("  (xz --format=lzma %s not available)\n", p);
        continue;
      }
      // As written: unknown size, ends with the end marker.
      exercise(("lzma " + std::string(p) + " " + input.name).c_str(), Kind::Lzma, compressed, input.data, false, false);
      // With the real size in the header (what mksquashfs writes): decoding stops at that size.
      Bytes sized = compressed;
      for (int i = 0; i < 8; ++i) sized[5 + i] = static_cast<uint8_t>(uint64_t(input.data.size()) >> (8 * i));
      Bytes result;
      CHECK(decode_to(Kind::Lzma, sized, &result) && result == input.data);
      if (!input.data.empty()) CHECK(!decode_to(Kind::Lzma, sized, &result, input.data.size() - 1));
      if (input.data.size() > 100) {  // a size larger than the data runs into the end marker: an error
        Bytes longer = compressed;
        for (int i = 0; i < 8; ++i) longer[5 + i] = static_cast<uint8_t>(uint64_t(input.data.size() + 1) >> (8 * i));
        CHECK(!decode_to(Kind::Lzma, longer, &result));
      }
    }
  }
  Bytes result;
  CHECK(!decode_to(Kind::Lzma, Bytes{0x5D, 0, 0, 1}, &result));
  Bytes bad_props(18, 0);
  bad_props[0] = 225;
  CHECK(!decode_to(Kind::Lzma, bad_props, &result));
}

static void test_lz4(const std::vector<Input>& inputs, bool helper) {
  printf("lz4\n");
  struct Variant {
    const char* options;
    bool checked;  // a content checksum
    bool block_checksum;
  };
  const Variant variants[] = {
      {"-1 -B4", true, false}, {"-1 -B5", true, false}, {"-1 -B6", true, false}, {"-1 -B7", true, false}, {"-9 -B4", true, false},
      {"-12 -B7", true, false}, {"-1 -B4 --no-frame-crc", false, false}, {"-1 -B4 -BD", true, false}, {"-9 -B5 -BD --no-frame-crc", false, false},
      {"-1 -B4 --content-size", true, false}, {"-1 -B4 -BX --no-frame-crc", true, true}, {"-9 -B6 -BD -BX --content-size", true, true},
      {"--fast=5 -B4", true, false},
  };
  for (const Input& input : inputs) {
    for (const Variant& v : variants) {
      if (input.big() && strcmp(v.options, "-1 -B4") && strcmp(v.options, "-9 -B6 -BD -BX --content-size") && strcmp(v.options, "-1 -B7")) continue;
      Bytes compressed;
      if (!compress_with(std::string("lz4 -c -q ") + v.options, input.data, &compressed)) {
        printf("  (lz4 %s not available)\n", v.options);
        continue;
      }
      // Without a checksum a truncation at a block boundary can still be a valid frame missing its end mark? No: the end
      // mark is required, so every proper prefix fails.
      exercise(("lz4 " + std::string(v.options) + " " + input.name).c_str(), Kind::Lz4Frame, compressed, input.data, v.checked, true);
    }
    if (helper) {
      for (const char* mode : {"lz4", "lz4hc"}) {
        Bytes compressed;
        if (!compress_with(kDir + "/helper " + mode, input.data, &compressed)) {
          CHECK(!"helper failed");
          continue;
        }
        if (input.data.empty()) continue;  // LZ4_decompress_safe refuses an empty block too
        exercise((std::string("lz4 block ") + mode + " " + input.name).c_str(), Kind::Lz4Block, compressed, input.data, false, false);
      }
    }
  }
  // Concatenated frames and a skippable frame between them.
  Bytes ca, cb;
  const Bytes& a = inputs[2].data;
  const Bytes& b = inputs[4].data;
  if (compress_with("lz4 -c -q", a, &ca) && compress_with("lz4 -c -q -BD", b, &cb)) {
    Bytes joined = ca;
    const uint8_t skippable[] = {0x5A, 0x2A, 0x4D, 0x18, 3, 0, 0, 0, 9, 9, 9};
    joined.insert(joined.end(), skippable, skippable + sizeof skippable);
    joined.insert(joined.end(), cb.begin(), cb.end());
    Bytes expect = a;
    expect.insert(expect.end(), b.begin(), b.end());
    Bytes result;
    CHECK(decode_to(Kind::Lz4Frame, joined, &result) && result == expect);
  }
  // Hand-made blocks.
  Bytes result;
  CHECK(decode_to(Kind::Lz4Block, Bytes{0x00}, &result) && result.empty());
  CHECK(decode_to(Kind::Lz4Block, Bytes{0x10, 'a'}, &result) && result == Bytes{'a'});
  // "ab" then a match of 4+6=10 at distance 2, then the literals "xyzzy".
  CHECK(decode_to(Kind::Lz4Block, Bytes{0x26, 'a', 'b', 2, 0, 0x50, 'x', 'y', 'z', 'z', 'y'}, &result) &&
        result == Bytes{'a', 'b', 'a', 'b', 'a', 'b', 'a', 'b', 'a', 'b', 'a', 'b', 'x', 'y', 'z', 'z', 'y'});
  CHECK(!decode_to(Kind::Lz4Block, Bytes{}, &result));
  CHECK(!decode_to(Kind::Lz4Block, Bytes{0x26, 'a', 'b', 3, 0, 0x50, 'x', 'y', 'z', 'z', 'y'}, &result));  // reaches before the start
  CHECK(!decode_to(Kind::Lz4Block, Bytes{0x26, 'a', 'b', 0, 0, 0x50, 'x', 'y', 'z', 'z', 'y'}, &result));  // offset 0
  CHECK(!decode_to(Kind::Lz4Block, Bytes{0x26, 'a', 'b', 2, 0}, &result));                                  // ends after a match
  CHECK(!decode_to(Kind::Lz4Block, Bytes{0xF0, 0xFF, 0xFF}, &result));                                       // length runs off the end
  CHECK(!decode_to(Kind::Lz4Frame, Bytes{}, &result));
  // Random noise in both formats: no crash.
  Rng rng{77};
  for (int i = 0; i < 2000; ++i) {
    Bytes noise = make_random(1 + rng.next() % 300, rng.next());
    (void)decode_to(Kind::Lz4Block, noise, &result, 1 << 20);
    noise[0] = 0x04; if (noise.size() > 3) { noise[1] = 0x22; noise[2] = 0x4D; noise[3] = 0x18; }
    (void)decode_to(Kind::Lz4Frame, noise, &result, 1 << 20);
  }
}

static void test_lzo(const std::vector<Input>& inputs, bool helper) {
  printf("lzo1x\n");
  if (helper) {
    for (const Input& input : inputs) {
      for (const char* mode : {"lzo1", "lzo999"}) {
        if (input.big() && strcmp(mode, "lzo999") == 0 && input.name != "big") continue;
        Bytes compressed;
        if (!compress_with(kDir + "/helper " + mode, input.data, &compressed)) {
          CHECK(!"helper failed");
          continue;
        }
        exercise((std::string("lzo ") + mode + " " + input.name).c_str(), Kind::Lzo, compressed, input.data, false, true);
        Bytes trailing = compressed;
        trailing.push_back(0);
        Bytes result;
        CHECK(!decode_to(Kind::Lzo, trailing, &result));
      }
    }
  }
  // Hand-made streams.
  Bytes result;
  CHECK(decode_to(Kind::Lzo, Bytes{0x11, 0x00, 0x00}, &result) && result.empty());
  CHECK(decode_to(Kind::Lzo, Bytes{20, 'a', 'b', 'c', 0x11, 0x00, 0x00}, &result) && result == Bytes{'a', 'b', 'c'});
  // 4 literals "abcd", then M2 (token 0x40 | (len-1)<<5 ... ): length 3 at distance 4, no trailing literals, then end.
  // M2: t >= 64, length = (t >> 5) + 1, distance = 1 + ((t >> 2) & 7) + (next << 3). t = 0x40 | (3 << 2) -> len 3, dist 4.
  CHECK(decode_to(Kind::Lzo, Bytes{21, 'a', 'b', 'c', 'd', 0x4C, 0x00, 0x11, 0x00, 0x00}, &result) &&
        result == Bytes{'a', 'b', 'c', 'd', 'a', 'b', 'c'});
  // M3 (t = 32 | (len - 2)): length 7 at distance 1 after one literal-run token: 0 = long run? use first-byte literals.
  // 'x' then M3 len 7 dist 1: t = 32 | 5, then the distance bytes (dist - 1) << 2 = 0, 0.
  CHECK(decode_to(Kind::Lzo, Bytes{18, 'x', 37, 0x00, 0x00, 0x11, 0x00, 0x00}, &result) && result == Bytes(8, 'x'));
  CHECK(!decode_to(Kind::Lzo, Bytes{18, 'x', 37, 0x04, 0x00, 0x11, 0x00, 0x00}, &result));  // distance 2: before the start
  CHECK(!decode_to(Kind::Lzo, Bytes{}, &result));
  CHECK(!decode_to(Kind::Lzo, Bytes{20, 'a', 'b', 'c'}, &result));                     // no end marker
  CHECK(!decode_to(Kind::Lzo, Bytes{20, 'a', 'b', 'c', 0x12, 0x00, 0x00}, &result));   // end marker with a wrong length
  CHECK(!decode_to(Kind::Lzo, Bytes{20, 'a', 'b', 'c', 0x11, 0x00, 0x00, 0x00}, &result));
  CHECK(!decode_to(Kind::Lzo, Bytes{20, 'a', 'b', 'c', 0x11, 0x00, 0x00}, &result, 2));  // max_output
  Rng rng{99};
  for (int i = 0; i < 3000; ++i) {
    Bytes noise = make_random(1 + rng.next() % 300, rng.next());
    (void)decode_to(Kind::Lzo, noise, &result, 1 << 20);
  }
}

static bool build_helper() {
  const char* source = R"(#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lzo/lzo1x.h>
#include <lz4.h>
#include <lz4hc.h>
int main(int argc, char** argv) {
  if (argc < 2 || lzo_init() != LZO_E_OK) return 2;
  size_t cap = 1 << 20, n = 0, got;
  unsigned char* in = malloc(cap);
  while ((got = fread(in + n, 1, cap - n, stdin)) > 0) { n += got; if (n == cap) in = realloc(in, cap *= 2); }
  size_t out_cap = n + n / 16 + 64 + 3 + LZ4_compressBound((int)n);
  unsigned char* out = malloc(out_cap);
  lzo_uint out_len = 0;
  if (!strcmp(argv[1], "lzo1")) {
    void* work = malloc(LZO1X_1_MEM_COMPRESS);
    if (lzo1x_1_compress(in, n, out, &out_len, work) != LZO_E_OK) return 1;
  } else if (!strcmp(argv[1], "lzo999")) {
    void* work = malloc(LZO1X_999_MEM_COMPRESS);
    if (lzo1x_999_compress(in, n, out, &out_len, work) != LZO_E_OK) return 1;
  } else if (!strcmp(argv[1], "lz4")) {
    int r = LZ4_compress_default((const char*)in, (char*)out, (int)n, (int)out_cap);
    if (r <= 0 && n) return 1;
    out_len = r;
  } else if (!strcmp(argv[1], "lz4hc")) {
    int r = LZ4_compress_HC((const char*)in, (char*)out, (int)n, (int)out_cap, 12);
    if (r <= 0 && n) return 1;
    out_len = r;
  } else return 2;
  fwrite(out, 1, out_len, stdout);
  return 0;
}
)";
  if (!write_file(kDir + "/helper.c", Bytes(source, source + strlen(source)))) return false;
  return run("cc -O2 -o " + kDir + "/helper " + kDir + "/helper.c -llzo2 -llz4 2>/dev/null");
}

int main() {
  if (!run("mkdir -p " + kDir)) {
    printf("cannot create %s\n", kDir.c_str());
    return 1;
  }
  std::vector<Input> inputs;
  inputs.push_back({"empty", {}});
  inputs.push_back({"one byte", {0x42}});
  inputs.push_back({"text", make_text(50000, 1)});
  inputs.push_back({"random", make_random(40000, 2)});
  inputs.push_back({"repetitive", make_repetitive(300000)});
  Bytes ls, libc;
  if (read_file("/usr/bin/ls", &ls)) inputs.push_back({"exe ls", ls});
  if (read_file("/usr/lib/libc.so.6", &libc) || read_file("/lib/x86_64-linux-gnu/libc.so.6", &libc)) inputs.push_back({"exe libc", libc});
  Bytes big = make_text(2500000, 3);
  const Bytes noise = make_random(300000, 4);
  big.insert(big.end(), noise.begin(), noise.end());
  big.insert(big.end(), ls.begin(), ls.end());
  const Bytes rep = make_repetitive(1500000);
  big.insert(big.end(), rep.begin(), rep.end());
  inputs.push_back({"big", big});

  const bool helper = build_helper();
  if (!helper) printf("(no liblzo2/liblz4 helper: LZO and raw LZ4 checked only with hand-made vectors)\n");

  test_xz(inputs);
  test_lzma(inputs);
  test_lz4(inputs, helper);
  test_lzo(inputs, helper);

  // crc64 against the check value of CRC-64/XZ.
  const char* nine = "123456789";
  CHECK(crc64(ConstByteView(reinterpret_cast<const uint8_t*>(nine), 9)) == 0x995DC9BBDF1939FAull);

  printf("%s: %d of %d checks failed\n", g_failed ? "XzLz4Test FAILED" : "XzLz4Test passed", g_failed, g_checks);
  return g_failed ? 1 : 0;
}
