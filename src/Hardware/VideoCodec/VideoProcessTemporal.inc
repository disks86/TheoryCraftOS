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
// The video processor's temporal stages (VideoProcessStages.inc includes this): motion compensated deinterlacing and inverse telecine.
//
// Motion compensated deinterlacing. A missing field row at time t (in fields) is bwdif's interpolation (ffmpeg's: yadif's temporal decision with w3fdif's filters, both
// across the current field and from the fields around it), but every sample it takes from another field is taken along the motion of the 8x8 block of the missing
// field it lies in instead of at the same place:
//   1. The missing field's neighbours in time - the field of the missing parity before (t - 1) and after (t + 1), yadif's prev2 and next2 - are made 8-bit pictures of
//      their own (luma; G-weighted for RGB) and Software/MotionEstimator estimates the vectors of the later's 8x8 blocks into the earlier: the motion over two fields.
//   2. Each block of the missing field at time t takes, of the candidates - no motion, its own vector and its four neighbours' -, the one whose bilateral match is best:
//      the least sum of absolute differences between the earlier field half a vector ahead and the later field half a vector back (what is at the block at time t was
//      there at t - 1 and t + 1 if it moves steadily). Ties keep no motion.
//   3. The block's samples are bwdif's with the field at t - 1 read half a vector ahead, the one at t + 1 half a vector back, the frames at t - 2 and t + 2 (the shown
//      parity's rows the temporal change is checked with) a whole vector ahead and back - between rows and columns by Catmull-Rom cubics, rows of one field. Without
//      motion they are bwdif's; where the motion is wrong the temporal differences grow and the clamp falls back to the spatial interpolation.
// The quality reference is the progressive original of a moving picture (VideoProcessTest test_mc_deinterlace): its PSNR against it is compared with ffmpeg's yadif,
// bwdif, w3fdif and mcdeint (which compensates motion with the snow encoder's estimator) on the same fields (tests/data/videoprocess/mcdeint_ffmpeg.sh).
//
// Inverse telecine (ffmpeg's fieldmatch then decimate, the usual pair, made one stage). A cycle of telecined frames - 3:2 pulldown's five frames of four films frames,
// 2:3:3:2 and 2:2:2:4 the same, 2:2 one frame of one - is made progressive again:
//   1. field matching: every frame keeps its first field (the top one of top field first content) and takes the other field from itself, the frame before or the frame
//      after (fieldmatch's c, p and n matches), whichever combs least - the sum over the kept field's neighbouring rows a, c and the row b between them of
//      max(0, (b - a)(b - c)) on luma. Ties keep c, then p. Every film frame has fields of both parities next to each other in time, so each one is matched whole.
//   2. decimation (cycle frames - outputs of them): the frames most like the frame before them (the largest sum of absolute luma differences of a 32x32 block, ffmpeg
//      decimate's measure) are the duplicates the pulldown made and are dropped; the first of equal ones goes. At the ends of a stream a frame not given (the nearest
//      one stands in) is dropped first, and the first frame given is no frame's duplicate.
//   3. a frame still combed after matching (fieldmatch's comb test: frame_combed; an orphan field at an edit) is deinterlaced instead - its own first field, motion
//      adaptively with the frames around it.
// The output of a clean telecine is the film frames bit for bit, as ffmpeg's `fieldmatch,decimate` gives them (VideoProcessTest test_inverse_telecine).

