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

AmdVcnSelfTest::Result AmdVcnSelfTest::run(allocators::IAllocator& allocator, const AmdVcnAvcDecoder::Engine& engine) {
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

  DecoderConfig config;
  config.codec = VideoCodec::H264;
  config.output = PixelFormat::I420;
  AmdVcnAvcDecoder session(allocator, engine, config);
  if (!session.prepare()) return Result::SessionRefused;
  uint32_t got = 0;
  for (uint32_t i = 0; i < kPictures; ++i) {
    VideoPacket packet;
    packet.data = stream.packet(i);
    packet.pts_us = int64_t{i} * 1000;
    packet.keyframe = i == 0;
    const SubmitResult submitted = session.submit(packet);
    if (submitted != SubmitResult::Accepted) return engine.ring->wedged() ? Result::EngineFailed : Result::SessionRefused;
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
  if (engine.ring->wedged()) return Result::EngineFailed;
  if (session.statistics().errors != 0) return Result::EngineFailed;
  return got == kPictures ? Result::Passed : Result::Mismatch;
}

const char* AmdVcnSelfTest::result_name(Result result) {
  switch (result) {
    case Result::Passed: return "passed";
    case Result::NoMemory: return "out of memory";
    case Result::StreamFailed: return "could not build the test stream";
    case Result::SessionRefused: return "the engine refused the session";
    case Result::EngineFailed: return "the engine did not finish a picture";
    case Result::Mismatch: return "the engine's pictures differ from the CPU's";
  }
  return "unknown";
}

}  // namespace hardware
