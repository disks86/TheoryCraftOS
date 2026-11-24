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


// Host-side tests for the H.265 decoder (Main, Main 10, Main Still Picture). Every stream in tests/data/hevc_*.h265 (made by scripts/gen_hevc_test_streams.sh with libx265, each carrying the encoder's MD5
// of every reconstructed picture) is decoded; the decoder checks every picture against that hash (H265Decoder::hash_statistics) and all of them must agree. Damaged
// streams (truncated, bits flipped) must not fault. Where tests/data/hevc_*.yuv exists the pictures are also compared with it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Crypto/Crypto.hpp"
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

struct Md5Hasher final : IPlaneHasher {
  void md5(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride, uint8_t out[16]) override {
    crypto::Hasher h(crypto::HashKind::Md5);
    for (uint32_t y = 0; y < height; ++y) h.update(ConstByteView(plane + size_t(y) * stride, width));
    h.finish(out);
  }
};
static Md5Hasher g_md5;

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
        if (pt >= 32 && pt <= 39) { cut = starts[j - 1]; --j; } else break;
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
  H265Decoder::HashStatistics hash;
};

// The MD5 of a P010 picture as FFmpeg's yuv420p10le (the frame MD5s of tests/data/hevc10_*.md5): every sample moved down to the low ten bits of its little-endian word,
// the planes Y, U, V one after the other.
static std::string p010_md5(const VideoFrame& f) {
  crypto::Hasher h(crypto::HashKind::Md5);
  const uint32_t w = f.format.width, hh = f.format.height;
  std::vector<uint8_t> row;
  for (uint32_t y = 0; y < hh; ++y) {
    row.clear();
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* q = f.plane[0] + size_t(y) * f.stride[0] + size_t(x) * 2;
      const uint16_t v = uint16_t((q[0] | (q[1] << 8)) >> 6);
      row.push_back(uint8_t(v)); row.push_back(uint8_t(v >> 8));
    }
    h.update(ConstByteView(row.data(), row.size()));
  }
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < hh / 2; ++y) {
      row.clear();
      for (uint32_t x = 0; x < w / 2; ++x) {
        const uint8_t* q = f.plane[1] + size_t(y) * f.stride[1] + size_t(x) * 4 + size_t(c) * 2;
        const uint16_t v = uint16_t((q[0] | (q[1] << 8)) >> 6);
        row.push_back(uint8_t(v)); row.push_back(uint8_t(v >> 8));
      }
      h.update(ConstByteView(row.data(), row.size()));
    }
  uint8_t d[16];
  h.finish(d);
  char hex[33];
  for (int i = 0; i < 16; ++i) snprintf(hex + 2 * i, 3, "%02x", d[i]);
  return hex;
}

