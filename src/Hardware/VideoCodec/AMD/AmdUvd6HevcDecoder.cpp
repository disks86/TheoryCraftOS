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


#include "Hardware/VideoCodec/AMD/AmdUvd6HevcDecoder.hpp"

namespace hardware {

namespace {
constexpr uint8_t kStartCode[3] = {0, 0, 1};
constexpr uint64_t kBitstreamAlignment = 128;   // bsd_size is the bitstream rounded up to this, the padding zeros
}  // namespace

AmdUvd6HevcDecoder::AmdUvd6HevcDecoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), cpu_(allocator), host_bits_(allocator), host_message_(allocator), row_(allocator), out_(allocator) {}

AmdUvd6HevcDecoder::~AmdUvd6HevcDecoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdUvd6HevcDecoder::prepare() {
  if (!engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;
  cpu_.set_offload(this);
  return config_.codec_config.empty() || cpu_.configure(config_.codec_config);
}

bool AmdUvd6HevcDecoder::send_session_message(bool create) {
  if (engine_.ring->wedged()) return false;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_uvd6::kMessageFeedbackItBytes)) return false;
  ByteView image = host_message_.writable_view();
  const bool built = create ? amd_uvd6::build_create_message(image, engine_.stream_handle, width_, height_, static_cast<uint32_t>(dpb_buffer_.bytes), amd_uvd6::kStreamTypeH265)
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

void AmdUvd6HevcDecoder::close_session() {
  if (open_) {
    send_session_message(false);
    open_ = false;
  }
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  message_ = session_context_ = context_ = dpb_buffer_ = bitstream_ = target_ = AmdVcnWorkMemory::Piece();
  width_ = height_ = 0;
}

bool AmdUvd6HevcDecoder::allocate_buffers() {
  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const bool ok = memory.allocate(amd_uvd6::kMessageFeedbackItBytes, page, &message_) && memory.allocate(amd_uvd6::kSessionContextBytes, page, &session_context_) &&
                  memory.allocate(sizes_.context, page, &context_) && memory.allocate(sizes_.dpb, page, &dpb_buffer_) &&
                  memory.allocate(amd_uvd6::bitstream_buffer_bytes(width_, height_), page, &bitstream_) && memory.allocate(layout_.bytes, page, &target_);
  if (!ok) return false;
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&message_, &session_context_, &context_, &dpb_buffer_, &bitstream_, &target_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  return true;
}

bool AmdUvd6HevcDecoder::accepts(const h265::Sps& sps, const h265::Pps&) {
  if (!leased_ || engine_.ring->wedged()) return open_;   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8 || sps.width <= 0 || sps.height <= 0) return false;
  const uint32_t width = static_cast<uint32_t>(sps.width), height = static_cast<uint32_t>(sps.height);
  if (open_ && width == width_ && height == height_) return true;
  const int dpb = sps.max_dec_pic_buffering[sps.max_sub_layers - 1];
  const amd_uvd6::HevcBufferSizes sizes = amd_uvd6::hevc_buffer_sizes(width, height, static_cast<uint32_t>(dpb > 0 ? dpb : 1));
  const amd_vcn::TargetLayout layout = amd_vcn::target_layout(width, height);
  const uint64_t bitstream = amd_uvd6::bitstream_buffer_bytes(width, height);
  if (sizes.dpb == 0 || layout.bytes == 0 || bitstream == 0) return false;
  // Whether it all fits in the work memory (with a page of slack for each piece's rounding).
  const uint64_t pieces[6] = {amd_uvd6::kMessageFeedbackItBytes, amd_uvd6::kSessionContextBytes, sizes.context, sizes.dpb, bitstream, layout.bytes};
  uint64_t need = 6 * AmdVcnWorkMemory::kPage;
  for (uint64_t bytes : pieces) need += bytes;
  return need <= engine_.memory->capacity_bytes();
}

