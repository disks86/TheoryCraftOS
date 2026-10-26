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


#include "Hardware/VideoCodec/AMD/AmdVcnSelfTest.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Vp9.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {

namespace {
// A picture with structure at every scale: a gradient, a square that moves from picture to picture, and fixed pseudo-random speckle (so inter prediction has
// something to find and residual to carry). Full-range I420, as the encoder takes.
void paint(VideoFrameBuffer* frame, uint32_t picture) {
  const ByteView y = frame->plane(0), u = frame->plane(1), v = frame->plane(2);
  uint32_t seed = 12345;
  for (uint32_t row = 0; row < AmdVcnSelfTest::kSize; ++row)
    for (uint32_t col = 0; col < AmdVcnSelfTest::kSize; ++col) {
      seed = seed * 1664525u + 1013904223u;
      uint32_t value = 40 + col * 2 + row + ((seed >> 24) & 7);
      const uint32_t left = 8 + picture * 4, top = 12 + picture * 3;
      if (col >= left && col < left + 20 && row >= top && row < top + 20) value = 200 - (col - left) * 3;
      y[uint64_t{row} * AmdVcnSelfTest::kSize + col] = static_cast<uint8_t>(value);
    }
  const uint32_t half = AmdVcnSelfTest::kSize / 2;
  for (uint32_t row = 0; row < half; ++row)
    for (uint32_t col = 0; col < half; ++col) {
      u[uint64_t{row} * half + col] = static_cast<uint8_t>(96 + col * 2 + picture * 2);
      v[uint64_t{row} * half + col] = static_cast<uint8_t>(160 - row * 2 + picture);
    }
}

bool same_picture(const VideoFrame& a, const VideoFrame& b) {
  if (a.format != b.format || a.pts_us != b.pts_us) return false;
  for (uint32_t p = 0; p < a.format.plane_count(); ++p)
    for (uint32_t row = 0; row < a.format.plane_height(p); ++row)
      for (uint32_t col = 0; col < a.format.plane_width(p); ++col)
        if (a.plane[p][uint64_t{row} * a.stride[p] + col] != b.plane[p][uint64_t{row} * b.stride[p] + col]) return false;
  return true;
}
}  // namespace

bool AmdVcnSelfTest::build_stream(allocators::IAllocator& allocator, Stream* stream) {
  H264Encoder encoder(allocator);
  VideoFrameBuffer frame(allocator);
  if (!encoder.configure(kSize, kSize, 30, 1, 60, 30) || !frame.allocate({kSize, kSize, PixelFormat::I420})) return false;
  stream->bytes.clear();
  stream->count = 0;
  for (uint32_t i = 0; i < kPictures; ++i) {
    paint(&frame, i);
    VideoFrame in = frame.frame(int64_t{i} * 1000, true);
    in.limited_range = false;
    bool keyframe = false;
    if (!encoder.encode(in, i == 0, &stream->bytes, &keyframe) || keyframe != (i == 0)) return false;
    stream->ends[i] = stream->bytes.size();
    ++stream->count;
  }
  return true;
}

