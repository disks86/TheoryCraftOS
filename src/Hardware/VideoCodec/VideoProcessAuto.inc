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
// The video processor's automatic processing (VideoProcessStages.inc includes this; VpStream::auto_processing). Each kind looks at the picture and does nothing to one
// that does not need it:
// - denoise: the noise is measured as Immerkaer's fast estimate (sigma = sqrt(pi / 2) / (6 n) sum |I * N|, N = [1 -2 1; -2 4 -2; 1 -2 1]) over the luma samples that
//   are not on an edge (Sobel |gx| + |gy| at most a tenth of the range: edges are not noise); above half an 8-bit code of it, the noise reduction filter runs at a
//   strength whose threshold is 2.5 sigma, on every plane.
// - deringing: luma samples near a strong edge (within two samples of one whose Sobel magnitude is more than a quarter of the range), not on an edge themselves
//   (Sobel at most an eighth of it) and rippling - above or below both neighbours across or down, by at most 1/16 of the range - are smoothed by a 3x3 sigma filter of
//   that threshold: the ripples a coarse transform leaves beside edges go, edges, flat areas and gradients stay.
// - edge enhancement: a picture enlarged by half again or more (its destination over its source rectangle, the larger of the two directions) is sharpened by the edge
//   enhancement filter at a quarter of the enlargement beyond 1, at most 0.5.
// - colour correction: a washed-out picture - the luma (R, G, B of RGB) between its 0.5th and 99.5th percentiles spanning a tenth to six tenths of the nominal range -
//   is stretched about its middle to nine tenths of the range (gain at most 1.5), kept inside the range; chroma is left alone.

constexpr float kSqrtHalfPi = 1.2533141373155003f;

float sobel(const ViewPlane& v, int32_t x, int32_t y, uint32_t c) {
  const float gx = (v.at(x + 1, y - 1, c) + 2.0f * v.at(x + 1, y, c) + v.at(x + 1, y + 1, c)) - (v.at(x - 1, y - 1, c) + 2.0f * v.at(x - 1, y, c) + v.at(x - 1, y + 1, c));
  const float gy = (v.at(x - 1, y + 1, c) + 2.0f * v.at(x, y + 1, c) + v.at(x + 1, y + 1, c)) - (v.at(x - 1, y - 1, c) + 2.0f * v.at(x, y - 1, c) + v.at(x + 1, y - 1, c));
  return (gx < 0.0f ? -gx : gx) + (gy < 0.0f ? -gy : gy);
}
ViewPlane whole_view(const PlaneAccess& p) {
  ViewPlane v;
  v.plane = &p;
  v.src_w = v.view_w = static_cast<int32_t>(p.width);
  v.src_h = v.view_h = static_cast<int32_t>(p.height);
  return v;
}

// The noise of channel c of a plane, in its codes (0: too few samples off edges to tell).
float estimate_noise(const PlaneAccess& p, uint32_t c, float range) {
  if (p.width < 3 || p.height < 3) return 0.0f;
  const ViewPlane v = whole_view(p);
  double sum = 0.0;
  uint64_t n = 0;
  for (int32_t y = 1; y + 1 < v.view_h; ++y)
    for (int32_t x = 1; x + 1 < v.view_w; ++x) {
      if (sobel(v, x, y, c) > 0.1f * range) continue;
      const float r = (v.at(x - 1, y - 1, c) + v.at(x + 1, y - 1, c) + v.at(x - 1, y + 1, c) + v.at(x + 1, y + 1, c)) -
                      2.0f * (v.at(x, y - 1, c) + v.at(x - 1, y, c) + v.at(x + 1, y, c) + v.at(x, y + 1, c)) + 4.0f * v.at(x, y, c);
      sum += r < 0.0f ? -r : r;
      ++n;
    }
  const uint64_t all = uint64_t{p.width - 2} * (p.height - 2);
  if (n * 10 < all) return 0.0f;
  return static_cast<float>(kSqrtHalfPi * sum / (6.0 * static_cast<double>(n)));
}

float fabs_ripple(float v) { return v < 0.0f ? -v : v; }

