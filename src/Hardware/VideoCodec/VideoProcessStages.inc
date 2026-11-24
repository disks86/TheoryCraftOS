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
// The video processor's stages before sampling (VideoProcess.cpp includes this inside its anonymous namespace): a stream that is a view of a stereo picture, interlaced,
// or noise reduced or edge enhanced is first made a picture of its own, in scratch planes of float codes laid out like the source's planes (each plane as many samples
// as the view needs of it, the same channels), which the rest of the processor then reads like any picture (SampleKind::F32).
//
// Every stage works on samples of a plane through PlaneAccess::read, whatever the format: a plane's rows, columns and channels are what check_plane found. Deinterlacing
// treats each plane on its own (as ffmpeg's yadif does), the parity of a sample's row in its plane giving its field; Adaptive is yadif's algorithm (mode "send_field",
// spatial check on), with its integer halving for integer codes, so it matches ffmpeg's yadif sample for sample.

// The float planes of one picture, from an allocator; given back when it goes.
struct ScratchPlanes {
  allocators::IAllocator* allocator = nullptr;
  ArrayView<float> data[2];
  PlaneAccess plane[2];

  ScratchPlanes() = default;
  ScratchPlanes(const ScratchPlanes&) = delete;
  ScratchPlanes& operator=(const ScratchPlanes&) = delete;
  ~ScratchPlanes() { release(); }
  void release() {
    for (ArrayView<float>& d : data)
      if (allocator && !d.empty()) release_view(*allocator, d);
  }
  // Plane k of w x h samples of `channels` channels.
  [[nodiscard]] bool make(allocators::IAllocator& a, uint32_t k, uint32_t w, uint32_t h, uint8_t channels) {
    allocator = &a;
    const uint64_t count = uint64_t{w} * h * channels;   // (32-bit factors and at most 4 channels: no overflow)
    data[k] = allocate_view<float>(a, count);
    if (data[k].empty()) return false;
    PlaneAccess& p = plane[k];
    p = PlaneAccess{};
    p.kind = SampleKind::F32;
    p.floats = data[k];
    p.pitch = uint64_t{w} * channels;
    p.step = channels;
    p.width = w;
    p.height = h;
    p.channels = channels;
    return true;
  }
  void put(uint32_t k, uint32_t x, uint32_t y, uint32_t c, float v) const { data[k][y * plane[k].pitch + x * plane[k].step + c] = v; }
};

// The size of the view a stream shows of a picture of w x h.
void view_size(VpStereoFormat f, uint32_t w, uint32_t h, uint32_t* vw, uint32_t* vh) {
  *vw = f == VpStereoFormat::Horizontal || f == VpStereoFormat::ColumnInterleaved ? w / 2 : w;
  *vh = f == VpStereoFormat::Vertical || f == VpStereoFormat::RowInterleaved ? h / 2 : h;
}

// One plane of a picture as the view shows it: sample (i, j) of the view (clamped to it) from the plane's samples (clamped to them).
struct ViewPlane {
  const PlaneAccess* plane = nullptr;
  int32_t src_w = 1, src_h = 1, view_w = 1, view_h = 1;
  VpStereoFormat format = VpStereoFormat::None;
  int32_t frame = 0, shift = 0;
  bool flip = false;

  float raw(int32_t si, int32_t sj, uint32_t c) const {
    si = si < 0 ? 0 : si >= src_w ? src_w - 1 : si;
    sj = sj < 0 ? 0 : sj >= src_h ? src_h - 1 : sj;
    return plane->read(static_cast<uint32_t>(si), static_cast<uint32_t>(sj), c);
  }
  float at(int32_t i, int32_t j, uint32_t c) const {
    i = i < 0 ? 0 : i >= view_w ? view_w - 1 : i;
    j = j < 0 ? 0 : j >= view_h ? view_h - 1 : j;
    if (flip) j = view_h - 1 - j;
    switch (format) {
      case VpStereoFormat::Horizontal: return raw(i + frame * (src_w / 2), j, c);
      case VpStereoFormat::Vertical: return raw(i, j + frame * (src_h / 2), c);
      case VpStereoFormat::RowInterleaved: return raw(i, 2 * j + frame, c);
      case VpStereoFormat::ColumnInterleaved: return raw(2 * i + frame, j, c);
      case VpStereoFormat::Checkerboard: {
        if (((i + j) & 1) == frame) return raw(i, j, c);
        // (a sample of the other frame: the mean of this frame's samples left and right of it)
        const bool left = i > 0, right = i + 1 < view_w;
        if (left && right) return (raw(i - 1, j, c) + raw(i + 1, j, c)) * 0.5f;
        if (left) return raw(i - 1, j, c);
        if (right) return raw(i + 1, j, c);
        return raw(i, j, c);
      }
      case VpStereoFormat::MonoOffset: return raw(i - shift, j, c);
      case VpStereoFormat::None: break;
    }
    return raw(i, j, c);
  }
  // The parity of the row of the plane that row j of the view comes from (its field).
  int32_t row_parity(int32_t j) const {
    if (flip) j = view_h - 1 - j;
    const int32_t sj = format == VpStereoFormat::Vertical ? j + frame * (src_h / 2) : j;
    return sj & 1;
  }
};