AmdVcnSelfTest::Result AmdVcnSelfTest::run_h264_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* context) {
  Stream stream(allocator);
  if (!build_stream(allocator, &stream)) return Result::StreamFailed;

  // The CPU's pictures, in display order (the stream has no reordering, so decode order).
  VideoFrameBuffer expected[kPictures] = {VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator),
                                          VideoFrameBuffer(allocator)};
  {
    H264Decoder cpu(allocator);
    uint32_t got = 0;
    VideoFrame f;
    for (uint32_t i = 0; i < kPictures; ++i) {
      if (!cpu.decode(stream.packet(i), int64_t{i} * 1000)) return Result::StreamFailed;
      while (cpu.receive(&f)) {
        if (got >= kPictures || !expected[got].allocate(f.format)) return Result::StreamFailed;
        for (uint32_t p = 0; p < f.format.plane_count(); ++p)
          for (uint32_t row = 0; row < f.format.plane_height(p); ++row)
            expected[got].plane(p).subview(uint64_t{row} * f.format.plane_stride(p), f.format.plane_width(p)).copy_from(ConstByteView(f.plane[p] + uint64_t{row} * f.stride[p], f.format.plane_width(p)));
        ++got;
      }
    }
    cpu.flush();
    while (cpu.receive(&f)) {
      if (got >= kPictures || !expected[got].allocate(f.format)) return Result::StreamFailed;
      for (uint32_t p = 0; p < f.format.plane_count(); ++p)
        for (uint32_t row = 0; row < f.format.plane_height(p); ++row)
          expected[got].plane(p).subview(uint64_t{row} * f.format.plane_stride(p), f.format.plane_width(p)).copy_from(ConstByteView(f.plane[p] + uint64_t{row} * f.stride[p], f.format.plane_width(p)));
      ++got;
    }
    if (got != kPictures) return Result::StreamFailed;
  }

  uint32_t got = 0;
  for (uint32_t i = 0; i < kPictures; ++i) {
    VideoPacket packet;
    packet.data = stream.packet(i);
    packet.pts_us = int64_t{i} * 1000;
    packet.keyframe = i == 0;
    const SubmitResult submitted = session.submit(packet);
    if (submitted != SubmitResult::Accepted) return wedged(context) ? Result::EngineFailed : Result::SessionRefused;
    VideoFrame f;
    while (session.receive(&f)) {
      if (got >= kPictures || !same_picture(f, expected[got].frame(int64_t{got} * 1000))) return Result::Mismatch;
      ++got;
    }
  }
  session.flush();
  VideoFrame f;
  while (session.receive(&f)) {
    if (got >= kPictures || !same_picture(f, expected[got].frame(int64_t{got} * 1000))) return Result::Mismatch;
    ++got;
  }
  if (wedged(context)) return Result::EngineFailed;
  if (session.statistics().errors != 0) return Result::EngineFailed;
  return got == kPictures ? Result::Passed : Result::Mismatch;
}

