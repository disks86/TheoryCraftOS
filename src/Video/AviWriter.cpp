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

#include "Video/AviWriter.hpp"

namespace video {

using namespace hardware;

namespace {
uint32_t fourcc(const char* tag) {
  return static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(tag[3])) << 24);
}
}  // namespace

bool AviWriter::begin(VideoCodec codec, uint32_t width, uint32_t height, uint32_t fps_numerator, uint32_t fps_denominator,
                      PixelFormat raw_format, const audio::StreamFormat* audio) {
  if ((codec != VideoCodec::Mjpeg && codec != VideoCodec::Raw) || width == 0 || height == 0 || fps_numerator == 0 || fps_denominator == 0)
    return false;
  if (codec == VideoCodec::Raw && raw_format == PixelFormat::Bgra8888) return false;
  if (audio && !audio->valid()) return false;
  codec_ = codec;
  raw_format_ = raw_format;
  width_ = width;
  height_ = height;
  fps_numerator_ = fps_numerator;
  fps_denominator_ = fps_denominator;
  has_audio_ = audio != nullptr;
  if (audio) audio_ = *audio;
  movi_.clear();
  file_.clear();
  index_.clear();
  frames_ = 0;
  audio_bytes_ = 0;
  largest_chunk_ = 0;
  begun_ = true;
  return true;
}

bool AviWriter::add_chunk(const char* id, ConstByteView data, uint32_t flags) {
  if (!begun_ || data.size() > 0x7FFFFFF0u) return false;
  const uint32_t offset = static_cast<uint32_t>(movi_.size()) + 4;  // from the "movi" tag
  if (!movi_.put_tag(id) || !movi_.put32(static_cast<uint32_t>(data.size())) || !movi_.append(data) || ((data.size() & 1) && !movi_.put8(0))) return false;
  if (!index_.push_back({fourcc(id), flags, offset, static_cast<uint32_t>(data.size())})) return false;
  if (data.size() > largest_chunk_) largest_chunk_ = static_cast<uint32_t>(data.size());
  return true;
}

bool AviWriter::add_frame(ConstByteView data, bool keyframe) {
  if (!add_chunk("00dc", data, keyframe ? 0x10 : 0)) return false;
  ++frames_;
  return true;
}

bool AviWriter::add_audio(ConstByteView pcm) {
  if (!has_audio_) return false;
  if (!add_chunk("01wb", pcm, 0x10)) return false;
  audio_bytes_ += pcm.size();
  return true;
}

bool AviWriter::finish() {
  if (!begun_) return false;
  file_.clear();
  const uint32_t streams = has_audio_ ? 2 : 1;
  const uint32_t frame_time_us = static_cast<uint32_t>(uint64_t{1000000} * fps_denominator_ / fps_numerator_);
  const uint32_t audio_block = has_audio_ ? audio_.frame_size() : 0;
  const uint32_t audio_rate_bytes = has_audio_ ? audio_.sample_rate * audio_block : 0;

  uint32_t fourcc_codec = fourcc("MJPG");
  uint32_t image_size = 0;
  if (codec_ == VideoCodec::Raw) {
    fourcc_codec = raw_format_ == PixelFormat::I420 ? fourcc("I420") : fourcc("NV12");
    image_size = static_cast<uint32_t>(VideoFormat{width_, height_, raw_format_}.frame_bytes());
  }
  const uint32_t video_strl = 4 + 8 + 56 + 8 + 40;
  const uint32_t audio_strl = has_audio_ ? 4 + 8 + 56 + 8 + 16 : 0;
  const uint32_t hdrl_size = 4 + 8 + 56 + 8 + video_strl + (has_audio_ ? 8 + audio_strl : 0);

  bool ok = file_.put_tag("RIFF") && file_.put32(0) && file_.put_tag("AVI ");
  ok = ok && file_.put_tag("LIST") && file_.put32(hdrl_size) && file_.put_tag("hdrl");
  // The main header.
  ok = ok && file_.put_tag("avih") && file_.put32(56) && file_.put32(frame_time_us) && file_.put32(0) && file_.put32(0) &&
       file_.put32(0x10 /* has an index */) && file_.put32(frames_) && file_.put32(0) && file_.put32(streams) &&
       file_.put32(largest_chunk_) && file_.put32(width_) && file_.put32(height_) && file_.put_zeros(16);
  // Video stream.
  ok = ok && file_.put_tag("LIST") && file_.put32(video_strl) && file_.put_tag("strl");
  ok = ok && file_.put_tag("strh") && file_.put32(56) && file_.put_tag("vids") && file_.put32(fourcc_codec) && file_.put32(0) && file_.put16(0) &&
       file_.put16(0) && file_.put32(0) && file_.put32(fps_denominator_) && file_.put32(fps_numerator_) && file_.put32(0) && file_.put32(frames_) &&
       file_.put32(largest_chunk_) && file_.put32(0xFFFFFFFF) && file_.put32(0) && file_.put16(0) && file_.put16(0) && file_.put16(width_) &&
       file_.put16(height_);
  ok = ok && file_.put_tag("strf") && file_.put32(40) && file_.put32(40) && file_.put32(width_) && file_.put32(height_) && file_.put16(1) &&
       file_.put16(codec_ == VideoCodec::Raw ? 12 : 24) && file_.put32(fourcc_codec) && file_.put32(image_size) && file_.put_zeros(16);
  if (has_audio_) {
    ok = ok && file_.put_tag("LIST") && file_.put32(audio_strl) && file_.put_tag("strl");
    ok = ok && file_.put_tag("strh") && file_.put32(56) && file_.put_tag("auds") && file_.put32(0) && file_.put32(0) && file_.put16(0) &&
         file_.put16(0) && file_.put32(0) && file_.put32(audio_block) && file_.put32(audio_rate_bytes) && file_.put32(0) &&
         file_.put32(static_cast<uint32_t>(audio_block ? audio_bytes_ / audio_block : 0)) && file_.put32(0) && file_.put32(0xFFFFFFFF) &&
         file_.put32(audio_block) && file_.put_zeros(8);
    ok = ok && file_.put_tag("strf") && file_.put32(16) && file_.put16(1) && file_.put16(audio_.channels) && file_.put32(audio_.sample_rate) &&
         file_.put32(audio_rate_bytes) && file_.put16(audio_block) && file_.put16(audio_.type == audio::SampleType::S16 ? 16 : 8);
  }
  // The chunks and the index.
  ok = ok && file_.put_tag("LIST") && file_.put32(4 + static_cast<uint32_t>(movi_.size())) && file_.put_tag("movi") && file_.append(movi_.view());
  ok = ok && file_.put_tag("idx1") && file_.put32(static_cast<uint32_t>(index_.count() * 16));
  for (uint64_t i = 0; ok && i < index_.count(); ++i) {
    const IndexEntry& e = index_[i];
    ok = file_.put32(e.id) && file_.put32(e.flags) && file_.put32(e.offset) && file_.put32(e.size);
  }
  ok = ok && file_.patch32(4, static_cast<uint32_t>(file_.size() - 8));
  return ok;
}

}  // namespace video