// Sample (x, y) of a view - x a fractional column, y a fractional row - from the rows of parity `parity` only (one field): Catmull-Rom cubic interpolation across
// columns and across that field's rows (four taps each, clamped to the field). At whole positions of the field it is the sample itself.
float cubic_weight(float d) {   // (Catmull-Rom, a = -0.5)
  d = d < 0.0f ? -d : d;
  if (d < 1.0f) return (1.5f * d - 2.5f) * d * d + 1.0f;
  if (d < 2.0f) return ((-0.5f * d + 2.5f) * d - 4.0f) * d + 2.0f;
  return 0.0f;
}
float field_sample(const ViewPlane& v, float x, float y, int32_t parity, uint32_t c) {
  const int32_t rows = (v.view_h - parity + 1) / 2;
  if (rows <= 0) return v.at(0, 0, c);
  float fr = (y - static_cast<float>(parity)) * 0.5f;
  const float top_x = static_cast<float>(v.view_w - 1), top_r = static_cast<float>(rows - 1);
  x = x > 0.0f ? (x < top_x ? x : top_x) : 0.0f;
  fr = fr > 0.0f ? (fr < top_r ? fr : top_r) : 0.0f;
  const int32_t x0 = static_cast<int32_t>(x), r0 = static_cast<int32_t>(fr);   // (non-negative: truncation is the floor)
  const float wx = x - static_cast<float>(x0), wy = fr - static_cast<float>(r0);
  if (wx == 0.0f && wy == 0.0f) return v.at(x0, 2 * r0 + parity, c);
  auto row = [&](int32_t r) {
    r = r < 0 ? 0 : r >= rows ? rows - 1 : r;
    const int32_t y_row = 2 * r + parity;
    if (wx == 0.0f) return v.at(x0, y_row, c);
    float sum = 0.0f;
    for (int32_t k = -1; k <= 2; ++k) sum += cubic_weight(static_cast<float>(k) - wx) * v.at(x0 + k, y_row, c);   // (v.at clamps the columns)
    return sum;
  };
  if (wy == 0.0f) return row(r0);
  float sum = 0.0f;
  for (int32_t k = -1; k <= 2; ++k) sum += cubic_weight(static_cast<float>(k) - wy) * row(r0 + k);
  return sum;
}

// Memory of `count` elements from the allocator, given back when it goes.
template <class T>
struct ScratchArray {
  allocators::IAllocator* allocator = nullptr;
  ArrayView<T> data;
  ScratchArray() = default;
  ScratchArray(const ScratchArray&) = delete;
  ScratchArray& operator=(const ScratchArray&) = delete;
  ~ScratchArray() { if (allocator && !data.empty()) release_view(*allocator, data); }
  [[nodiscard]] bool make(allocators::IAllocator& a, uint64_t count) {
    allocator = &a;
    data = allocate_view<T>(a, count);
    return !data.empty();
  }
};

// The motion of the missing field's blocks: per 8x8 block of it (in its own rows), the offset of what is there at time t in the field before (t - 1), in samples of
// plane 0 and rows of the frame; the field after has it at minus that.
struct McVectors {
  ScratchArray<float> dx, dy;
  int32_t blocks_x = 0, blocks_y = 0;
  int32_t parity = 0;   // the missing rows' parity (in the view's rows)
  bool any = false;     // some block moves

  // The offset of plane k's sample (i, j) - a plane `ratio_x`, `ratio_y` times smaller than plane 0 -, in that plane's samples and rows.
  void at(int32_t i, int32_t j, float ratio_x, float ratio_y, float* ox, float* oy) const {
    int32_t bx = static_cast<int32_t>(static_cast<float>(i) * ratio_x) / 8, by = static_cast<int32_t>(static_cast<float>(j) * ratio_y) / 16;
    bx = bx < 0 ? 0 : bx >= blocks_x ? blocks_x - 1 : bx;
    by = by < 0 ? 0 : by >= blocks_y ? blocks_y - 1 : by;
    const uint64_t n = static_cast<uint64_t>(by) * static_cast<uint64_t>(blocks_x) + static_cast<uint64_t>(bx);
    *ox = dx.data[n] / ratio_x;
    *oy = dy.data[n] / ratio_y;
  }
};

// An 8-bit picture of one field of a plane-0 view (rows of `parity`), for the motion estimator: the luma code (RGB: (R + 2 G + B) / 4) scaled to 0..255.
[[nodiscard]] bool field_proxy(const ImageAccess& img, const ViewPlane& v, int32_t parity, allocators::IAllocator& a, ScratchArray<uint8_t>* out, int32_t* rows) {
  *rows = (v.view_h - parity + 1) / 2;
  if (*rows <= 0) return false;
  if (!out->make(a, static_cast<uint64_t>(v.view_w) * static_cast<uint64_t>(*rows))) return false;
  const float scale = img.is_float ? 255.0f : 255.0f / img.max[0];
  uint64_t at = 0;
  for (int32_t r = 0; r < *rows; ++r)
    for (int32_t x = 0; x < v.view_w; ++x) {
      const int32_t y = 2 * r + parity;
      const float code = img.yuv ? v.at(x, y, 0) : (v.at(x, y, 0) + 2.0f * v.at(x, y, 1) + v.at(x, y, 2)) * 0.25f;
      float q = code * scale + 0.5f;
      q = q > 0.0f ? (q < 255.0f ? q : 255.0f) : 0.0f;
      out->data[at++] = static_cast<uint8_t>(q);
    }
  return true;
}