AmdVcnSelfTest::Result AmdVcnSelfTest::run(allocators::IAllocator& allocator, const AmdVcnAvcDecoder::Engine& engine) {
  DecoderConfig config;
  config.codec = VideoCodec::H264;
  config.output = PixelFormat::I420;
  AmdVcnAvcDecoder session(allocator, engine, config);
  if (!session.prepare()) return Result::SessionRefused;
  return run_h264_session(
      allocator, session, [](const void* ring) { return static_cast<const AmdVcn1Ring*>(ring)->wedged(); }, engine.ring);
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestHevcStream.inc"

// A copy of a picture the CPU decoder returned (its planes are only valid until the next call).
bool keep_picture(const VideoFrame& f, VideoFrameBuffer* out) {
  if (!out->allocate(f.format)) return false;
  for (uint32_t p = 0; p < f.format.plane_count(); ++p)
    for (uint32_t row = 0; row < f.format.plane_height(p); ++row)
      out->plane(p).subview(uint64_t{row} * f.format.plane_stride(p), f.format.plane_width(p)).copy_from(ConstByteView(f.plane[p] + uint64_t{row} * f.stride[p], f.format.plane_width(p)));
  return true;
}
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_hevc_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring,
                                                       uint64_t (*decoded)(const void*), const void* decoder) {
  const ConstByteView stream(kHevcSelfTestStream);
  auto packet = [&](uint32_t i) {
    const uint64_t start = i == 0 ? 0 : kHevcSelfTestEnds[i - 1];
    return stream.subview(start, kHevcSelfTestEnds[i] - start);
  };
  // The CPU's pictures, in display order.
  VideoFrameBuffer expected[kHevcSelfTestPictures] = {VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator),
                                                      VideoFrameBuffer(allocator), VideoFrameBuffer(allocator)};
  int64_t pts[kHevcSelfTestPictures] = {};
  {
    H265Decoder cpu(allocator);
    uint32_t got = 0;
    VideoFrame f;
    for (uint32_t i = 0; i <= kHevcSelfTestPictures; ++i) {
      if (i < kHevcSelfTestPictures) {
        if (!cpu.decode(packet(i), int64_t{i} * 1000)) return Result::StreamFailed;
      } else {
        cpu.flush();
      }
      while (cpu.receive(&f)) {
        if (got >= kHevcSelfTestPictures || !keep_picture(f, &expected[got])) return Result::StreamFailed;
        pts[got++] = f.pts_us;
      }
    }
    if (got != kHevcSelfTestPictures) return Result::StreamFailed;
  }

  uint32_t got = 0;
  VideoFrame f;
  for (uint32_t i = 0; i < kHevcSelfTestPictures; ++i) {
    VideoPacket p;
    p.data = packet(i);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return wedged(ring) ? Result::EngineFailed : Result::SessionRefused;
    while (session.receive(&f)) {
      if (got >= kHevcSelfTestPictures || !same_picture(f, expected[got].frame(pts[got]))) return Result::Mismatch;
      ++got;
    }
  }
  session.flush();
  while (session.receive(&f)) {
    if (got >= kHevcSelfTestPictures || !same_picture(f, expected[got].frame(pts[got]))) return Result::Mismatch;
    ++got;
  }
  if (wedged(ring) || session.statistics().errors != 0) return Result::EngineFailed;
  // The engine must really have done the work: every picture went through it (a stream the session passed to the CPU would "pass" too).
  if (decoded(decoder) != kHevcSelfTestPictures) return Result::SessionRefused;
  return got == kHevcSelfTestPictures ? Result::Passed : Result::Mismatch;
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestHevc10Stream.inc"
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_hevc10_session(allocators::IAllocator&, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring, uint64_t (*decoded)(const void*),
                                                          H265Decoder::HashStatistics (*hashes)(const void*), const void* decoder) {
  const ConstByteView stream(kHevc10SelfTestStream);
  uint32_t got = 0;
  VideoFrame f;
  auto drain = [&]() {
    while (session.receive(&f)) {
      if (f.format.pixel_format != PixelFormat::P010 || f.format.width != kHevc10SelfTestSize || f.format.height != kHevc10SelfTestSize) return false;
      ++got;
    }
    return true;
  };
  for (uint32_t i = 0; i < kHevc10SelfTestPictures; ++i) {
    const uint64_t start = i == 0 ? 0 : kHevc10SelfTestEnds[i - 1];
    VideoPacket p;
    p.data = stream.subview(start, kHevc10SelfTestEnds[i] - start);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return wedged(ring) ? Result::EngineFailed : Result::SessionRefused;
    if (!drain()) return Result::Mismatch;
  }
  session.flush();
  if (!drain()) return Result::Mismatch;
  if (wedged(ring) || session.statistics().errors != 0) return Result::EngineFailed;
  // Every picture through the engine (a stream the session passed to the CPU would fail there: the CPU cannot decode it), and every one confirmed by the stream's own CRC.
  if (decoded(decoder) != kHevc10SelfTestPictures) return Result::SessionRefused;
  const H265Decoder::HashStatistics h = hashes(decoder);
  if (h.mismatched != 0 || h.checked != kHevc10SelfTestPictures) return Result::HashMismatch;
  return got == kHevc10SelfTestPictures ? Result::Passed : Result::Mismatch;
}

