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

#include "Media/VideoFiles.hpp"
#include "Media/ImageCodecs.hpp"
#include "Media/Mp4.hpp"

namespace media {

namespace {
bool starts_with(ConstByteView data, const char* text) {
  uint64_t n = 0;
  while (text[n]) ++n;
  if (data.size() < n) return false;
  for (uint64_t i = 0; i < n; ++i)
    if (data[i] != static_cast<uint8_t>(text[i])) return false;
  return true;
}

bool is_apng(ConstByteView data) {
  uint64_t at = 8;  // after the signature
  while (at + 12 <= data.size()) {
    const uint64_t size = (uint64_t{data[at]} << 24) | (uint64_t{data[at + 1]} << 16) | (uint64_t{data[at + 2]} << 8) | data[at + 3];
    const uint8_t* t = data.data() + at + 4;
    if (t[0] == 'a' && t[1] == 'c' && t[2] == 'T' && t[3] == 'L') return true;
    if (t[0] == 'I' && t[1] == 'D' && t[2] == 'A' && t[3] == 'T') return false;  // animation control comes before the pixels
    at += 12 + size;
  }
  return false;
}

// Reads a decimal number at data[*at]; false if there is none.
bool read_number(ConstByteView data, uint64_t* at, uint64_t* value) {
  uint64_t v = 0, digits = 0;
  while (*at < data.size() && data[*at] >= '0' && data[*at] <= '9' && digits < 10) {
    v = v * 10 + (data[*at] - '0');
    ++*at;
    ++digits;
  }
  *value = v;
  return digits > 0;
}

int32_t clamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

uint32_t yuv_to_argb(int32_t y, int32_t u, int32_t v) {
  const int32_t c = 298 * (y - 16), d = u - 128, e = v - 128;
  const int32_t r = clamp8((c + 409 * e + 128) >> 8);
  const int32_t g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
  const int32_t b = clamp8((c + 516 * d + 128) >> 8);
  return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}
}  // namespace

const char* video_file_format_name(VideoFileFormat format) {
  switch (format) {
    case VideoFileFormat::Avi: return "AVI";
    case VideoFileFormat::Gif: return "GIF";
    case VideoFileFormat::Apng: return "APNG";
    case VideoFileFormat::Y4m: return "Y4M";
    case VideoFileFormat::AnimatedWebp: return "animated WebP";
    case VideoFileFormat::Mp4: return "MP4";
    default: return "unknown";
  }
}

VideoFileFormat detect_video_file(ConstByteView data) {
  if (data.size() >= 12 && starts_with(data, "RIFF") && data[8] == 'A' && data[9] == 'V' && data[10] == 'I' && data[11] == ' ') return VideoFileFormat::Avi;
  if (starts_with(data, "YUV4MPEG2 ")) return VideoFileFormat::Y4m;
  if (looks_like_mp4(data)) return VideoFileFormat::Mp4;
  const ImageFormat image = detect_image_format(data);
  if (image == ImageFormat::Gif) return VideoFileFormat::Gif;
  if (image == ImageFormat::Png && is_apng(data)) return VideoFileFormat::Apng;
  if (image == ImageFormat::Webp) {  // an extended file whose VP8X flags say "animation"
    for (uint64_t at = 12; at + 18 <= data.size();) {
      const uint64_t size = data[at + 4] | (data[at + 5] << 8) | (data[at + 6] << 16) | (uint64_t{data[at + 7]} << 24);
      if (data[at] == 'V' && data[at + 1] == 'P' && data[at + 2] == '8' && data[at + 3] == 'X') return (data[at + 8] & 2) ? VideoFileFormat::AnimatedWebp : VideoFileFormat::Unknown;
      at += 8 + size + (size & 1);
    }
  }
  return VideoFileFormat::Unknown;
}

bool decode_y4m(ConstByteView data, Image* out) {
  out->clear();
  if (!starts_with(data, "YUV4MPEG2")) return false;
  uint64_t at = 9, width = 0, height = 0, rate_num = 25, rate_den = 1;
  enum { C420, C422, C444, Mono } chroma = C420;
  // Stream parameters: space-separated tokens up to the newline.
  while (at < data.size() && data[at] != '\n') {
    if (data[at] != ' ') return false;
    ++at;
    if (at >= data.size()) return false;
    const uint8_t key = data[at++];
    if (key == 'W') {
      if (!read_number(data, &at, &width)) return false;
    } else if (key == 'H') {
      if (!read_number(data, &at, &height)) return false;
    } else if (key == 'F') {
      if (!read_number(data, &at, &rate_num) || at >= data.size() || data[at++] != ':' || !read_number(data, &at, &rate_den)) return false;
    } else if (key == 'I') {
      if (at >= data.size()) return false;
      if (data[at] != 'p' && data[at] != '?') return false;  // interlaced material is not woven back together
      ++at;
    } else {
      uint64_t start = at;
      while (at < data.size() && data[at] != ' ' && data[at] != '\n') ++at;
      if (key == 'C') {
        const ConstByteView tag = data.subview(start, at - start);
        if (starts_with(tag, "420")) chroma = C420;
        else if (starts_with(tag, "422")) chroma = C422;
        else if (starts_with(tag, "444") && tag.size() == 3) chroma = C444;
        else if (starts_with(tag, "mono") && tag.size() == 4) chroma = Mono;
        else return false;
      }
    }
  }
  if (at >= data.size() || width == 0 || height == 0 || width > 65535 || height > 65535 || rate_num == 0 || rate_den == 0) return false;
  ++at;  // the newline

  const uint64_t cw = chroma == C444 ? width : (width + 1) / 2;
  const uint64_t ch = chroma == C420 ? (height + 1) / 2 : height;
  const uint64_t luma = width * height;
  const uint64_t frame_bytes = chroma == Mono ? luma : luma + 2 * cw * ch;

  // Count the whole frames first (each "FRAME" line may carry parameters, which are ignored).
  uint64_t frames = 0, scan = at;
  while (scan < data.size()) {
    if (!starts_with(data.subview(scan, data.size() - scan), "FRAME")) break;
    while (scan < data.size() && data[scan] != '\n') ++scan;
    if (scan >= data.size() || data.size() - (scan + 1) < frame_bytes) break;
    scan += 1 + frame_bytes;
    ++frames;
    if (frames * width * height > Image::kMaxPixels) return false;
  }
  if (frames == 0 || !out->create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(frames))) return false;

