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

// EdidAudio against hand-built EDIDs: the CTA extension block's audio data blocks (Short Audio Descriptors), speaker allocation block and HDMI vendor block with the lip sync latency,
// written from the CTA-861 layouts as literal bytes; blocks of several extensions, blocks to be skipped (revision, tag, bad data offset), data blocks that run past the end, the
// descriptor limit, and every bit flip and truncation of a rich EDID under the sanitizers.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/EdidAudio.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
using Bytes = std::vector<uint8_t>;

Bytes base_block() {
  Bytes b(128, 0);
  b[0] = 0x00; b[1] = 0xFF; b[2] = 0xFF; b[3] = 0xFF; b[4] = 0xFF; b[5] = 0xFF; b[6] = 0xFF; b[7] = 0x00;
  b[126] = 1;
  return b;
}

// An extension block: the tag, the revision, the data blocks (back to back from byte 4) and the offset of the end of them, as given or as it comes out
Bytes ext(uint8_t tag, uint8_t rev, const Bytes& data_blocks, int offset = -1) {
  Bytes b(128, 0);
  b[0] = tag;
  b[1] = rev;
  b[2] = static_cast<uint8_t>(offset >= 0 ? offset : 4 + data_blocks.size());
  for (size_t i = 0; i < data_blocks.size(); ++i) b[4 + i] = data_blocks[i];
  return b;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes r;
  for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end());
  return r;
}

// a data block: tag in bits 7:5, payload length in 4:0
Bytes db(uint8_t tag, std::initializer_list<uint8_t> payload) {
  Bytes r;
  r.push_back(static_cast<uint8_t>(tag << 5 | payload.size()));
  r.insert(r.end(), payload.begin(), payload.end());
  return r;
}

// stereo PCM at 32, 44.1 and 48 kHz with 16, 20 and 24 bits; AC-3 6 channels at 32 to 48 kHz, 640 kbit/s (80 * 8); DTS 8 channels at 44.1 to 96 kHz; PCM 8 channels at 48, 96, 192 kHz
const Bytes kSads = db(1, {0x09, 0x07, 0x07, 0x15, 0x07, 0x50, 0x3F, 0x1E, 0x18, 0x0F, 0x54, 0x07});
// the speaker allocation: front left/right and front center, and the HDMI vendor block: physical address 1.0.0.0, flags, max TMDS, latency present both, video 30 audio 40, interlaced video 50 audio 60
const Bytes kSpeakers = db(4, {0x05, 0x00, 0x00});
const Bytes kHdmi = db(3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60});
}  // namespace

