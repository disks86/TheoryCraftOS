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

#include <initializer_list>
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

AviFile::AviFile(allocators::IAllocator& allocator)
    : allocator_(allocator), headers_(allocator), scratch_(allocator), video_frames_(allocator), audio_chunks_(allocator), audio_starts_(allocator), keyframes_(allocator) {}

namespace {
bool is_stream_chunk_id(const uint8_t* id) { return id[0] >= '0' && id[0] <= '9' && id[1] >= '0' && id[1] <= '9'; }
}  // namespace

// Finds the chunks of the two streams. A chunk's id is the stream number in two digits and a type ("00dc").
void AviFile::scan_movi(media::IByteSource& source, uint64_t start, uint64_t end) {
  uint64_t at = start;
  while (at + 8 <= end) {
    uint8_t h[12] = {};
    if (source.read(at, ByteView(h, 12)) < 8) return;
    const uint64_t size = le32(ConstByteView(h, 12), 4);
    const uint64_t body = at + 8;
    if (tag_is(ConstByteView(h, 12), 0, "LIST") && size >= 4) {  // "rec " lists group chunks: look inside
      scan_movi(source, body + 4, body + size < end ? body + size : end);
    } else if (is_stream_chunk_id(h)) {
      const uint32_t stream = (h[0] - '0') * 10 + (h[1] - '0');
      const bool is_audio_chunk = h[2] == 'w' && h[3] == 'b';
      const bool is_video_chunk = h[2] == 'd' && (h[3] == 'c' || h[3] == 'b');
      uint64_t length = size;
      if (body + length > source.size()) length = source.size() - body;  // a truncated last chunk
      if (stream == video_stream_ && is_video_chunk && length > 0) video_frames_.push_back({body, static_cast<uint32_t>(length)});
      else if (stream == audio_stream_ && is_audio_chunk && length > 0) audio_chunks_.push_back({body, static_cast<uint32_t>(length)});
    }
    at = body + size + (size & 1);
  }
}

namespace {
// Calls fn(entry) for each 16-byte entry of an "idx1" index, reading it in pieces. fn returns false to stop.
template <class Fn>
void for_each_index_entry(media::IByteSource& source, uint64_t start, uint64_t size, Fn fn) {
  uint8_t buffer[16 * 2048];
  for (uint64_t done = 0; done + 16 <= size;) {
    uint64_t n = size - done < sizeof buffer ? size - done : sizeof buffer;
    n -= n % 16;
    if (source.read(start + done, ByteView(buffer, n)) != n) return;
    for (uint64_t i = 0; i < n; i += 16)
      if (!fn(buffer + i)) return;
    done += n;
  }
}
}  // namespace

// The "idx1" index lists every chunk with a flag for keyframes; its offsets count from the start of the "movi" list's
// data (the "movi" tag) in most files, but from the start of the file in some. Builds the frame and sound tables from it. False if it does
// not describe the file (then the "movi" list is walked instead).
bool AviFile::load_index(media::IByteSource& source, uint64_t index_start, uint64_t index_size, uint64_t movi_tag_offset) {
  if (index_size < 16 || video_stream_ > 99) return false;
  const uint8_t vid[2] = {static_cast<uint8_t>('0' + video_stream_ / 10), static_cast<uint8_t>('0' + video_stream_ % 10)};
  const uint8_t aid[2] = {static_cast<uint8_t>('0' + (audio_stream_ <= 99 ? audio_stream_ / 10 : 0)), static_cast<uint8_t>('0' + (audio_stream_ <= 99 ? audio_stream_ % 10 : 0))};
  // Which convention? An entry's offset leads to its chunk header: look at the first video entry's.
  uint64_t base = movi_tag_offset;
  bool found = false;
  for_each_index_entry(source, index_start, index_size, [&](const uint8_t* e) {
    if (e[0] != vid[0] || e[1] != vid[1] || e[2] != 'd') return true;
    for (uint64_t candidate : {movi_tag_offset, uint64_t{0}}) {
      uint8_t header[8];
      const uint64_t at = candidate + le32(ConstByteView(e, 16), 8);
      if (source.read(at, ByteView(header)) == 8 && header[0] == e[0] && header[1] == e[1] && header[2] == e[2] && header[3] == e[3]) {
        base = candidate;
        found = true;
        return false;
      }
    }
    return false;
  });
  if (!found) return false;
  video_frames_.clear();
  audio_chunks_.clear();
  keyframes_.clear();
  bool ok = true;
  for_each_index_entry(source, index_start, index_size, [&](const uint8_t* e) {
    const uint64_t offset = base + le32(ConstByteView(e, 16), 8) + 8;
    uint64_t length = le32(ConstByteView(e, 16), 12);
    if (offset > source.size()) return true;
    if (offset + length > source.size()) length = source.size() - offset;
    if (length == 0) return true;
    if (e[0] == vid[0] && e[1] == vid[1] && e[2] == 'd' && (e[3] == 'c' || e[3] == 'b')) {
      ok = ok && video_frames_.push_back({offset, static_cast<uint32_t>(length)}) && keyframes_.push_back((le32(ConstByteView(e, 16), 4) & 0x10) != 0);
    } else if (audio_stream_ <= 99 && e[0] == aid[0] && e[1] == aid[1] && e[2] == 'w' && e[3] == 'b') {
      ok = ok && audio_chunks_.push_back({offset, static_cast<uint32_t>(length)});
    }
    return ok;
  });
  if (!ok || video_frames_.count() == 0) return false;
  // The last frame's chunk header should agree with the index too.
  uint8_t header[8];
  const Chunk& last = video_frames_[video_frames_.count() - 1];
  if (source.read(last.offset - 8, ByteView(header)) != 8 || header[0] != vid[0] || header[1] != vid[1] || header[2] != 'd') return false;
  return true;
}

