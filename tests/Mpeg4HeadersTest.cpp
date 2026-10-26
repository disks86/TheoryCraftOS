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

// Host-side tests for the MPEG-4 Part 2 header parser (Mpeg4Headers) on streams from FFmpeg's encoder (scripts/gen_mpeg4_test_streams.sh): the layer parameters (size,
// time base, interlacing, quarter-sample motion, the quantiser type and matrices), the plane types in decode order (B planes only where the encoder was told to use them,
// the first plane intra), the planes of every packet, and a parser that survives truncation and bit flips (under the sanitizers).
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/Software/Mpeg4Headers.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

// Splits a stream into packets: each ends before the first start code that follows a plane's data (so a GOP header or user data belongs to the plane after it).
static std::vector<std::pair<size_t, size_t>> split(const std::vector<uint8_t>& d) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 < d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { starts.push_back(i); i += 2; }
  std::vector<std::pair<size_t, size_t>> packets;
  size_t begin = 0;
  bool seen_plane = false;
  for (size_t k = 0; k < starts.size(); ++k) {
    const uint8_t code = d[starts[k] + 3];
    if (seen_plane) {   // (the data of a plane runs to the next start code, whatever it is)
      packets.push_back({begin, starts[k]});
      begin = starts[k];
      seen_plane = false;
    }
    if (code == 0xB6) seen_plane = true;
  }
  packets.push_back({begin, d.size()});
  return packets;
}

struct Result {
  mpeg4::Vol vol;
  std::vector<mpeg4::VopType> types;
  bool all_parsed = true;
  bool every_packet_one_plane = true;
};

static Result read_stream(const char* path) {
  Result r;
  const std::vector<uint8_t> d = read_file(path);
  CHECK(!d.empty());
  mpeg4::Vol current;
  bool have = false;
  for (const auto& p : split(d)) {
    mpeg4::Packet packet;
    if (!mpeg4::parse_packet(ConstByteView(d.data() + p.first, p.second - p.first), have ? &current : nullptr, &packet)) {
      r.all_parsed = false;
      continue;
    }
    if (packet.has_vol) {
      current = packet.vol;
      r.vol = packet.vol;
      have = true;
    }
    if (packet.vop_count != 1) r.every_packet_one_plane = false;
    if (packet.vop_count > 0) {
      r.types.push_back(packet.vop.type);
      CHECK(packet.vop.coded);
    }
  }
  return r;
}

static uint32_t count(const Result& r, mpeg4::VopType t) {
  uint32_t n = 0;
  for (mpeg4::VopType x : r.types) n += x == t;
  return n;
}

static void test_streams() {
  {
    const Result r = read_stream("tests/data/mpeg4_plain.m4v");
    CHECK(r.all_parsed && r.every_packet_one_plane && r.types.size() == 12 && r.types[0] == mpeg4::VopType::I);
    CHECK(count(r, mpeg4::VopType::B) == 0 && count(r, mpeg4::VopType::P) == 11 && r.vol.low_delay);   // (Simple Profile: no B planes)
    CHECK(r.vol.width == 160 && r.vol.height == 96 && r.vol.time_increment_resolution == 25 && r.vol.time_increment_bits == 5);
    CHECK(!r.vol.interlaced && !r.vol.quarter_sample && !r.vol.quant_type_mpeg && r.vol.verid == 1 && !r.vol.data_partitioned);
    CHECK(r.vol.intra_matrix[0] == 8 && r.vol.non_intra_matrix[0] == 16);   // (the defaults, in scan order: unused with the H.263 quantiser)
  }
  {
    const Result r = read_stream("tests/data/mpeg4_bframes.m4v");
    CHECK(r.all_parsed && r.every_packet_one_plane && r.types.size() == 12 && r.types[0] == mpeg4::VopType::I && count(r, mpeg4::VopType::B) > 0);
    CHECK(!r.vol.low_delay && r.vol.verid != 0 && r.vol.width == 160 && r.vol.height == 96);
    // In decode order a B plane follows an anchor that comes later in display order: the plane before the first B is an anchor.
    for (size_t i = 0; i < r.types.size(); ++i)
      if (r.types[i] == mpeg4::VopType::B) CHECK(i > 1);
  }
  {
    const Result r = read_stream("tests/data/mpeg4_mpegq.m4v");
    CHECK(r.all_parsed && r.vol.quant_type_mpeg);
    CHECK(r.vol.quarter_sample == false);
  }
  {
    const Result r = read_stream("tests/data/mpeg4_qpel.m4v");
    CHECK(r.all_parsed && r.vol.quarter_sample && r.vol.verid != 1);
  }
  {
    const Result r = read_stream("tests/data/mpeg4_interl.m4v");
    CHECK(r.all_parsed && r.vol.interlaced);
  }
  {
    // Custom matrices (given to the encoder in raster order; the stream sends them in scan order): the last values differ from the defaults.
    const Result r = read_stream("tests/data/mpeg4_matrix.m4v");
    CHECK(r.all_parsed && r.vol.quant_type_mpeg);
    CHECK(r.vol.intra_matrix[63] == 46 && r.vol.intra_matrix[0] == 8 && r.vol.non_intra_matrix[63] == 34 && r.vol.non_intra_matrix[0] == 16);
    for (uint32_t i = 0; i < 64; ++i) {
      CHECK(r.vol.intra_matrix[i] == mpeg4::kDefaultIntraMatrix[mpeg4::kZigzag[i]] + (mpeg4::kZigzag[i] == 63 ? 1 : 0));
      CHECK(r.vol.non_intra_matrix[i] == mpeg4::kDefaultNonIntraMatrix[mpeg4::kZigzag[i]] + (mpeg4::kZigzag[i] == 63 ? 1 : 0));
    }
  }
}