float proxy_at(const ScratchArray<uint8_t>& p, int32_t w, int32_t h, float x, float y) {
  const float tx = static_cast<float>(w - 1), ty = static_cast<float>(h - 1);
  x = x > 0.0f ? (x < tx ? x : tx) : 0.0f;
  y = y > 0.0f ? (y < ty ? y : ty) : 0.0f;
  const int32_t x0 = static_cast<int32_t>(x), y0 = static_cast<int32_t>(y), x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
  const float wx = x - static_cast<float>(x0), wy = y - static_cast<float>(y0);
  auto s = [&](int32_t xx, int32_t yy) { return static_cast<float>(p.data[static_cast<uint64_t>(yy) * static_cast<uint64_t>(w) + static_cast<uint64_t>(xx)]); };
  return (s(x0, y0) * (1.0f - wx) + s(x1, y0) * wx) * (1.0f - wy) + (s(x0, y1) * (1.0f - wx) + s(x1, y1) * wx) * wy;
}

// Step 1 and 2 of motion compensated deinterlacing for plane 0: `prev2` and `next2` the views of the missing field before and after.
[[nodiscard]] VpResult estimate_field_motion(const ImageAccess& img, const ViewPlane& prev2, const ViewPlane& next2, int32_t parity, allocators::IAllocator& a, McVectors* out) {
  ScratchArray<uint8_t> earlier, later;
  int32_t rows = 0, rows2 = 0;
  if (!field_proxy(img, prev2, parity, a, &earlier, &rows) || !field_proxy(img, next2, parity, a, &later, &rows2)) return VpResult::NoMemory;
  const int32_t w = prev2.view_w;
  out->parity = parity;
  out->blocks_x = (w + 7) / 8;
  out->blocks_y = (rows + 7) / 8;
  const uint64_t blocks = static_cast<uint64_t>(out->blocks_x) * static_cast<uint64_t>(out->blocks_y);
  ScratchArray<MotionBlockResult> found;
  if (!found.make(a, blocks) || !out->dx.make(a, blocks) || !out->dy.make(a, blocks)) return VpResult::NoMemory;
  const LumaPlane cur{ConstByteView(later.data.data(), later.data.size()), w, rows, w};
  const LumaPlane ref{ConstByteView(earlier.data.data(), earlier.data.size()), w, rows, w};
  if (!estimate_motion(cur, ref, 8, ArrayView<const MotionVector>(), found.data)) return VpResult::Invalid;
  out->any = false;
  for (int32_t by = 0; by < out->blocks_y; ++by)
    for (int32_t bx = 0; bx < out->blocks_x; ++bx) {
      // the candidates (quarter samples of the field: the motion from t + 1 back to t - 1)
      MotionVector candidates[6];
      int count = 0;
      candidates[count++] = MotionVector{};
      auto add = [&](int32_t x, int32_t y) {
        if (x < 0 || y < 0 || x >= out->blocks_x || y >= out->blocks_y) return;
        candidates[count++] = found.data[static_cast<uint64_t>(y) * static_cast<uint64_t>(out->blocks_x) + static_cast<uint64_t>(x)].vector;
      };
      add(bx, by);
      add(bx - 1, by);
      add(bx + 1, by);
      add(bx, by - 1);
      add(bx, by + 1);
      const int32_t x0 = bx * 8, y0 = by * 8, x1 = x0 + 8 < w ? x0 + 8 : w, y1 = y0 + 8 < rows ? y0 + 8 : rows;
      float best = 0.0f;
      int chosen = 0;
      for (int k = 0; k < count; ++k) {
        const float ox = static_cast<float>(candidates[k].x) * 0.125f, oy = static_cast<float>(candidates[k].y) * 0.125f;   // (half the vector, in field samples)
        float sad = 0.0f;
        for (int32_t y = y0; y < y1; ++y)
          for (int32_t x = x0; x < x1; ++x) {
            const float d = proxy_at(earlier, w, rows, static_cast<float>(x) + ox, static_cast<float>(y) + oy) -
                            proxy_at(later, w, rows, static_cast<float>(x) - ox, static_cast<float>(y) - oy);
            sad += d < 0.0f ? -d : d;
          }
        if (k == 0 || sad < best) { best = sad; chosen = k; }
      }
      const uint64_t n = static_cast<uint64_t>(by) * static_cast<uint64_t>(out->blocks_x) + static_cast<uint64_t>(bx);
      out->dx.data[n] = static_cast<float>(candidates[chosen].x) * 0.125f;   // (plane-0 samples)
      out->dy.data[n] = static_cast<float>(candidates[chosen].y) * 0.25f;    // (frame rows: two a field row)
      if (candidates[chosen].x || candidates[chosen].y) out->any = true;
    }
  return VpResult::Ok;
}

