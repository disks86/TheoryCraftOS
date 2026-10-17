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
constexpr uint32_t kHeaderSize = 14;
constexpr uint8_t kIndex = 0x00, kDiff = 0x40, kLuma = 0x80, kRun = 0xC0, kRgb = 0xFE, kRgba = 0xFF;
uint32_t qoi_hash(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (r * 3 + g * 5 + b * 7 + a * 11) % 64; }
uint32_t be32(ConstByteView v, uint64_t at) { return (uint32_t{v[at]} << 24) | (uint32_t{v[at + 1]} << 16) | (uint32_t{v[at + 2]} << 8) | v[at + 3]; }
}  // namespace

bool decode_qoi(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < kHeaderSize + 8 || detect_image_format(data) != ImageFormat::Qoi) return false;
  const uint32_t width = be32(data, 4), height = be32(data, 8);
  const uint8_t channels = data[12];
  if (width == 0 || height == 0 || (channels != 3 && channels != 4) || !out->create(width, height, 1)) return false;
  uint32_t index[64] = {};
  uint32_t r = 0, g = 0, b = 0, a = 255;
  uint64_t at = kHeaderSize;
  const uint64_t end = data.size() - 8;  // the end marker
  uint32_t run = 0;
  ArrayView<uint32_t> pixels = out->frame(0);
  bool has_alpha = false;
  for (uint64_t i = 0; i < pixels.size(); ++i) {
    if (run > 0) {
      --run;
    } else if (at < end) {
      const uint32_t byte = data[at++];
      if (byte == kRgb) {
        if (at + 3 > end) goto fail;
        r = data[at]; g = data[at + 1]; b = data[at + 2];
        at += 3;
      } else if (byte == kRgba) {
        if (at + 4 > end) goto fail;
        r = data[at]; g = data[at + 1]; b = data[at + 2]; a = data[at + 3];
        at += 4;
      } else if ((byte & 0xC0) == kIndex) {
        const uint32_t c = index[byte];
        a = c >> 24; r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
      } else if ((byte & 0xC0) == kDiff) {
        r = (r + ((byte >> 4) & 3) - 2) & 0xFF;
        g = (g + ((byte >> 2) & 3) - 2) & 0xFF;
        b = (b + (byte & 3) - 2) & 0xFF;
      } else if ((byte & 0xC0) == kLuma) {
        if (at >= end) goto fail;
        const uint32_t second = data[at++];
        const int dg = static_cast<int>(byte & 0x3F) - 32;
        r = (r + dg - 8 + ((second >> 4) & 0x0F)) & 0xFF;
        g = (g + dg) & 0xFF;
        b = (b + dg - 8 + (second & 0x0F)) & 0xFF;
      } else {  // run
        run = byte & 0x3F;
      }
      index[qoi_hash(r, g, b, a)] = (a << 24) | (r << 16) | (g << 8) | b;
    } else {
      goto fail;  // ran out of data
    }
    pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    if (a != 255) has_alpha = true;
  }
  out->set_has_alpha(has_alpha);
  return true;
fail:
  out->clear();
  return false;
}

bool encode_qoi(const Image& image, ByteBuffer* out) {
  if (image.empty()) return false;
  const bool alpha = image.has_alpha();
  bool ok = out->put_tag("qoif");
  const uint32_t w = image.width(), h = image.height();
  const uint8_t header[10] = {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w),
                              static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h),
                              static_cast<uint8_t>(alpha ? 4 : 3), 0};
  ok = ok && out->append(ConstByteView(header, 10));
  uint32_t index[64] = {};
  uint32_t pr = 0, pg = 0, pb = 0, pa = 255;
  uint32_t run = 0;
  const ArrayView<uint32_t> pixels = image.frame(0);
  for (uint64_t i = 0; i < pixels.size() && ok; ++i) {
    const uint32_t p = pixels[i];
    const uint32_t a = alpha ? p >> 24 : 255, r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    const uint32_t packed = (a << 24) | (r << 16) | (g << 8) | b;
    if (r == pr && g == pg && b == pb && a == pa) {
      ++run;
      if (run == 62 || i == pixels.size() - 1) {
        ok = out->put8(static_cast<uint8_t>(kRun | (run - 1)));
        run = 0;
      }
      continue;
    }
    if (run > 0) {
      ok = out->put8(static_cast<uint8_t>(kRun | (run - 1)));
      run = 0;
    }
    const uint32_t slot = qoi_hash(r, g, b, a);
    if (index[slot] == packed) {
      ok = ok && out->put8(static_cast<uint8_t>(kIndex | slot));
    } else {
      index[slot] = packed;
      if (a == pa) {
        const int dr = static_cast<int>(r) - static_cast<int>(pr), dg = static_cast<int>(g) - static_cast<int>(pg), db = static_cast<int>(b) - static_cast<int>(pb);
        // Differences are taken modulo 256 as signed bytes.
        const int sr = static_cast<int8_t>(dr), sg = static_cast<int8_t>(dg), sb = static_cast<int8_t>(db);
        const int dr_dg = sr - sg, db_dg = sb - sg;
        if (sr >= -2 && sr <= 1 && sg >= -2 && sg <= 1 && sb >= -2 && sb <= 1) {
          ok = ok && out->put8(static_cast<uint8_t>(kDiff | ((sr + 2) << 4) | ((sg + 2) << 2) | (sb + 2)));
        } else if (sg >= -32 && sg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
          ok = ok && out->put8(static_cast<uint8_t>(kLuma | (sg + 32))) && out->put8(static_cast<uint8_t>(((dr_dg + 8) << 4) | (db_dg + 8)));
        } else {
          ok = ok && out->put8(kRgb) && out->put8(static_cast<uint8_t>(r)) && out->put8(static_cast<uint8_t>(g)) && out->put8(static_cast<uint8_t>(b));
        }
      } else {
        ok = ok && out->put8(kRgba) && out->put8(static_cast<uint8_t>(r)) && out->put8(static_cast<uint8_t>(g)) && out->put8(static_cast<uint8_t>(b)) &&
             out->put8(static_cast<uint8_t>(a));
      }
    }
    pr = r; pg = g; pb = b; pa = a;
  }
  static const uint8_t kEnd[8] = {0, 0, 0, 0, 0, 0, 0, 1};
  return ok && out->append(ConstByteView(kEnd, 8));
}

}  // namespace media