AmdVcnSelfTest::Result AmdVcnSelfTest::run_hevc(allocators::IAllocator& allocator, const AmdVcnHevcDecoder::Engine& engine) {
  DecoderConfig config;
  config.codec = VideoCodec::Hevc;
  config.output = PixelFormat::I420;
  AmdVcnHevcDecoder session(allocator, engine, config);
  if (!session.prepare()) return Result::SessionRefused;
  return run_hevc_session(
      allocator, session, [](const void* ring) { return static_cast<const AmdVcn1Ring*>(ring)->wedged(); }, engine.ring,
      [](const void* decoder) { return static_cast<const AmdVcnHevcDecoder*>(decoder)->pictures_decoded(); }, &session);
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestAv1Stream.inc"
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_av1(allocators::IAllocator& allocator, const AmdVcnAv1Decoder::Engine& engine) {
  const ConstByteView stream(kAv1SelfTestStream);
  auto packet = [&](uint32_t i) {
    const uint64_t start = i == 0 ? 0 : kAv1SelfTestEnds[i - 1];
    return stream.subview(start, kAv1SelfTestEnds[i] - start);
  };
  // The CPU decodes the same packets, and for each of them the engine's picture must be the CPU's: one picture per packet.
  Av1Decoder cpu(allocator);
  DecoderConfig config;
  config.codec = VideoCodec::Av1;
  config.output = PixelFormat::I420;
  AmdVcnAv1Decoder session(allocator, engine, config);
  if (!session.prepare()) return Result::SessionRefused;
  for (uint32_t i = 0; i < kAv1SelfTestPictures; ++i) {
    VideoFrame expected;
    if (!cpu.decode(packet(i), int64_t{i} * 1000) || !cpu.receive(&expected)) return Result::StreamFailed;
    VideoPacket p;
    p.data = packet(i);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return engine.ring->wedged() ? Result::EngineFailed : Result::SessionRefused;
    VideoFrame f;
    if (!session.receive(&f)) return Result::Mismatch;
    if (!same_picture(f, expected)) return Result::Mismatch;
  }
  if (engine.ring->wedged() || session.statistics().errors != 0) return Result::EngineFailed;
  // The engine must really have done the work: a stream the session passed to the CPU would "pass" too.
  if (session.frames_decoded() == 0) return Result::SessionRefused;
  return Result::Passed;
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestVp9Stream.inc"
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_vp9(allocators::IAllocator& allocator, const AmdVcnVp9Decoder::Engine& engine) {
  const ConstByteView stream(kVp9SelfTestStream);
  auto packet = [&](uint32_t i) {
    const uint64_t start = i == 0 ? 0 : kVp9SelfTestEnds[i - 1];
    return stream.subview(start, kVp9SelfTestEnds[i] - start);
  };
  // The CPU decodes the same packets, and for each of them the engine's picture must be the CPU's: one picture per packet.
  Vp9Decoder cpu(allocator);
  DecoderConfig config;
  config.codec = VideoCodec::Vp9;
  config.output = PixelFormat::I420;
  AmdVcnVp9Decoder session(allocator, engine, config);
  if (!session.prepare()) return Result::SessionRefused;
  for (uint32_t i = 0; i < kVp9SelfTestPictures; ++i) {
    VideoFrame expected;
    if (!cpu.decode(packet(i), int64_t{i} * 1000) || !cpu.receive(&expected)) return Result::StreamFailed;
    VideoPacket p;
    p.data = packet(i);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return engine.ring->wedged() ? Result::EngineFailed : Result::SessionRefused;
    VideoFrame f;
    if (!session.receive(&f)) return Result::Mismatch;
    if (!same_picture(f, expected)) return Result::Mismatch;
  }
  if (engine.ring->wedged() || session.statistics().errors != 0) return Result::EngineFailed;
  // The engine must really have done the work: a stream the session passed to the CPU would "pass" too.
  if (session.frames_decoded() == 0) return Result::SessionRefused;
  return Result::Passed;
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg2Stream.inc"

// MPEG-2 decoders agree on what a picture should look like to within the accuracy of the inverse transform (a level or two, carried along a prediction chain), not
// bit for bit: the pictures must differ by little. A picture the engine failed to produce is grey and far off.
constexpr uint32_t kMpeg2MaxDifference = 12;
constexpr uint64_t kMpeg2MaxTotalDifference = 64 * 64 * 3 / 2;   // (a mean of one level over the picture)
constexpr uint32_t kMpeg4MaxDifference = 12;   // and the same for MPEG-4 (a mean of one level over a 32 x 32 picture, 12 at most anywhere)

bool close_picture(const VideoFrame& a, const VideoFrame& b, uint32_t max_difference = kMpeg2MaxDifference, uint64_t max_total = kMpeg2MaxTotalDifference) {
  if (a.format != b.format || a.pts_us != b.pts_us) return false;
  uint64_t total = 0;
  for (uint32_t p = 0; p < a.format.plane_count(); ++p)
    for (uint32_t row = 0; row < a.format.plane_height(p); ++row)
      for (uint32_t col = 0; col < a.format.plane_width(p); ++col) {
        const int x = a.plane[p][uint64_t{row} * a.stride[p] + col], y = b.plane[p][uint64_t{row} * b.stride[p] + col];
        const uint32_t d = static_cast<uint32_t>(x > y ? x - y : y - x);
        if (d > max_difference) return false;
        total += d;
      }
  return total <= max_total;
}
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_mpeg2_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring,
                                                        uint64_t (*decoded)(const void*), const void* decoder) {
  const ConstByteView stream(kMpeg2SelfTestStream);
  auto packet = [&](uint32_t i) {
    const uint64_t start = i == 0 ? 0 : kMpeg2SelfTestEnds[i - 1];
    return stream.subview(start, kMpeg2SelfTestEnds[i] - start);
  };
  // The CPU's pictures, in display order.
  VideoFrameBuffer expected[kMpeg2SelfTestPictures] = {VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator),
                                                       VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator)};
  int64_t pts[kMpeg2SelfTestPictures] = {};
  {
    Mpeg2Decoder cpu(allocator);
    uint32_t got = 0;
    VideoFrame f;
    for (uint32_t i = 0; i <= kMpeg2SelfTestPictures; ++i) {
      if (i < kMpeg2SelfTestPictures) {
        if (!cpu.decode(packet(i), int64_t{i} * 1000)) return Result::StreamFailed;
      } else {
        cpu.flush();
      }
      while (cpu.receive(&f)) {
        if (got >= kMpeg2SelfTestPictures || !keep_picture(f, &expected[got])) return Result::StreamFailed;
        pts[got++] = f.pts_us;
      }
    }
    if (got != kMpeg2SelfTestPictures) return Result::StreamFailed;
  }

  uint32_t got = 0;
  VideoFrame f;
  for (uint32_t i = 0; i < kMpeg2SelfTestPictures; ++i) {
    VideoPacket p;
    p.data = packet(i);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return wedged(ring) ? Result::EngineFailed : Result::SessionRefused;
    while (session.receive(&f)) {
      if (got >= kMpeg2SelfTestPictures || !close_picture(f, expected[got].frame(pts[got]))) return Result::Mismatch;
      ++got;
    }
  }
  session.flush();
  while (session.receive(&f)) {
    if (got >= kMpeg2SelfTestPictures || !close_picture(f, expected[got].frame(pts[got]))) return Result::Mismatch;
    ++got;
  }
  if (wedged(ring) || session.statistics().errors != 0) return Result::EngineFailed;
  // The engine must really have done the work: every picture went through it (a stream the session passed to the CPU would "pass" too).
  if (decoded(decoder) != kMpeg2SelfTestPictures) return Result::SessionRefused;
  return got == kMpeg2SelfTestPictures ? Result::Passed : Result::Mismatch;
}

