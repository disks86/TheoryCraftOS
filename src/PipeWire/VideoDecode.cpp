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
    default: return false;
  }
}

bool can_decode_subtype(uint32_t subtype, const IVideoDecoders* decoders) {
  if (subtype == kSubtypeMjpg) return true;
  hardware::VideoCodec codec;
  return decoders && codec_of_subtype(subtype, &codec) && decoders->can_decode(codec);
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

bool VideoDecodeSession::decode(uint32_t subtype, ConstByteView data, IVideoDecoders* decoders, VideoFormat* format, ConstByteView* pixels,
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