void test_blocks() {
  EdidAudio::Info info;
  // nothing: no extension, an empty view, base block only
  EdidAudio::parse(ConstByteView(), &info);
  CHECK(info.sad_count == 0 && info.speaker_count == 0 && !info.latency_present[0] && !info.latency_present[1]);
  Bytes edid = base_block();
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 0 && info.speaker_count == 0);
  // the rich one
  edid = cat({base_block(), ext(0x02, 3, cat({kSads, kSpeakers, kHdmi}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 4);
  CHECK(info.sads[0].format == 1 && info.sads[0].channels == 1 && info.sads[0].freq == 0x07 && info.sads[0].byte2 == 0x07);
  CHECK(info.sads[1].format == 2 && info.sads[1].channels == 5 && info.sads[1].freq == 0x07 && info.sads[1].byte2 == 0x50);
  CHECK(info.sads[2].format == 7 && info.sads[2].channels == 7 && info.sads[2].freq == 0x1E && info.sads[2].byte2 == 0x18);
  CHECK(info.sads[3].format == 1 && info.sads[3].channels == 7 && info.sads[3].freq == 0x54 && info.sads[3].byte2 == 0x07);
  CHECK(info.speaker_count == 3 && info.speaker_allocation == 0x05);
  CHECK(info.latency_present[0] && info.latency_present[1] && info.video_latency[0] == 30 && info.audio_latency[0] == 40 && info.video_latency[1] == 50 && info.audio_latency[1] == 60);
  // the top bit of the frequencies byte is reserved, and the format is four bits
  edid = cat({base_block(), ext(0x02, 3, db(1, {0xFF, 0xFF, 0xAB}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 1 && info.sads[0].format == 15 && info.sads[0].channels == 7 && info.sads[0].freq == 0x7F && info.sads[0].byte2 == 0xAB);
  // a payload that is not a whole number of descriptors: the last partial one is dropped
  edid = cat({base_block(), ext(0x02, 3, db(1, {0x09, 0x07, 0x07, 0x15, 0x07}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 1);
  edid = cat({base_block(), ext(0x02, 3, db(1, {0x09, 0x07}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 0);
  edid = cat({base_block(), ext(0x02, 3, db(1, {}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 0);
}

void test_vendor_and_speakers() {
  EdidAudio::Info info;
  // latency fields by the length of the vendor block: 5 and 6 and 7 bytes have none; 8 only the flags; each further byte one more field
  for (uint32_t len = 5; len <= 12; ++len) {
    Bytes payload = {0x03, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60};
    payload.resize(len);
    Bytes v;
    v.push_back(static_cast<uint8_t>(3 << 5 | len));
    v.insert(v.end(), payload.begin(), payload.end());
    Bytes edid = cat({base_block(), ext(0x02, 3, v)});
    EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
    CHECK(info.latency_present[0] == (len >= 8) && info.latency_present[1] == (len >= 8));
    CHECK(info.video_latency[0] == (len >= 9 ? 30 : 0) && info.audio_latency[0] == (len >= 10 ? 40 : 0) && info.video_latency[1] == (len >= 11 ? 50 : 0) && info.audio_latency[1] == (len >= 12 ? 60 : 0));
  }
  // the present flags are separate bits: 0x80 progressive only, 0x40 interlaced only
  Bytes edid = cat({base_block(), ext(0x02, 3, db(3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80, 1, 2, 3, 4}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.latency_present[0] && !info.latency_present[1] && info.video_latency[0] == 1 && info.audio_latency[0] == 2 && info.video_latency[1] == 3 && info.audio_latency[1] == 4);
  edid = cat({base_block(), ext(0x02, 3, db(3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 0x00, 0x40, 1, 2, 3, 4}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(!info.latency_present[0] && info.latency_present[1]);
  // not the HDMI vendor block: another OUI (each byte), the HDMI forum's, a payload under 5 bytes
  const Bytes others[] = {db(3, {0x04, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60}), db(3, {0x03, 0x0D, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60}),
                          db(3, {0x03, 0x0C, 0x01, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60}), db(3, {0xD8, 0x5D, 0xC4, 0x01, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60}),
                          db(3, {0x03, 0x0C, 0x00, 0x10}), db(5, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60})};
  for (const Bytes& o : others) {
    edid = cat({base_block(), ext(0x02, 3, o)});
    EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
    CHECK(!info.latency_present[0] && !info.latency_present[1] && info.video_latency[0] == 0);
  }
  // the first HDMI vendor block counts
  edid = cat({base_block(), ext(0x02, 3, cat({db(3, {0x03, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0x80, 7, 8}), kHdmi}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.latency_present[0] && !info.latency_present[1] && info.video_latency[0] == 7 && info.audio_latency[0] == 8 && info.video_latency[1] == 0);
  // speaker allocation: the payload must be 3 bytes; the first good one counts
  const struct { Bytes block; uint32_t count; uint8_t alloc; } speakers[] = {
      {db(4, {0x01, 0x00}), 0, 0}, {db(4, {0x01, 0x00, 0x00, 0x00}), 0, 0}, {db(4, {}), 0, 0}, {db(4, {0x7F, 0x12, 0x34}), 3, 0x7F}, {db(2, {0x01, 0x00, 0x00}), 0, 0}};
  for (const auto& s : speakers) {
    edid = cat({base_block(), ext(0x02, 3, s.block)});
    EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
    CHECK(info.speaker_count == s.count && info.speaker_allocation == s.alloc);
  }
  edid = cat({base_block(), ext(0x02, 3, cat({db(4, {0x01, 0x00}), db(4, {0x03, 0x00, 0x00}), db(4, {0x7F, 0x00, 0x00})}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.speaker_count == 3 && info.speaker_allocation == 0x03);
}

void test_extensions() {
  EdidAudio::Info info;
  // several CTA extension blocks: the descriptors of all of them, in order
  Bytes edid = cat({base_block(), ext(0x02, 3, db(1, {0x09, 0x07, 0x07})), ext(0x02, 3, db(1, {0x15, 0x07, 0x50})), ext(0x02, 3, db(1, {0x3F, 0x1E, 0x18}))});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 3 && info.sads[0].format == 1 && info.sads[1].format == 2 && info.sads[2].format == 7);
  // skipped: other tags (a block map 0xF0, VTB 0x10, DisplayID 0x70), revisions under 3, data offsets under 4 and over 127
  const Bytes data = db(1, {0x09, 0x07, 0x07});
  const Bytes skipped[] = {ext(0xF0, 3, data), ext(0x10, 3, data), ext(0x70, 3, data), ext(0x02, 0, data), ext(0x02, 1, data), ext(0x02, 2, data), ext(0x02, 3, data, 3), ext(0x02, 3, data, 1),
                           ext(0x02, 3, data, 128), ext(0x02, 3, data, 255)};
  for (const Bytes& s : skipped) {
    edid = cat({base_block(), s});
    EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
    CHECK(info.sad_count == 0);
  }
  // revisions above 3 are fine; an offset of 0 means "up to 127"; 127 itself too
  const Bytes taken[] = {ext(0x02, 4, data), ext(0x02, 255, data), ext(0x02, 3, data, 0), ext(0x02, 3, data, 127), ext(0x02, 3, data, 8)};
  for (const Bytes& t : taken) {
    edid = cat({base_block(), t});
    EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
    CHECK(info.sad_count == 1);
  }
  // the data blocks end at the offset: a block that starts before it and runs past it is not read, and the ones after it are not either
  edid = cat({base_block(), ext(0x02, 3, cat({db(1, {0x09, 0x07, 0x07}), db(1, {0x15, 0x07, 0x50})}), 8)});       // (the second block starts at 8: not read)
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 1 && info.sads[0].format == 1);
  edid = cat({base_block(), ext(0x02, 3, cat({db(1, {0x09, 0x07, 0x07}), db(1, {0x15, 0x07, 0x50})}), 12)});     // (it ends at 12 exactly: read)
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 2);
  edid = cat({base_block(), ext(0x02, 3, cat({db(1, {0x09, 0x07, 0x07}), db(1, {0x15, 0x07, 0x50})}), 11)});     // (one short: it would end at 12)
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 1);
  edid = cat({base_block(), ext(0x02, 3, cat({db(1, {0x09, 0x07, 0x07}), db(1, {0x15, 0x07, 0x50})}), 10)});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 1);
  // a length that goes past the 127th byte of a block that has data blocks up to 127: the block is dropped
  Bytes tail(120, 0x00);
  tail[119] = 0x3F;     // tag 1, length 31 at the last data byte
  edid = cat({base_block(), ext(0x02, 3, tail, 0)});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 0);
  // the limit: ten descriptors to a data block, as many blocks as there are room for
  Bytes many;
  for (int i = 0; i < 2; ++i) many = cat({many, db(1, {0x09, 0x07, 0x07, 0x09, 0x07, 0x07, 0x09, 0x07, 0x07, 0x09, 0x07, 0x07, 0x09, 0x07, 0x07, 0x09, 0x07, 0x07, 0x09, 0x07, 0x07, 0x09, 0x07, 0x07,
                                                    0x09, 0x07, 0x07, 0x09, 0x07, 0x07})});
  edid = cat({base_block(), ext(0x02, 3, many)});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 20);
  edid = cat({base_block(), ext(0x02, 3, many), ext(0x02, 3, many)});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == EdidAudio::kMaxSads);
  // trailing bytes that are not a whole block are not read; neither is a view shorter than two blocks
  edid = cat({base_block(), ext(0x02, 3, data)});
  edid.resize(edid.size() - 1);
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 0);
  edid = cat({base_block(), ext(0x02, 3, data), Bytes(50, 0x02)});
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  CHECK(info.sad_count == 1);
  // the base block alone is never read as an extension, even if it looks like one
  Bytes only = ext(0x02, 3, data);
  EdidAudio::parse(ConstByteView(only.data(), only.size()), &info);
  CHECK(info.sad_count == 0);
  // the old info is cleared
  info.sad_count = 5;
  info.speaker_count = 3;
  info.latency_present[0] = true;
  EdidAudio::parse(ConstByteView(), &info);
  CHECK(info.sad_count == 0 && info.speaker_count == 0 && !info.latency_present[0]);
}

void test_flips() {
  const Bytes rich = cat({base_block(), ext(0x02, 3, cat({kSads, kSpeakers, kHdmi})), ext(0x02, 3, cat({kSpeakers, kSads}))});
  for (size_t at = 0; at < rich.size(); ++at) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes e = rich;
      e[at] ^= static_cast<uint8_t>(1 << bit);
      EdidAudio::Info info;
      EdidAudio::parse(ConstByteView(e.data(), e.size()), &info);
      CHECK(info.sad_count <= EdidAudio::kMaxSads && (info.speaker_count == 0 || info.speaker_count == 3));
    }
  }
  for (size_t n = 0; n <= rich.size(); ++n) {
    EdidAudio::Info info;
    EdidAudio::parse(ConstByteView(rich.data(), n), &info);
    CHECK(info.sad_count <= EdidAudio::kMaxSads);
  }
  // noise
  uint32_t seed = 12345;
  for (int round = 0; round < 300; ++round) {
    Bytes e = rich;
    for (int k = 0; k < 8; ++k) {
      seed = seed * 1664525u + 1013904223u;
      e[(seed >> 8) % e.size()] = static_cast<uint8_t>(seed >> 24);
    }
    EdidAudio::Info info;
    EdidAudio::parse(ConstByteView(e.data(), e.size()), &info);
    CHECK(info.sad_count <= EdidAudio::kMaxSads);
  }
}

int main() {
  test_blocks();
  test_vendor_and_speakers();
  test_extensions();
  test_flips();
  printf(g_failed ? "EdidAudioTest: %d FAILED\n" : "EdidAudioTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