AmdVcnSelfTest::Result AmdVcnSelfTest::run_mpeg2(allocators::IAllocator& allocator, const AmdVcnMpeg2Decoder::Engine& engine) {
  DecoderConfig config;
  config.codec = VideoCodec::Mpeg2;
  config.output = PixelFormat::I420;
  AmdVcnMpeg2Decoder session(allocator, engine, config);
  if (!session.prepare()) return Result::SessionRefused;
  return run_mpeg2_session(
      allocator, session, [](const void* ring) { return static_cast<const AmdVcn1Ring*>(ring)->wedged(); }, engine.ring,
      [](const void* decoder) { return static_cast<const AmdVcnMpeg2Decoder*>(decoder)->pictures_decoded(); }, &session);
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg4Stream.inc"
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_mpeg4_session(allocators::IAllocator&, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring, uint64_t (*decoded)(const void*),
                                                        const void* decoder) {
  constexpr uint32_t kSize = kMpeg4SelfTestSize, kLuma = kSize * kSize, kChroma = kLuma / 4, kFrame = kLuma + 2 * kChroma;
  const ConstByteView stream(kMpeg4SelfTestStream);
  uint32_t got = 0, seen_pts = 0;
  VideoFrame f;
  // The picture that should come out `index`-th, as FFmpeg decoded it; the pictures must be close, as for MPEG-2.
  auto check = [&](const VideoFrame& out) {
    if (got >= kMpeg4SelfTestPictures || out.format.width != kSize || out.format.height != kSize || out.format.pixel_format != PixelFormat::I420) return false;
    const int64_t k = out.pts_us / 1000;
    if (k < 0 || k >= int64_t{kMpeg4SelfTestPictures} || (seen_pts & (1u << k))) return false;   // (every packet's picture comes out once)
    seen_pts |= 1u << k;
    VideoFrame want;
    want.format = out.format;
    want.pts_us = out.pts_us;
    const uint8_t* base = kMpeg4SelfTestExpected + uint64_t{got} * kFrame;
    want.plane[0] = base;
    want.plane[1] = base + kLuma;
    want.plane[2] = base + kLuma + kChroma;
    want.stride[0] = kSize;
    want.stride[1] = want.stride[2] = kSize / 2;
    return close_picture(out, want, kMpeg4MaxDifference, kFrame);
  };
  for (uint32_t i = 0; i < kMpeg4SelfTestPictures; ++i) {
    const uint64_t start = i == 0 ? 0 : kMpeg4SelfTestEnds[i - 1];
    VideoPacket p;
    p.data = stream.subview(start, kMpeg4SelfTestEnds[i] - start);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return wedged(ring) ? Result::EngineFailed : Result::SessionRefused;
    while (session.receive(&f)) {
      if (!check(f)) return Result::Mismatch;
      ++got;
    }
  }
  session.flush();
  while (session.receive(&f)) {
    if (!check(f)) return Result::Mismatch;
    ++got;
  }
  if (wedged(ring) || session.statistics().errors != 0) return Result::EngineFailed;
  // The engine must really have done the work: every picture went through it.
  if (decoded(decoder) != kMpeg4SelfTestPictures) return Result::SessionRefused;
  return got == kMpeg4SelfTestPictures ? Result::Passed : Result::Mismatch;
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc"

// How close the engine's pictures must be to the CPU decoder's (see run_vc1): the same limits as for MPEG-2, a level or two a sample on average.
constexpr uint32_t kVc1MaxDifference = 12;
constexpr uint64_t kVc1MaxTotalDifference = 64 * 64 * 3 / 2;

// One of the two streams: `config` is the Main profile sequence header (null for the Advanced profile, whose headers are in the stream), `ends` where each of the
// kVc1SelfTestPictures packets ends in `stream`.
AmdVcnSelfTest::Result run_vc1_stream(allocators::IAllocator& allocator, AmdVcnSelfTest::Vc1SessionHost& host, const uint8_t* config, ConstByteView stream, const uint64_t* ends) {
  using Result = AmdVcnSelfTest::Result;
  constexpr uint32_t kCount = kVc1SelfTestPictures;
  auto packet = [&](uint32_t i) {
    const uint64_t start = i == 0 ? 0 : ends[i - 1];
    return stream.subview(start, ends[i] - start);
  };
  const ConstByteView header = config ? ConstByteView(config, 4) : ConstByteView();
  // The CPU's pictures, in display order.
  VideoFrameBuffer expected[kCount] = {VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator),
                                       VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator), VideoFrameBuffer(allocator)};
  int64_t pts[kCount] = {};
  {
    Vc1Decoder cpu(allocator);
    if (config && !cpu.configure(header, kVc1SelfTestSize, kVc1SelfTestSize)) return Result::StreamFailed;
    uint32_t got = 0;
    VideoFrame f;
    for (uint32_t i = 0; i <= kCount; ++i) {
      if (i < kCount) {
        if (!cpu.decode(packet(i), int64_t{i} * 1000)) return Result::StreamFailed;
      } else {
        cpu.flush();
      }
      while (cpu.receive(&f)) {
        if (got >= kCount || !keep_picture(f, &expected[got])) return Result::StreamFailed;
        pts[got++] = f.pts_us;
      }
    }
    if (got != kCount) return Result::StreamFailed;
  }

  DecoderConfig dc;
  dc.codec = VideoCodec::Vc1;
  dc.output = PixelFormat::I420;
  dc.width = dc.height = kVc1SelfTestSize;
  dc.codec_config = header;
  if (!host.open(dc)) return Result::SessionRefused;
  struct Closer {   // (the session goes away before the caller can stop the engine under it)
    AmdVcnSelfTest::Vc1SessionHost& host;
    ~Closer() { host.close(); }
  } closer{host};
  IVideoDecoder& session = host.session();
  uint32_t got = 0;
  VideoFrame f;
  auto check = [&]() {
    while (session.receive(&f)) {
      if (got >= kCount || !close_picture(f, expected[got].frame(pts[got]), kVc1MaxDifference, kVc1MaxTotalDifference)) return false;
      ++got;
    }
    return true;
  };
  for (uint32_t i = 0; i < kCount; ++i) {
    VideoPacket p;
    p.data = packet(i);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return host.wedged() ? Result::EngineFailed : Result::SessionRefused;
    if (!check()) return Result::Mismatch;
  }
  session.flush();
  if (!check()) return Result::Mismatch;
  if (host.wedged() || session.statistics().errors != 0) return Result::EngineFailed;
  // The engine must really have done the work: every picture went through it (a stream the session passed to the CPU would "pass" too).
  if (host.decoded() != kCount) return Result::SessionRefused;
  return got == kCount ? Result::Passed : Result::Mismatch;
}
}  // namespace