  uint64_t elapsed_num = 0;  // exact running time in units of 1/rate_num milliseconds, to spread the rounding
  uint64_t previous_end_ms = 0;
  for (uint64_t f = 0; f < frames; ++f) {
    while (data[at] != '\n') ++at;
    ++at;
    const uint8_t* y = data.data() + at;
    const uint8_t* u = y + luma;
    const uint8_t* v = u + cw * ch;
    ArrayView<uint32_t> px = out->frame(static_cast<uint32_t>(f));
    for (uint64_t row = 0; row < height; ++row) {
      const uint64_t crow = chroma == C420 ? row / 2 : row;
      for (uint64_t col = 0; col < width; ++col) {
        const uint64_t ccol = chroma == C444 ? col : col / 2;
        const int32_t yy = y[row * width + col];
        px[row * width + col] = chroma == Mono ? (0xFF000000u | static_cast<uint32_t>(yy) * 0x010101u) : yuv_to_argb(yy, u[crow * cw + ccol], v[crow * cw + ccol]);
      }
    }
    at += frame_bytes;
    elapsed_num += 1000 * rate_den;
    const uint64_t end_ms = elapsed_num / rate_num;
    out->set_delay_ms(static_cast<uint32_t>(f), static_cast<uint32_t>(end_ms - previous_end_ms));
    previous_end_ms = end_ms;
  }
  return true;
}

namespace {
bool put_text(ByteBuffer* out, const char* text) {
  for (; *text; ++text)
    if (!out->put8(static_cast<uint8_t>(*text))) return false;
  return true;
}
bool put_decimal(ByteBuffer* out, uint64_t v) {
  char digits[20];
  int n = 0;
  do digits[n++] = static_cast<char>('0' + v % 10); while ((v /= 10) != 0);
  while (n > 0)
    if (!out->put8(static_cast<uint8_t>(digits[--n]))) return false;
  return true;
}
}  // namespace

bool encode_y4m(const Image& image, ByteBuffer* out) {
  if (image.empty()) return false;
  const uint64_t start = out->size();
  const uint32_t width = image.width(), height = image.height();
  const uint32_t delay = image.delay_ms(0) ? image.delay_ms(0) : 100;
  bool ok = put_text(out, "YUV4MPEG2 W") && put_decimal(out, width) && put_text(out, " H") && put_decimal(out, height) && put_text(out, " F1000:") &&
            put_decimal(out, delay) && put_text(out, " Ip A1:1 C420jpeg\n");
  const uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
  for (uint32_t f = 0; ok && f < image.frame_count(); ++f) {
    ArrayView<uint32_t> px = image.frame(f);
    ok = put_text(out, "FRAME\n");
    for (uint32_t row = 0; ok && row < height; ++row)
      for (uint32_t col = 0; ok && col < width; ++col) {
        const uint32_t p = px[uint64_t{row} * width + col];
        const int32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        ok = out->put8(static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16));
      }
    for (int plane = 0; ok && plane < 2; ++plane)
      for (uint32_t row = 0; ok && row < ch; ++row)
        for (uint32_t col = 0; ok && col < cw; ++col) {
          // Average the (up to) 2x2 block of pixels.
          int32_t r = 0, g = 0, b = 0, n = 0;
          for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t x = col * 2 + dx, y = row * 2 + dy;
              if (x >= width || y >= height) continue;
              const uint32_t p = px[uint64_t{y} * width + x];
              r += (p >> 16) & 255;
              g += (p >> 8) & 255;
              b += p & 255;
              ++n;
            }
          r /= n;
          g /= n;
          b /= n;
          const int32_t value = plane == 0 ? ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128 : ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
          ok = out->put8(static_cast<uint8_t>(clamp8(value)));
        }
  }
  if (!ok) out->truncate(start);
  return ok;
}

}  // namespace media
