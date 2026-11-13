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

// Host tests for the Zstandard decompressor (src/Media/Zstd.hpp). Fixtures are made at test time with the host's
// `zstd` command (skipped, with a note, if it is missing) over many inputs and settings, and must decode byte-exact.
// Then truncation at every offset of small frames (and at strides of a big one), random bit flips, garbage, a
// dictionary frame, skippable and concatenated frames, the max_output limit, and XXH64 against known values.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Media/Zstd.hpp"

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint64_t next_random() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 7;
  g_seed ^= g_seed << 17;
  return g_seed;
}

static std::string g_dir;

static bool write_file(const std::string& path, const Bytes& data) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

static bool read_file(const std::string& path, Bytes* data) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  data->clear();
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) data->insert(data->end(), chunk, chunk + n);
  fclose(f);
  return true;
}

// Compresses `input` with the zstd command and `flags`.
static bool compress(const Bytes& input, const std::string& flags, Bytes* out) {
  const std::string in = g_dir + "/input", z = g_dir + "/input.zst";
  if (!write_file(in, input)) return false;
  const std::string command = "zstd -q -f " + flags + " -o '" + z + "' '" + in + "' 2>/dev/null";
  if (system(command.c_str()) != 0) return false;
  return read_file(z, out);
}

static bool decode(const Bytes& in, Bytes* out, uint64_t max_output = ~uint64_t{0}) {
  TestAllocator allocator;
  bool ok;
  {
    ByteBuffer buffer(allocator);
    ok = media::zstd_decompress(ConstByteView(in.data(), in.size()), &buffer, max_output);
    ConstByteView v = buffer.view();
    out->assign(v.data(), v.data() + v.size());
  }
  if (allocator.live != 0) {
    printf("FAILED: %d allocations leaked\n", allocator.live);
    ++g_failed;
  }
  return ok;
}

// Inputs.
static Bytes text(uint64_t size) {
  static const char* words[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "zstandard", "frame", "block", "literal",
                                "sequence", "offset", "huffman", "entropy", "kernel", "squashfs", "TheoryCraftOS", "and", "of", "a", "to", "in"};
  Bytes b;
  while (b.size() < size) {
    const char* w = words[next_random() % (sizeof words / sizeof words[0])];
    b.insert(b.end(), w, w + strlen(w));
    const uint64_t r = next_random() % 16;
    b.push_back(r == 0 ? '\n' : r == 1 ? '.' : ' ');
  }
  b.resize(size);
  return b;
}

static Bytes random_bytes(uint64_t size) {
  Bytes b(size);
  for (auto& x : b) x = static_cast<uint8_t>(next_random());
  return b;
}

static Bytes repeated(uint64_t size, uint8_t value) { return Bytes(size, value); }

static Bytes pattern(uint64_t size, uint64_t period) {
  Bytes unit = random_bytes(period), b;
  while (b.size() < size) b.insert(b.end(), unit.begin(), unit.end());
  b.resize(size);
  return b;
}

// Runs of text, random bytes, zeros, small-alphabet noise and copies of earlier pieces.
static Bytes mixed(uint64_t size) {
  Bytes b;
  while (b.size() < size) {
    const uint64_t kind = next_random() % 5, length = 1 + next_random() % 20000;
    Bytes piece;
    if (kind == 0) piece = text(length);
    else if (kind == 1) piece = random_bytes(length);
    else if (kind == 2) piece = repeated(length, static_cast<uint8_t>(next_random()));
    else if (kind == 3) {
      piece.resize(length);
      for (auto& x : piece) x = static_cast<uint8_t>('a' + next_random() % 4);
    } else if (!b.empty()) {
      const uint64_t from = next_random() % b.size();
      const uint64_t n = length < b.size() - from ? length : b.size() - from;
      piece.assign(b.begin() + from, b.begin() + from + n);
    }
    b.insert(b.end(), piece.begin(), piece.end());
  }
  b.resize(size);
  return b;
}

