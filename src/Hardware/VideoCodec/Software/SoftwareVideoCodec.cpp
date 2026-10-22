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
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Hardware/VideoCodec/Software/Vp9.hpp"
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

// Decodes H.264: packets are access units; pictures come out in display order, a few packets late.
class H264Adapter final : public IVideoDecoder {
 public:
  H264Adapter(allocators::IAllocator& allocator, const DecoderConfig& config) : config_(config), dec_(allocator), out_(allocator) {}

  bool prepare() { return config_.codec_config.empty() || dec_.configure(config_.codec_config); }

  VideoCodec codec() const override { return VideoCodec::H264; }
  SubmitResult submit(const VideoPacket& packet) override {
    if (dec_.ready()) return SubmitResult::Full;
    if (!dec_.decode(packet.data, packet.pts_us)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    ++stats_.packets;
    stats_.bytes += packet.data.size();
    return SubmitResult::Accepted;
  }
  bool receive(VideoFrame* frame) override {
    VideoFrame in;
    if (!dec_.receive(&in)) return false;
    if (!out_.allocate({in.format.width, in.format.height, config_.output}) || !convert_frame(in, out_)) {
      ++stats_.errors;
      return false;
    }
    *frame = out_.frame(in.pts_us, in.keyframe);
    frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
    ++stats_.frames;
    return true;
  }
  void flush() override { dec_.flush(); }
  void reset() override { dec_.reset(); }
  VideoFormat output_format() const override {
    VideoFormat f = dec_.output_format();
    f.pixel_format = config_.output;
    return f;
  }
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  DecoderConfig config_;
  H264Decoder dec_;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

// Decodes VP9: packets are frames or superframes (as WebM and IVF store them); a picture comes out per packet that shows one.
class Vp9Adapter final : public IVideoDecoder {
 public:
  Vp9Adapter(allocators::IAllocator& allocator, const DecoderConfig& config) : config_(config), dec_(allocator), out_(allocator) {}

  VideoCodec codec() const override { return VideoCodec::Vp9; }
  SubmitResult submit(const VideoPacket& packet) override {
    if (dec_.ready()) return SubmitResult::Full;
    if (!dec_.decode(packet.data, packet.pts_us)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    ++stats_.packets;
    stats_.bytes += packet.data.size();
    return SubmitResult::Accepted;
  }
  bool receive(VideoFrame* frame) override {
    VideoFrame in;
    if (!dec_.receive(&in)) return false;
    if (!out_.allocate({in.format.width, in.format.height, config_.output}) || !convert_frame(in, out_)) {
      ++stats_.errors;
      return false;
    }
    *frame = out_.frame(in.pts_us, in.keyframe);
    frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
    ++stats_.frames;
    return true;
  }
  void flush() override { dec_.flush(); }
  void reset() override { dec_.reset(); }
  VideoFormat output_format() const override {
    VideoFormat f = dec_.output_format();
    f.pixel_format = config_.output;
    return f;
  }
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  DecoderConfig config_;
  Vp9Decoder dec_;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

// Encodes H.264: each picture becomes one Annex B access unit (keyframes carry the parameter sets), available at once.
class H264EncoderAdapter final : public IVideoEncoder {
 public:
  H264EncoderAdapter(allocators::IAllocator& allocator, const EncoderConfig& config)
      : config_(config), enc_(allocator), yuv_(allocator), out_(allocator) {}

  bool prepare() {
    return enc_.configure(config_.width, config_.height, config_.fps_numerator, config_.fps_denominator, config_.quality, config_.keyframe_interval) &&
           yuv_.allocate({config_.width, config_.height, PixelFormat::I420});
  }

  VideoCodec codec() const override { return VideoCodec::H264; }
  SubmitResult submit(const VideoFrame& frame) override {
    if (pending_) return SubmitResult::Full;
    if (frame.format.width != config_.width || frame.format.height != config_.height || frame.format.pixel_format != config_.input) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    // The encoder takes full-range I420; a limited-range YUV input is expanded by convert_frame() on the way.
    out_.clear();
    bool key = false;
    if (!convert_frame(frame, yuv_) || !enc_.encode(yuv_.frame(frame.pts_us, true), false, &out_, &key)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    ++stats_.frames;
    pts_ = frame.pts_us;
    key_ = key;
    pending_ = true;
    return SubmitResult::Accepted;
  }
  bool receive(VideoPacket* packet) override {
    if (!pending_) return false;
    pending_ = false;
    packet->data = out_.view();
    packet->pts_us = pts_;
    packet->keyframe = key_;
    ++stats_.packets;
    stats_.bytes += packet->data.size();
    return true;
  }
  void flush() override {}  // nothing is held back
  VideoCodecStatistics statistics() const override { return stats_; }

 private:
  EncoderConfig config_;
  H264Encoder enc_;
  VideoFrameBuffer yuv_;
  ByteBuffer out_;
  int64_t pts_ = 0;
  bool key_ = false;
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
  if (config.codec == VideoCodec::H264) {
    UniquePointer<H264Adapter> avc = UniquePointer<H264Adapter>::make(allocator_, allocator_, config);
    if (avc && avc->prepare()) return UniquePointer<IVideoDecoder>(util::move(avc));
    return UniquePointer<IVideoDecoder>();
  }
  if (config.codec == VideoCodec::Vp9) return UniquePointer<IVideoDecoder>(UniquePointer<Vp9Adapter>::make(allocator_, allocator_, config));
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
  if (config.codec == VideoCodec::H264) {
    UniquePointer<H264EncoderAdapter> avc = UniquePointer<H264EncoderAdapter>::make(allocator_, allocator_, config);
    if (avc && avc->prepare()) return UniquePointer<IVideoEncoder>(util::move(avc));
  }
  return UniquePointer<IVideoEncoder>();
}

}  // namespace hardware