bool AmdUvd6HevcDecoder::open_session(const h265::Sps& sps) {
  close_session();
  if (engine_.ring->wedged()) return false;
  width_ = static_cast<uint32_t>(sps.width);
  height_ = static_cast<uint32_t>(sps.height);
  const int dpb = sps.max_dec_pic_buffering[sps.max_sub_layers - 1];
  sizes_ = amd_uvd6::hevc_buffer_sizes(width_, height_, static_cast<uint32_t>(dpb > 0 ? dpb : 1));
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

// The slices of one picture, each behind a start code, padded with zeros to the multiple of 128 bytes the firmware is told.
bool AmdUvd6HevcDecoder::gather_bitstream(ArrayView<const ConstByteView> slices) {
  host_bits_.clear();
  for (const ConstByteView& nal : slices) {
    if (nal.empty()) return false;
    if (!host_bits_.append(ConstByteView(kStartCode)) || !host_bits_.append(nal)) return false;
  }
  const uint64_t size = host_bits_.size();
  if (size == 0 || size > bitstream_.bytes - kBitstreamAlignment) return false;   // (a picture the buffer cannot hold)
  return host_bits_.put_zeros((size + kBitstreamAlignment - 1) / kBitstreamAlignment * kBitstreamAlignment - size);
}

// The decoded picture is linear NV12 in the target surface: the luma rows go straight to the CPU decoder's plane, the interleaved chroma rows are split into its two.
void AmdUvd6HevcDecoder::read_back(const h265::OffloadPicture& picture) {
  const uint32_t width = width_, height = height_;
  for (uint32_t y = 0; y < height; ++y) mmio_read_bytes(target_.cpu, uint64_t{layout_.luma_pitch} * y, picture.plane[0].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[0]), width));
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

bool AmdUvd6HevcDecoder::decode_picture(const h265::OffloadPicture& picture) {
  if (!picture.sps || !picture.pps || !picture.first || engine_.ring->wedged()) {
    ++failed_;
    return false;
  }
  const h265::Sps& sps = *picture.sps;
  if (!open_ || width_ != static_cast<uint32_t>(sps.width) || height_ != static_cast<uint32_t>(sps.height)) {
    if (!open_session(sps)) {
      ++failed_;
      return false;
    }
  }
  if (!gather_bitstream(picture.slices)) {
    ++failed_;
    return false;
  }
  amd_uvd6::DecodeBuffers buffers;
  buffers.bitstream_bytes = static_cast<uint32_t>(host_bits_.size());   // (includes the padding, which bsd_size rounds to the same)
  buffers.dpb_bytes = dpb_buffer_.bytes;
  buffers.context_bytes = context_.bytes;
  buffers.target = layout_;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_uvd6::kMessageFeedbackItBytes)) {
    ++failed_;
    return false;
  }
  ByteView image = host_message_.writable_view();
  if (!amd_uvd6::build_hevc_decode_message(image, engine_.stream_handle, ++picture_number_, picture, buffers) || !amd_uvd6::prepare_hevc_feedback_and_tables(image, sps, *picture.pps)) {
    ++failed_;
    return false;
  }
  mmio_write_bytes(bitstream_.cpu, 0, host_bits_.view());
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image));
  amd_uvd6::JobAddresses addresses;
  addresses.session_context = session_context_.gpu_address;
  addresses.message = message_.gpu_address;
  addresses.dpb = dpb_buffer_.gpu_address;
  addresses.context = context_.gpu_address;
  addresses.bitstream = bitstream_.gpu_address;
  addresses.target = target_.gpu_address;
  AmdUvd6Ring::Buffer job[amd_uvd6::kDecodeJobBuffers];
  const uint32_t n = amd_uvd6::decode_job_buffers(addresses, ArrayView<AmdUvd6Ring::Buffer>(job));
  if (n == 0 || engine_.ring->submit(ArrayView<const AmdUvd6Ring::Buffer>(job, n), /*start_engine=*/true, engine_.job_timeout_ms) != AmdUvd6Ring::Result::Ok) {
    ++failed_;
    return false;
  }
  read_back(picture);
  ++decoded_;
  return true;
}

SubmitResult AmdUvd6HevcDecoder::submit(const VideoPacket& packet) {
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

bool AmdUvd6HevcDecoder::receive(VideoFrame* frame) {
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

VideoFormat AmdUvd6HevcDecoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics AmdUvd6HevcDecoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
