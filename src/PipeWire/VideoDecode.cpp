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

#include "PipeWire/VideoDecode.hpp"
#include "PipeWire/VideoConvert.hpp"

namespace pipewire {

namespace {

constexpr uint32_t kBGRA = 12;      // SPA_VIDEO_FORMAT_BGRA: hardware::PixelFormat::Bgra8888's bytes
constexpr int64_t kFrameUs = 33333;      // (the presentation times given to the decoder: frames in order, 30 a second)

// The first plane of a decoded picture as bytes. A VideoFrame's planes are views the decoder owns and keeps valid until its next call; the first one
// is `stride` bytes a row for `rows` rows (the last row may stop at its pixels' end: `last` bytes).
ConstByteView first_plane(const hardware::VideoFrame& frame, uint32_t rows, uint64_t last) {
  if (!frame.plane[0] || rows == 0) return ConstByteView();
  return ConstByteView(frame.plane[0], uint64_t{frame.stride[0]} * (rows - 1) + last);
}

}  // namespace

bool codec_of_subtype(uint32_t subtype, hardware::VideoCodec* out) {
  switch (subtype) {
    case kSubtypeH264: *out = hardware::VideoCodec::H264; return true;
    case kSubtypeH265: *out = hardware::VideoCodec::Hevc; return true;
    case kSubtypeMpeg2: *out = hardware::VideoCodec::Mpeg2; return true;
    case kSubtypeVc1: *out = hardware::VideoCodec::Vc1; return true;
    case kSubtypeVp8: *out = hardware::VideoCodec::Vp8; return true;
    case kSubtypeVp9: *out = hardware::VideoCodec::Vp9; return true;
    case kSubtypeMpeg4: case kSubtypeXvid: *out = hardware::VideoCodec::Mpeg4; return true;      // (MPEG-4 Part 2: where a codec decodes it)
    default: return false;
  }
}

bool can_decode_subtype(uint32_t subtype, const IVideoCodecs* decoders) {
  if (subtype == kSubtypeMjpg) return true;
  hardware::VideoCodec codec;
  return decoders && codec_of_subtype(subtype, &codec) && decoders->can_decode(codec);
}

bool can_encode_subtype(uint32_t subtype, const IVideoCodecs* codecs) {
  hardware::VideoCodec codec;
  if (subtype == kSubtypeMjpg) codec = hardware::VideoCodec::Mjpeg;
  else if (!codec_of_subtype(subtype, &codec)) return false;
  return codecs && codecs->can_encode(codec);
}

bool VideoDecodeSession::size(uint32_t* width, uint32_t* height) const {
  if (width_ && height_) {
    *width = width_;
    *height = height_;
    return true;
  }
  if (jpeg_ && jpeg_->width() && jpeg_->height()) {
    *width = jpeg_->width();
    *height = jpeg_->height();
    return true;
  }
  if (!decoder_) return false;
  const hardware::VideoFormat f = decoder_->output_format();
  if (!f.valid()) return false;
  *width = f.width;
  *height = f.height;
  return true;
}

bool picture_size(uint32_t subtype, ConstByteView frame, IVideoCodecs* codecs, allocators::IAllocator& allocator, uint32_t* width, uint32_t* height) {
  VideoDecodeSession probe(allocator);
  VideoFormat f;
  ConstByteView pixels;
  bool failed = false;
  probe.decode(subtype, frame, codecs, &f, &pixels, &failed);
  return probe.size(width, height) && *width >= 1 && *height >= 1 && *width <= VideoFormat::kMaxSide && *height <= VideoFormat::kMaxSide;
}

bool VideoEncodeSession::encode(const VideoFormat& from, ConstByteView frame, const VideoFormat& to, IVideoCodecs* codecs, ConstByteView* out) {
  if (from.encoded() || !to.encoded() || !to.width || !to.height || !codecs) return false;
  hardware::VideoCodec codec;
  if (to.subtype == kSubtypeMjpg) codec = hardware::VideoCodec::Mjpeg;
  else if (!codec_of_subtype(to.subtype, &codec)) return false;
  if (!encoder_ || !(made_for_ == to)) {      // (a new encoder for another codec, size or rate: it starts with a keyframe)
    hardware::EncoderConfig config;
    config.codec = codec;
    config.width = to.width;
    config.height = to.height;
    config.input = hardware::PixelFormat::Bgra8888;
    config.fps_numerator = to.rate_num ? to.rate_num : 30;
    config.fps_denominator = to.rate_den ? to.rate_den : 1;
    config.quality = 85;
    config.keyframe_interval = 30;      // (a keyframe a second or so: a watcher can start from one soon)
    encoder_ = codecs->create_encoder(config);
    made_for_ = to;
    if (!encoder_) return false;
  }
  // the frame as BGRA at the watcher's size, then into the encoder
  VideoFormat bgra;
  bgra.format = 12;
  bgra.width = to.width;
  bgra.height = to.height;
  if (!convert_video(from, frame, bgra, &bgra_)) return false;
  hardware::VideoFrame picture;
  picture.format = hardware::VideoFormat{to.width, to.height, hardware::PixelFormat::Bgra8888};
  picture.plane[0] = bgra_.view().data();      // (the encoder's ABI: a plane by pointer, read during submit only)
  picture.stride[0] = to.width * 4;
  picture.pts_us = static_cast<int64_t>(frames_ * 1000000 * (to.rate_den ? to.rate_den : 1) / (to.rate_num ? to.rate_num : 30));
  if (encoder_->submit(picture) != hardware::SubmitResult::Accepted) return false;
  ++frames_;
  hardware::VideoPacket packet;
  bool got = false;
  packet_.clear();
  while (encoder_->receive(&packet)) {      // (the newest packet: one a frame from these encoders)
    packet_.clear();
    got = packet_.append(packet.data);
  }
  if (!got) return false;
  *out = packet_.view();
  return true;
}

bool VideoDecodeSession::take_picture(const hardware::VideoFrame& frame) {
  const hardware::VideoFormat& f = frame.format;
  if (!f.valid() || f.pixel_format != hardware::PixelFormat::Bgra8888 || frame.stride[0] < uint64_t{f.width} * 4) return false;
  const uint64_t row = uint64_t{f.width} * 4;
  const ConstByteView plane = first_plane(frame, f.height, row);
  if (plane.empty()) return false;
  picture_.clear();
  if (!picture_.put_zeros(row * f.height)) return false;
  ByteView to = picture_.writable_view();
  for (uint32_t y = 0; y < f.height; ++y) to.subview(row * y, row).copy_from(plane.subview(uint64_t{frame.stride[0]} * y, row));
  width_ = f.width;
  height_ = f.height;
  return true;
}

bool VideoDecodeSession::decode(uint32_t subtype, ConstByteView data, IVideoCodecs* decoders, VideoFormat* format, ConstByteView* pixels,
                                bool* failed) {
  *failed = false;
  if (subtype != subtype_) {      // (another codec: a new decoder)
    subtype_ = subtype;
    jpeg_ = UniquePointer<hardware::JpegDecoder>();
    decoder_ = UniquePointer<hardware::IVideoDecoder>();
    width_ = height_ = 0;
    next_pts_us_ = 0;
  }
  VideoFormat out;
  out.format = kBGRA;
  if (subtype == kSubtypeMjpg) {
    if (!jpeg_) jpeg_ = UniquePointer<hardware::JpegDecoder>::make(allocator_, allocator_);
    if (!jpeg_ || !jpeg_->decode(data) || jpeg_->width() == 0 || jpeg_->height() == 0) {
      ++errors_;
      *failed = true;
      return false;
    }
    out.width = jpeg_->width();
    out.height = jpeg_->height();
    if (!out.valid()) {
      *failed = true;
      return false;
    }
    const uint64_t n = uint64_t{out.width} * out.height;
    if (words_.count() < n) {
      if (!words_.reserve(n)) return false;
      while (words_.count() < n)
        if (!words_.push_back(0)) return false;
    }
    ArrayView<uint32_t> words = words_.view().first(n);
    if (!jpeg_->output_bgra(words.data(), out.width)) {
      *failed = true;
      return false;
    }
    *format = out;
    *pixels = words_as_bytes(ArrayView<const uint32_t>(words));
    return true;
  }
  hardware::VideoCodec codec;
  if (!decoders || !codec_of_subtype(subtype, &codec) || !decoders->can_decode(codec)) {
    *failed = true;
    return false;
  }
  if (!decoder_) {
    hardware::DecoderConfig config;
    config.codec = codec;
    config.output = hardware::PixelFormat::Bgra8888;
    decoder_ = decoders->create_decoder(config);
    if (!decoder_) {
      *failed = true;
      return false;
    }
  }
  hardware::VideoPacket packet;
  packet.data = data;
  packet.pts_us = next_pts_us_;
  packet.keyframe = false;
  next_pts_us_ += kFrameUs;
  // (a decoder that still holds pictures takes the packet once they are read; the newest of them is kept)
  bool got = false;
  hardware::SubmitResult r = decoder_->submit(packet);
  for (int tries = 0; r == hardware::SubmitResult::Full && tries < 64; ++tries) {
    hardware::VideoFrame frame;
    if (!decoder_->receive(&frame)) break;
    got |= take_picture(frame);
    r = decoder_->submit(packet);
  }
  if (r != hardware::SubmitResult::Accepted) {
    ++errors_;
    *failed = !got;
  }
  hardware::VideoFrame frame;
  for (int k = 0; k < 64 && decoder_->receive(&frame); ++k) got |= take_picture(frame);
  if (!got) return false;
  out.width = width_;
  out.height = height_;
  if (!out.valid()) return false;
  *format = out;
  *pixels = picture_.view();
  return true;
}

}  // namespace pipewire