static bool has_checksum(const Bytes& frame) { return frame.size() > 4 && (frame[4] & 4); }

static int g_fixtures = 0;

static void round_trip(const char* name, const Bytes& input, const std::string& flags) {
  Bytes z, out;
  if (!compress(input, flags, &z)) {
    printf("FAILED: zstd %s on %s\n", flags.c_str(), name);
    ++g_failed;
    return;
  }
  ++g_fixtures;
  const bool ok = decode(z, &out);
  if (!ok || out != input) {
    printf("FAILED: %s with '%s' (%zu bytes -> %zu): ok=%d, got %zu bytes\n", name, flags.c_str(), input.size(), z.size(), ok, out.size());
    ++g_failed;
    char keep[512];
    snprintf(keep, sizeof keep, "cp '%s/input' '%s/fail_%d.bin'; cp '%s/input.zst' '%s/fail_%d.zst'", g_dir.c_str(), g_dir.c_str(), g_fixtures, g_dir.c_str(), g_dir.c_str(), g_fixtures);
    if (system(keep) != 0) printf("  (could not keep the fixture)\n");
  }
  ++g_checks;
}

static void truncations(const Bytes& frame, uint64_t stride) {
  Bytes out;
  for (uint64_t n = 0; n < frame.size(); n += stride) {
    Bytes cut(frame.begin(), frame.begin() + n);
    if (decode(cut, &out)) {
      printf("FAILED: a frame of %zu bytes cut to %llu decoded\n", frame.size(), (unsigned long long)n);
      ++g_failed;
      return;
    }
    if (!out.empty()) {
      printf("FAILED: output left behind after a failure (cut to %llu)\n", (unsigned long long)n);
      ++g_failed;
      return;
    }
  }
  ++g_checks;
}