// Deringing of channel c of plane k from `from` into `to` (every other channel copied).
void dering_plane(const ImageAccess& img, const ScratchPlanes& from, ScratchPlanes& to, uint32_t k, uint32_t luma, float range, ScratchArray<uint8_t>& near) {
  const PlaneAccess& p = from.plane[k];
  const ViewPlane v = whole_view(p);
  const int32_t w = v.view_w, h = v.view_h;
  // edges, then what is within two samples of one
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x) near.data[static_cast<uint64_t>(y) * static_cast<uint64_t>(w) + static_cast<uint64_t>(x)] = sobel(v, x, y, luma) > 0.25f * range ? 2 : 0;
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x) {
      uint8_t& m = near.data[static_cast<uint64_t>(y) * static_cast<uint64_t>(w) + static_cast<uint64_t>(x)];
      if (m == 2) continue;
      for (int32_t dy = -2; dy <= 2 && !m; ++dy)
        for (int32_t dx = -2; dx <= 2 && !m; ++dx) {
          const int32_t xx = x + dx, yy = y + dy;
          if (xx >= 0 && yy >= 0 && xx < w && yy < h && near.data[static_cast<uint64_t>(yy) * static_cast<uint64_t>(w) + static_cast<uint64_t>(xx)] == 2) m = 1;
        }
    }
  uint8_t list[4];
  const uint32_t count = stage_channels(img, k, list);
  const float threshold = range / 16.0f;
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x)
      for (uint32_t ci = 0; ci < count; ++ci) {
        const uint32_t c = list[ci];
        const float centre = v.at(x, y, c);
        float out = centre;
        // (a ripple: a sample above or below both its neighbours across or down, by less than the threshold)
        auto ripple = [&](float l, float r) { return ((centre > l && centre > r) || (centre < l && centre < r)) && fabs_ripple(centre - l) <= threshold && fabs_ripple(centre - r) <= threshold; };
        const bool smooth = (c == luma || (!img.yuv && c < 3)) && near.data[static_cast<uint64_t>(y) * static_cast<uint64_t>(w) + static_cast<uint64_t>(x)] == 1 &&
                            sobel(v, x, y, luma) <= 0.125f * range && (ripple(v.at(x - 1, y, c), v.at(x + 1, y, c)) || ripple(v.at(x, y - 1, c), v.at(x, y + 1, c)));
        if (smooth) {
          float sum = 0.0f;
          int n = 0;
          for (int32_t dy = -1; dy <= 1; ++dy)
            for (int32_t dx = -1; dx <= 1; ++dx) {
              const float s = v.at(x + dx, y + dy, c), d = s - centre;
              if (d <= threshold && d >= -threshold) { sum += s; ++n; }
            }
          out = sum / static_cast<float>(n);
        }
        to.put(k, static_cast<uint32_t>(x), static_cast<uint32_t>(y), c, out);
      }
}

// The colour correction's map of luma codes (v' = (v - mid) gain + target), or false when the picture needs none.
bool colour_correction(const ImageAccess& img, const PlaneAccess& p, allocators::IAllocator& a, float* mid, float* gain, float* target, bool* no_memory) {
  *no_memory = false;
  const float black = img.is_float ? (img.space.full ? 0.0f : 16.0f / 255.0f) : img.bias[0];
  const float white = black + (img.is_float ? (img.space.full ? 1.0f : 219.0f / 255.0f) : img.scale[0]);
  const float range = white - black;
  constexpr uint32_t kBins = 1024;
  ScratchArray<uint32_t> histogram;
  if (!histogram.make(a, kBins)) { *no_memory = true; return false; }
  histogram.data.fill(0);
  const float top = img.is_float ? 1.0f : img.max[0];
  uint8_t list[4];
  const uint32_t count = stage_channels(img, 0, list);
  uint64_t total = 0;
  for (uint32_t y = 0; y < p.height; ++y)
    for (uint32_t x = 0; x < p.width; ++x)
      for (uint32_t ci = 0; ci < count; ++ci) {
        if (list[ci] == 3) continue;   // (not alpha)
        float v = p.read(x, y, list[ci]) / top * static_cast<float>(kBins - 1) + 0.5f;
        v = v > 0.0f ? (v < static_cast<float>(kBins - 1) ? v : static_cast<float>(kBins - 1)) : 0.0f;
        ++histogram.data[static_cast<uint32_t>(v)];
        ++total;
      }
  if (total == 0) return false;
  auto percentile = [&](uint64_t rank) {
    uint64_t seen = 0;
    for (uint32_t b = 0; b < kBins; ++b) {
      seen += histogram.data[b];
      if (seen > rank) return static_cast<float>(b) / static_cast<float>(kBins - 1) * top;
    }
    return top;
  };
  const float lo = percentile(total / 200), hi = percentile(total - 1 - total / 200);
  const float spread = hi - lo;
  if (!(spread >= 0.1f * range) || spread >= 0.6f * range) return false;
  float g = 0.9f * range / spread;
  if (g > 1.5f) g = 1.5f;
  const float half = 0.5f * g * spread;
  float t = 0.5f * (lo + hi);
  if (t < black + half) t = black + half;
  if (t > white - half) t = white - half;
  *mid = 0.5f * (lo + hi);
  *gain = g;
  *target = t;
  return true;
}