static void collect(H265Decoder& dec, const std::vector<uint8_t>& ref, Result* r, size_t* frame_bytes_out, const std::vector<std::string>* md5s = nullptr) {
  VideoFrame f;
  while (dec.receive(&f)) {
    if (f.format.pixel_format == PixelFormat::P010) {
      // (a 10-bit picture: checked against FFmpeg's frame MD5s when there are some)
      r->width = f.format.width;
      r->height = f.format.height;
      if (md5s) {
        const bool same = size_t(r->frames) < md5s->size() && p010_md5(f) == (*md5s)[size_t(r->frames)];
        if (!same) {
          if (r->first_bad < 0) printf("    frame %d differs from the reference decode\n", r->frames);
          ++r->mismatched;
          if (r->first_bad < 0) r->first_bad = r->frames;
        }
      }
      ++r->frames;
      continue;
    }
    const size_t bytes = size_t(f.format.width) * f.format.height * 3 / 2;
    *frame_bytes_out = bytes;
    r->width = f.format.width;
    r->height = f.format.height;
    if (getenv("H265_DUMP")) {
      char name[200]; snprintf(name, sizeof name, "%s.%d.yuv", getenv("H265_DUMP"), r->frames);
      if (FILE* o = fopen(name, "wb")) { for (uint32_t pl = 0; pl < 3; ++pl) for (uint32_t y = 0; y < f.format.plane_height(pl); ++y) fwrite(f.plane[pl] + size_t(y) * f.stride[pl], 1, f.format.plane_width(pl), o); fclose(o); }
    }
    const size_t off = size_t(r->frames) * bytes;
    bool same = ref.empty() || off + bytes <= ref.size();
    size_t p = off;
    for (uint32_t pl = 0; pl < 3 && same && !ref.empty(); ++pl)
      for (uint32_t y = 0; y < f.format.plane_height(pl) && same; ++y)
        for (uint32_t x = 0; x < f.format.plane_width(pl); ++x, ++p)
          if (f.plane[pl][size_t(y) * f.stride[pl] + x] != ref[p]) {
            if (getenv("H265_VERBOSE") || r->first_bad < 0 || r->frames < r->first_bad) {
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

static Result decode_stream(allocators::IAllocator& a, const std::vector<uint8_t>& stream, const std::vector<uint8_t>& ref, const std::vector<std::string>* md5s = nullptr) {
  Result r;
  H265Decoder dec(a);
  dec.set_plane_hasher(&g_md5);
  size_t fb = 0;
  for (auto u : split_pictures(stream)) {
    dec.decode(ConstByteView(stream.data() + u.first, u.second - u.first), int64_t(r.frames) * 1000);
    collect(dec, ref, &r, &fb, md5s);
  }
  dec.flush();
  collect(dec, ref, &r, &fb, md5s);
  r.hash = dec.hash_statistics();
  return r;
}

// Damaged streams: every picture cut short at many points, and single bits flipped all over the stream. Whatever the decoder makes of them, it must stay inside its
// buffers (the sanitizers say so), not hang, and not leak.
static void test_damaged(const char* name) {
  char path[160];
  const char* dir = getenv("H265_DIR") ? getenv("H265_DIR") : "tests/data";
  snprintf(path, sizeof path, "%s/%s%s.h265", dir, strncmp(name, "hevc", 4) == 0 ? "" : "hevc_", name);
  const std::vector<uint8_t> stream = read_file(path);
  CHECK(!stream.empty());
  if (stream.empty()) return;
  TestAllocator a;
  int runs = 0;
  const auto units = split_pictures(stream);
  {
    for (int variant = 0; variant < 40; ++variant) {
      H265Decoder dec(a);
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
  CHECK(runs == 40 * int(units.size()) && units.size() >= 2);
  CHECK(a.live == 0);
}

static const char* const kStreams[] = {"lossless", "cra", "cip", "highrate", "lowrate", "maxref", "oddsize", "qcif", "intra", "intra_filt", "ipp", "ipp_filt", "bframes", "weighted", "slices", "ctu64", "tools", "scaling", "wpp", "cutree_off"};

struct Stream10 {
  const char* name;
  int frames;
  bool hashed;   // the stream carries a decoded picture hash for every picture
};
static const Stream10 kStreams10[] = {
    {"hevc10_intra", 12, true},    {"hevc10_intra_raw", 12, true}, {"hevc10_ipp", 12, true},     {"hevc10_bframes", 12, true}, {"hevc10_weighted", 12, true},
    {"hevc10_tools", 12, true},    {"hevc10_scaling", 12, true},   {"hevc10_lossless", 12, true}, {"hevc10_culossless", 12, true}, {"hevc10_sao", 12, true},
    {"hevc10_lowrate", 12, true},  {"hevc10_highrate", 12, true},  {"hevc10_chromaqp", 12, true}, {"hevc10_slices", 12, true},  {"hevc10_wpp", 12, true},
    {"hevc10_cip", 12, true},      {"hevc10_cra", 12, true},       {"hevc10_oddsize", 12, true},  {"hevc10_still", 1, true},    {"hevc_still", 1, true},
    {"hevc10_pcm", 2, false},
};

// The range extension streams: every chroma format at 8 to 12 bits (tests/data/hevcrext_*.h265, scripts/gen_hevc_rext_test_streams.sh, x265's), and the range extension
// tools (tests/data/hevcrexttool_*.h265, scripts/gen_hevc_rext_tool_streams.py). Every picture carries an MD5 of each of its planes - x265's of its reconstruction, and in
// the tool streams FFmpeg's decode of them, checked against libde265's - which the decoder checks over its own planes (the 4:2:0 frames it outputs say less for 4:2:2 and
// 4:4:4: their chroma is averaged down).
static const Stream10 kStreamsRext[] = {
    {"hevcrext_400_8", 8, true},      {"hevcrext_400_12", 8, true},     {"hevcrext_420_12", 8, true},    {"hevcrext_420_12_lossless", 8, true}, {"hevcrext_422_8", 8, true},
    {"hevcrext_422_10", 8, true},     {"hevcrext_422_10_intra", 4, true}, {"hevcrext_422_12", 8, true},  {"hevcrext_444_8", 8, true},           {"hevcrext_444_8_intra", 4, true},
    {"hevcrext_444_10", 8, true},     {"hevcrext_444_12", 8, true},     {"hevcrext_444_12_ctu64", 6, true}, {"hevcrext_422_10_oddsize", 6, true},
    {"hevcrext_444_10_scaling", 4, true},
    {"hevcrexttool_444_8", 3, true},  {"hevcrexttool_444_8_lossless", 3, true}, {"hevcrexttool_444_12", 3, true}, {"hevcrexttool_422_10", 3, true},
    {"hevcrexttool_420_8", 3, true},  {"hevcrexttool_400_10", 3, true}, {"hevcrexttool_444_10_lossless", 3, true}, {"hevcrexttool_444_12_hpo", 3, true},
    {"hevcrexttool_422_10_sao_lossless", 3, true}, {"hevcrexttool_400_12_hpo", 3, true},
};

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (const char* name : kStreams) {
    if (argc > 1 && strcmp(argv[1], name) != 0) continue;
    { char probe[160]; snprintf(probe, sizeof probe, "%s/hevc_%s.h265", getenv("H265_DIR") ? getenv("H265_DIR") : "tests/data", name); FILE* t = fopen(probe, "rb"); if (!t) continue; fclose(t); }
    char path[160];
    const char* dir = getenv("H265_DIR") ? getenv("H265_DIR") : "tests/data";
    snprintf(path, sizeof path, "%s/hevc_%s.h265", dir, name);
    const std::vector<uint8_t> stream = read_file(path);
    CHECK(!stream.empty());
    if (stream.empty()) continue;
    snprintf(path, sizeof path, "%s/hevc_%s.yuv", dir, name);
    const std::vector<uint8_t> ref = read_file(path);   // (optional)
    TestAllocator a;
    {
      const Result r = decode_stream(a, stream, ref);
      printf("  %-12s %d pictures, %llu checked against the stream's hashes, %llu differ%s\n", name, r.frames, (unsigned long long)r.hash.checked,
             (unsigned long long)r.hash.mismatched, ref.empty() ? "" : (r.mismatched ? " (and some from the reference pictures)" : ""));
      CHECK(r.frames == 12 && r.hash.checked == uint64_t(r.frames) && r.hash.mismatched == 0);
    }
    CHECK(a.live == 0);
  }
  // Main 10 (tests/data/hevc10_*.h265, scripts/gen_hevc10_test_streams.sh): the stream's own MD5s over the 16-bit samples, and the reference decode (hevc10_*.md5: FFmpeg's,
  // libde265's for hevc10_pcm) of the P010 pictures; the Still Picture streams have one picture.
  for (const Stream10& st : kStreams10) {
    if (argc > 1 && strcmp(argv[1], st.name) != 0) continue;
    const char* dir = getenv("H265_DIR") ? getenv("H265_DIR") : "tests/data";
    char path[160];
    snprintf(path, sizeof path, "%s/%s.h265", dir, st.name);
    const std::vector<uint8_t> stream = read_file(path);
    CHECK(!stream.empty());
    if (stream.empty()) continue;
    std::vector<std::string> md5s;
    snprintf(path, sizeof path, "%s/%s.md5", dir, st.name);
    if (FILE* m = fopen(path, "r")) {
      char line[80];
      while (fgets(line, sizeof line, m))
        if (strlen(line) >= 32) md5s.push_back(std::string(line, 32));
      fclose(m);
    }
    const bool ten_bit = strncmp(st.name, "hevc10_", 7) == 0;
    CHECK(!ten_bit || md5s.size() == size_t(st.frames));
    TestAllocator a;
    {
      const Result r = decode_stream(a, stream, {}, ten_bit ? &md5s : nullptr);
      printf("  %-16s %d pictures, %llu checked against the stream's hashes, %llu differ; %d differ from the reference\n", st.name, r.frames, (unsigned long long)r.hash.checked,
             (unsigned long long)r.hash.mismatched, r.mismatched);
      CHECK(r.frames == st.frames && r.mismatched == 0 && r.hash.mismatched == 0);
      CHECK(r.hash.checked == uint64_t(st.hashed ? r.frames : 0));
    }
    CHECK(a.live == 0);
  }
  for (const Stream10& st : kStreamsRext) {
    if (argc > 1 && strcmp(argv[1], st.name) != 0) continue;
    const char* dir = getenv("H265_DIR") ? getenv("H265_DIR") : "tests/data";
    char path[160];
    snprintf(path, sizeof path, "%s/%s.h265", dir, st.name);
    const std::vector<uint8_t> stream = read_file(path);
    CHECK(!stream.empty());
    if (stream.empty()) continue;
    TestAllocator a;
    {
      const Result r = decode_stream(a, stream, {});
      printf("  %-32s %d pictures, %llu checked against the stream's hashes, %llu differ\n", st.name, r.frames, (unsigned long long)r.hash.checked,
             (unsigned long long)r.hash.mismatched);
      CHECK(r.frames == st.frames && r.hash.mismatched == 0);
      CHECK(r.hash.checked == uint64_t(st.hashed ? r.frames : 0));
    }
    CHECK(a.live == 0);
  }
  if (argc == 1) {
    test_damaged("hevcrext_422_10");
    test_damaged("hevcrext_444_8");
    test_damaged("hevcrext_444_12");
    test_damaged("hevcrext_400_12");
    test_damaged("hevcrexttool_444_8");
    test_damaged("hevcrexttool_444_12");
    test_damaged("hevcrexttool_422_10_sao_lossless");
    test_damaged("hevc10_tools");
    test_damaged("hevc10_sao");
    test_damaged("hevc10_lossless");
    test_damaged("hevc10_pcm");
    test_damaged("slices");
    test_damaged("tools");
    test_damaged("cra");
    test_damaged("ctu64");
    test_damaged("lossless");
  }
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("H265Test passed\n");
  return 0;
}
