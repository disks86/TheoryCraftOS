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

#include "Video/AviFile.hpp"

namespace video {

using namespace hardware;

namespace {
uint32_t le16(ConstByteView v, uint64_t at) { return v[at] | (v[at + 1] << 8); }
uint32_t le32(ConstByteView v, uint64_t at) { return le16(v, at) | (le16(v, at + 2) << 16); }
bool tag_is(ConstByteView v, uint64_t at, const char* tag) {
  for (int i = 0; i < 4; ++i)
    if (v[at + i] != static_cast<uint8_t>(tag[i])) return false;
  return true;
}
bool tag_is_ignore_case(ConstByteView v, uint64_t at, const char* tag) {
  for (int i = 0; i < 4; ++i) {
    uint8_t a = v[at + i], b = static_cast<uint8_t>(tag[i]);
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}
}  // namespace

// The stream list inside "hdrl": a "strh" and "strf" per stream, in stream order.
bool AviFile::parse_headers(ConstByteView list) {
  uint32_t stream = 0;
  uint64_t at = 0;
  while (at + 8 <= list.size()) {
    const uint64_t size = le32(list, at + 4);
    const uint64_t body = at + 8;
    if (body + size > list.size()) break;
    if (tag_is(list, at, "LIST") && size >= 4 && tag_is(list, body, "strl")) {
      // Inside: strh, strf, ...
      const ConstByteView strl = list.subview(body + 4, size - 4);
      bool have_header = false, is_video = false, is_audio = false;
      uint32_t scale = 1, rate = 0;
      uint64_t p = 0;
      while (p + 8 <= strl.size()) {
        const uint64_t csize = le32(strl, p + 4), cbody = p + 8;
        if (cbody + csize > strl.size()) break;
        if (tag_is(strl, p, "strh") && csize >= 36) {
          is_video = tag_is(strl, cbody, "vids");
          is_audio = tag_is(strl, cbody, "auds");
          scale = le32(strl, cbody + 20);
          rate = le32(strl, cbody + 24);
          have_header = true;
        } else if (tag_is(strl, p, "strf") && have_header) {
          if (is_video && !video_.present && csize >= 40) {
            video_.present = true;
            video_stream_ = stream;
            video_.width = le32(strl, cbody + 4);
            const int32_t h = static_cast<int32_t>(le32(strl, cbody + 8));
            video_.height = static_cast<uint32_t>(h < 0 ? -h : h);
            video_.rate = rate ? rate : 30;
            video_.scale = scale ? scale : 1;
            video_.supported = true;
            if (tag_is_ignore_case(strl, cbody + 16, "mjpg") || tag_is_ignore_case(strl, cbody + 16, "jpeg") ||
                tag_is_ignore_case(strl, cbody + 16, "mjpa")) {
              video_.codec = VideoCodec::Mjpeg;
            } else if (tag_is_ignore_case(strl, cbody + 16, "i420") || tag_is_ignore_case(strl, cbody + 16, "iyuv")) {
              video_.codec = VideoCodec::Raw;
              video_.raw_format = PixelFormat::I420;
            } else if (tag_is_ignore_case(strl, cbody + 16, "nv12")) {
              video_.codec = VideoCodec::Raw;
              video_.raw_format = PixelFormat::Nv12;
            } else if (tag_is_ignore_case(strl, cbody + 16, "h264") || tag_is_ignore_case(strl, cbody + 16, "avc1") ||
                       tag_is_ignore_case(strl, cbody + 16, "x264")) {
              video_.codec = VideoCodec::H264;
            } else if (tag_is_ignore_case(strl, cbody + 16, "hevc") || tag_is_ignore_case(strl, cbody + 16, "hvc1")) {
              video_.codec = VideoCodec::Hevc;
            } else if (tag_is_ignore_case(strl, cbody + 16, "vp90")) {
              video_.codec = VideoCodec::Vp9;
            } else if (tag_is_ignore_case(strl, cbody + 16, "av01")) {
              video_.codec = VideoCodec::Av1;
            } else {
              video_.supported = false;
            }
          } else if (is_audio && !audio_.present && csize >= 16) {
            const uint32_t tag = le16(strl, cbody), channels = le16(strl, cbody + 2), rate_hz = le32(strl, cbody + 4), bits = le16(strl, cbody + 14);
            audio::StreamFormat f;
            f.sample_rate = rate_hz;
            f.channels = static_cast<uint8_t>(channels);
            f.type = bits == 16 ? audio::SampleType::S16 : audio::SampleType::U8;
            const bool pcm = tag == 1 && (bits == 8 || bits == 16);
            const bool compressed = tag == 2 || tag == 6 || tag == 7 || tag == 0x11;
            if (compressed) f.type = audio::SampleType::S16;  // decoded
            if ((pcm || compressed) && f.valid()) {
              audio_.present = true;
              audio_.format = f;
              audio_.tag = tag;
              audio_.wave_format = strl.subview(cbody, csize);
              audio_stream_ = stream;
            }
          }
        }
        p = cbody + csize + (csize & 1);
      }
      ++stream;
    }
    at = body + size + (size & 1);
  }
  return video_.present;
}

// Finds the chunks of the two streams. A chunk's id is the stream number in two digits and a type ("00dc").
void AviFile::scan_movi(uint64_t start, uint64_t end) {
  uint64_t at = start;
  while (at + 8 <= end) {
    const uint64_t size = le32(file_, at + 4);
    const uint64_t body = at + 8;
    if (tag_is(file_, at, "LIST") && size >= 4) {  // "rec " lists group chunks: look inside
      scan_movi(body + 4, body + size < end ? body + size : end);
    } else if (file_[at] >= '0' && file_[at] <= '9' && file_[at + 1] >= '0' && file_[at + 1] <= '9') {
      const uint32_t stream = (file_[at] - '0') * 10 + (file_[at + 1] - '0');
      const bool is_audio_chunk = file_[at + 2] == 'w' && file_[at + 3] == 'b';
      const bool is_video_chunk = file_[at + 2] == 'd' && (file_[at + 3] == 'c' || file_[at + 3] == 'b');
      uint64_t length = size;
      if (body + length > file_.size()) length = file_.size() - body;  // a truncated last chunk
      if (stream == video_stream_ && is_video_chunk && length > 0) video_frames_.push_back({body, static_cast<uint32_t>(length)});
      else if (stream == audio_stream_ && is_audio_chunk && length > 0) audio_chunks_.push_back({body, static_cast<uint32_t>(length)});
    }
    at = body + size + (size & 1);
  }
}

// The "idx1" index lists every chunk with a flag for keyframes; its offsets count from the start of the "movi" list's
// data (the "movi" tag) in most files, but from the start of the file in some.
void AviFile::read_index(ConstByteView index, uint64_t movi_tag_offset) {
  const uint64_t entries = index.size() / 16;
  if (entries == 0 || video_frames_.count() == 0 || video_stream_ > 99) return;
  auto is_video_entry = [&](uint64_t at) {
    return index[at] == '0' + video_stream_ / 10 && index[at + 1] == '0' + video_stream_ % 10 && index[at + 2] == 'd';
  };
  // Which convention? An entry's offset leads to its chunk header; the data is 8 bytes on.
  uint64_t base = movi_tag_offset;
  for (uint64_t i = 0; i < entries; ++i) {
    if (!is_video_entry(i * 16)) continue;
    if (uint64_t{le32(index, i * 16 + 8)} + 8 == video_frames_[0].offset) base = 0;
    break;
  }
  uint64_t next = 0;
  for (uint64_t i = 0; i < entries && next < video_frames_.count(); ++i) {
    if (!is_video_entry(i * 16)) continue;
    const uint64_t offset = base + le32(index, i * 16 + 8) + 8;
    if (offset == video_frames_[next].offset) keyframes_[next] = (le32(index, i * 16 + 4) & 0x10) != 0;
    ++next;
  }
}

bool AviFile::open(ConstByteView file) {
  file_ = file;
  video_ = AviVideoInfo();
  audio_ = AviAudioInfo();
  video_frames_.clear();
  audio_chunks_.clear();
  keyframes_.clear();
  video_stream_ = audio_stream_ = 0xFFFFFFFF;
  all_keyframes_ = true;
  if (file.size() < 12 || !tag_is(file, 0, "RIFF") || !tag_is(file, 8, "AVI ")) return false;

  uint64_t movi_start = 0, movi_end = 0, index_start = 0, index_size = 0;
  bool have_headers = false;
  uint64_t at = 12;
  while (at + 8 <= file.size()) {
    const uint64_t size = le32(file, at + 4), body = at + 8;
    const uint64_t available = file.size() - body;
    const uint64_t length = size < available ? size : available;
    if (tag_is(file, at, "LIST") && length >= 4) {
      if (tag_is(file, body, "hdrl")) have_headers = parse_headers(file.subview(body + 4, length - 4)) || have_headers;
      else if (tag_is(file, body, "movi")) {
        movi_start = body + 4;
        movi_end = body + length;
      }
    } else if (tag_is(file, at, "idx1")) {
      index_start = body;
      index_size = length;
    }
    at = body + size + (size & 1);
  }
  if (!have_headers || movi_end == 0) return false;
  scan_movi(movi_start, movi_end);
  if (video_frames_.count() == 0) return false;

  video_.frame_count = static_cast<uint32_t>(video_frames_.count());
  keyframes_.reserve(video_frames_.count());
  const bool intra = video_.codec == VideoCodec::Mjpeg || video_.codec == VideoCodec::Raw;
  for (uint64_t i = 0; i < video_frames_.count(); ++i) keyframes_.push_back(intra);
  if (!intra && index_size) {
    for (uint64_t i = 0; i < keyframes_.count(); ++i) keyframes_[i] = false;
    read_index(file.subview(index_start, index_size), movi_start - 4 /* the "movi" tag */);
    keyframes_[0] = true;  // a stream starts with one
    all_keyframes_ = false;
  } else if (!intra) {
    all_keyframes_ = false;  // no index: only the first is known to be
    for (uint64_t i = 0; i < keyframes_.count(); ++i) keyframes_[i] = i == 0;
  }
  for (uint64_t i = 0; i < audio_chunks_.count(); ++i) audio_.bytes += audio_chunks_[i].size;
  if (audio_.present && audio_.tag == 1) audio_.bytes -= audio_.bytes % audio_.format.frame_size();
  return true;
}

int64_t AviFile::frame_time_us(uint32_t index) const {
  if (video_.rate == 0) return 0;
  return static_cast<int64_t>(static_cast<int64_t>(index) * 1000000 * video_.scale / video_.rate);
}

bool AviFile::frame(uint32_t index, VideoPacket* packet) const {
  if (index >= video_frames_.count()) return false;
  const Chunk& c = video_frames_[index];
  packet->data = file_.subview(c.offset, c.size);
  packet->pts_us = frame_time_us(index);
  packet->keyframe = keyframes_[index];
  return true;
}

uint32_t AviFile::frame_at_time(int64_t time_us) const {
  const uint32_t n = frame_count();
  if (n == 0 || time_us <= 0 || video_.scale == 0) return 0;
  const int64_t index = time_us * video_.rate / (1000000 * static_cast<int64_t>(video_.scale));
  return index >= static_cast<int64_t>(n) ? n - 1 : static_cast<uint32_t>(index);
}

uint32_t AviFile::keyframe_at_or_before(uint32_t index) const {
  if (index >= frame_count()) index = frame_count() ? frame_count() - 1 : 0;
  while (index > 0 && !keyframes_[index]) --index;
  return index;
}

uint64_t AviFile::copy_audio(ByteView out) const {
  uint64_t done = 0;
  for (uint64_t i = 0; i < audio_chunks_.count() && done < out.size(); ++i) {
    const Chunk& c = audio_chunks_[i];
    uint64_t n = c.size;
    if (n > out.size() - done) n = out.size() - done;
    out.subview(done, n).copy_from(file_.subview(c.offset, n));
    done += n;
  }
  return done - (audio_.present && audio_.tag == 1 ? done % audio_.format.frame_size() : 0);
}

}  // namespace video
