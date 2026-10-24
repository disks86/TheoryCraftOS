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


// Host-side tests for the MPEG-2 decoder. Every stream in tests/data/mpeg2_*.m2v (made by scripts/gen_mpeg2_test_streams.sh with ffmpeg's mpeg2video encoder) is decoded
// and compared with the pictures a reference decoder (ffmpeg's, with its "simple" inverse transform) made of it: the same pictures in the same order, each sample within
// a few levels of the reference's (the decoders' inverse transforms round differently, which the standard allows). Damaged streams (truncated, bits flipped) must not
// fault, hang or leak.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"

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

// Splits an elementary stream into units that each end before the next picture's headers (a sequence header, a group of pictures header or a picture header that
// follows slices starts a unit).
static std::vector<std::pair<size_t, size_t>> split_pictures(const std::vector<uint8_t>& d) {
  std::vector<std::pair<size_t, size_t>> units;
  size_t begin = 0;
  bool slices = false;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
    const int code = d[i + 3];
    if ((code == 0xB3 || code == 0xB8 || code == 0x00) && slices) {
      units.push_back({begin, i});
      begin = i;
      slices = false;
    }
    if (code >= 0x01 && code <= 0xAF) slices = true;
  }
  units.push_back({begin, d.size()});
  return units;
}

struct Result {
  int frames = 0;
  int max_diff = 0;
  double mean_diff = 0;
  uint32_t width = 0, height = 0;
};

static void collect(Mpeg2Decoder& dec, const std::vector<uint8_t>& ref, Result* r, double* sum, uint64_t* count) {
  VideoFrame f;
  while (dec.receive(&f)) {
    r->width = f.format.width;
    r->height = f.format.height;
    const size_t bytes = size_t(f.format.width) * f.format.height + 2 * size_t(f.format.chroma_width()) * f.format.chroma_height();
    size_t offset = size_t(r->frames) * bytes;
    if (!ref.empty() && offset + bytes <= ref.size()) {
      size_t p = offset;
      for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t y = 0; y < f.format.plane_height(c); ++y)
          for (uint32_t x = 0; x < f.format.plane_width(c); ++x) {
            const int d = abs(int(f.plane[c][size_t(y) * f.stride[c] + x]) - int(ref[p++]));
            if (d > r->max_diff) r->max_diff = d;
            *sum += d;
            ++*count;
          }
    } else {
      r->max_diff = 255;   // more pictures than the reference has
    }
    ++r->frames;
  }
}

// `group`: how many pictures go into one call.
static Result decode_stream(allocators::IAllocator& a, const std::vector<uint8_t>& stream, const std::vector<uint8_t>& ref, size_t group) {
  Result r;
  double sum = 0;
  uint64_t count = 0;
  Mpeg2Decoder dec(a);
  const auto units = split_pictures(stream);
  int64_t k = 0;
  for (size_t i = 0; i < units.size(); i += group) {
    const size_t last = i + group < units.size() ? i + group : units.size();
    dec.decode(ConstByteView(stream.data() + units[i].first, units[last - 1].second - units[i].first), k++ * 1000);
    collect(dec, ref, &r, &sum, &count);
  }
  dec.flush();
  collect(dec, ref, &r, &sum, &count);
  r.mean_diff = count ? sum / double(count) : 0;
  return r;
}

static void test_stream(const char* name) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/mpeg2_%s.m2v", name);
  const std::vector<uint8_t> stream = read_file(path);
  snprintf(path, sizeof path, "tests/data/mpeg2_%s.yuv", name);
  const std::vector<uint8_t> ref = read_file(path);
  CHECK(!stream.empty() && !ref.empty());
  if (stream.empty() || ref.empty()) return;
  TestAllocator a;
  {
    const Result r = decode_stream(a, stream, ref, 1);
    const size_t frame_bytes = size_t(r.width) * r.height + 2 * size_t((r.width + 1) / 2) * ((r.height + 1) / 2);
    printf("  %-16s %ux%u, %d pictures, largest difference %d, mean %.4f\n", name, r.width, r.height, r.frames, r.max_diff, r.mean_diff);
    CHECK(frame_bytes && size_t(r.frames) * frame_bytes == ref.size());
    CHECK(r.max_diff <= 3 && r.mean_diff < 0.05);
    // Two pictures in a call give the same pictures.
    const Result pairs = decode_stream(a, stream, ref, 2);
    CHECK(pairs.frames == r.frames && pairs.max_diff == r.max_diff);
  }
  CHECK(a.live == 0);
}

// Damaged streams: every picture cut short at many points, and single bits flipped all over the stream. Whatever the decoder makes of them, it must stay inside its
// buffers (the sanitizers say so), not hang, and not leak.
static void test_damaged(const char* name) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/mpeg2_%s.m2v", name);
  const std::vector<uint8_t> stream = read_file(path);
  CHECK(!stream.empty());
  if (stream.empty()) return;
  TestAllocator a;
  int runs = 0;
  {
    const auto units = split_pictures(stream);
    for (int variant = 0; variant < 40; ++variant) {
      Mpeg2Decoder dec(a);
      VideoFrame f;
      for (size_t u = 0; u < units.size(); ++u) {
        std::vector<uint8_t> au(stream.begin() + units[u].first, stream.begin() + units[u].second);
        if (variant % 2 == 1 && u > 0) au.resize(au.size() * (variant + 1) / 41 + 1);                                           // cut short
        else if (variant % 2 == 0 && u > 0) au[(size_t(variant) * 7919 + u * 104729) % au.size()] ^= uint8_t(1u << (variant % 8));   // one bit flipped
        dec.decode(ConstByteView(au.data(), au.size()), int64_t(u) * 1000);
        while (dec.receive(&f)) {}
        ++runs;
      }
      dec.flush();
      while (dec.receive(&f)) {}
    }
  }
  CHECK(runs > 100 && a.live == 0);
}

int main() {
  static const char* const streams[] = {"intra", "ipp", "ibp", "interlaced", "interlaced_bff", "alt", "dc10", "highrate", "lowrate", "matrix", "oddsize", "syn_frames", "syn_fields", "syn_fields_bff"};
  for (const char* name : streams) test_stream(name);
  for (const char* name : {"ibp", "interlaced", "alt", "highrate", "syn_fields", "syn_frames"}) test_damaged(name);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("Mpeg2Test passed\n");
  return 0;
}
