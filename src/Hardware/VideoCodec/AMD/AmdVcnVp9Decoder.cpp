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


#include "Hardware/VideoCodec/AMD/AmdVcnVp9Decoder.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamAlignment = 128;   // bsd_size is the bitstream rounded up to this, the padding zeros

uint32_t aligned8(uint32_t value) { return static_cast<uint32_t>(amd_vcn::align_up(value, 8)); }
}  // namespace

AmdVcnVp9Decoder::AmdVcnVp9Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), cpu_(allocator), host_bits_(allocator), host_message_(allocator), row_(allocator), out_(allocator) {
  for (uint8_t& slot : ref_slot_) slot = amd_vcn::kVp9NoPicture;
}

AmdVcnVp9Decoder::~AmdVcnVp9Decoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdVcnVp9Decoder::prepare() {
  if (!engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;
  cpu_.set_offload(this);
  return true;
}

bool AmdVcnVp9Decoder::send_session_message(bool create) {
  if (engine_.ring->wedged()) return false;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_vcn::kMessageBufferBytes)) return false;
  ByteView image = host_message_.writable_view();
  const bool built = create ? amd_vcn::build_create_message(image, engine_.stream_handle, width_, height_, amd_vcn::kStreamTypeVp9)
                            : amd_vcn::build_destroy_message(image, engine_.stream_handle);
  if (!built) return false;
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image).first(create ? amd_vcn::create_message_bytes() : amd_vcn::destroy_message_bytes()));
  const AmdVcn1Ring::Buffer buffers[2] = {{amd_vcn::kCmdSessionContextBuffer, session_context_.gpu_address}, {amd_vcn::kCmdMsgBuffer, message_.gpu_address}};
  return engine_.ring->submit(ArrayView<const AmdVcn1Ring::Buffer>(buffers), /*start_engine=*/false, engine_.job_timeout_ms) == AmdVcn1Ring::Result::Ok;
}

void AmdVcnVp9Decoder::close_session() {
  if (open_) {
    send_session_message(false);
    open_ = false;
  }
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  message_ = session_context_ = context_ = dpb_buffer_ = bitstream_ = target_ = AmdVcnWorkMemory::Piece();
  width_ = height_ = 0;
  for (uint8_t& slot : ref_slot_) slot = amd_vcn::kVp9NoPicture;
}

bool AmdVcnVp9Decoder::allocate_buffers() {
  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const bool ok = memory.allocate(amd_vcn::vp9_message_buffer_bytes(), page, &message_) && memory.allocate(amd_vcn::kSessionContextBytes, page, &session_context_) &&
                  memory.allocate(sizes_.context, page, &context_) && memory.allocate(sizes_.dpb, page, &dpb_buffer_) &&
                  memory.allocate(amd_vcn::bitstream_buffer_bytes(aligned8(width_), aligned8(height_)), page, &bitstream_) && memory.allocate(layout_.bytes, page, &target_);
  if (!ok) return false;
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them; the context buffer then starts with the default probabilities.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&message_, &session_context_, &context_, &dpb_buffer_, &bitstream_, &target_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  mmio_write_bytes(context_.cpu, 0, amd_vcn::vp9_default_probabilities());
  return true;
}

bool AmdVcnVp9Decoder::accepts(const vp9::FrameHeaderInfo& info, int ss_x, int ss_y) {
  if (!leased_) return false;
  if (engine_.ring->wedged()) return open_;   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (info.profile != 0 || info.bit_depth != 8 || ss_x != 1 || ss_y != 1 || info.width <= 0 || info.height <= 0) return false;
  const uint32_t width = static_cast<uint32_t>(info.width), height = static_cast<uint32_t>(info.height);
  if (open_ && width == width_ && height == height_) return true;
  const amd_vcn::Vp9BufferSizes sizes = amd_vcn::vp9_buffer_sizes(width, height);
  const amd_vcn::TargetLayout layout = amd_vcn::target_layout(aligned8(width), aligned8(height));
  const uint64_t bitstream = amd_vcn::bitstream_buffer_bytes(aligned8(width), aligned8(height));
  if (sizes.dpb == 0 || layout.bytes == 0 || bitstream == 0) return false;
  // Whether it all fits in the work memory (with a page of slack for each piece's rounding).
  const uint64_t pieces[6] = {amd_vcn::vp9_message_buffer_bytes(), amd_vcn::kSessionContextBytes, sizes.context, sizes.dpb, bitstream, layout.bytes};
  uint64_t need = 6 * AmdVcnWorkMemory::kPage;
  for (uint64_t bytes : pieces) need += bytes;
  return need <= engine_.memory->capacity_bytes();
}

bool AmdVcnVp9Decoder::open_session(uint32_t width, uint32_t height) {
  close_session();
  if (engine_.ring->wedged()) return false;
  width_ = width;
  height_ = height;
  sizes_ = amd_vcn::vp9_buffer_sizes(width_, height_);
  layout_ = amd_vcn::target_layout(aligned8(width_), aligned8(height_));
  if (sizes_.dpb == 0 || layout_.bytes == 0) return false;
  if (!allocate_buffers()) {
    engine_.memory->reset();
    return false;
  }
  if (!send_session_message(true)) return false;
  open_ = true;
  return true;
}