// Step 3: bwdif's interpolation of the missing sample (i, j) with the other fields read along (ox, oy) - the offset at t - 1, in this plane's samples and rows - and the
// current field's rows where they are. (bwdif: ffmpeg's libavfilter/bwdif.c, yadif's decision with w3fdif's filters: coef_lf {4309, 213}, coef_hf {5570, 3801,
// 1016}, coef_sp {5077, 981} over 8192; in floats here.)
struct McBwdif {
  const Yadif* base;   // (its views, size and integer rule)

  float sample(int32_t i, int32_t j, uint32_t c, float ox, float oy) const {
    const Yadif& y = *base;
    const int32_t h = y.h;
    auto row = [&](int32_t r) { return r < 0 ? -r : r >= h ? 2 * (h - 1) - r : r; };   // (mirrored at the picture's edges, as yadif's references)
    const int32_t m = row(j - 1), p = row(j + 1), m3 = row(j - 3), p3 = row(j + 3);
    const int32_t miss = j & 1;
    const float fi = static_cast<float>(i), fj = static_cast<float>(j);
    auto before = [&](float dy) { return field_sample(*y.prev2, fi + ox, fj + dy + oy, miss, c); };   // (the missing parity's field at t - 1, along the motion)
    auto after = [&](float dy) { return field_sample(*y.next2, fi - ox, fj + dy - oy, miss, c); };
    const float cv = y.cur->at(i, m, c), ev = y.cur->at(i, p, c);
    const float b0 = before(0.0f), a0 = after(0.0f);
    const float d = (b0 + a0) * 0.5f;
    const float td0 = Yadif::fabs_f(b0 - a0);
    const float td1 = (Yadif::fabs_f(field_sample(*y.prev, fi + 2 * ox, static_cast<float>(m) + 2 * oy, m & 1, c) - cv) +
                       Yadif::fabs_f(field_sample(*y.prev, fi + 2 * ox, static_cast<float>(p) + 2 * oy, p & 1, c) - ev)) * 0.5f;
    const float td2 = (Yadif::fabs_f(field_sample(*y.next, fi - 2 * ox, static_cast<float>(m) - 2 * oy, m & 1, c) - cv) +
                       Yadif::fabs_f(field_sample(*y.next, fi - 2 * ox, static_cast<float>(p) - 2 * oy, p & 1, c) - ev)) * 0.5f;
    float diff = Yadif::max3(td0 * 0.5f, td1, td2);
    if (diff == 0.0f) return d;
    const float bm2 = (before(-2.0f) + after(-2.0f)) * 0.5f, bp2 = (before(2.0f) + after(2.0f)) * 0.5f;
    {   // (the spatial interlacing check)
      const float bb = bm2 - cv, ff = bp2 - ev, dc = d - cv, de = d - ev;
      const float mx = Yadif::max3(de, dc, Yadif::min2(bb, ff)), mn = Yadif::min3(de, dc, Yadif::max2(bb, ff));
      diff = Yadif::max3(diff, mn, -mx);
    }
    const float c3 = y.cur->at(i, m3, c), e3 = y.cur->at(i, p3, c);
    float interpol;
    if (Yadif::fabs_f(cv - ev) > td0) {
      const float hf = 5570.0f * (b0 + a0) - 3801.0f * (before(-2.0f) + after(-2.0f) + before(2.0f) + after(2.0f)) +
                       1016.0f * (before(-4.0f) + after(-4.0f) + before(4.0f) + after(4.0f));
      interpol = (hf * 0.25f + 4309.0f * (cv + ev) - 213.0f * (c3 + e3)) / 8192.0f;
    } else {
      interpol = (5077.0f * (cv + ev) - 981.0f * (c3 + e3)) / 8192.0f;
    }
    if (interpol > d + diff) interpol = d + diff;
    else if (interpol < d - diff) interpol = d - diff;
    return interpol;
  }
};

