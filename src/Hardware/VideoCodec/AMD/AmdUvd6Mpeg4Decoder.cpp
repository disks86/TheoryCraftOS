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

#include "Hardware/VideoCodec/AMD/AmdUvd6Mpeg4Decoder.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamAlignment = 128;   // bsd_size is the bitstream rounded up to this, the padding zeros
}  // namespace

AmdUvd6Mpeg4Decoder::AmdUvd6Mpeg4Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), host_bits_(allocator), host_message_(allocator), row_(allocator), slots_{Slot(allocator), Slot(allocator), Slot(allocator)}, out_(allocator) {}

AmdUvd6Mpeg4Decoder::~AmdUvd6Mpeg4Decoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdUvd6Mpeg4Decoder::prepare() {
  if (!engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;
  if (config_.codec_config.empty()) return true;
  mpeg4::Packet packet;
  if (!mpeg4::parse_packet(config_.codec_config, nullptr, &packet) || !packet.has_vol) return false;
  vol_ = packet.vol;
  have_vol_ = true;
  return true;
}

void AmdUvd6Mpeg4Decoder::forget_pictures() {
  held_ = ready_ = -1;
  newest_anchor_ = older_anchor_ = 0;
}

void AmdUvd6Mpeg4Decoder::reset() {
  forget_pictures();
  have_vol_ = config_.codec_config.empty() ? false : have_vol_;
  if (config_.codec_config.empty()) vol_ = mpeg4::Vol();
  close_session();
}

bool AmdUvd6Mpeg4Decoder::send_session_message(bool create) {
  if (engine_.ring->wedged()) return false;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_uvd6::kMessageFeedbackItBytes)) return false;
  ByteView image = host_message_.writable_view();
  const uint32_t width16 = (width_ + 15) / 16 * 16, height16 = (height_ + 15) / 16 * 16;
  const bool built = create ? amd_uvd6::build_create_message(image, engine_.stream_handle, width16, height16, static_cast<uint32_t>(dpb_buffer_.bytes), amd_uvd6::kStreamTypeMpeg4)
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

void AmdUvd6Mpeg4Decoder::close_session() {
  if (open_) {
    send_session_message(false);
    open_ = false;
  }
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  message_ = session_context_ = dpb_buffer_ = bitstream_ = target_ = AmdVcnWorkMemory::Piece();
  width_ = height_ = 0;
  newest_anchor_ = older_anchor_ = 0;
  picture_number_ = 0;   // (a new session has a new picture buffer and counts its jobs from the start)
}

bool AmdUvd6Mpeg4Decoder::allocate_buffers() {
  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const bool ok = memory.allocate(amd_uvd6::kMessageFeedbackItBytes, page, &message_) && memory.allocate(amd_uvd6::kSessionContextBytes, page, &session_context_) &&
                  memory.allocate(sizes_.dpb, page, &dpb_buffer_) && memory.allocate(amd_uvd6::bitstream_buffer_bytes((width_ + 15) / 16 * 16, (height_ + 15) / 16 * 16), page, &bitstream_) &&
                  memory.allocate(layout_.bytes, page, &target_);
  if (!ok) return false;
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&message_, &session_context_, &dpb_buffer_, &bitstream_, &target_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  return true;
}

bool AmdUvd6Mpeg4Decoder::accepts(const mpeg4::Vol& vol) const {
  if (engine_.ring->wedged()) return false;
  const uint32_t width16 = (vol.width + 15) / 16 * 16, height16 = (vol.height + 15) / 16 * 16;
  const amd_uvd6::Mpeg4BufferSizes sizes = amd_uvd6::mpeg4_buffer_sizes(vol.width, vol.height);
  const amd_vcn::TargetLayout layout = amd_vcn::target_layout(width16, height16);
  const uint64_t bitstream = amd_uvd6::bitstream_buffer_bytes(width16, height16);
  if (sizes.dpb == 0 || layout.bytes == 0 || bitstream == 0) return false;
  // Whether it all fits in the work memory (with a page of slack for each piece's rounding).
  const uint64_t pieces[5] = {amd_uvd6::kMessageFeedbackItBytes, amd_uvd6::kSessionContextBytes, sizes.dpb, bitstream, layout.bytes};
  uint64_t need = 5 * AmdVcnWorkMemory::kPage;
  for (uint64_t bytes : pieces) need += bytes;
  return need <= engine_.memory->capacity_bytes();
}

