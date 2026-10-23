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

// Golden-image checks for the host tests (host only: it reads and writes files). A test renders something into 0xAARRGGBB pixels and
// asks golden::check() whether it matches tests/data/golden/<name>.png, allowing each colour channel to differ by `tolerance` (so
// a change of anti-aliasing does not fail a layout test, a change of a box position or colour does). Environment:
//   TCOS_UPDATE_GOLDEN=1   write the pixels as the new golden instead of comparing (review the PNG, then commit it)
//   TCOS_GOLDEN_DIR=dir    where the goldens live (default tests/data/golden, relative to the repository root: the test script runs there)
//   TCOS_GOLDEN_OUT=dir    where <name>.actual.png and <name>.diff.png go on a mismatch (default $TMPDIR/tcos-golden, else /tmp/tcos-golden)
// A missing golden is a failure (it says how to create it). The diff image is the golden dimmed with every differing pixel in red.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include "Media/Image.hpp"
#include "Media/ImageCodecs.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace golden {

struct Report {
  bool ok = false;
  bool wrote = false;           // the golden was (re)written (TCOS_UPDATE_GOLDEN)
  uint64_t differing = 0;       // pixels that differ by more than the tolerance
  uint32_t max_delta = 0;       // the largest single-channel difference seen
  std::string message;          // what to tell the person running the test
};

namespace detail {

inline std::string env(const char* name, const char* fallback) {
  const char* v = getenv(name);
  return v && *v ? v : fallback;
}

inline std::string out_dir() {
  const char* explicit_dir = getenv("TCOS_GOLDEN_OUT");
  if (explicit_dir && *explicit_dir) return explicit_dir;
  const char* tmp = getenv("TMPDIR");
  return std::string(tmp && *tmp ? tmp : "/tmp") + "/tcos-golden";
}

inline void make_dirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
  mkdir(path.c_str(), 0755);
}

inline bool read_file(const std::string& path, ByteBuffer* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  unsigned char chunk[4096];
  bool ok = true;
  for (;;) {
    const size_t n = fread(chunk, 1, sizeof(chunk), f);
    if (n == 0) break;
    ok = ok && out->append(ConstByteView(chunk, n));
  }
  fclose(f);
  return ok;
}

inline bool write_file(const std::string& path, ConstByteView bytes) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  fclose(f);
  return ok;
}

inline bool write_png(allocators::IAllocator& a, const std::string& path, ArrayView<const uint32_t> pixels, uint32_t w, uint32_t h) {
  media::Image image(a);
  if (!image.create(w, h)) return false;
  image.frame(0).copy_from(pixels);
  ByteBuffer png(a);
  return media::encode_png(image, 6, &png) && write_file(path, png.view());
}

inline uint32_t channel_delta(uint32_t x, uint32_t y, int shift) {
  const uint32_t a = (x >> shift) & 0xFF, b = (y >> shift) & 0xFF;
  return a > b ? a - b : b - a;
}

}  // namespace detail

// Compares `pixels` (w x h, 0xAARRGGBB, rows one after the other) with the golden called `name`.
inline Report check(allocators::IAllocator& a, const char* name, ArrayView<const uint32_t> pixels, uint32_t w, uint32_t h, uint32_t tolerance = 0) {
  Report r;
  const std::string golden_dir = detail::env("TCOS_GOLDEN_DIR", "tests/data/golden");
  const std::string golden_path = golden_dir + "/" + name + ".png";
  if (uint64_t{w} * h != pixels.size() || w == 0 || h == 0) {
    r.message = std::string(name) + ": the pixel count does not match " + std::to_string(w) + "x" + std::to_string(h);
    return r;
  }
  const char* update = getenv("TCOS_UPDATE_GOLDEN");
  if (update && *update && update[0] != '0') {
    detail::make_dirs(golden_dir);
    r.ok = r.wrote = detail::write_png(a, golden_path, pixels, w, h);
    r.message = r.ok ? std::string(name) + ": golden written to " + golden_path : std::string(name) + ": could not write " + golden_path;
    return r;
  }
  ByteBuffer file(a);
  if (!detail::read_file(golden_path, &file)) {
    r.message = std::string(name) + ": no golden at " + golden_path + " (run with TCOS_UPDATE_GOLDEN=1 to create it, look at it, then keep it)";
    return r;
  }
  media::Image expected(a);
  if (!media::decode_image(file.view(), &expected) || expected.empty()) {
    r.message = std::string(name) + ": the golden " + golden_path + " is not a readable image";
    return r;
  }
  if (expected.width() != w || expected.height() != h) {
    r.message = std::string(name) + ": the golden is " + std::to_string(expected.width()) + "x" + std::to_string(expected.height()) + " but the page drew " +
                std::to_string(w) + "x" + std::to_string(h);
    return r;
  }
  const ArrayView<uint32_t> want = expected.frame(0);
  media::Image diff(a);
  const bool have_diff = diff.create(w, h);
  for (uint64_t i = 0; i < pixels.size(); ++i) {
    uint32_t worst = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      const uint32_t d = detail::channel_delta(pixels[i], want[i], shift);
      if (d > worst) worst = d;
    }
    if (worst > r.max_delta) r.max_delta = worst;
    const bool bad = worst > tolerance;
    if (bad) ++r.differing;
    if (have_diff) diff.frame(0)[i] = bad ? 0xFFFF0000u : (0xFF000000u | ((want[i] >> 1) & 0x7F7F7Fu) | 0x808080u);
  }
  if (r.differing == 0) {
    r.ok = true;
    r.message = std::string(name) + ": matches";
    return r;
  }
  const std::string out = detail::out_dir();
  detail::make_dirs(out);
  const std::string actual_path = out + "/" + name + ".actual.png", diff_path = out + "/" + name + ".diff.png";
  detail::write_png(a, actual_path, pixels, w, h);
  if (have_diff) {
    ByteBuffer png(a);
    if (media::encode_png(diff, 6, &png)) detail::write_file(diff_path, png.view());
  }
  r.message = std::string(name) + ": " + std::to_string(r.differing) + " pixel(s) differ (largest channel difference " + std::to_string(r.max_delta) +
              ", tolerance " + std::to_string(tolerance) + "); see " + actual_path + " and " + diff_path;
  return r;
}

}  // namespace golden