static void bit_flips(const Bytes& frame, const Bytes& original, int count) {
  const bool checked = has_checksum(frame);
  Bytes out;
  int wrong = 0;
  for (int i = 0; i < count; ++i) {
    Bytes damaged = frame;
    const int flips = 1 + static_cast<int>(next_random() % 3);
    for (int f = 0; f < flips; ++f) {
      const uint64_t bit = next_random() % (damaged.size() * 8);
      damaged[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    }
    if (decode(damaged, &out, original.size() * 2 + 1024) && out != original) ++wrong;
  }
  if (checked && wrong) {
    printf("FAILED: %d damaged frames with a checksum decoded to wrong data\n", wrong);
    ++g_failed;
  }
  ++g_checks;
}

static void xxh64_vectors() {
  const uint8_t abc[] = {'a', 'b', 'c'};
  CHECK(media::xxh64(ConstByteView()) == 0xEF46DB3751D8E999ull);
  CHECK(media::xxh64(ConstByteView(abc, 3)) == 0x44BC2CF5AD770999ull);
  // Longer inputs (all stripe and tail paths) against the zstd command's own checksum, via frames with --check.
  for (uint64_t size : {1ull, 4ull, 7ull, 8ull, 31ull, 32ull, 33ull, 63ull, 100ull, 1000ull}) {
    Bytes in = random_bytes(size), z;
    if (!compress(in, "-1 --check", &z) || z.size() < 4) { CHECK(false); continue; }
    const uint32_t want = z[z.size() - 4] | (z[z.size() - 3] << 8) | (z[z.size() - 2] << 16) | (uint32_t(z[z.size() - 1]) << 24);
    CHECK(static_cast<uint32_t>(media::xxh64(ConstByteView(in.data(), in.size()))) == want);
  }
}

// Frames made by hand: raw blocks, RLE blocks, a dictionary id, skippable frames, an empty input.
static void hand_made() {
  Bytes out;
  // magic, FHD = single segment + 1-byte content size, FCS = 5, raw last block of 5 bytes.
  Bytes raw = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x05, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
  CHECK(decode(raw, &out) && out == Bytes({'h', 'e', 'l', 'l', 'o'}));
  // The same with an RLE block of 5 'x'.
  Bytes rle = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x05, 0x2B, 0x00, 0x00, 'x'};
  CHECK(decode(rle, &out) && out == Bytes(5, 'x'));
  // A compressed block of RLE literals (5 'z') and no sequences; then with stray bytes after the sequence count.
  Bytes rle_literals = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x05, 0x1D, 0x00, 0x00, 0x29, 'z', 0x00};
  CHECK(decode(rle_literals, &out) && out == Bytes(5, 'z'));
  Bytes stray = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x05, 0x25, 0x00, 0x00, 0x29, 'z', 0x00, 0x00};
  CHECK(!decode(stray, &out));
  // Content size says 6 but the block holds 5.
  Bytes wrong_size = raw;
  wrong_size[5] = 6;
  CHECK(!decode(wrong_size, &out));
  // A dictionary id (FHD bit 0: one byte): id 7 is refused, id 0 means none.
  Bytes dict = {0x28, 0xB5, 0x2F, 0xFD, 0x21, 0x07, 0x05, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
  CHECK(!decode(dict, &out));
  dict[5] = 0;
  CHECK(decode(dict, &out) && out.size() == 5);
  // Reserved block type 3, reserved FHD bit.
  Bytes reserved = raw;
  reserved[6] = 0x2F;
  CHECK(!decode(reserved, &out));
  reserved = raw;
  reserved[4] |= 8;
  CHECK(!decode(reserved, &out));
  // Skippable frames around a frame, and alone.
  Bytes skip = {0x5A, 0x2A, 0x4D, 0x18, 0x03, 0x00, 0x00, 0x00, 1, 2, 3};
  Bytes both = skip;
  both.insert(both.end(), raw.begin(), raw.end());
  both.insert(both.end(), skip.begin(), skip.end());
  CHECK(decode(both, &out) && out == Bytes({'h', 'e', 'l', 'l', 'o'}));
  CHECK(decode(skip, &out) && out.empty());
  Bytes short_skip(skip.begin(), skip.end() - 1);
  CHECK(!decode(short_skip, &out));
  // Nothing at all, a bad magic, trailing garbage after a frame.
  CHECK(!decode(Bytes(), &out));
  CHECK(!decode(Bytes({1, 2, 3, 4, 5, 6, 7, 8}), &out));
  Bytes trailing = raw;
  trailing.push_back(0);
  CHECK(!decode(trailing, &out));
  // A real zstd dictionary frame.
  Bytes samples = text(4000), z;
  std::string dict_file = g_dir + "/dict";
  if (write_file(g_dir + "/dict_input", text(100000)) &&
      system(("zstd -q -f --train --maxdict=4096 -B1000 -o '" + dict_file + "' '" + g_dir + "/dict_input' >/dev/null 2>&1").c_str()) == 0 &&
      compress(samples, "-3 -D '" + dict_file + "'", &z)) {
    CHECK(!decode(z, &out));
  } else {
    printf("note: could not train a dictionary; skipping the dictionary-frame case\n");
  }
}

// Output appended after what the buffer held, and the max_output limit.
static void limits() {
  Bytes input = mixed(300000), z, out;
  if (!compress(input, "-3 --check", &z)) { CHECK(false); return; }
  CHECK(decode(z, &out, input.size()) && out == input);
  CHECK(!decode(z, &out, input.size() - 1) && out.empty());
  CHECK(!decode(z, &out, 0));
  Bytes zeros = repeated(10 << 20, 0);
  if (!compress(zeros, "-19", &z)) { CHECK(false); return; }
  CHECK(!decode(z, &out, 1 << 20));
  CHECK(decode(z, &out, 10 << 20) && out == zeros);
  // Appending: a buffer that already holds bytes keeps them, and the limit counts only new ones.
  TestAllocator allocator;
  {
    ByteBuffer buffer(allocator);
    const uint8_t prefix[3] = {9, 8, 7};
    CHECK(buffer.append(ConstByteView(prefix, 3)));
    CHECK(media::zstd_decompress(ConstByteView(z.data(), z.size()), &buffer, zeros.size()));
    CHECK(buffer.size() == zeros.size() + 3 && buffer.view()[0] == 9 && buffer.view()[3] == 0);
    CHECK(!media::zstd_decompress(ConstByteView(z.data(), z.size()), &buffer, 5));
    CHECK(buffer.size() == zeros.size() + 3);
  }
  CHECK(allocator.live == 0);
}

