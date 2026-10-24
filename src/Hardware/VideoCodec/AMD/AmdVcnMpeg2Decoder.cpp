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

#include "Hardware/VideoCodec/AMD/AmdVcnMpeg2Decoder.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamAlignment = 128;   // bsd_size is the bitstream rounded up to this, the padding zeros
}  // namespace

AmdVcnMpeg2Decoder::AmdVcnMpeg2Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), cpu_(allocator), host_bits_(allocator), host_message_(allocator), row_(allocator), out_(allocator) {
  for (int& s : engine_slot_of_) s = kNoSlot;
  for (int& o : owner_) o = kNoSlot;
}

AmdVcnMpeg2Decoder::~AmdVcnMpeg2Decoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdVcnMpeg2Decoder::prepare() {
  if (!config_.codec_config.empty()) return false;   // (MPEG-2 has no configuration record: the sequence header is in the stream)
  if (!engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;
  cpu_.set_offload(this);
  return true;
}

void AmdVcnMpeg2Decoder::reset() {
  cpu_.reset();
  for (int& s : engine_slot_of_) s = kNoSlot;
  for (int& o : owner_) o = kNoSlot;
}

bool AmdVcnMpeg2Decoder::send_session_message(bool create) {
  if (engine_.ring->wedged()) return false;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_vcn::kMessageBufferBytes)) return false;
  ByteView image = host_message_.writable_view();
  const bool built = create ? amd_vcn::build_create_message(image, engine_.stream_handle, width_, height_, amd_vcn::kStreamTypeMpeg2)
                            : amd_vcn::build_destroy_message(image, engine_.stream_handle);
  if (!built) return false;
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image).first(create ? amd_vcn::create_message_bytes() : amd_vcn::destroy_message_bytes()));
  const AmdVcn1Ring::Buffer buffers[2] = {{amd_vcn::kCmdSessionContextBuffer, session_context_.gpu_address}, {amd_vcn::kCmdMsgBuffer, message_.gpu_address}};
  return engine_.ring->submit(ArrayView<const AmdVcn1Ring::Buffer>(buffers), /*start_engine=*/false, engine_.job_timeout_ms) == AmdVcn1Ring::Result::Ok;
}

void AmdVcnMpeg2Decoder::close_session() {
  if (open_) {
    send_session_message(false);
    open_ = false;
  }
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  message_ = session_context_ = dpb_buffer_ = bitstream_ = target_ = AmdVcnWorkMemory::Piece();
  width_ = height_ = 0;
  for (int& s : engine_slot_of_) s = kNoSlot;
  for (int& o : owner_) o = kNoSlot;
}

bool AmdVcnMpeg2Decoder::allocate_buffers() {
  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const bool ok = memory.allocate(amd_vcn::kMessageFeedbackItBytes, page, &message_) && memory.allocate(amd_vcn::kSessionContextBytes, page, &session_context_) &&
                  memory.allocate(sizes_.dpb, page, &dpb_buffer_) && memory.allocate(amd_vcn::bitstream_buffer_bytes(width_, height_), page, &bitstream_) &&
                  memory.allocate(layout_.bytes, page, &target_);
  if (!ok) return false;
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&message_, &session_context_, &dpb_buffer_, &bitstream_, &target_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  return true;
}

bool AmdVcnMpeg2Decoder::accepts(const mpeg2::SequenceHeader& seq) {
  if (!leased_ || engine_.ring->wedged()) return open_;   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (seq.chroma_format != 1 || seq.mb_width <= 0 || seq.mb_height <= 0) return false;
  const uint32_t width = static_cast<uint32_t>(seq.mb_width) * 16, height = static_cast<uint32_t>(seq.mb_height) * 16;
  if (open_ && width == width_ && height == height_) return true;
  const amd_vcn::Mpeg2BufferSizes sizes = amd_vcn::mpeg2_buffer_sizes(width, height);
  const amd_vcn::TargetLayout layout = amd_vcn::target_layout(width, height);
  const uint64_t bitstream = amd_vcn::bitstream_buffer_bytes(width, height);
  if (sizes.dpb == 0 || layout.bytes == 0 || bitstream == 0) return false;
  // Whether it all fits in the work memory (with a page of slack for each piece's rounding).
  const uint64_t pieces[5] = {amd_vcn::kMessageFeedbackItBytes, amd_vcn::kSessionContextBytes, sizes.dpb, bitstream, layout.bytes};
  uint64_t need = 5 * AmdVcnWorkMemory::kPage;
  for (uint64_t bytes : pieces) need += bytes;
  return need <= engine_.memory->capacity_bytes();
}

