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

#include "Media/ImageCodecs.hpp"

namespace media {

namespace {
struct Reader {
  ConstByteView d;
  uint64_t at;
  bool ok = true;
  bool skip_space() {  // whitespace and # comments
    while (at < d.size()) {
      if (d[at] == '#') {
        while (at < d.size() && d[at] != '\n') ++at;
      } else if (d[at] == ' ' || d[at] == '\n' || d[at] == '\r' || d[at] == '\t') {
        ++at;
      } else {
        return true;
      }
    }
    return false;
  }
  uint32_t number() {
    if (!skip_space() || d[at] < '0' || d[at] > '9') {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    while (at < d.size() && d[at] >= '0' && d[at] <= '9') {
      v = v * 10 + (d[at++] - '0');
      if (v > 0xFFFFFFFFull) {
        ok = false;
        return 0;
      }
    }
    return static_cast<uint32_t>(v);
  }
};
}  // namespace

bool decode_pnm(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 3 || detect_image_format(data) != ImageFormat::Pnm) return false;
  const char kind = static_cast<char>(data[1]);
  Reader r{data, 2};
  uint32_t width = 0, height = 0, maxval = 1, depth = 0;
  bool has_alpha = false, grey_alpha = false;
  if (kind == '7') {  // PAM: "KEY value" lines up to ENDHDR
    bool end = false;
    char tuple[32] = {};
    while (!end && r.skip_space()) {
      char key[16] = {};
      uint32_t n = 0;
      while (r.at < data.size() && data[r.at] > ' ' && n < 15) key[n++] = static_cast<char>(data[r.at++]);
      auto is = [&](const char* k) {
        for (uint32_t i = 0;; ++i) {
          if (key[i] != k[i]) return false;
          if (!k[i]) return true;
        }
      };
      if (is("ENDHDR")) end = true;
      else if (is("WIDTH")) width = r.number();
      else if (is("HEIGHT")) height = r.number();
      else if (is("DEPTH")) depth = r.number();
      else if (is("MAXVAL")) maxval = r.number();
      else if (is("TUPLTYPE")) {
        r.skip_space();
        uint32_t t = 0;
        while (r.at < data.size() && data[r.at] != '\n' && t < 31) tuple[t++] = static_cast<char>(data[r.at++]);
      } else return false;
      if (!r.ok) return false;
    }
    if (!end) return false;
    ++r.at;  // the newline after ENDHDR
    has_alpha = tuple[0] == 'R' ? depth == 4 : depth == 2;
    grey_alpha = depth == 2;
    if (depth != 1 && depth != 2 && depth != 3 && depth != 4) return false;
  } else {
    width = r.number();
    height = r.number();
    if (kind != '1' && kind != '4') maxval = r.number();
    depth = (kind == '3' || kind == '6') ? 3 : 1;
  }
  if (!r.ok || width == 0 || height == 0 || maxval == 0 || maxval > 65535 || !out->create(width, height, 1)) return false;
  ArrayView<uint32_t> pixels = out->frame(0);
  const bool ascii = kind == '1' || kind == '2' || kind == '3';
  const bool bits = kind == '1' || kind == '4';
  const uint32_t bytes_per_sample = maxval > 255 ? 2 : 1;
  if (!ascii) {
    if (kind != '7') ++r.at;  // exactly one whitespace byte follows the header
    const uint64_t row = bits ? (uint64_t{width} + 7) / 8 : uint64_t{width} * depth * bytes_per_sample;
    if (r.at + row * height > data.size()) {
      out->clear();
      return false;
    }
  }
  auto sample8 = [&](uint32_t v) { return static_cast<uint32_t>(maxval == 255 ? v : (uint64_t{v} * 255 + maxval / 2) / maxval); };
  bool alpha_seen = false;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t c[4] = {0, 0, 0, maxval};
      if (bits) {
        uint32_t bit;
        if (ascii) {
          if (!r.skip_space() || (data[r.at] != '0' && data[r.at] != '1')) goto fail;
          bit = data[r.at++] - '0';
        } else {
          const uint64_t row = (uint64_t{width} + 7) / 8;
          bit = (data[r.at + y * row + x / 8] >> (7 - x % 8)) & 1;
        }
        c[0] = c[1] = c[2] = bit ? 0 : maxval;  // in PBM a 1 is black
      } else {
        for (uint32_t k = 0; k < depth; ++k) {
          uint32_t v;
          if (ascii) {
            v = r.number();
            if (!r.ok) goto fail;
          } else {
            const uint64_t at = r.at + (uint64_t{y} * width + x) * depth * bytes_per_sample + uint64_t{k} * bytes_per_sample;
            v = bytes_per_sample == 2 ? (uint32_t{data[at]} << 8) | data[at + 1] : data[at];
          }
          if (v > maxval) v = maxval;
          c[k] = v;
        }
        if (depth == 1 || (depth == 2 && grey_alpha)) {
          const uint32_t grey = c[0], a = depth == 2 ? c[1] : maxval;
          c[0] = c[1] = c[2] = grey;
          c[3] = a;
        } else if (depth == 3) {
          c[3] = maxval;
        }
      }
      const uint32_t a8 = has_alpha ? sample8(c[3]) : 255;
      if (a8 != 255) alpha_seen = true;
      pixels[uint64_t{y} * width + x] = (a8 << 24) | (sample8(c[0]) << 16) | (sample8(c[1]) << 8) | sample8(c[2]);
    }
  }
  out->set_has_alpha(alpha_seen);
  return true;
fail:
  out->clear();
  return false;
}

bool encode_pnm(const Image& image, ByteBuffer* out) {
  if (image.empty()) return false;
  const ArrayView<uint32_t> pixels = image.frame(0);
  bool grey = true;
  for (uint64_t i = 0; i < pixels.size() && grey; ++i) {
    const uint32_t p = pixels[i];
    grey = ((p >> 16) & 0xFF) == ((p >> 8) & 0xFF) && ((p >> 8) & 0xFF) == (p & 0xFF);
  }
  // The header as text.
  ByteBuffer& b = *out;
  bool ok = b.put8('P') && b.put8(grey ? '5' : '6') && b.put8('\n');
  auto put_number = [&](uint32_t v) {
    char digits[12];
    int n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v);
    for (int i = n - 1; i >= 0; --i) ok = ok && b.put8(static_cast<uint8_t>(digits[i]));
  };
  put_number(image.width());
  ok = ok && b.put8(' ');
  put_number(image.height());
  ok = ok && b.put8('\n');
  put_number(255);
  ok = ok && b.put8('\n');
  for (uint64_t i = 0; i < pixels.size() && ok; ++i) {
    const uint32_t p = pixels[i];
    if (grey) ok = b.put8(static_cast<uint8_t>(p));
    else ok = b.put8(static_cast<uint8_t>(p >> 16)) && b.put8(static_cast<uint8_t>(p >> 8)) && b.put8(static_cast<uint8_t>(p));
  }
  return ok;
}

}  // namespace media