// yadif's interpolation of the missing sample (i, j) of `cur` (its rows j - 1 and j + 1 are of the shown field), from the frames before and after and the other field's
// neighbours in time (prev2, next2: the missing field's rows before and after the shown field). In integer codes when `integer` (as yadif's >> 1), else in floats.
struct Yadif {
  const ViewPlane *prev, *cur, *next, *prev2, *next2;
  int32_t w, h;
  bool integer;

  float half(float sum) const { return integer ? static_cast<float>(floor_d(sum * 0.5)) : sum * 0.5f; }
  float sample(int32_t i, int32_t j, uint32_t c) const {
    const int32_t m = j > 0 ? j - 1 : j + 1, p = j + 1 < h ? j + 1 : j - 1;     // (yadif's mrefs / prefs: mirrored at the picture's edges)
    const int32_t m2 = j > 0 ? j - 2 : j + 2, p2 = j + 1 < h ? j + 2 : j - 2;
    const float cv = cur->at(i, m, c), ev = cur->at(i, p, c);
    const float d = half(prev2->at(i, j, c) + next2->at(i, j, c));
    const float td0 = fabs_f(prev2->at(i, j, c) - next2->at(i, j, c));
    const float td1 = half(fabs_f(prev->at(i, m, c) - cv) + fabs_f(prev->at(i, p, c) - ev));
    const float td2 = half(fabs_f(next->at(i, m, c) - cv) + fabs_f(next->at(i, p, c) - ev));
    float diff = max3(half(td0), td1, td2);
    float spatial = half(cv + ev);
    if (i >= 3 && i + 3 < w) {   // edge-directed: the direction (up to two samples either way) along which the rows above and below agree best
      float score = fabs_f(cur->at(i - 1, m, c) - cur->at(i - 1, p, c)) + fabs_f(cv - ev) + fabs_f(cur->at(i + 1, m, c) - cur->at(i + 1, p, c)) - (integer ? 1.0f : 0.0f);
      auto check = [&](int32_t k) {
        const float s = fabs_f(cur->at(i - 1 + k, m, c) - cur->at(i - 1 - k, p, c)) + fabs_f(cur->at(i + k, m, c) - cur->at(i - k, p, c)) +
                        fabs_f(cur->at(i + 1 + k, m, c) - cur->at(i + 1 - k, p, c));
        if (s < score) {
          score = s;
          spatial = half(cur->at(i + k, m, c) + cur->at(i - k, p, c));
          return true;
        }
        return false;
      };
      if (check(-1)) check(-2);
      if (check(1)) check(2);
    }
    if (!(j == 1 || j + 2 == h)) {   // the spatial interlacing check (yadif skips it on the second and the last but one row)
      const float b = half(prev2->at(i, m2, c) + next2->at(i, m2, c)), f = half(prev2->at(i, p2, c) + next2->at(i, p2, c));
      const float mx = max3(d - ev, d - cv, min2(b - cv, f - ev)), mn = min3(d - ev, d - cv, max2(b - cv, f - ev));
      diff = max3(diff, mn, -mx);
    }
    if (spatial > d + diff) spatial = d + diff;
    else if (spatial < d - diff) spatial = d - diff;
    return spatial;
  }