bool AmdVcnMpeg2Decoder::open_session(const mpeg2::SequenceHeader& seq) {
  close_session();
  if (engine_.ring->wedged()) return false;
  width_ = static_cast<uint32_t>(seq.mb_width) * 16;
  height_ = static_cast<uint32_t>(seq.mb_height) * 16;
  sizes_ = amd_vcn::mpeg2_buffer_sizes(width_, height_);
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

// The slices of one picture, each with its start code already, padded with zeros to the multiple of 128 bytes the firmware is told.
bool AmdVcnMpeg2Decoder::gather_bitstream(ArrayView<const ConstByteView> slices) {
  host_bits_.clear();
  for (const ConstByteView& slice : slices) {
    if (slice.empty() || !host_bits_.append(slice)) return false;
  }
  const uint64_t size = host_bits_.size();
  if (size == 0 || size > bitstream_.bytes - kBitstreamAlignment) return false;   // (a picture the buffer cannot hold)
  return host_bits_.put_zeros(amd_vcn::align_up(size, kBitstreamAlignment) - size);
}

// Gives the picture and the pictures it predicts from a slot of the engine's picture buffer each. The pictures the CPU decoder still holds as anchors, and this
// one, keep theirs; every other slot is free again.
bool AmdVcnMpeg2Decoder::assign_slots(const mpeg2::OffloadPicture& picture, amd_vcn::Mpeg2Slots* slots) {
  auto valid = [](int pool) { return pool >= 0 && pool < mpeg2::kPoolSize; };
  if (!valid(picture.slot)) return false;
  bool keep[mpeg2::kPoolSize] = {};
  keep[picture.slot] = true;
  const int needed[4] = {picture.anchor_slot[0], picture.anchor_slot[1], picture.forward_slot, picture.backward_slot};
  for (int pool : needed)
    if (valid(pool)) keep[pool] = true;
  for (uint32_t s = 0; s < amd_vcn::kMpeg2References; ++s) {
    const int owner = owner_[s];
    if (owner != kNoSlot && (!keep[owner] || (owner == picture.slot && !picture.second_field))) {   // (a new picture in a recycled pool entry has no use for the old one's slot)
      engine_slot_of_[owner] = kNoSlot;
      owner_[s] = kNoSlot;
    }
  }
  if (engine_slot_of_[picture.slot] == kNoSlot) {
    for (uint32_t s = 0; s < amd_vcn::kMpeg2References; ++s) {
      if (owner_[s] != kNoSlot) continue;
      owner_[s] = picture.slot;
      engine_slot_of_[picture.slot] = static_cast<int>(s);
      break;
    }
    if (engine_slot_of_[picture.slot] == kNoSlot) return false;
  }
  const uint32_t current = static_cast<uint32_t>(engine_slot_of_[picture.slot]);
  auto slot_of = [&](int pool) { return valid(pool) && engine_slot_of_[pool] != kNoSlot ? static_cast<uint32_t>(engine_slot_of_[pool]) : current; };
  slots->current = current;
  slots->forward = slot_of(picture.forward_slot);
  // A predicted picture names its one reference twice, as Mesa names the latest picture for the reference it does not have; a second field that may also predict from
  // the first field of its own frame names that one.
  slots->backward = picture.backward_slot >= 0 ? slot_of(picture.backward_slot) : (picture.second_field ? current : slots->forward);
  return true;
}

// The decoded picture is linear NV12 in the target surface: the luma rows go straight to the CPU decoder's plane, the interleaved chroma rows are split into its two.
// A field picture is one parity of the lines of the frame.
void AmdVcnMpeg2Decoder::read_back(const mpeg2::OffloadPicture& picture) {
  const uint32_t width = width_, height = height_;
  const bool field = picture.pic->structure != mpeg2::kFramePicture;
  const uint32_t first = field && picture.pic->structure == mpeg2::kBottomField ? 1 : 0, step = field ? 2 : 1;
  for (uint32_t y = first; y < height; y += step)
    mmio_read_bytes(target_.cpu, uint64_t{layout_.luma_pitch} * y, picture.plane[0].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[0]), width));
  const uint32_t half_w = width / 2, half_h = height / 2;
  row_.clear();
  if (!row_.put_zeros(uint64_t{half_w} * 2)) return;
  ByteView row = row_.writable_view();
  for (uint32_t y = first; y < half_h; y += step) {
    mmio_read_bytes(target_.cpu, layout_.chroma_offset + uint64_t{layout_.chroma_pitch} * y, row);
    ByteView u = picture.plane[1].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[1]), half_w);
    ByteView v = picture.plane[2].subview(uint64_t{y} * static_cast<uint32_t>(picture.stride[2]), half_w);
    for (uint32_t x = 0; x < half_w; ++x) {
      u[x] = row[uint64_t{x} * 2];
      v[x] = row[uint64_t{x} * 2 + 1];
    }
  }
}