// With the table built by walking the list: the keyframe flags from the index.
void AviFile::read_index_flags(media::IByteSource& source, uint64_t index_start, uint64_t index_size, uint64_t movi_tag_offset) {
  if (index_size < 16 || video_frames_.count() == 0 || video_stream_ > 99) return;
  const uint8_t vid[2] = {static_cast<uint8_t>('0' + video_stream_ / 10), static_cast<uint8_t>('0' + video_stream_ % 10)};
  uint64_t base = movi_tag_offset;
  bool first = true;
  uint64_t next = 0;
  for_each_index_entry(source, index_start, index_size, [&](const uint8_t* e) {
    if (e[0] != vid[0] || e[1] != vid[1] || e[2] != 'd') return true;
    if (first) {
      if (uint64_t{le32(ConstByteView(e, 16), 8)} + 8 == video_frames_[0].offset) base = 0;
      first = false;
    }
    if (next >= video_frames_.count()) return false;
    const uint64_t offset = base + le32(ConstByteView(e, 16), 8) + 8;
    if (offset == video_frames_[next].offset) keyframes_[next] = (le32(ConstByteView(e, 16), 4) & 0x10) != 0;
    ++next;
    return true;
  });
}

bool AviFile::open(ConstByteView file) {
  memory_.reset(file);
  const bool ok = open(memory_);
  in_memory_ = ok;
  return ok;
}

