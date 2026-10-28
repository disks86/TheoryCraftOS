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

#include "Hardware/GraphicsProcessingUnit/AMD/EdidAudio.hpp"

namespace hardware {

namespace {
constexpr uint32_t kBlockBytes = 128;
constexpr uint8_t kCtaExtensionTag = 0x02;
constexpr uint8_t kCtaMinRevision = 3;
constexpr uint32_t kCtaDataStart = 4;                   // the data blocks begin here in the extension block
constexpr uint8_t kDbAudio = 1, kDbVendor = 3, kDbSpeaker = 4;
constexpr uint8_t kHdmiOui[3] = {0x03, 0x0C, 0x00};     // 00-0C-03, as the vendor block stores it
constexpr uint32_t kHdmiVsdbMinPayload = 5;

// The vendor block's lip sync fields (drm_parse_hdmi_vsdb_audio): db[0] is the header byte, so the payload byte n is db[n + 1]
void parse_hdmi_latency(ConstByteView db, uint32_t len, EdidAudio::Info* out) {
  if (len >= 8) {
    out->latency_present[0] = db[8] >> 7 & 1;
    out->latency_present[1] = db[8] >> 6 & 1;
  }
  if (len >= 9) out->video_latency[0] = db[9];
  if (len >= 10) out->audio_latency[0] = db[10];
  if (len >= 11) out->video_latency[1] = db[11];
  if (len >= 12) out->audio_latency[1] = db[12];
}
}  // namespace

void EdidAudio::parse(ConstByteView edid, Info* out) {
  *out = Info();
  bool have_vsdb = false;
  for (uint32_t base = kBlockBytes; base <= edid.size() && edid.size() - base >= kBlockBytes; base += kBlockBytes) {
    const ConstByteView block = edid.subview(base, kBlockBytes);
    if (block[0] != kCtaExtensionTag || block[1] < kCtaMinRevision) continue;
    uint32_t end = block[2];
    if (end == 0) end = 127;
    if (end < kCtaDataStart || end > 127) continue;
    for (uint32_t at = kCtaDataStart; at < end;) {
      const uint8_t tag = block[at] >> 5;
      const uint32_t len = block[at] & 0x1F;
      if (at + 1 + len > end) break;                    // (a data block that runs past the end of the data blocks)
      const ConstByteView db = block.subview(at, 1 + len);
      if (tag == kDbAudio) {
        for (uint32_t j = 1; j + 3 <= len + 1 && out->sad_count < kMaxSads; j += 3) {
          Sad& sad = out->sads[out->sad_count++];
          sad.format = db[j] >> 3 & 0x0F;
          sad.channels = db[j] & 0x07;
          sad.freq = db[j + 1] & 0x7F;
          sad.byte2 = db[j + 2];
        }
      } else if (tag == kDbSpeaker && len == 3 && out->speaker_count == 0) {
        out->speaker_count = len;
        out->speaker_allocation = db[1];
      } else if (tag == kDbVendor && len >= kHdmiVsdbMinPayload && !have_vsdb && db[1] == kHdmiOui[0] && db[2] == kHdmiOui[1] && db[3] == kHdmiOui[2]) {
        have_vsdb = true;
        parse_hdmi_latency(db, len, out);
      }
      at += 1 + len;
    }
  }
}

}  // namespace hardware