  static float fabs_f(float v) { return v < 0.0f ? -v : v; }
  static float max2(float a, float b) { return a > b ? a : b; }
  static float min2(float a, float b) { return a < b ? a : b; }
  static float max3(float a, float b, float c) { return max2(max2(a, b), c); }
  static float min3(float a, float b, float c) { return min2(min2(a, b), c); }
};

// Whether a stream needs the stages.
bool needs_stages(const VpStream& in) {
  return in.stereo != VpStereoFormat::None || (in.field_order != VpFieldOrder::Progressive && in.deinterlace != VpDeinterlace::Weave) || in.noise_reduction > 0.0f ||
         in.edge_enhancement > 0.0f;
}

// Whether channel c of plane k is alpha, and the codes' range of it (for the filters' thresholds).
bool is_alpha_channel(const ImageAccess&, uint32_t k, uint32_t c) { return k == 0 && c == 3; }
bool is_chroma_channel(const ImageAccess& img, uint32_t k, uint32_t c) { return img.yuv && (k > 0 || c == 1 || c == 2); }
float channel_range(const ImageAccess& img, uint32_t k, uint32_t c) {
  if (img.is_float) return 1.0f;
  return is_chroma_channel(img, k, c) ? img.max[1] : img.max[0];
}

// A 3x3 filter over plane k of `from` into `to` (planes of the same size): noise reduction (a sigma filter - the mean of the neighbours within a threshold of the sample,
// the threshold growing with `strength`; 0 keeps the picture) or edge enhancement (an unsharp mask: the sample plus `amount` times its difference from the 3x3 mean).
void filter_plane(const ImageAccess& img, const ScratchPlanes& from, ScratchPlanes& to, uint32_t k, bool noise, float strength) {
  const PlaneAccess& p = from.plane[k];
  ViewPlane v;
  v.plane = &p;
  v.src_w = v.view_w = static_cast<int32_t>(p.width);
  v.src_h = v.view_h = static_cast<int32_t>(p.height);
  for (uint32_t y = 0; y < p.height; ++y)
    for (uint32_t x = 0; x < p.width; ++x)
      for (uint32_t c = 0; c < p.channels; ++c) {
        const int32_t i = static_cast<int32_t>(x), j = static_cast<int32_t>(y);
        const float centre = v.at(i, j, c);
        const bool luma = !is_chroma_channel(img, k, c);
        if (is_alpha_channel(img, k, c) || (!noise && img.yuv && !luma)) { to.put(k, x, y, c, centre); continue; }
        float out = centre;
        if (noise) {
          const float threshold = strength * 0.1f * channel_range(img, k, c);
          float sum = 0.0f;
          int n = 0;
          for (int32_t dj = -1; dj <= 1; ++dj)
            for (int32_t di = -1; di <= 1; ++di) {
              const float s = v.at(i + di, j + dj, c), d = s - centre;
              if (d <= threshold && d >= -threshold) { sum += s; ++n; }
            }
          out = sum / static_cast<float>(n);   // (n >= 1: the centre itself)
        } else {
          float sum = 0.0f;
          for (int32_t dj = -1; dj <= 1; ++dj)
            for (int32_t di = -1; di <= 1; ++di) sum += v.at(i + di, j + dj, c);
          out = centre + 2.0f * strength * (centre - sum / 9.0f);
          const float top = channel_range(img, k, c);
          out = out < 0.0f ? 0.0f : (!img.is_float && out > top) ? top : out;
        }
        to.put(k, x, y, c, out);
      }
}

