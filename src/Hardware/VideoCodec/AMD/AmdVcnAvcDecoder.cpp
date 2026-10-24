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


#include "Hardware/VideoCodec/AMD/AmdVcnAvcDecoder.hpp"

namespace hardware {

namespace {
constexpr uint8_t kStartCode[3] = {0, 0, 1};
constexpr uint64_t kBitstreamAlignment = 128;   // bsd_size is the bitstream rounded up to this, the padding zeros

bool supported_profile(int profile_idc) { return profile_idc == 66 || profile_idc == 77 || profile_idc == 88 || profile_idc == 100; }
}  // namespace

AmdVcnAvcDecoder::AmdVcnAvcDecoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), parser_(allocator), slices_(allocator), host_bits_(allocator), host_message_(allocator), i420_(allocator), out_(allocator) {
  h264::PictureInfo* slots[h264::kMaxPictures];
  for (int i = 0; i < h264::kMaxPictures; ++i) slots[i] = &infos_[i];
  dpb_.attach(slots, h264::kMaxPictures);
}

AmdVcnAvcDecoder::~AmdVcnAvcDecoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdVcnAvcDecoder::prepare() {
  if (!engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;
  if (!(config_.codec_config.empty() || parser_.configure(config_.codec_config))) return false;
  if (config_.codec_config.empty()) return true;
  const h264::ParameterSets* sets = parser_.parameter_sets();
  if (!sets) return true;
  for (const h264::Sps& sps : sets->sps) {
    if (!sps.valid) continue;
    dpb_.set_sps(&sps);
    return configure_stream(sps);   // the first sequence parameter set: a stream with another size reconfigures at its first picture
  }
  return true;
}

bool AmdVcnAvcDecoder::send_session_message(bool create) {
  if (engine_.ring->wedged()) return false;
  host_message_.clear();
  if (!host_message_.put_zeros(amd_vcn::kMessageBufferBytes)) return false;
  ByteView image = host_message_.writable_view();
  const bool built = create ? amd_vcn::build_create_message(image, engine_.stream_handle, static_cast<uint32_t>(mb_w_) * 16, static_cast<uint32_t>(mb_h_) * 16)
                            : amd_vcn::build_destroy_message(image, engine_.stream_handle);
  if (!built) return false;
  mmio_write_bytes(message_.cpu, 0, ConstByteView(image).first(create ? amd_vcn::create_message_bytes() : amd_vcn::destroy_message_bytes()));
  const AmdVcn1Ring::Buffer buffers[2] = {{amd_vcn::kCmdSessionContextBuffer, session_context_.gpu_address}, {amd_vcn::kCmdMsgBuffer, message_.gpu_address}};
  return engine_.ring->submit(ArrayView<const AmdVcn1Ring::Buffer>(buffers), /*start_engine=*/false, engine_.job_timeout_ms) == AmdVcn1Ring::Result::Ok;
}

void AmdVcnAvcDecoder::close_session() {
  if (open_) {
    send_session_message(false);
    open_ = false;
  }
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  message_ = session_context_ = context_ = dpb_buffer_ = bitstream_ = target_ = AmdVcnWorkMemory::Piece();
  configured_ = false;
}

bool AmdVcnAvcDecoder::allocate_buffers() {
  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const bool ok = memory.allocate(amd_vcn::kMessageFeedbackItBytes, page, &message_) && memory.allocate(amd_vcn::kSessionContextBytes, page, &session_context_) &&
                  memory.allocate(sizes_.context, page, &context_) && memory.allocate(sizes_.dpb, page, &dpb_buffer_) &&
                  memory.allocate(amd_vcn::bitstream_buffer_bytes(static_cast<uint32_t>(mb_w_) * 16, static_cast<uint32_t>(mb_h_) * 16), page, &bitstream_) &&
                  memory.allocate(layout_.bytes, page, &target_);
  if (!ok) return false;
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&message_, &session_context_, &context_, &dpb_buffer_, &bitstream_, &target_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  return true;
}

bool AmdVcnAvcDecoder::configure_stream(const h264::Sps& sps) {
  close_session();
  if (engine_.ring->wedged()) return false;
  if (sps.mb_w < 1 || sps.mb_h < 1 || sps.chroma_format_idc != 1 || sps.mbaff || !sps.frame_mbs_only || !supported_profile(sps.profile_idc)) return false;
  const uint64_t width = uint64_t{static_cast<uint32_t>(sps.mb_w)} * 16, height = uint64_t{static_cast<uint32_t>(sps.mb_h)} * 16;
  if (width > amd_vcn::kMaxWidth || height > amd_vcn::kMaxHeight) return false;
  if (sps.crop_l < 0 || sps.crop_r < 0 || sps.crop_t < 0 || sps.crop_b < 0 || uint64_t(sps.crop_l) + uint64_t(sps.crop_r) >= width || uint64_t(sps.crop_t) + uint64_t(sps.crop_b) >= height) return false;
  mb_w_ = sps.mb_w;
  mb_h_ = sps.mb_h;
  crop_x_ = static_cast<uint32_t>(sps.crop_l);
  crop_y_ = static_cast<uint32_t>(sps.crop_t);
  shown_w_ = static_cast<uint32_t>(width) - crop_x_ - static_cast<uint32_t>(sps.crop_r);
  shown_h_ = static_cast<uint32_t>(height) - crop_y_ - static_cast<uint32_t>(sps.crop_b);
  sizes_ = amd_vcn::avc_buffer_sizes(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(sps.level_idc),
                                     static_cast<uint32_t>(sps.max_num_ref_frames > 0 ? sps.max_num_ref_frames : 1));
  layout_ = amd_vcn::target_layout(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  if (sizes_.dpb == 0 || layout_.bytes == 0) return false;
  for (int i = 0; i < h264::kMaxPictures; ++i) {
    infos_[i].has_memory = false;
    frames_[i].reset();
  }
  if (!allocate_buffers()) {
    engine_.memory->reset();
    return false;
  }
  if (!send_session_message(true)) return false;
  open_ = true;
  configured_ = true;
  return true;
}

bool AmdVcnAvcDecoder::ensure_frame(int slot) {
  if (!frames_[slot]) {
    frames_[slot] = UniquePointer<VideoFrameBuffer>::make(allocator_, allocator_);
    if (!frames_[slot]) return false;
  }
  return frames_[slot]->allocate({shown_w_, shown_h_, PixelFormat::Nv12});
}

void AmdVcnAvcDecoder::paint_grey(int slot) {
  if (!frames_[slot] || !frames_[slot]->valid()) return;
  for (uint32_t p = 0; p < 2; ++p) frames_[slot]->plane(p).fill(128);
}

SubmitResult AmdVcnAvcDecoder::submit(const VideoPacket& packet) {
  if (!leased_ || engine_.ring->wedged()) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (dpb_.output_ready()) return SubmitResult::Full;
  if (!parser_.parse(packet.data, &slices_)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  ArrayView<const H264Slice> all = slices_.view();
  if (!all.empty()) {
    const H264Slice& first = all[0];
    const h264::Sps* sps = parser_.sps_for(first.header);
    if (!sps) {
      ++stats_.errors;
      return SubmitResult::Invalid;
    }
    if (!configured_ || mb_w_ != sps->mb_w || mb_h_ != sps->mb_h) {
      // Nothing is decoded before the first IDR picture (an I picture will do), and a new picture size needs a new session: first let everything of the old size out.
      if (!configured_ && !first.header.idr() && first.header.type != h264::kSliceI) {
        ++stats_.errors;
        return SubmitResult::Invalid;
      }
      dpb_.bump_output(true);
      if (dpb_.output_ready()) {
        parser_.reset();   // the packet will come again
        return SubmitResult::Full;
      }
      dpb_.set_sps(sps);
      dpb_.clear_pictures();
      if (!configure_stream(*sps)) {
        ++stats_.errors;
        return SubmitResult::Invalid;
      }
    }
  }
  uint64_t i = 0;
  const uint64_t n = all.size();
  while (i < n) {
    uint64_t j = i + 1;
    while (j < n && !all[j].starts_picture) ++j;
    decode_picture(packet.data, all.subview(i, j - i), packet.pts_us);
    i = j;
  }
  ++stats_.packets;
  stats_.bytes += packet.data.size();
  return SubmitResult::Accepted;
}

// The slices of one picture, each behind a start code, padded with zeros to the multiple of 128 bytes the firmware is told.
bool AmdVcnAvcDecoder::gather_bitstream(ConstByteView packet, ArrayView<const H264Slice> slices) {
  host_bits_.clear();
  for (const H264Slice& slice : slices) {
    ConstByteView nal;
    if (!packet.try_subview(slice.nal_offset, slice.nal_size, &nal) || nal.empty()) return false;
    if (!host_bits_.append(ConstByteView(kStartCode)) || !host_bits_.append(nal)) return false;
  }
  const uint64_t size = host_bits_.size();
  if (size == 0 || size > bitstream_.bytes - kBitstreamAlignment) return false;   // (a picture the buffer cannot hold)
  return host_bits_.put_zeros(amd_vcn::align_up(size, kBitstreamAlignment) - size);
}

bool AmdVcnAvcDecoder::read_back(h264::PictureInfo* picture) {
  VideoFrameBuffer& frame = *frames_[picture->slot];
  const ByteView luma = frame.plane(0), chroma = frame.plane(1);
  const uint32_t luma_stride = frame.format().plane_stride(0), chroma_stride = frame.format().plane_stride(1);
  for (uint32_t y = 0; y < shown_h_; ++y)
    mmio_read_bytes(target_.cpu, uint64_t{layout_.luma_pitch} * (crop_y_ + y) + crop_x_, luma.subview(uint64_t{y} * luma_stride, shown_w_));
  const uint32_t chroma_rows = frame.format().chroma_height(), chroma_bytes = 2 * frame.format().chroma_width();
  for (uint32_t y = 0; y < chroma_rows; ++y)
    mmio_read_bytes(target_.cpu, layout_.chroma_offset + uint64_t{layout_.chroma_pitch} * (crop_y_ / 2 + y) + crop_x_, chroma.subview(uint64_t{y} * chroma_stride, chroma_bytes));
  return true;
}

bool AmdVcnAvcDecoder::decode_picture(ConstByteView packet, ArrayView<const H264Slice> group, int64_t pts) {
  const H264Slice& first = group[0];
  const h264::Sps* sps = parser_.sps_for(first.header);
  const h264::Pps* pps = parser_.pps_for(first.header);
  if (!sps || !pps) { ++stats_.errors; return false; }
  // Like the CPU decoder: nothing decodes before an IDR picture (an I picture will do to start with).
  if (waiting_for_idr_ && !first.header.idr() && first.header.type != h264::kSliceI) return false;
  if (!configured_ || mb_w_ != sps->mb_w || mb_h_ != sps->mb_h) { ++stats_.errors; return false; }   // a size change in the middle of a packet
  sps_ = sps;
  dpb_.set_sps(sps);
  h264::PictureInfo* cur = dpb_.begin_picture(first.header, pts);
  if (!cur) { ++stats_.errors; return false; }
  if (!ensure_frame(cur->slot)) {
    dpb_.abort_picture();
    ++stats_.errors;
    return false;
  }
  cur->has_memory = true;
  waiting_for_idr_ = false;

  // The reference pictures the engine is told about: those marked for reference now (the engine keeps their samples itself).
  amd_vcn::AvcReference refs[16];
  uint32_t ref_count = 0;
  for (int slot = 0; slot < h264::kMaxPictures; ++slot) {
    const h264::PictureInfo& p = infos_[slot];
    if (&p == cur || !p.is_ref || ref_count >= 16) continue;
    refs[ref_count++] = {static_cast<uint32_t>(p.is_long ? p.long_term_idx : p.frame_num), p.top_poc, p.bottom_poc};
  }

  bool decoded = false;
  amd_vcn::AvcPictureState state;
  if (amd_vcn::make_avc_picture_state(*parser_.parameter_sets(), *sps, *pps, first.header, cur->top_poc, cur->bottom_poc, ArrayView<const amd_vcn::AvcReference>(refs, ref_count), &state) &&
      gather_bitstream(packet, group)) {
    const uint32_t bitstream_bytes = static_cast<uint32_t>(host_bits_.size());   // (includes the padding, which bsd_size rounds to the same)
    amd_vcn::DecodeBuffers buffers;
    buffers.bitstream_bytes = bitstream_bytes;
    buffers.dpb_bytes = dpb_buffer_.bytes;
    buffers.context_bytes = context_.bytes;
    buffers.target = layout_;
    host_message_.clear();
    if (host_message_.put_zeros(amd_vcn::kMessageFeedbackItBytes)) {
      ByteView image = host_message_.writable_view();
      if (amd_vcn::build_avc_decode_message(image, engine_.stream_handle, ++picture_number_, state, buffers) && amd_vcn::prepare_feedback_and_tables(image, state)) {
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
            {amd_vcn::kCmdItScalingTableBuffer, message_.gpu_address + amd_vcn::kItTableOffset},
        };
        if (engine_.ring->submit(ArrayView<const AmdVcn1Ring::Buffer>(job), /*start_engine=*/true, engine_.job_timeout_ms) == AmdVcn1Ring::Result::Ok) {
          uint8_t feedback[amd_vcn::kMessageFeedbackItBytes];
          mmio_read_bytes(message_.cpu, 0, ByteView(feedback));
          feedback_ = amd_vcn::read_feedback(ConstByteView(feedback));
          decoded = read_back(cur);
        }
      }
    }
  }
  if (!decoded) {
    // A picture the engine could not decode (damaged stream, or the engine failed): show mid-grey, as the CPU decoder does for what it cannot decode, and keep the
    // picture buffer in order.
    ++stats_.errors;
    paint_grey(cur->slot);
  }
  dpb_.finish_picture(group[group.size() - 1].header);
  return decoded;
}

bool AmdVcnAvcDecoder::receive(VideoFrame* frame) {
  h264::PictureInfo* p = dpb_.take_output();
  if (!p || !sps_) return false;
  const h264::Sps& s = *sps_;
  if (!frames_[p->slot] || !frames_[p->slot]->valid()) {
    ++stats_.errors;
    return false;
  }
  VideoFrame in = frames_[p->slot]->frame(p->pts, p->keyframe);
  in.limited_range = !s.full_range;
  if (!out_.allocate({shown_w_, shown_h_, config_.output}) || !convert_frame(in, out_)) {
    ++stats_.errors;
    return false;
  }
  *frame = out_.frame(p->pts, p->keyframe);
  frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
  ++stats_.frames;
  return true;
}

void AmdVcnAvcDecoder::reset() {
  dpb_.reset();
  waiting_for_idr_ = true;
  parser_.reset();
}

VideoFormat AmdVcnAvcDecoder::output_format() const {
  VideoFormat f;
  if (sps_ && configured_) {
    f.width = shown_w_;
    f.height = shown_h_;
    f.pixel_format = config_.output;
  }
  return f;
}

}  // namespace hardware
