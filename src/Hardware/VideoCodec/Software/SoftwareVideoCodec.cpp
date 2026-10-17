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

#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {

namespace {

// Decodes Motion JPEG: every packet is one JPEG file, every picture a keyframe.
class MjpegDecoder final : public IVideoDecoder {
 public:
  MjpegDecoder(allocators::IAllocator& allocator, const DecoderConfig& config)
      : config_(config), jpeg_(allocator), out_(allocator) {}

  VideoCodec codec() const override { return VideoCodec::Mjpeg; }
  SubmitResult submit(const VideoPacket& packet) override {
    if (pending_) return SubmitResult::Full;
    if (!jpeg_.decode(packet.data) || !out_.allocate({jpeg_.width(), jpeg_.height(), config_.output}) || !jpeg_.output(out_)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    ++stats_.packets;
    stats_.bytes += packet.data.size();
    pts_ = packet.pts_us;
    pending_ = true;
    return SubmitResult::Accepted;
  }
  bool receive(VideoFrame* frame) override {
    if (!pending_) return false;
    pending_ = false;
    *frame = out_.frame(pts_, true);
    ++stats_.frames;
    return true;
  }
  void flush() override {}  // nothing is held back
  void reset() override { pending_ = false; }
  VideoFormat output_format() const override { return out_.valid() ? out_.format() : VideoFormat{}; }
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  DecoderConfig config_;
  JpegDecoder jpeg_;
  VideoFrameBuffer out_;
  int64_t pts_ = 0;
  bool pending_ = false;
  VideoCodecStatistics stats_;
};

// "Decodes" uncompressed pictures: the packet is the picture, in the configured raw layout, converted to what was asked for.
class RawDecoder final : public IVideoDecoder {
 public:
  RawDecoder(allocators::IAllocator& allocator, const DecoderConfig& config)
      : config_(config), raw_{config.width, config.height, config.raw_format}, out_(allocator) {}

  bool prepare() { return raw_.valid() && out_.allocate({config_.width, config_.height, config_.output}); }

  VideoCodec codec() const override { return VideoCodec::Raw; }
  SubmitResult submit(const VideoPacket& packet) override {
    if (pending_) return SubmitResult::Full;
    if (packet.data.size() != raw_.frame_bytes()) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    VideoFrame in;
    in.format = raw_;
    uint64_t offset = 0;
    for (uint32_t p = 0; p < raw_.plane_count(); ++p) {
      in.plane[p] = packet.data.data() + offset;
      in.stride[p] = raw_.plane_stride(p);
      offset += raw_.plane_bytes(p);
    }
    if (!convert_frame(in, out_)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    ++stats_.packets;
    stats_.bytes += packet.data.size();
    pts_ = packet.pts_us;
    pending_ = true;
    return SubmitResult::Accepted;
  }
  bool receive(VideoFrame* frame) override {
    if (!pending_) return false;
    pending_ = false;
    *frame = out_.frame(pts_, true);
    ++stats_.frames;
    return true;
  }
  void flush() override {}
  void reset() override { pending_ = false; }
  VideoFormat output_format() const override { return out_.format(); }
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  DecoderConfig config_;
  VideoFormat raw_;
  VideoFrameBuffer out_;
  int64_t pts_ = 0;
  bool pending_ = false;
  VideoCodecStatistics stats_;
};

class MjpegEncoder final : public IVideoEncoder {
 public:
  MjpegEncoder(allocators::IAllocator& allocator, const EncoderConfig& config) : config_(config), jpeg_(allocator) {}

  VideoCodec codec() const override { return VideoCodec::Mjpeg; }
  SubmitResult submit(const VideoFrame& frame) override {
    if (pending_) return SubmitResult::Full;
    if (frame.format.width != config_.width || frame.format.height != config_.height || frame.format.pixel_format != config_.input ||
        !jpeg_.encode(frame, config_.quality)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    ++stats_.frames;
    pts_ = frame.pts_us;
    pending_ = true;
    return SubmitResult::Accepted;
  }
  bool receive(VideoPacket* packet) override {
    if (!pending_) return false;
    pending_ = false;
    packet->data = jpeg_.output();
    packet->pts_us = pts_;
    packet->keyframe = true;
    ++stats_.packets;
    stats_.bytes += packet->data.size();
    return true;
  }
  void flush() override {}
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  EncoderConfig config_;
  JpegEncoder jpeg_;
  int64_t pts_ = 0;
  bool pending_ = false;
  VideoCodecStatistics stats_;
};

// "Encodes" to raw: the packet is the picture as given.
class RawEncoder final : public IVideoEncoder {
 public:
  RawEncoder(allocators::IAllocator& allocator, const EncoderConfig& config) : config_(config), copy_(allocator) {}

  VideoCodec codec() const override { return VideoCodec::Raw; }
  SubmitResult submit(const VideoFrame& frame) override {
    if (pending_) return SubmitResult::Full;
    const VideoFormat want{config_.width, config_.height, config_.input};
    if (frame.format != want || !copy_.allocate(want)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    for (uint32_t p = 0; p < want.plane_count(); ++p) {
      if (!frame.plane[p]) {
        ++stats_.errors;
        return SubmitResult::Invalid;
      }
      for (uint32_t row = 0; row < want.plane_height(p); ++row)  // the frame may have a wider stride than ours
        copy_.plane(p).subview(uint64_t{row} * want.plane_stride(p), want.plane_stride(p))
            .copy_from(ConstByteView(frame.plane[p] + uint64_t{row} * frame.stride[p], want.plane_stride(p)));
    }
    ++stats_.frames;
    pts_ = frame.pts_us;
    pending_ = true;
    return SubmitResult::Accepted;
  }
  bool receive(VideoPacket* packet) override {
    if (!pending_) return false;
    pending_ = false;
    // The planes are consecutive in the buffer, so together they are one run of bytes.
    packet->data = ConstByteView(copy_.plane(0).data(), copy_.format().frame_bytes());
    packet->pts_us = pts_;
    packet->keyframe = true;
    ++stats_.packets;
    stats_.bytes += packet->data.size();
    return true;
  }
  void flush() override {}
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  EncoderConfig config_;
  VideoFrameBuffer copy_;
  int64_t pts_ = 0;
  bool pending_ = false;
  VideoCodecStatistics stats_;
};

}  // namespace

UniquePointer<IVideoDecoder> SoftwareVideoCodec::create_decoder(const DecoderConfig& config) {
  if (config.codec == VideoCodec::Mjpeg) return UniquePointer<IVideoDecoder>(UniquePointer<MjpegDecoder>::make(allocator_, allocator_, config));
  if (config.codec == VideoCodec::Raw) {
    UniquePointer<RawDecoder> raw = UniquePointer<RawDecoder>::make(allocator_, allocator_, config);
    if (raw && raw->prepare()) return UniquePointer<IVideoDecoder>(util::move(raw));
  }
  return UniquePointer<IVideoDecoder>();
}

UniquePointer<IVideoEncoder> SoftwareVideoCodec::create_encoder(const EncoderConfig& config) {
  if (!VideoFormat{config.width, config.height, config.input}.valid() || config.width > 65535 || config.height > 65535)
    return UniquePointer<IVideoEncoder>();
  if (config.codec == VideoCodec::Mjpeg) return UniquePointer<IVideoEncoder>(UniquePointer<MjpegEncoder>::make(allocator_, allocator_, config));
  if (config.codec == VideoCodec::Raw) return UniquePointer<IVideoEncoder>(UniquePointer<RawEncoder>::make(allocator_, allocator_, config));
  return UniquePointer<IVideoEncoder>();
}

}  // namespace hardware