// ---- inverse telecine

// The luma (RGB: green) channel of a picture's plane 0, and its range.
uint32_t luma_channel(const ImageAccess& img) { return img.yuv ? 0u : 1u; }
float luma_range(const ImageAccess& img) { return img.is_float ? 1.0f : img.max[0]; }

// How much the frame woven of `kept`'s rows of parity `kp` and `other`'s rows of the other parity combs (step 1's sum).
double comb_of(const ImageAccess& kept, const ImageAccess& other, int32_t kp) {
  const PlaneAccess& k = kept.plane[0];
  const PlaneAccess& o = other.plane[0];
  const uint32_t c = luma_channel(kept), w = k.width, h = k.height;
  const float range = luma_range(kept);
  double sum = 0.0;
  for (uint32_t y = 1; y + 1 < h; ++y) {
    if (static_cast<int32_t>(y & 1) == kp) continue;   // (rows of the taken field, between two kept ones)
    for (uint32_t x = 0; x < w; ++x) {
      const float a = k.read(x, y - 1, c), b = o.read(x, y, c), d = k.read(x, y + 1, c);
      const float v = (b - a) * (b - d);
      if (v > 0.0f) sum += v;
    }
  }
  return sum / (static_cast<double>(range) * range);
}

// The woven frame's sample.
float woven(const ImageAccess& kept, const ImageAccess& other, int32_t kp, uint32_t plane, uint32_t x, uint32_t y, uint32_t c) {
  return static_cast<int32_t>(y & 1) == kp ? kept.plane[plane].read(x, y, c) : other.plane[plane].read(x, y, c);
}

// Whether a woven frame is combed, by ffmpeg fieldmatch's test (TIVTC's): a luma sample whose rows above and below differ from it by more than cthresh (9 of 255) the
// same way, with |a2 + 4 b + c2 - 3 (a + c)| over 6 cthresh (the rows two away: of its own field), is combed; the frame is when a 16x16 block (blocks overlapping by
// half) has more than combpel (80) of them. `no_memory`: the count could not be kept.
bool frame_combed(const ImageAccess& kept, const ImageAccess& other, int32_t kp, allocators::IAllocator& a, bool* no_memory) {
  *no_memory = false;
  const uint32_t c = luma_channel(kept), w = kept.plane[0].width, h = kept.plane[0].height;
  const float t = 9.0f / 255.0f * luma_range(kept);
  const uint32_t cx = (w + 7) / 8, cy = (h + 7) / 8;
  ScratchArray<uint32_t> cells;
  if (!cells.make(a, uint64_t{cx} * cy)) { *no_memory = true; return false; }
  cells.data.fill(0);
  auto at = [&](int64_t y, uint32_t x) {
    const int64_t top = static_cast<int64_t>(h) - 1;
    y = y < 0 ? -y : y > top ? 2 * top - y : y;   // (mirrored at the edges)
    y = y < 0 ? 0 : y > top ? top : y;
    return woven(kept, other, kp, 0, x, static_cast<uint32_t>(y), c);
  };
  for (uint32_t y = 1; y + 1 < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const float b = at(y, x), up = at(int64_t{y} - 1, x), down = at(int64_t{y} + 1, x);
      const float first = b - up, second = b - down;
      if (!((first > t && second > t) || (first < -t && second < -t))) continue;
      const float shape = at(int64_t{y} - 2, x) + 4.0f * b + at(int64_t{y} + 2, x) - 3.0f * (up + down);
      if ((shape < 0.0f ? -shape : shape) > 6.0f * t) ++cells.data[uint64_t{y / 8} * cx + x / 8];
    }
  for (uint32_t by = 0; by < cy; ++by)
    for (uint32_t bx = 0; bx < cx; ++bx) {
      uint32_t n = 0;
      for (uint32_t dy = 0; dy < 2 && by + dy < cy; ++dy)
        for (uint32_t dx = 0; dx < 2 && bx + dx < cx; ++dx) n += cells.data[uint64_t{by + dy} * cx + bx + dx];
      if (n > 80) return true;
    }
  return false;
}