bool AmdUvd6Mpeg4Decoder::open_session(const mpeg4::Vol& vol) {
  close_session();
  if (engine_.ring->wedged() || !accepts(vol)) return false;
  width_ = vol.width;
  height_ = vol.height;
  const uint32_t width16 = (width_ + 15) / 16 * 16, height16 = (height_ + 15) / 16 * 16;
  sizes_ = amd_uvd6::mpeg4_buffer_sizes(width_, height_);
  layout_ = amd_vcn::target_layout(width16, height16);
  if (sizes_.dpb == 0 || layout_.bytes == 0) return false;
  if (!allocate_buffers()) {
    engine_.memory->reset();
    return false;
  }
  if (!send_session_message(true)) return false;
  open_ = true;
  return true;
}

// The decoded picture is linear NV12 in the target surface, at the layer's size (the surface is whole macroblocks): the rows go to an I420 buffer.
void AmdUvd6Mpeg4Decoder::read_back(Slot* slot) {
  const uint32_t width = width_, height = height_;
  for (uint32_t y = 0; y < height; ++y)
    mmio_read_bytes(target_.cpu, uint64_t{layout_.luma_pitch} * y, slot->buffer.plane(0).subview(uint64_t{y} * width, width));
  const uint32_t half_w = (width + 1) / 2, half_h = (height + 1) / 2;
  row_.clear();
  if (!row_.put_zeros(uint64_t{half_w} * 2)) return;
  ByteView row = row_.writable_view();
  for (uint32_t y = 0; y < half_h; ++y) {
    mmio_read_bytes(target_.cpu, layout_.chroma_offset + uint64_t{layout_.chroma_pitch} * y, row);
    ByteView u = slot->buffer.plane(1).subview(uint64_t{y} * half_w, half_w);
    ByteView v = slot->buffer.plane(2).subview(uint64_t{y} * half_w, half_w);
    for (uint32_t x = 0; x < half_w; ++x) {
      u[x] = row[uint64_t{x} * 2];
      v[x] = row[uint64_t{x} * 2 + 1];
    }
  }
}

bool AmdUvd6Mpeg4Decoder::decode_plane(const VideoPacket& packet, const mpeg4::Vop& vop, Slot* slot) {
  if (!slot->buffer.allocate({width_, height_, PixelFormat::I420})) return false;
  // The job numbers: an intra plane names the latest job (as Mesa does), a predicted one the newest anchor, a B plane the two anchors.
  amd_uvd6::Mpeg2Pictures pictures;
  pictures.current = picture_number_ + 1;
  const uint32_t latest = picture_number_;
  switch (vop.type) {
    case mpeg4::VopType::I: pictures.forward = pictures.backward = latest; break;
    case mpeg4::VopType::P:
      if (newest_anchor_ == 0) return false;
      pictures.forward = pictures.backward = newest_anchor_;
      break;
    case mpeg4::VopType::B:
      if (newest_anchor_ == 0 || older_anchor_ == 0) return false;
      pictures.forward = older_anchor_;
      pictures.backward = newest_anchor_;
      break;
    case mpeg4::VopType::S: return false;
  }
  // The data goes to the engine as it is: the planes (and the headers in front of them) behind their own start codes, zero padded to the multiple of 128 bytes the
  // firmware is told.
  host_bits_.clear();
  if (packet.data.empty() || !host_bits_.append(packet.data)) return false;
  const uint64_t size = host_bits_.size();
  if (size > bitstream_.bytes - kBitstreamAlignment) return false;   // (a picture the buffer cannot hold)
  if (!host_bits_.put_zeros((size + kBitstreamAlignment - 1) / kBitstreamAlignment * kBitstreamAlignment - size)) return false;
  amd_uvd6::DecodeBuffers buffers;
  buffers.bitstream_bytes = static_cast<uint32_t>(host_bits_.size());
  buffers.dpb_bytes = dpb_buffer_.bytes;
  buffers.context_bytes = 0;   // (no context buffer for MPEG-4)
  buffers.target = layout_;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_uvd6::kMessageFeedbackItBytes)) return false;
  ByteView image = host_message_.writable_view();
  if (!amd_uvd6::build_mpeg4_decode_message(image, engine_.stream_handle, pictures.current, vol_, pictures, buffers) || !amd_uvd6::prepare_mpeg4_feedback(image)) return false;
  mmio_write_bytes(bitstream_.cpu, 0, host_bits_.view());
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image));
  amd_uvd6::JobAddresses addresses;
  addresses.session_context = session_context_.gpu_address;
  addresses.message = message_.gpu_address;
  addresses.dpb = dpb_buffer_.gpu_address;
  addresses.bitstream = bitstream_.gpu_address;
  addresses.target = target_.gpu_address;
  AmdUvd6Ring::Buffer job[amd_uvd6::kMpeg4JobBuffers];
  const uint32_t n = amd_uvd6::mpeg4_job_buffers(addresses, ArrayView<AmdUvd6Ring::Buffer>(job));
  picture_number_ = pictures.current;   // (the number is spent even when the job fails: the engine may have seen it)
  if (n == 0 || engine_.ring->submit(ArrayView<const AmdUvd6Ring::Buffer>(job, n), /*start_engine=*/true, engine_.job_timeout_ms) != AmdUvd6Ring::Result::Ok) return false;
  if (vop.type != mpeg4::VopType::B) {
    older_anchor_ = newest_anchor_;
    newest_anchor_ = pictures.current;
  }
  read_back(slot);
  slot->pts_us = packet.pts_us;
  slot->keyframe = vop.type == mpeg4::VopType::I;
  ++decoded_;
  return true;
}