static void test_tables() {
  // The zigzag is a permutation, and the defaults are the standard's (corners and a few cells).
  bool seen[64] = {};
  for (uint32_t i = 0; i < 64; ++i) seen[mpeg4::kZigzag[i]] = true;
  bool all = true;
  for (bool s : seen) all = all && s;
  CHECK(all && mpeg4::kZigzag[0] == 0 && mpeg4::kZigzag[1] == 1 && mpeg4::kZigzag[2] == 8 && mpeg4::kZigzag[63] == 63);
  CHECK(mpeg4::kDefaultIntraMatrix[0] == 8 && mpeg4::kDefaultIntraMatrix[63] == 45 && mpeg4::kDefaultNonIntraMatrix[0] == 16 && mpeg4::kDefaultNonIntraMatrix[63] == 33);
}

// A header cut anywhere, or with any bit of its first bytes flipped, is refused or parsed - never read beyond the data.
static void test_damage() {
  const std::vector<uint8_t> d = read_file("tests/data/mpeg4_matrix.m4v");
  const auto packets = split(d);
  CHECK(packets.size() == 12);
  const size_t first_end = packets[0].second;
  uint32_t refused = 0;
  for (size_t cut = 0; cut < 400 && cut < first_end; ++cut) {
    mpeg4::Packet packet;
    if (!mpeg4::parse_packet(ConstByteView(d.data(), cut), nullptr, &packet)) ++refused;
  }
  CHECK(refused > 0);   // (a layer header cut short is refused)
  std::vector<uint8_t> copy(d.begin(), d.begin() + 400);
  uint32_t parsed = 0;
  for (size_t byte = 0; byte < 80; ++byte)
    for (uint32_t bit = 0; bit < 8; ++bit) {
      copy[byte] ^= static_cast<uint8_t>(1u << bit);
      mpeg4::Packet packet;
      if (mpeg4::parse_packet(ConstByteView(copy.data(), copy.size()), nullptr, &packet)) ++parsed;
      copy[byte] ^= static_cast<uint8_t>(1u << bit);
    }
  CHECK(parsed > 0);
  // No start codes, or a plane without a layer to say its time base: refused.
  const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
  mpeg4::Packet packet;
  CHECK(!mpeg4::parse_packet(ConstByteView(junk), nullptr, &packet) && !mpeg4::parse_packet(ConstByteView(), nullptr, &packet));
  const uint8_t plane_only[] = {0, 0, 1, 0xB6, 0x10, 0x20, 0x30};
  CHECK(!mpeg4::parse_packet(ConstByteView(plane_only), nullptr, &packet));
}

// Tools the parser does not take, set by flipping a bit of the layer header of the B-plane stream (an Advanced Simple one with no VBV parameters and no fixed rate, so the
// fields sit at known bit positions after the start code: object type at 1, shape at 26, sprite_enable at 78, not_8_bit at 80, complexity_estimation_disable at 83; found by
// reading the header by hand).
static void test_refused_tools() {
  std::vector<uint8_t> d = read_file("tests/data/mpeg4_bframes.m4v");
  size_t vol = 0;
  while (vol + 3 < d.size() && !(d[vol] == 0 && d[vol + 1] == 0 && d[vol + 2] == 1 && d[vol + 3] == 0x20)) ++vol;
  CHECK(vol + 3 < d.size());
  const size_t base = vol + 4;
  auto flip = [&](uint32_t bit) { d[base + bit / 8] ^= static_cast<uint8_t>(0x80 >> (bit % 8)); };
  mpeg4::Packet packet;
  CHECK(mpeg4::parse_packet(ConstByteView(d.data(), 200), nullptr, &packet) && packet.vol.width == 160);
  const uint32_t refused_bits[] = {27 /* shape: binary */, 79 /* sprite_enable: static */, 78 /* sprite_enable: GMC */, 80 /* not_8_bit */, 83 /* complexity estimation on */};
  for (uint32_t bit : refused_bits) {
    flip(bit);
    CHECK(!mpeg4::parse_packet(ConstByteView(d.data(), 200), nullptr, &packet));
    flip(bit);
  }
  CHECK(mpeg4::parse_packet(ConstByteView(d.data(), 200), nullptr, &packet));
}

