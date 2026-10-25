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

// G.711 (ITU-T), the two telephone codecs every WebRTC endpoint has: PCMU (mu-law, payload type 0) and PCMA (A-law, payload type 8), 8 kHz, one byte for each 16-bit sample.
#pragma once
#include <stdint.h>

namespace rtc {

inline uint8_t g711_ulaw_encode(int16_t sample) {
  static const int16_t kEnd[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
  int pcm = sample >> 2, mask;
  if (pcm < 0) {
    pcm = -pcm;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }
  if (pcm > 8159) pcm = 8159;
  pcm += 0x21;
  int seg = 0;
  while (seg < 8 && pcm > kEnd[seg]) ++seg;
  if (seg >= 8) return static_cast<uint8_t>(0x7F ^ mask);
  return static_cast<uint8_t>(((seg << 4) | ((pcm >> (seg + 1)) & 0xF)) ^ mask);
}

inline int16_t g711_ulaw_decode(uint8_t byte) {
  const int u = ~byte & 0xFF;
  int t = ((u & 0xF) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return static_cast<int16_t>((u & 0x80) ? 0x84 - t : t - 0x84);
}

inline uint8_t g711_alaw_encode(int16_t sample) {
  static const int16_t kEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
  int pcm = sample >> 3, mask;
  if (pcm >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    pcm = -pcm - 1;
  }
  int seg = 0;
  while (seg < 8 && pcm > kEnd[seg]) ++seg;
  if (seg >= 8) return static_cast<uint8_t>(0x7F ^ mask);
  int value = seg << 4;
  value |= seg < 2 ? (pcm >> 1) & 0xF : (pcm >> seg) & 0xF;
  return static_cast<uint8_t>(value ^ mask);
}

inline int16_t g711_alaw_decode(uint8_t byte) {
  const int a = byte ^ 0x55;
  int t = (a & 0xF) << 4;
  const int seg = (a & 0x70) >> 4;
  if (seg == 0) {
    t += 8;
  } else {
    t += 0x108;
    if (seg > 1) t <<= seg - 1;
  }
  return static_cast<int16_t>((a & 0x80) ? t : -t);
}

}  // namespace rtc