bool AmdVcnMpeg2Decoder::decode_picture(const mpeg2::OffloadPicture& picture) {
  if (!picture.seq || !picture.pic || engine_.ring->wedged()) {
    ++failed_;
    return false;
  }
  const mpeg2::SequenceHeader& seq = *picture.seq;
  if (!open_ || width_ != static_cast<uint32_t>(seq.mb_width) * 16 || height_ != static_cast<uint32_t>(seq.mb_height) * 16) {
    if (!open_session(seq)) {
      ++failed_;
      return false;
    }
  }
  amd_vcn::Mpeg2Slots slots;
  if (!assign_slots(picture, &slots) || !gather_bitstream(picture.slices)) {
    ++failed_;
    return false;
  }
  amd_vcn::DecodeBuffers buffers;
  buffers.bitstream_bytes = static_cast<uint32_t>(host_bits_.size());   // (includes the padding, which bsd_size rounds to the same)
  buffers.dpb_bytes = dpb_buffer_.bytes;
  buffers.context_bytes = 0;
  buffers.target = layout_;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_vcn::kMessageFeedbackItBytes)) {
    ++failed_;
    return false;
  }
  ByteView image = host_message_.writable_view();
  if (!amd_vcn::build_mpeg2_decode_message(image, engine_.stream_handle, ++picture_number_, picture, slots, buffers) || !amd_vcn::prepare_mpeg2_feedback(image)) {
    ++failed_;
    return false;
  }
  mmio_write_bytes(bitstream_.cpu, 0, host_bits_.view());
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image));
  const AmdVcn1Ring::Buffer job[6] = {
      {amd_vcn::kCmdSessionContextBuffer, session_context_.gpu_address},
      {amd_vcn::kCmdMsgBuffer, message_.gpu_address},
      {amd_vcn::kCmdDpbBuffer, dpb_buffer_.gpu_address},
      {amd_vcn::kCmdBitstreamBuffer, bitstream_.gpu_address},
      {amd_vcn::kCmdDecodingTargetBuffer, target_.gpu_address},
      {amd_vcn::kCmdFeedbackBuffer, message_.gpu_address + amd_vcn::kFeedbackOffset},
  };
  if (engine_.ring->submit(ArrayView<const AmdVcn1Ring::Buffer>(job), /*start_engine=*/true, engine_.job_timeout_ms) != AmdVcn1Ring::Result::Ok) {
    ++failed_;
    return false;
  }
  uint8_t feedback[amd_vcn::kMessageFeedbackItBytes];
  mmio_read_bytes(message_.cpu, 0, ByteView(feedback));
  feedback_ = amd_vcn::read_feedback(ConstByteView(feedback));
  read_back(picture);
  ++decoded_;
  return true;
}

SubmitResult AmdVcnMpeg2Decoder::submit(const VideoPacket& packet) {
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

bool AmdVcnMpeg2Decoder::receive(VideoFrame* frame) {
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

VideoFormat AmdVcnMpeg2Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics AmdVcnMpeg2Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
