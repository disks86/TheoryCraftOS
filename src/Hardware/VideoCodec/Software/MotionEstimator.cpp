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

#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"

namespace hardware {

namespace {

inline int clampi(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline uint8_t clip_pixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
inline int iabs(int v) { return v < 0 ? -v : v; }

// The plane's samples with the edge repeated outside it.
struct EdgeReader {
  const LumaPlane& p;
  int at(int x, int y) const { return p.samples[static_cast<uint64_t>(clampi(0, p.height - 1, y)) * static_cast<uint64_t>(p.pitch) + static_cast<uint64_t>(clampi(0, p.width - 1, x))]; }
  // The unrounded six-tap sums: horizontal between (x, y) and (x + 1, y), vertical between (x, y) and (x, y + 1).
  int h1(int x, int y) const { return at(x - 2, y) - 5 * at(x - 1, y) + 20 * at(x, y) + 20 * at(x + 1, y) - 5 * at(x + 2, y) + at(x + 3, y); }
  int v1(int x, int y) const { return at(x, y - 2) - 5 * at(x, y - 1) + 20 * at(x, y) + 20 * at(x, y + 1) - 5 * at(x, y + 2) + at(x, y + 3); }
  int b(int x, int y) const { return clip_pixel((h1(x, y) + 16) >> 5); }
  int h(int x, int y) const { return clip_pixel((v1(x, y) + 16) >> 5); }
  int j(int x, int y) const {
    const int j1 = h1(x, y - 2) - 5 * h1(x, y - 1) + 20 * h1(x, y) + 20 * h1(x, y + 1) - 5 * h1(x, y + 2) + h1(x, y + 3);
    return clip_pixel((j1 + 512) >> 10);
  }
  // Table 8-12: the sample at (x + fx / 4, y + fy / 4).
  int sample(int x, int y, int fx, int fy) const {
    auto avg = [](int a, int c) { return (a + c + 1) >> 1; };
    switch (fy * 4 + fx) {
      case 0: return at(x, y);
      case 1: return avg(at(x, y), b(x, y));                // a
      case 2: return b(x, y);                               // b
      case 3: return avg(b(x, y), at(x + 1, y));            // c
      case 4: return avg(at(x, y), h(x, y));                // d
      case 5: return avg(b(x, y), h(x, y));                 // e
      case 6: return avg(b(x, y), j(x, y));                 // f
      case 7: return avg(b(x, y), h(x + 1, y));             // g
      case 8: return h(x, y);                               // h
      case 9: return avg(h(x, y), j(x, y));                 // i
      case 10: return j(x, y);                              // j
      case 11: return avg(j(x, y), h(x + 1, y));            // k
      case 12: return avg(h(x, y), at(x, y + 1));           // n
      case 13: return avg(h(x, y), b(x, y + 1));            // p
      case 14: return avg(j(x, y), b(x, y + 1));            // q
      default: return avg(h(x + 1, y), b(x, y + 1));        // r
    }
  }
};

}  // namespace

void h264_luma_prediction(const LumaPlane& plane, int x, int y, int fx, int fy, int w, int h, ByteView out) {
  const EdgeReader r{plane};
  if (fx == 0 && fy == 0 && x >= 0 && y >= 0 && x + w <= plane.width && y + h <= plane.height) {
    for (int row = 0; row < h; ++row)
      out.subview(static_cast<uint64_t>(row) * static_cast<uint64_t>(w), static_cast<uint64_t>(w))
          .copy_from(plane.samples.subview(static_cast<uint64_t>(y + row) * static_cast<uint64_t>(plane.pitch) + static_cast<uint64_t>(x), static_cast<uint64_t>(w)));
    return;
  }
  for (int row = 0; row < h; ++row)
    for (int col = 0; col < w; ++col) out[static_cast<uint64_t>(row * w + col)] = static_cast<uint8_t>(r.sample(x + col, y + row, fx, fy));
}

namespace {

class BlockSearch {
 public:
  BlockSearch(const LumaPlane& cur, const LumaPlane& ref, int bx, int by, int bw, int bh) : cur_(cur), ref_(ref), bx_(bx), by_(by), bw_(bw), bh_(bh) {}

