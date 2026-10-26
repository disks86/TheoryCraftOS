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

#include "Hardware/VideoCodec/AMD/AmdUvd6Vc1Decoder.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamAlignment = 128;   // bsd_size is the bitstream rounded up to this, the padding zeros
}  // namespace

AmdUvd6Vc1Decoder::AmdUvd6Vc1Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), cpu_(allocator), host_bits_(allocator), host_message_(allocator), row_(allocator), out_(allocator) {}

AmdUvd6Vc1Decoder::~AmdUvd6Vc1Decoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdUvd6Vc1Decoder::prepare() {
  if (!engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;
  cpu_.set_offload(this);
  // Simple and Main profile: the sequence header is the codec configuration (read with the offload in place, which is asked about the stream then). Advanced profile has
  // it in the stream.
  if (!config_.codec_config.empty() && !cpu_.configure(config_.codec_config, config_.width, config_.height)) {
    cpu_.set_offload(nullptr);
    engine_.memory->release_lease();
    leased_ = false;
    return false;
  }
  return true;
}

void AmdUvd6Vc1Decoder::reset() {
  cpu_.reset();
}

bool AmdUvd6Vc1Decoder::send_session_message(bool create) {
  if (engine_.ring->wedged()) return false;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_uvd6::kMessageFeedbackItBytes)) return false;
  ByteView image = host_message_.writable_view();
  const bool built = create ? amd_uvd6::build_create_message(image, engine_.stream_handle, width_, height_, static_cast<uint32_t>(dpb_buffer_.bytes), amd_uvd6::kStreamTypeVc1)
                            : amd_uvd6::build_destroy_message(image, engine_.stream_handle);
  if (!built) return false;
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image).first(amd_uvd6::message_bytes()));
  amd_uvd6::JobAddresses addresses;
  addresses.session_context = session_context_.gpu_address;
  addresses.message = message_.gpu_address;
  AmdUvd6Ring::Buffer buffers[amd_uvd6::kSessionJobBuffers];
  const uint32_t n = amd_uvd6::session_job_buffers(addresses, ArrayView<AmdUvd6Ring::Buffer>(buffers));
  return n != 0 && engine_.ring->submit(ArrayView<const AmdUvd6Ring::Buffer>(buffers, n), /*start_engine=*/false, engine_.job_timeout_ms) == AmdUvd6Ring::Result::Ok;
}

void AmdUvd6Vc1Decoder::close_session() {
  if (open_) {
    send_session_message(false);
    open_ = false;
  }
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  message_ = session_context_ = dpb_buffer_ = bitstream_ = target_ = AmdVcnWorkMemory::Piece();
  width_ = height_ = 0;
}

bool AmdUvd6Vc1Decoder::allocate_buffers() {
  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const bool ok = memory.allocate(amd_uvd6::kMessageFeedbackItBytes, page, &message_) && memory.allocate(amd_uvd6::kSessionContextBytes, page, &session_context_) &&
                  memory.allocate(sizes_.dpb, page, &dpb_buffer_) && memory.allocate(amd_uvd6::bitstream_buffer_bytes(width_, height_), page, &bitstream_) &&
                  memory.allocate(layout_.bytes, page, &target_);
  if (!ok) return false;
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&message_, &session_context_, &dpb_buffer_, &bitstream_, &target_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  return true;
}

bool AmdUvd6Vc1Decoder::accepts(const vc1::SequenceHeader& seq) {
  if (!leased_ || engine_.ring->wedged()) return open_;   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (seq.mb_width <= 0 || seq.mb_height <= 0 || (seq.interlace && !interlace_allowed_)) return false;
  const uint32_t width = static_cast<uint32_t>(seq.mb_width) * 16, height = static_cast<uint32_t>(seq.mb_height) * 16;
  if (open_ && width == width_ && height == height_) return true;
  const amd_uvd6::Vc1BufferSizes sizes = amd_uvd6::vc1_buffer_sizes(width, height);
  const amd_vcn::TargetLayout layout = amd_vcn::target_layout(width, height);
  const uint64_t bitstream = amd_uvd6::bitstream_buffer_bytes(width, height);
  if (sizes.dpb == 0 || layout.bytes == 0 || bitstream == 0) return false;
  // Whether it all fits in the work memory (with a page of slack for each piece's rounding).
  const uint64_t pieces[5] = {amd_uvd6::kMessageFeedbackItBytes, amd_uvd6::kSessionContextBytes, sizes.dpb, bitstream, layout.bytes};
  uint64_t need = 5 * AmdVcnWorkMemory::kPage;
  for (uint64_t bytes : pieces) need += bytes;
  return need <= engine_.memory->capacity_bytes();
}

bool AmdUvd6Vc1Decoder::open_session(const vc1::SequenceHeader& seq) {
  close_session();
  if (engine_.ring->wedged()) return false;
  width_ = static_cast<uint32_t>(seq.mb_width) * 16;
  height_ = static_cast<uint32_t>(seq.mb_height) * 16;
  sizes_ = amd_uvd6::vc1_buffer_sizes(width_, height_);
  layout_ = amd_vcn::target_layout(width_, height_);
  if (sizes_.dpb == 0 || layout_.bytes == 0) return false;
  if (!allocate_buffers()) {
    engine_.memory->reset();
    return false;
  }
  if (!send_session_message(true)) return false;
  open_ = true;
  return true;
}