// The decoded picture is linear NV12 in the target surface: luma rows are copied, interleaved chroma rows are split into the CPU decoder's two planes.
void AmdVcnVp9Decoder::read_back(const vp9::OffloadFrame& frame, const amd_vcn::TargetLayout& layout) {
  const uint32_t width = static_cast<uint32_t>(frame.plane_width[0]), height = static_cast<uint32_t>(frame.plane_height[0]);
  const uint32_t chroma_w = static_cast<uint32_t>(frame.plane_width[1]), chroma_h = static_cast<uint32_t>(frame.plane_height[1]);
  row_.clear();
  if (!row_.put_zeros(uint64_t{width > chroma_w * 2 ? width : chroma_w * 2})) return;
  ByteView row = row_.writable_view();
  for (uint32_t y = 0; y < height; ++y) {
    mmio_read_bytes(target_.cpu, uint64_t{layout.luma_pitch} * y, row.first(width));
    for (uint32_t x = 0; x < width; ++x) frame.plane[0][uint64_t{y} * frame.stride[0] + x] = row[x];
  }
  for (uint32_t y = 0; y < chroma_h; ++y) {
    mmio_read_bytes(target_.cpu, layout.chroma_offset + uint64_t{layout.chroma_pitch} * y, row.first(chroma_w * 2));
    for (uint32_t x = 0; x < chroma_w; ++x) {
      frame.plane[1][uint64_t{y} * frame.stride[1] + x] = row[uint64_t{x} * 2];
      frame.plane[2][uint64_t{y} * frame.stride[2] + x] = row[uint64_t{x} * 2 + 1];
    }
  }
}

uint8_t AmdVcnVp9Decoder::pick_slot() const {
  for (uint8_t slot = 0; slot < amd_vcn::kVp9References; ++slot) {
    bool used = false;
    for (uint8_t held : ref_slot_) used = used || held == slot;
    if (!used) return slot;
  }
  return 0;   // (cannot happen: eight reference slots, nine pictures)
}

bool AmdVcnVp9Decoder::decode_frame(const vp9::OffloadFrame& frame) {
  bool ok = false;
  const uint8_t current = pick_slot();
  const uint32_t width = static_cast<uint32_t>(frame.info.width), height = static_cast<uint32_t>(frame.info.height);
  if (!engine_.ring->wedged()) {
    // A key frame may start the stream anew, at another size.
    if (!open_ || (frame.info.key_frame && (width != width_ || height != height_))) open_session(width, height);
    const amd_vcn::TargetLayout layout = amd_vcn::target_layout(aligned8(width), aligned8(height));
    const uint64_t size = frame.data.size();
    if (open_ && width <= width_ && height <= height_ && layout.bytes != 0 && layout.bytes <= target_.bytes && size != 0 && size <= bitstream_.bytes - kBitstreamAlignment) {
      host_bits_.clear();
      if (host_bits_.append(frame.data) && host_bits_.put_zeros(amd_vcn::align_up(size, kBitstreamAlignment) - size)) {
        amd_vcn::Vp9Slots slots;
        slots.current = current;
        for (uint32_t i = 0; i < 8; ++i) slots.ref_frame_map[i] = ref_slot_[i];
        amd_vcn::DecodeBuffers buffers;
        buffers.bitstream_bytes = static_cast<uint32_t>(host_bits_.size());   // (includes the padding, which the frame size rounds to the same)
        buffers.dpb_bytes = dpb_buffer_.bytes;
        buffers.context_bytes = context_.bytes;
        buffers.target = layout;
        host_message_.clear();
        if (host_message_.put_zeros(amd_vcn::vp9_message_buffer_bytes())) {
          ByteView image = host_message_.writable_view();
          if (amd_vcn::build_vp9_decode_message(image, engine_.stream_handle, ++picture_number_, frame, slots, buffers) && amd_vcn::prepare_vp9_feedback_and_tables(image, frame)) {
            mmio_write_bytes(bitstream_.cpu, 0, host_bits_.view());
            mmio_write_bytes(message_.cpu, 0, ConstByteView(image));
            const AmdVcn1Ring::Buffer job[8] = {
                {amd_vcn::kCmdSessionContextBuffer, session_context_.gpu_address},
                {amd_vcn::kCmdMsgBuffer, message_.gpu_address},
                {amd_vcn::kCmdDpbBuffer, dpb_buffer_.gpu_address},
                {amd_vcn::kCmdContextBuffer, context_.gpu_address},
                {amd_vcn::kCmdBitstreamBuffer, bitstream_.gpu_address},
                {amd_vcn::kCmdDecodingTargetBuffer, target_.gpu_address},
                {amd_vcn::kCmdFeedbackBuffer, message_.gpu_address + amd_vcn::kFeedbackOffset},
                {amd_vcn::kCmdProbTableBuffer, message_.gpu_address + amd_vcn::kItTableOffset},
            };
            if (engine_.ring->submit(ArrayView<const AmdVcn1Ring::Buffer>(job), /*start_engine=*/true, engine_.job_timeout_ms) == AmdVcn1Ring::Result::Ok) {
              uint8_t feedback[amd_vcn::kMessageFeedbackItBytes];
              mmio_read_bytes(message_.cpu, 0, ByteView(feedback));
              feedback_ = amd_vcn::read_feedback(ConstByteView(feedback));
              read_back(frame, layout);
              ok = true;
            }
          }
        }
      }
    }
  }
  if (ok) ++decoded_; else ++failed_;
  // The reference slots the frame refreshes hold it from now on, decoded or not (a damaged frame is what the stream's later frames would refer to).
  for (unsigned i = 0; i < 8; ++i)
    if (frame.refresh_frame_flags & (1 << i)) ref_slot_[i] = current;
  return ok;
}

void AmdVcnVp9Decoder::reset() {
  cpu_.reset();
  for (uint8_t& slot : ref_slot_) slot = amd_vcn::kVp9NoPicture;
}

SubmitResult AmdVcnVp9Decoder::submit(const VideoPacket& packet) {
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

bool AmdVcnVp9Decoder::receive(VideoFrame* frame) {
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

VideoFormat AmdVcnVp9Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics AmdVcnVp9Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
