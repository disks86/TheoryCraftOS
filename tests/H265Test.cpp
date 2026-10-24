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


// Host-side tests for the H.265 decoder: every stream in tests/data/hevc_*.h265 (made by scripts/gen_hevc_test_streams.sh with libx265) is decoded and each
// picture compared byte for byte with the one FFmpeg's decoder made of the same stream (hevc_*.yuv). Damaged streams (truncated, bits flipped) must not fault.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/Software/H265.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

// Splits an Annex B stream into units that each end before the next picture's first slice (the parameter sets in front of a picture go with the picture before it).
static std::vector<std::pair<size_t, size_t>> split_pictures(const std::vector<uint8_t>& d) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 <= d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { starts.push_back(i); i += 2; }
  std::vector<std::pair<size_t, size_t>> units;
  size_t unit_begin = 0;
  bool have_slice = false;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t at = starts[k];
    if (at + 5 > d.size()) break;
    const int type = (d[at + 3] >> 1) & 0x3F;
    const bool slice = type < 32;
    const bool first = slice && (d[at + 5] & 0x80);
    if (first && have_slice) {
      // the new picture starts here, but the parameter sets just before belong to it: back up over them
      size_t cut = at;
      size_t j = k;
      while (j > 0) {
        const int pt = (d[starts[j - 1] + 3] >> 1) & 0x3F;
        if (pt >= 32 && pt <= 40) { cut = starts[j - 1]; --j; } else break;
      }
      if (cut > unit_begin) units.push_back({unit_begin, cut});
      unit_begin = cut;
      have_slice = false;
    }
    if (slice) have_slice = true;
  }
  units.push_back({unit_begin, d.size()});
  return units;
}

struct Result {
  int frames = 0, mismatched = 0, first_bad = -1;
  uint32_t width = 0, height = 0;
};

static void collect(H265Decoder& dec, const std::vector<uint8_t>& ref, Result* r, size_t* frame_bytes_out) {
  VideoFrame f;
  while (dec.receive(&f)) {
    const size_t bytes = size_t(f.format.width) * f.format.height * 3 / 2;
    *frame_bytes_out = bytes;
    r->width = f.format.width;
    r->height = f.format.height;
    const size_t off = size_t(r->frames) * bytes;
    bool same = off + bytes <= ref.size();
    size_t p = off;
    for (uint32_t pl = 0; pl < 3 && same; ++pl)
      for (uint32_t y = 0; y < f.format.plane_height(pl) && same; ++y)
        for (uint32_t x = 0; x < f.format.plane_width(pl); ++x, ++p)
          if (f.plane[pl][size_t(y) * f.stride[pl] + x] != ref[p]) {
            if (r->first_bad < 0 || r->frames < r->first_bad) {
              printf("    frame %d plane %u: first difference at x=%u y=%u: got %u expected %u\n", r->frames, pl, x, y, f.plane[pl][size_t(y) * f.stride[pl] + x], ref[p]);
            }
            same = false;
            break;
          }
    if (!same) {
      ++r->mismatched;
      if (r->first_bad < 0) r->first_bad = r->frames;
    }
    ++r->frames;
  }
}

static Result decode_stream(allocators::IAllocator& a, const std::vector<uint8_t>& stream, const std::vector<uint8_t>& ref) {
  Result r;
  H265Decoder dec(a);
  size_t fb = 0;
  for (auto u : split_pictures(stream)) {
    dec.decode(ConstByteView(stream.data() + u.first, u.second - u.first), int64_t(r.frames) * 1000);
    collect(dec, ref, &r, &fb);
  }
  dec.flush();
  collect(dec, ref, &r, &fb);
  return r;
}

static const char* const kStreams[] = {"intra", "intra_filt", "ipp", "ipp_filt", "bframes", "weighted", "slices", "ctu64", "tools", "scaling", "wpp", "cutree_off"};

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (const char* name : kStreams) {
    if (argc > 1 && strcmp(argv[1], name) != 0) continue;
    char path[160];
    snprintf(path, sizeof path, "tests/data/hevc_%s.h265", name);
    const std::vector<uint8_t> stream = read_file(path);
    snprintf(path, sizeof path, "tests/data/hevc_%s.yuv", name);
    const std::vector<uint8_t> ref = read_file(path);
    CHECK(!stream.empty() && !ref.empty());
    if (stream.empty() || ref.empty()) continue;
    TestAllocator a;
    {
      const Result r = decode_stream(a, stream, ref);
      printf("  %-12s %d pictures, %d differ\n", name, r.frames, r.mismatched);
      CHECK(r.frames * size_t(r.width) * r.height * 3 / 2 == ref.size() && r.mismatched == 0);
    }
    CHECK(a.live == 0);
  }
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("H265Test passed\n");
  return 0;
}
