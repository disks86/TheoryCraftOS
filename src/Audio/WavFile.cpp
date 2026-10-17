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

#include "Audio/WavFile.hpp"

namespace audio {

namespace {
uint32_t le16(ConstByteView v, uint64_t at) { return v[at] | (v[at + 1] << 8); }
uint32_t le32(ConstByteView v, uint64_t at) { return le16(v, at) | (le16(v, at + 2) << 16); }
bool tag_is(ConstByteView v, uint64_t at, const char* tag) {
  for (int i = 0; i < 4; ++i)
    if (v[at + i] != static_cast<uint8_t>(tag[i])) return false;
  return true;
}
}  // namespace

bool WavFile::parse(ConstByteView file, StreamFormat* format, ConstByteView* data) {
  if (file.size() < 12 || !tag_is(file, 0, "RIFF") || !tag_is(file, 8, "WAVE")) return false;
  StreamFormat found;
  bool have_format = false;
  uint64_t at = 12;
  while (at + 8 <= file.size()) {
    const uint64_t size = le32(file, at + 4), body = at + 8;
    const uint64_t available = file.size() - body;
    if (tag_is(file, at, "fmt ")) {
      if (size < 16 || available < 16) return false;
      const uint32_t tag = le16(file, body), channels = le16(file, body + 2), rate = le32(file, body + 4);
      const uint32_t block = le16(file, body + 12), bits = le16(file, body + 14);
      if (tag != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) return false;
      found.sample_rate = rate;
      found.channels = static_cast<uint8_t>(channels);
      found.type = bits == 16 ? SampleType::S16 : SampleType::U8;
      if (!found.valid() || block != found.frame_size()) return false;
      have_format = true;
    } else if (tag_is(file, at, "data")) {
      if (!have_format) return false;  // the format must come first
      const uint64_t length = size < available ? size : available;
      const uint64_t whole = length - length % found.frame_size();
      *format = found;
      *data = file.subview(body, whole);
      return true;
    }
    at = body + size + (size & 1);  // chunks are padded to an even length
  }
  return false;
}

}  // namespace audio