  // The SAD of the block against the reference at vector v (quarter samples).
  uint32_t sad(MotionVector v) {
    const int x = bx_ + (v.x >> 2), y = by_ + (v.y >> 2);
    h264_luma_prediction(ref_, x, y, v.x & 3, v.y & 3, bw_, bh_, ByteView(pred_, sizeof pred_));
    uint32_t s = 0;
    for (int row = 0; row < bh_; ++row) {
      const ConstByteView c = cur_.samples.subview(static_cast<uint64_t>(by_ + row) * static_cast<uint64_t>(cur_.pitch) + static_cast<uint64_t>(bx_), static_cast<uint64_t>(bw_));
      for (int col = 0; col < bw_; ++col) s += static_cast<uint32_t>(iabs(int{c[static_cast<uint64_t>(col)]} - int{pred_[row * bw_ + col]}));
    }
    return s;
  }
  // Tries v; keeps it if it is strictly better.
  void consider(MotionVector v) {
    if (v.x < -8192 || v.x > 8188 || v.y < -2048 || v.y > 2044) return;
    const uint32_t s = sad(v);
    if (!have_ || s < best_.sad) {
      best_.vector = v;
      best_.sad = s;
      have_ = true;
    }
  }
  const MotionBlockResult& best() const { return best_; }

 private:
  const LumaPlane& cur_;
  const LumaPlane& ref_;
  int bx_, by_, bw_, bh_;
  uint8_t pred_[256];
  MotionBlockResult best_;
  bool have_ = false;
};

MotionVector whole(MotionVector v) { return {static_cast<int16_t>(v.x & ~3), static_cast<int16_t>(v.y & ~3)}; }

}  // namespace

bool estimate_motion(const LumaPlane& current, const LumaPlane& reference, int block, ArrayView<const MotionVector> hints, ArrayView<MotionBlockResult> out) {
  if ((block != 8 && block != 16) || current.width <= 0 || current.height <= 0 || current.width != reference.width || current.height != reference.height) return false;
  if (current.pitch < current.width || reference.pitch < reference.width) return false;
  if (current.samples.size() < static_cast<uint64_t>(current.pitch) * static_cast<uint64_t>(current.height - 1) + static_cast<uint64_t>(current.width)) return false;
  if (reference.samples.size() < static_cast<uint64_t>(reference.pitch) * static_cast<uint64_t>(reference.height - 1) + static_cast<uint64_t>(reference.width)) return false;
  const int cols = (current.width + block - 1) / block, rows = (current.height + block - 1) / block;
  const uint64_t count = static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows);
  if (out.size() < count || (!hints.empty() && hints.size() < count)) return false;
  for (int by = 0; by < rows; ++by) {
    for (int bx = 0; bx < cols; ++bx) {
      const uint64_t index = static_cast<uint64_t>(by) * static_cast<uint64_t>(cols) + static_cast<uint64_t>(bx);
      const int x = bx * block, y = by * block;
      const int w = x + block <= current.width ? block : current.width - x, h = y + block <= current.height ? block : current.height - y;
      BlockSearch s(current, reference, x, y, w, h);
      // 1. the candidates
      s.consider({0, 0});
      if (!hints.empty()) s.consider(whole(hints[index]));
      if (bx > 0) s.consider(whole(out[index - 1].vector));
      if (by > 0) s.consider(whole(out[index - static_cast<uint64_t>(cols)].vector));
      if (by > 0 && bx + 1 < cols) s.consider(whole(out[index - static_cast<uint64_t>(cols) + 1].vector));
      // 2. whole samples around the best candidate and around (0, 0)
      const MotionVector centre = s.best().vector;
      for (int pass = 0; pass < 2; ++pass) {
        const MotionVector c = pass == 0 ? centre : MotionVector{};
        if (pass == 1 && centre.x == 0 && centre.y == 0) break;
        for (int dy = -kWholeSearchRange; dy <= kWholeSearchRange; ++dy)
          for (int dx = -kWholeSearchRange; dx <= kWholeSearchRange; ++dx) s.consider({static_cast<int16_t>(c.x + 4 * dx), static_cast<int16_t>(c.y + 4 * dy)});
      }
      // 3. half, then quarter samples
      for (int step = 2; step >= 1; --step) {
        const MotionVector c = s.best().vector;
        static const int kAround[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (const auto& d : kAround) s.consider({static_cast<int16_t>(c.x + step * d[0]), static_cast<int16_t>(c.y + step * d[1])});
      }
      out[index] = s.best();
    }
  }
  return true;
}

}  // namespace hardware