AmdVcnSelfTest::Result AmdVcnSelfTest::run_vc1_sessions(allocators::IAllocator& allocator, Vc1SessionHost& host) {
  const Result main_profile = run_vc1_stream(allocator, host, kVc1SelfTestMainConfig, ConstByteView(kVc1SelfTestMainStream), kVc1SelfTestMainEnds);
  if (main_profile != Result::Passed) return main_profile;
  return run_vc1_stream(allocator, host, nullptr, ConstByteView(kVc1SelfTestAdvancedStream), kVc1SelfTestAdvancedEnds);
}

AmdVcnSelfTest::Result AmdVcnSelfTest::run_vc1_interlaced_sessions(allocators::IAllocator& allocator, Vc1SessionHost& host) {
  return run_vc1_stream(allocator, host, nullptr, ConstByteView(kVc1SelfTestInterlacedStream), kVc1SelfTestInterlacedEnds);
}

AmdVcnSelfTest::Result AmdVcnSelfTest::run_vc1(allocators::IAllocator& allocator, const AmdVcnVc1Decoder::Engine& engine) {
  class Host final : public Vc1SessionHost {
   public:
    Host(allocators::IAllocator& allocator, const AmdVcnVc1Decoder::Engine& engine) : allocator_(allocator), engine_(engine) {}
    bool open(const DecoderConfig& config) override {
      session_ = UniquePointer<AmdVcnVc1Decoder>::make(allocator_, allocator_, engine_, config);
      return session_ && session_->prepare();
    }
    IVideoDecoder& session() override { return *session_; }
    bool wedged() const override { return engine_.ring->wedged(); }
    uint64_t decoded() const override { return session_->pictures_decoded(); }
    void close() override { session_.reset(); }

   private:
    allocators::IAllocator& allocator_;
    AmdVcnVc1Decoder::Engine engine_;
    UniquePointer<AmdVcnVc1Decoder> session_;
  } host(allocator, engine);
  return run_vc1_sessions(allocator, host);
}

const char* AmdVcnSelfTest::result_name(Result result) {
  switch (result) {
    case Result::Passed: return "passed";
    case Result::NoMemory: return "out of memory";
    case Result::StreamFailed: return "could not build the test stream";
    case Result::SessionRefused: return "the engine refused the session";
    case Result::EngineFailed: return "the engine did not finish a picture";
    case Result::Mismatch: return "the engine's pictures differ from the CPU's";
    case Result::HashMismatch: return "the engine's pictures differ from the stream's hashes";
  }
  return "unknown";
}

}  // namespace hardware