SubmitResult AmdUvd6Mpeg4Decoder::submit(const VideoPacket& packet) {
  if (!leased_ || engine_.ring->wedged()) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (ready_ >= 0) return SubmitResult::Full;
  mpeg4::Packet parsed;
  if (!mpeg4::parse_packet(packet.data, have_vol_ ? &vol_ : nullptr, &parsed) || parsed.vop_count > 1 || (parsed.vop_count == 1 && !parsed.vop.coded)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (parsed.has_vol) {
    // A layer header: the first one, or a repeat. A different size needs reset() first.
    if (open_ && (parsed.vol.width != width_ || parsed.vol.height != height_)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    vol_ = parsed.vol;
    have_vol_ = true;
  }
  ++stats_.packets;
  stats_.bytes += packet.data.size();
  if (parsed.vop_count == 0) return SubmitResult::Accepted;   // (only headers)
  if (!open_ && !open_session(vol_)) {
    ++failed_;
    return SubmitResult::Invalid;
  }
  const int target = free_slot();
  if (!decode_plane(packet, parsed.vop, &slots_[target])) {
    ++failed_;
    return SubmitResult::Invalid;
  }
  // The output order: a B plane goes out at once; an anchor waits for the next anchor (a stream without B planes does not wait).
  if (parsed.vop.type == mpeg4::VopType::B || vol_.low_delay) {
    ready_ = target;
  } else {
    ready_ = held_;   // (-1 for the first anchor)
    held_ = target;
  }
  return SubmitResult::Accepted;
}

void AmdUvd6Mpeg4Decoder::flush() {
  if (held_ >= 0 && ready_ < 0) {
    ready_ = held_;
    held_ = -1;
  }
}

bool AmdUvd6Mpeg4Decoder::receive(VideoFrame* frame) {
  if (ready_ < 0) return false;
  const Slot& slot = slots_[ready_];
  ready_ = -1;
  if (!out_.allocate({width_, height_, config_.output}) || !convert_frame(slot.buffer.frame(slot.pts_us, slot.keyframe), out_)) {
    ++stats_.errors;
    return false;
  }
  *frame = out_.frame(slot.pts_us, slot.keyframe);
  frame->limited_range = config_.output != PixelFormat::Bgra8888;
  ++stats_.frames;
  return true;
}

VideoFormat AmdUvd6Mpeg4Decoder::output_format() const {
  VideoFormat f;
  f.width = have_vol_ ? vol_.width : 0;
  f.height = have_vol_ ? vol_.height : 0;
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics AmdUvd6Mpeg4Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