int main() {
  const char* base = getenv("ZSTD_TEST_DIR");
  std::string root = base ? base : "/tmp/claude-1000/zstdtest";
  if (system(("mkdir -p '" + root + "'").c_str()) != 0) return 1;
  std::string templ = root + "/run.XXXXXX";
  std::vector<char> name(templ.begin(), templ.end());
  name.push_back(0);
  if (!mkdtemp(name.data())) { printf("cannot make a temporary directory under %s\n", root.c_str()); return 1; }
  g_dir = name.data();
  if (system("zstd --version >/dev/null 2>&1") != 0) {
    printf("zstd command not found: only the hand-made cases run\n");
    hand_made();
    printf("%s: %d checks, %d failed\n", g_failed ? "FAILED" : "PASSED", g_checks, g_failed);
    return g_failed ? 1 : 0;
  }

  xxh64_vectors();
  hand_made();

  struct Input { const char* name; Bytes data; };
  std::vector<Input> inputs;
  inputs.push_back({"empty", Bytes()});
  inputs.push_back({"one byte", Bytes(1, 'A')});
  inputs.push_back({"short text", text(100)});
  inputs.push_back({"text 64K", text(65536)});
  inputs.push_back({"text 300K", text(300000)});
  inputs.push_back({"random 200K", random_bytes(200000)});
  inputs.push_back({"zeros 1M", repeated(1 << 20, 0)});
  inputs.push_back({"period 7", pattern(500000, 7)});
  inputs.push_back({"period 3000", pattern(400000, 3000)});
  inputs.push_back({"mixed 1M", mixed(1 << 20)});
  inputs.push_back({"text 128K-1", text(131071)});
  inputs.push_back({"text 128K", text(131072)});
  inputs.push_back({"text 128K+1", text(131073)});
  inputs.push_back({"mixed 256K+1", mixed(262145)});
  inputs.push_back({"random 128K", random_bytes(131072)});
  inputs.push_back({"small alphabet", [] { Bytes b(200000); for (auto& x : b) x = static_cast<uint8_t>("ab"[next_random() % 2]); return b; }()});
  const char* level_flags[] = {"--fast=5", "--fast=1", "-1", "-3", "-9", "-19", "--ultra -22"};
  for (const Input& in : inputs)
    for (const char* level : level_flags)
      for (const char* check : {"--check", "--no-check"}) {
        std::string flags = std::string(level) + " " + check;
        round_trip(in.name, in.data, flags);
      }
  // Frames without a content size (not single-segment), long-distance matching, multithreaded, small windows.
  for (const Input& in : inputs) {
    round_trip(in.name, in.data, "-3 --no-content-size");
    round_trip(in.name, in.data, "-19 --no-content-size --check");
    round_trip(in.name, in.data, "-3 --zstd=wlog=10 --no-content-size");
    round_trip(in.name, in.data, "-6 --zstd=wlog=17");
  }
  // Big inputs: several MB, a repeat far back for --long, a 16 MiB window.
  Bytes big = mixed(6 << 20);
  Bytes far = random_bytes(3 << 20);
  far.insert(far.end(), far.begin(), far.end());
  for (const char* flags : {"-1", "-3 --check", "-9", "-19 --check", "--long=27 -3", "--long=27 -19 --check", "-T4 -5 --check", "--fast=3",
                            "-3 --zstd=wlog=24 --no-content-size"}) {
    round_trip("mixed 6M", big, flags);
    round_trip("random 3M twice", far, flags);
  }
  round_trip("mixed 6M ultra", big, "--ultra -22 --check");

  // Concatenated frames (and a skippable frame between them) decode to the joined inputs.
  {
    Bytes a = text(50000), b = mixed(200000), za, zb, out;
    if (compress(a, "-3 --check", &za) && compress(b, "-19 --no-check", &zb)) {
      Bytes both = za;
      const uint8_t skip[] = {0x50, 0x2A, 0x4D, 0x18, 0x02, 0x00, 0x00, 0x00, 0xAA, 0xBB};
      both.insert(both.end(), skip, skip + sizeof skip);
      both.insert(both.end(), zb.begin(), zb.end());
      both.insert(both.end(), za.begin(), za.end());
      Bytes want = a;
      want.insert(want.end(), b.begin(), b.end());
      want.insert(want.end(), a.begin(), a.end());
      CHECK(decode(both, &out) && out == want);
      // A match may not reach into the previous frame: the second frame decodes alone just the same.
      CHECK(decode(zb, &out) && out == b);
      // Cut inside the last frame: false.
      Bytes cut(both.begin(), both.end() - 3);
      CHECK(!decode(cut, &out));
    } else {
      CHECK(false);
    }
  }

  // Truncation at every offset of small frames, at strides of a big one.
  {
    Bytes z;
    struct Case { Bytes data; const char* flags; };
    std::vector<Case> cases = {{text(3000), "-19 --check"}, {text(3000), "-3 --no-check"}, {mixed(20000), "-9 --no-content-size"},
                               {random_bytes(500), "-3"}, {pattern(9000, 5), "-1 --check"}, {Bytes(1, 'q'), "-3"}, {Bytes(), "-3"}};
    for (const Case& c : cases)
      if (compress(c.data, c.flags, &z)) truncations(z, 1); else CHECK(false);
    if (compress(mixed(1 << 20), "-19 --check", &z)) truncations(z, 997); else CHECK(false);
    if (compress(mixed(1 << 20), "-3 --no-check --no-content-size", &z)) truncations(z, 1009); else CHECK(false);
  }

  // Bit flips and garbage: must never crash; with a checksum, never wrong data.
  {
    struct Case { Bytes data; const char* flags; int count; };
    std::vector<Case> cases = {{text(5000), "-19 --check", 4000}, {text(5000), "-3 --no-check", 4000}, {mixed(60000), "-9 --check", 1500},
                               {mixed(60000), "--fast=1 --no-check", 1500}, {pattern(30000, 11), "-1 --check", 2000},
                               {random_bytes(3000), "-3 --check", 1000}, {mixed(300000), "-19 --check --no-content-size", 300},
                               {text(200000), "--ultra -22 --check", 300}};
    for (const Case& c : cases) {
      Bytes z;
      if (compress(c.data, c.flags, &z)) bit_flips(z, c.data, c.count); else CHECK(false);
    }
    // Pure garbage after a valid magic and header byte.
    Bytes out;
    for (int i = 0; i < 20000; ++i) {
      Bytes g = random_bytes(4 + next_random() % 300);
      g[0] = 0x28; g[1] = 0xB5; g[2] = 0x2F; g[3] = 0xFD;
      (void)decode(g, &out, 1 << 20);
    }
    ++g_checks;
  }

  limits();

  printf("%s: %d checks (%d zstd fixtures), %d failed\n", g_failed ? "FAILED" : "PASSED", g_checks, g_fixtures, g_failed);
  if (!g_failed) {
    if (system(("rm -rf '" + g_dir + "'").c_str()) != 0) printf("note: could not remove %s\n", g_dir.c_str());
  } else {
    printf("fixtures kept in %s\n", g_dir.c_str());
  }
  return g_failed ? 1 : 0;
}