// Decimation's measure: the largest sum of absolute luma differences of a 32x32 block between two woven frames.
double block_difference(const ImageAccess* const* window, int32_t kp, int32_t ka, int32_t oa, int32_t kb, int32_t ob) {
  const ImageAccess& a = *window[ka];
  const uint32_t c = luma_channel(a), w = a.plane[0].width, h = a.plane[0].height;
  double worst = 0.0;
  for (uint32_t by = 0; by < h; by += 32)
    for (uint32_t bx = 0; bx < w; bx += 32) {
      double sum = 0.0;
      for (uint32_t y = by; y < by + 32 && y < h; ++y)
        for (uint32_t x = bx; x < bx + 32 && x < w; ++x) {
          const float d = woven(*window[ka], *window[oa], kp, 0, x, y, c) - woven(*window[kb], *window[ob], kp, 0, x, y, c);
          sum += d < 0.0f ? -d : d;
        }
      if (sum > worst) worst = sum;
    }
  return worst;
}

// Which frames make the picture shown: the window's frame whose first field is kept, the frame its other field comes from, and whether that is still combed (then the
// kept frame is deinterlaced instead).
struct IvtcChoice {
  int32_t kept = 1, other = 1;
  bool combed = false;
};
IvtcChoice inverse_telecine(const ImageAccess* const* window, const bool* given, uint32_t cycle, uint32_t drop, uint32_t output, int32_t kp, allocators::IAllocator& a, bool* no_memory) {
  const uint32_t n = cycle + 2;   // (window[0] is before the cycle, window[n - 1] after it)
  int32_t match[kVpMaxTelecineFrames] = {};
  for (uint32_t i = 0; i + 1 < n; ++i) {
    // c, then p, then n (fieldmatch's order); the least combing wins, ties keep the earlier
    const int32_t order[3] = {static_cast<int32_t>(i), static_cast<int32_t>(i) - 1, static_cast<int32_t>(i) + 1};
    double best = 0.0;
    bool have = false;
    for (int32_t o : order) {
      if (o < 0 || o >= static_cast<int32_t>(n)) continue;
      const double metric = comb_of(*window[i], *window[o], kp);
      if (!have || metric < best) {
        best = metric;
        have = true;
        match[i] = o;
      }
    }
  }
  // decimation: drop the `drop` frames of the cycle most like the frame before them
  bool dropped[kVpMaxTelecineFrames] = {};
  if (drop > 0) {
    double diff[kVpMaxTelecineFrames] = {};
    // (a frame standing in for one not given goes first; one after such a frame is not a duplicate of it: the ends of a stream)
    for (uint32_t i = 1; i <= cycle; ++i)
      diff[i] = !given[i] ? -1.0 : !given[i - 1] ? 1e300 : block_difference(window, kp, static_cast<int32_t>(i), match[i], static_cast<int32_t>(i) - 1, match[i - 1]);
    for (uint32_t k = 0; k < drop; ++k) {
      int32_t lowest = -1;
      for (uint32_t i = 1; i <= cycle; ++i)
        if (!dropped[i] && (lowest < 0 || diff[i] < diff[lowest])) lowest = static_cast<int32_t>(i);
      if (lowest > 0) dropped[lowest] = true;
    }
  }
  IvtcChoice choice;
  uint32_t seen = 0;
  int32_t last = 1;
  for (uint32_t i = 1; i <= cycle; ++i) {
    if (dropped[i]) continue;
    last = static_cast<int32_t>(i);
    if (seen++ == output) break;
  }
  choice.kept = last;
  choice.other = match[last];
  choice.combed = frame_combed(*window[last], *window[choice.other], kp, a, no_memory);
  if (choice.combed) choice.other = last;
  return choice;
}