// The stages for stream `in`, whose current picture `img` is prepared (and its references, when given, as well): `img` becomes the stream's picture in `scratch[0]`
// or `scratch[1]`, of the view's size.
[[nodiscard]] VpResult run_stages(const VpStream& in, ImageAccess* img, const ImageAccess* past, const ImageAccess* future, allocators::IAllocator* allocator,
                                  ScratchPlanes (&scratch)[2]) {
  if (!allocator) return VpResult::Unsupported;
  uint32_t vw = 0, vh = 0;
  view_size(in.stereo, img->width, img->height, &vw, &vh);
  if (vw == 0 || vh == 0) return VpResult::Invalid;
  const bool interlaced = in.field_order != VpFieldOrder::Progressive && in.deinterlace != VpDeinterlace::Weave;
  if (interlaced && (in.stereo == VpStereoFormat::RowInterleaved || in.stereo == VpStereoFormat::Checkerboard)) return VpResult::Invalid;   // (each view is one field)
  const uint32_t planes = img->plane[1].width ? 2 : 1;
  // (the references are of the picture's format and size; their planes must also have its channels: a sample's channels are read as the current picture's)
  const ImageAccess* const references[2] = {past, future};
  for (const ImageAccess* ref : references)
    for (uint32_t k = 0; ref && k < 2; ++k)
      if (ref->plane[k].width != img->plane[k].width || ref->plane[k].height != img->plane[k].height || ref->plane[k].channels != img->plane[k].channels) return VpResult::Invalid;
  const bool integer = img->plane[0].kind != SampleKind::Half && img->plane[0].kind != SampleKind::F32;
  // the field shown (the parity of its rows) and which frames are the missing field's before and after it
  const int32_t shown = in.field_order == VpFieldOrder::TopFieldFirst ? (in.field == 0 ? 0 : 1) : (in.field == 0 ? 1 : 0);
  const bool first = in.field == 0;
  uint32_t pw[2] = {}, ph[2] = {};
  for (uint32_t k = 0; k < planes; ++k) {
    const PlaneAccess& p = img->plane[k];
    pw[k] = static_cast<uint32_t>((uint64_t{vw} * p.width + img->width - 1) / img->width);   // (what sampling a picture of the view's size reads of the plane)
    ph[k] = static_cast<uint32_t>((uint64_t{vh} * p.height + img->height - 1) / img->height);
    if (!scratch[0].make(*allocator, k, pw[k], ph[k], p.channels)) return VpResult::NoMemory;
  }
  for (uint32_t k = 0; k < planes; ++k) {
    ViewPlane views[3];
    const ImageAccess* frames[3] = {img, past ? past : img, future ? future : img};
    for (uint32_t f = 0; f < 3; ++f) {
      ViewPlane& v = views[f];
      v.plane = &frames[f]->plane[k];
      v.src_w = static_cast<int32_t>(v.plane->width);
      v.src_h = static_cast<int32_t>(v.plane->height);
      v.view_w = static_cast<int32_t>(pw[k]);
      v.view_h = static_cast<int32_t>(ph[k]);
      v.format = in.stereo;
      v.frame = in.stereo_frame ? 1 : 0;
      v.flip = in.stereo_flip;
      v.shift = static_cast<int32_t>((int64_t{in.stereo_offset} * v.src_w) / static_cast<int64_t>(img->width));   // (in the plane's samples)
    }
    const ViewPlane& cur = views[0];
    Yadif yadif{&views[1], &cur, &views[2], first ? &views[1] : &cur, first ? &cur : &views[2], cur.view_w, cur.view_h, integer};
    const uint8_t channels = img->plane[k].channels;
    for (int32_t j = 0; j < cur.view_h; ++j) {
      const bool missing = interlaced && cur.row_parity(j) != shown;
      for (int32_t i = 0; i < cur.view_w; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
          float v;
          if (!missing) v = cur.at(i, j, c);
          else if (in.deinterlace == VpDeinterlace::Bob) v = (cur.at(i, j > 0 ? j - 1 : j + 1, c) + cur.at(i, j + 1 < cur.view_h ? j + 1 : j - 1, c)) * 0.5f;
          else v = yadif.sample(i, j, c);
          scratch[0].put(k, static_cast<uint32_t>(i), static_cast<uint32_t>(j), c, v);
        }
    }
  }
  uint32_t at = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const bool noise = pass == 0;
    const float strength = noise ? in.noise_reduction : in.edge_enhancement;
    if (!(strength > 0.0f)) continue;
    ScratchPlanes& to = scratch[at ^ 1];
    for (uint32_t k = 0; k < planes; ++k)
      if (to.data[k].empty() && !to.make(*allocator, k, pw[k], ph[k], img->plane[k].channels)) return VpResult::NoMemory;
    for (uint32_t k = 0; k < planes; ++k) filter_plane(*img, scratch[at], to, k, noise, strength);
    at ^= 1;
  }
  for (uint32_t k = 0; k < planes; ++k) img->plane[k] = scratch[at].plane[k];
  img->width = vw;
  img->height = vh;
  return VpResult::Ok;
}