// A layer header built bit by bit: quantiser matrices that stop early repeat their last value, and the low-delay default follows the object type.
struct BitWriter {
  std::vector<uint8_t> bytes;
  uint32_t used = 0;
  void put(uint32_t value, uint32_t n) {
    for (uint32_t i = n; i-- > 0;) {
      if (used % 8 == 0) bytes.push_back(0);
      bytes.back() |= static_cast<uint8_t>(((value >> i) & 1) << (7 - used % 8));
      ++used;
    }
  }
};

static std::vector<uint8_t> make_layer(uint32_t object_type, bool with_matrices) {
  BitWriter w;
  const uint8_t start[] = {0, 0, 1, 0x20};
  w.bytes.assign(start, start + 4);
  w.put(0, 1);             // random_accessible_vol
  w.put(object_type, 8);   // video_object_type_indication
  w.put(1, 1); w.put(5, 4); w.put(0, 3);   // is_object_layer_identifier, verid 5, priority
  w.put(1, 4);             // aspect_ratio_info
  w.put(0, 1);             // vol_control_parameters: none
  w.put(0, 2);             // shape: rectangular
  w.put(1, 1); w.put(30, 16); w.put(1, 1);   // marker, time increment resolution, marker
  w.put(0, 1);             // fixed_vop_rate
  w.put(1, 1); w.put(48, 13); w.put(1, 1); w.put(32, 13); w.put(1, 1);   // size
  w.put(0, 1); w.put(1, 1);   // interlaced, obmc_disable
  w.put(0, 2);             // sprite_enable
  w.put(0, 1);             // not_8_bit
  w.put(with_matrices ? 1 : 0, 1);   // quant_type
  if (with_matrices) {
    w.put(1, 1);   // load_intra_quant_mat: 9, 10, 11, then the end marker
    w.put(9, 8); w.put(10, 8); w.put(11, 8); w.put(0, 8);
    w.put(1, 1);   // load_nonintra_quant_mat: 20, then the end marker
    w.put(20, 8); w.put(0, 8);
  }
  w.put(0, 1);   // quarter_sample
  w.put(1, 1);   // complexity_estimation_disable
  w.put(1, 1);   // resync_marker_disable
  w.put(0, 1);   // data_partitioned
  w.put(0, 1); w.put(0, 1);   // newpred_enable, reduced_resolution_vop_enable
  w.put(0, 1);   // scalability
  return w.bytes;
}

static void test_built_layers() {
  mpeg4::Packet packet;
  {
    const std::vector<uint8_t> d = make_layer(17, true);
    CHECK(mpeg4::parse_packet(ConstByteView(d.data(), d.size()), nullptr, &packet) && packet.has_vol);
    const mpeg4::Vol& v = packet.vol;
    CHECK(v.width == 48 && v.height == 32 && v.time_increment_resolution == 30 && v.time_increment_bits == 5 && v.verid == 5 && v.obmc_disable && v.resync_marker_disable);
    CHECK(v.quant_type_mpeg && v.low_delay);   // (no control parameters: an Advanced Simple layer is taken as low delay, as FFmpeg does)
    // 9, 10, 11 and then 11 for the rest; 20 for the whole non-intra matrix.
    CHECK(v.intra_matrix[0] == 9 && v.intra_matrix[1] == 10 && v.intra_matrix[2] == 11 && v.intra_matrix[3] == 11 && v.intra_matrix[63] == 11);
    CHECK(v.non_intra_matrix[0] == 20 && v.non_intra_matrix[1] == 20 && v.non_intra_matrix[63] == 20);
  }
  {
    // Object type 1 is Simple (low delay too); another type is not.
    const std::vector<uint8_t> simple = make_layer(1, false), other = make_layer(3, false);
    CHECK(mpeg4::parse_packet(ConstByteView(simple.data(), simple.size()), nullptr, &packet) && packet.vol.low_delay && !packet.vol.quant_type_mpeg);
    CHECK(mpeg4::parse_packet(ConstByteView(other.data(), other.size()), nullptr, &packet) && !packet.vol.low_delay);
    // Without the quantiser matrices loaded the defaults stand, in scan order.
    CHECK(packet.vol.intra_matrix[0] == 8 && packet.vol.intra_matrix[1] == 17 && packet.vol.intra_matrix[2] == 17 && packet.vol.non_intra_matrix[2] == 17);
  }
  {
    // The studio object types have another header: refused.
    const std::vector<uint8_t> studio = make_layer(14, false);
    CHECK(!mpeg4::parse_packet(ConstByteView(studio.data(), studio.size()), nullptr, &packet));
  }
}

int main() {
  test_built_layers();
  test_tables();
  test_streams();
  test_damage();
  test_refused_tools();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("mpeg4 header tests passed\n");
  return 0;
}