bool AviFile::open(media::IByteSource& source) {
  source_ = &source;
  in_memory_ = false;
  video_ = AviVideoInfo();
  audio_ = AviAudioInfo();
  video_frames_.clear();
  audio_chunks_.clear();
  audio_starts_.clear();
  audio_total_ = 0;
  keyframes_.clear();
  headers_.clear();
  video_stream_ = audio_stream_ = 0xFFFFFFFF;
  all_keyframes_ = true;
  uint8_t head[12];
  if (source.size() < 12 || source.read(0, ByteView(head)) != 12 || !tag_is(ConstByteView(head, 12), 0, "RIFF") || !tag_is(ConstByteView(head, 12), 8, "AVI ")) return false;

  media::CachedByteSource cache(source, allocator_, 65536);
  uint64_t movi_start = 0, movi_end = 0, index_start = 0, index_size = 0;
  bool have_headers = false;
  uint64_t at = 12;
  while (at + 8 <= source.size()) {
    uint8_t h[12] = {};
    if (cache.read(at, ByteView(h, 12)) < 8) break;
    const uint64_t size = le32(ConstByteView(h, 12), 4), body = at + 8;
    const uint64_t available = source.size() - body;
    const uint64_t length = size < available ? size : available;
    if (tag_is(ConstByteView(h, 12), 0, "LIST") && length >= 4) {
      if (tag_is(ConstByteView(h, 12), 8, "hdrl")) {
        if (length - 4 > (16u << 20) || !headers_.put_zeros(length - 4) ||
            cache.read(body + 4, ByteView(const_cast<uint8_t*>(headers_.view().data()), length - 4)) != length - 4)
          return false;
        have_headers = parse_headers(headers_.view()) || have_headers;
      } else if (tag_is(ConstByteView(h, 12), 8, "movi")) {
        movi_start = body + 4;
        movi_end = body + length;
      }
    } else if (tag_is(ConstByteView(h, 12), 0, "idx1")) {
      index_start = body;
      index_size = length;
    }
    at = body + size + (size & 1);
  }
  if (!have_headers || movi_end == 0) return false;
  const bool intra = video_.codec == VideoCodec::Mjpeg || video_.codec == VideoCodec::Raw;
  bool indexed = index_size >= 16 && load_index(source, index_start, index_size, movi_start - 4 /* the "movi" tag */);
  if (indexed) {
    if (intra) {
      for (uint64_t i = 0; i < keyframes_.count(); ++i) keyframes_[i] = true;
    } else {
      keyframes_[0] = true;  // a stream starts with one
      all_keyframes_ = false;
    }
  } else {
    video_frames_.clear();
    audio_chunks_.clear();
    keyframes_.clear();
    scan_movi(cache, movi_start, movi_end);
    if (video_frames_.count() == 0) return false;
    keyframes_.reserve(video_frames_.count());
    for (uint64_t i = 0; i < video_frames_.count(); ++i) keyframes_.push_back(intra);
    if (!intra && index_size) {
      for (uint64_t i = 0; i < keyframes_.count(); ++i) keyframes_[i] = false;
      read_index_flags(source, index_start, index_size, movi_start - 4);
      keyframes_[0] = true;
      all_keyframes_ = false;
    } else if (!intra) {
      all_keyframes_ = false;  // no index: only the first is known to be
      for (uint64_t i = 0; i < keyframes_.count(); ++i) keyframes_[i] = i == 0;
    }
  }

  video_.frame_count = static_cast<uint32_t>(video_frames_.count());
  for (uint64_t i = 0; i < audio_chunks_.count(); ++i) {
    if (!audio_starts_.push_back(audio_total_)) return false;
    audio_total_ += audio_chunks_[i].size;
  }
  audio_.bytes = audio_total_;
  if (audio_.present && audio_.tag == 1) audio_.bytes -= audio_.bytes % audio_.format.frame_size();
  return true;
}

int64_t AviFile::frame_time_us(uint32_t index) const {
  if (video_.rate == 0) return 0;
  return static_cast<int64_t>(static_cast<int64_t>(index) * 1000000 * video_.scale / video_.rate);
}

bool AviFile::frame(uint32_t index, VideoPacket* packet) const {
  if (index >= video_frames_.count() || !source_) return false;
  const Chunk& c = video_frames_[index];
  if (in_memory_) {
    packet->data = memory_.data().subview(c.offset, c.size);
  } else {
    scratch_.clear();
    if (!scratch_.put_zeros(c.size) || source_->read(c.offset, ByteView(const_cast<uint8_t*>(scratch_.view().data()), c.size)) != c.size) return false;
    packet->data = scratch_.view();
  }
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

uint64_t AviFile::AudioChunks::read(uint64_t offset, ByteView out) {
  AviFile& f = owner_;
  if (offset >= f.audio_total_ || !f.source_) return 0;
  // The chunk that holds `offset`: the last one that starts at or before it.
  uint64_t lo = 0, hi = f.audio_chunks_.count() - 1;
  while (lo < hi) {
    const uint64_t mid = (lo + hi + 1) / 2;
    if (f.audio_starts_[mid] <= offset) lo = mid;
    else hi = mid - 1;
  }
  uint64_t done = 0;
  for (uint64_t i = lo; i < f.audio_chunks_.count() && done < out.size(); ++i) {
    const Chunk& c = f.audio_chunks_[i];
    const uint64_t within = offset + done - f.audio_starts_[i];
    uint64_t n = c.size - within;
    if (n > out.size() - done) n = out.size() - done;
    const uint64_t got = f.source_->read(c.offset + within, out.subview(done, n));
    done += got;
    if (got < n) break;
  }
  return done;
}

uint64_t AviFile::copy_audio(ByteView out) const {
  AviFile& self = const_cast<AviFile&>(*this);
  uint64_t want = out.size() < audio_total_ ? out.size() : audio_total_;
  const uint64_t done = self.audio_chunks_source_.read(0, out.first(want));
  return done - (audio_.present && audio_.tag == 1 ? done % audio_.format.frame_size() : 0);
}

}  // namespace video