// The frame of one picture as the engine takes it: the units of an Advanced profile frame each with its start code (a frame given without any gets the frame start
// code, as Mesa adds it), the frame of a Simple or Main profile stream as it is; padded with zeros to the multiple of 128 bytes the firmware is told.
bool AmdUvd6Vc1Decoder::gather_bitstream(const vc1::OffloadPicture& picture) {
  static constexpr uint8_t kFrameStartCode[4] = {0x00, 0x00, 0x01, 0x0D};
  host_bits_.clear();
  bool first = true;
  for (const ConstByteView& unit : picture.units) {
    if (unit.empty()) return false;
    const bool coded = unit.size() >= 4 && unit[0] == 0 && unit[1] == 0 && unit[2] == 1;
    if (first && !coded && picture.seq->profile == vc1::kAdvanced && !host_bits_.append(ConstByteView(kFrameStartCode))) return false;
    if (!host_bits_.append(unit)) return false;
    first = false;
  }
  const uint64_t size = host_bits_.size();
  if (size == 0 || size > bitstream_.bytes - kBitstreamAlignment) return false;   // (a picture the buffer cannot hold)
  return host_bits_.put_zeros((size + kBitstreamAlignment - 1) / kBitstreamAlignment * kBitstreamAlignment - size);
}

// The decoded picture is linear NV12 in the target surface: the luma rows go straight to the CPU decoder's plane, the interleaved chroma rows are split into its two.
void AmdUvd6Vc1Decoder::read_back(const vc1::OffloadPicture& picture) {
  const uint32_t width = width_, height = height_;
  for (uint32_t y = 0; y < height; ++y)
    mmio_read_bytes(target_.cpu, uint64_t{layout_.luma_pitch} * y, picture.plane[0].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[0]), width));
  const uint32_t half_w = width / 2, half_h = height / 2;
  row_.clear();
  if (!row_.put_zeros(uint64_t{half_w} * 2)) return;
  ByteView row = row_.writable_view();
  for (uint32_t y = 0; y < half_h; ++y) {
    mmio_read_bytes(target_.cpu, layout_.chroma_offset + uint64_t{layout_.chroma_pitch} * y, row);
    ByteView u = picture.plane[1].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[1]), half_w);
    ByteView v = picture.plane[2].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[2]), half_w);
    for (uint32_t x = 0; x < half_w; ++x) {
      u[x] = row[uint64_t{x} * 2];
      v[x] = row[uint64_t{x} * 2 + 1];
    }
  }
}

bool AmdUvd6Vc1Decoder::decode_picture(const vc1::OffloadPicture& picture) {
  if (!picture.seq || !picture.pic || engine_.ring->wedged()) {
    ++failed_;
    return false;
  }
  const vc1::SequenceHeader& seq = *picture.seq;
  if (!open_ || width_ != static_cast<uint32_t>(seq.mb_width) * 16 || height_ != static_cast<uint32_t>(seq.mb_height) * 16) {
    if (!open_session(seq)) {
      ++failed_;
      return false;
    }
  }
  if (picture.pic->skipped) return true;   // (the CPU decoder has copied the picture it repeats; the engine is not sent anything)
  if (!gather_bitstream(picture)) {
    ++failed_;
    return false;
  }
  amd_uvd6::DecodeBuffers buffers;
  buffers.bitstream_bytes = static_cast<uint32_t>(host_bits_.size());   // (includes the padding, which bsd_size rounds to the same)
  buffers.dpb_bytes = dpb_buffer_.bytes;
  buffers.context_bytes = 0;
  buffers.target = layout_;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_uvd6::kMessageFeedbackItBytes)) {
    ++failed_;
    return false;
  }
  ByteView image = host_message_.writable_view();
  if (!amd_uvd6::build_vc1_decode_message(image, engine_.stream_handle, ++picture_number_, picture, buffers) || !amd_uvd6::prepare_vc1_feedback(image)) {
    ++failed_;
    return false;
  }
  mmio_write_bytes(bitstream_.cpu, 0, host_bits_.view());
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image));
  amd_uvd6::JobAddresses addresses;
  addresses.session_context = session_context_.gpu_address;
  addresses.message = message_.gpu_address;
  addresses.dpb = dpb_buffer_.gpu_address;
  addresses.bitstream = bitstream_.gpu_address;
  addresses.target = target_.gpu_address;
  AmdUvd6Ring::Buffer job[amd_uvd6::kVc1JobBuffers];
  const uint32_t n = amd_uvd6::vc1_job_buffers(addresses, ArrayView<AmdUvd6Ring::Buffer>(job));
  if (n == 0 || engine_.ring->submit(ArrayView<const AmdUvd6Ring::Buffer>(job, n), /*start_engine=*/true, engine_.job_timeout_ms) != AmdUvd6Ring::Result::Ok) {
    ++failed_;
    return false;
  }
  read_back(picture);
  ++decoded_;
  return true;
}

SubmitResult AmdUvd6Vc1Decoder::submit(const VideoPacket& packet) {
  if (!leased_) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (cpu_.ready()) return SubmitResult::Full;
  if (!cpu_.decode(packet.data, packet.pts_us)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  ++stats_.packets;
  stats_.bytes += packet.data.size();
  return SubmitResult::Accepted;
}

bool AmdUvd6Vc1Decoder::receive(VideoFrame* frame) {
  VideoFrame in;
  if (!cpu_.receive(&in)) return false;
  if (!out_.allocate({in.format.width, in.format.height, config_.output}) || !convert_frame(in, out_)) {
    ++stats_.errors;
    return false;
  }
  *frame = out_.frame(in.pts_us, in.keyframe);
  frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
  ++stats_.frames;
  return true;
}

VideoFormat AmdUvd6Vc1Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics AmdUvd6Vc1Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
