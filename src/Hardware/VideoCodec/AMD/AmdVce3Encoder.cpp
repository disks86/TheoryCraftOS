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


#include "Hardware/VideoCodec/AMD/AmdVce3Encoder.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMaxTaskDwords = 512;   // a session job (session, create, configuration, feedback) is about 120 dwords, a picture's about 125
constexpr uint32_t kMinBitstreamBytes = 64 * 1024;

// quality 1 .. 100 -> QP 51 .. 15 (the CPU encoder's own mapping is not shared: the engine's quantiser is the H.264 one)
uint32_t quantiser_for(uint32_t quality) {
  const uint32_t q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  return 51 - q * 36 / 100;
}
}  // namespace

AmdVce3Encoder::AmdVce3Encoder(allocators::IAllocator& allocator, const Engine& engine, const EncoderConfig& config)
    : allocator_(allocator), engine_(engine), config_(config), yuv_(allocator), packed_(allocator), out_(allocator), scratch_(allocator) {}

AmdVce3Encoder::~AmdVce3Encoder() {
  close_session();
  if (leased_) engine_.memory->release_lease();
}

bool AmdVce3Encoder::prepare() {
  if (config_.codec != VideoCodec::H264 || !engine_.ring || !engine_.memory || !engine_.memory->configured()) return false;
  if (config_.input != PixelFormat::I420 && config_.input != PixelFormat::Nv12 && config_.input != PixelFormat::Bgra8888) return false;
  amd_vce3::EncoderParameters p;
  p.width = config_.width;
  p.height = config_.height;
  p.profile_idc = 66;
  p.level = 40;   // (a level the coded picture buffer is sized for; the stream's own level is the firmware's to state)
  p.max_references = 1;
  p.rate_control = amd_vce3::RateControlMethod::Disable;   // fixed quantisers
  p.frame_rate_num = config_.fps_numerator;
  p.frame_rate_den = config_.fps_denominator;
  p.gop_size = config_.keyframe_interval < 1 ? 1 : config_.keyframe_interval;
  p.quant_i_frames = p.quant_p_frames = p.quant_b_frames = quantiser_for(config_.quality);
  if (!amd_vce3::parameters_ok(p)) return false;
  setup_ = amd_vce3::make_setup(p);
  // The picture order count of the stream is 2 * frame_num: its field is wide enough for a long group of pictures.
  setup_.pc.log2_max_pic_order_cnt_lsb_minus4 = 4;
  layout_ = amd_vce3::picture_layout(config_.width, config_.height);
  slots_ = amd_vce3::cpb_slot_count(config_.width, config_.height, p.level);
  if (slots_ < 2) return false;   // a P picture needs its reference and a place of its own
  bitstream_bytes_ = config_.width * config_.height * 2 > kMinBitstreamBytes ? config_.width * config_.height * 2 : kMinBitstreamBytes;
  if (!yuv_.allocate({config_.width, config_.height, PixelFormat::I420})) return false;
  if (!engine_.memory->try_lease()) return false;   // the engine serves one session at a time
  leased_ = true;

  AmdVcnWorkMemory& memory = *engine_.memory;
  memory.reset();
  const uint64_t page = AmdVcnWorkMemory::kPage;
  const uint64_t input_bytes = uint64_t{layout_.frame_bytes};
  if (!memory.allocate(input_bytes, page, &input_) || !memory.allocate(amd_vce3::cpb_bytes(config_.width, config_.height, p.level), page, &cpb_) ||
      !memory.allocate(bitstream_bytes_, page, &bitstream_) || !memory.allocate(amd_vce3::kFeedbackBytes, page, &feedback_)) {
    memory.reset();
    return false;
  }
  // Everything the engine reads starts as zeros, as Mesa clears its buffers when it makes them.
  const AmdVcnWorkMemory::Piece* const pieces[] = {&input_, &cpb_, &bitstream_, &feedback_};
  for (const AmdVcnWorkMemory::Piece* piece : pieces) mmio_fill(piece->cpu, 0, piece->bytes, 0);
  return true;
}

// Writes the task into the ring's indirect buffer and waits for the engine to have run it.
bool AmdVce3Encoder::run(amd_vce3::TaskWriter& writer, ArrayView<uint32_t> storage) {
  if (writer.overflow() || engine_.ring->wedged()) return false;
  return engine_.ring->submit(ArrayView<const uint32_t>(storage.data(), writer.size()), engine_.job_timeout_ms) == AmdVce3Ring::Result::Ok;
}

bool AmdVce3Encoder::open_session() {
  uint32_t storage[kMaxTaskDwords];
  amd_vce3::TaskWriter w{ArrayView<uint32_t>(storage)};
  // rvce_begin_frame for the first picture: session, create, the configuration, the feedback buffer.
  amd_vce3::write_session(w, engine_.stream_handle);
  amd_vce3::write_create(w, setup_, layout_);
  amd_vce3::write_config(w, setup_);
  amd_vce3::write_feedback(w, feedback_.gpu_address, setup_.feedback_ring_size);
  if (!run(w, ArrayView<uint32_t>(storage))) return false;
  open_ = true;
  return true;
}

void AmdVce3Encoder::close_session() {
  if (open_ && !engine_.ring->wedged()) {
    uint32_t storage[kMaxTaskDwords];
    amd_vce3::TaskWriter w{ArrayView<uint32_t>(storage)};
    amd_vce3::write_session(w, engine_.stream_handle);
    amd_vce3::write_destroy(w, feedback_.gpu_address, setup_.feedback_ring_size);
    run(w, ArrayView<uint32_t>(storage));
  }
  open_ = false;
  // A ring that timed out may still have the engine writing into these buffers; they stay taken until the engine is restarted.
  if (engine_.memory && engine_.ring && !engine_.ring->wedged()) engine_.memory->reset();
  input_ = cpb_ = bitstream_ = feedback_ = AmdVcnWorkMemory::Piece();
}

// The full-range I420 picture as the engine takes it: limited-range NV12 with the layout's pitch (luma rows, then the interleaved chroma rows below).
void AmdVce3Encoder::pack_input(const VideoFrameBuffer& i420) {
  const uint32_t width = config_.width, height = config_.height;
  scratch_.clear();
  scratch_.put_zeros(layout_.frame_bytes);
  ByteView out = scratch_.writable_view();
  const ConstByteView y = i420.plane(0), u = i420.plane(1), v = i420.plane(2);
  for (uint32_t row = 0; row < height; ++row)
    for (uint32_t col = 0; col < width; ++col) {
      const uint32_t value = y[uint64_t{row} * width + col];
      out[uint64_t{row} * layout_.pitch + col] = static_cast<uint8_t>(16 + (value * 219 + 127) / 255);
    }
  const uint32_t cw = width / 2, ch = height / 2;
  const uint64_t chroma = uint64_t{layout_.pitch} * layout_.vpitch;
  for (uint32_t row = 0; row < ch; ++row)
    for (uint32_t col = 0; col < cw; ++col) {
      const int32_t cu = u[uint64_t{row} * cw + col], cv = v[uint64_t{row} * cw + col];
      out[chroma + uint64_t{row} * layout_.pitch + col * 2] = static_cast<uint8_t>(128 + ((cu - 128) * 224 + (cu >= 128 ? 127 : -127)) / 255);
      out[chroma + uint64_t{row} * layout_.pitch + col * 2 + 1] = static_cast<uint8_t>(128 + ((cv - 128) * 224 + (cv >= 128 ? 127 : -127)) / 255);
    }
  mmio_write_bytes(input_.cpu, 0, ConstByteView(out));
}

SubmitResult AmdVce3Encoder::submit(const VideoFrame& frame) {
  if (!leased_ || !input_.valid()) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (pending_) return SubmitResult::Full;
  if (frame.format.width != config_.width || frame.format.height != config_.height || frame.format.pixel_format != config_.input) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (engine_.ring->wedged() || !convert_frame(frame, yuv_)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (!open_ && !open_session()) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  // The picture's place in the group of pictures: an IDR starts one (frame number 0, in slot 0); a P picture follows the last picture, which is its reference.
  const uint32_t interval = config_.keyframe_interval < 1 ? 1 : config_.keyframe_interval;
  const bool idr = pictures_ == 0 || gop_position_ + 1 >= interval;
  amd_vce3::EncodeTask task;
  if (idr) {
    gop_position_ = 0;
    frame_num_ = 0;
  } else {
    ++gop_position_;
    ++frame_num_;
  }
  const uint32_t slot = idr ? 0 : (last_slot_ + 1) % slots_;
  task.picture_type = idr ? amd_vce3::PictureType::IDR : amd_vce3::PictureType::P;
  task.frame_num = frame_num_;
  task.frame_num_cnt = pictures_ + 1;
  task.pic_order_cnt = 2 * frame_num_;
  task.i_remain = 0;
  task.p_remain = interval - 1 - gop_position_;
  task.ref_idx_l0 = idr ? 0 : last_.frame_num;
  amd_vce3::frame_offset(layout_, slot, &task.current_luma_offset, &task.current_chroma_offset);
  if (!idr) task.l0 = last_;
  task.input_luma_gpu_address = input_.gpu_address;
  task.input_chroma_gpu_address = input_.gpu_address + uint64_t{layout_.pitch} * layout_.vpitch;
  task.cpb_gpu_address = cpb_.gpu_address;
  task.bitstream_gpu_address = bitstream_.gpu_address;
  task.bitstream_bytes = bitstream_bytes_;
  task.feedback_gpu_address = feedback_.gpu_address;

  pack_input(yuv_);
  mmio_fill(feedback_.cpu, 0, amd_vce3::kFeedbackBytes, 0);
  uint32_t storage[kMaxTaskDwords];
  amd_vce3::TaskWriter w{ArrayView<uint32_t>(storage)};
  amd_vce3::write_encode(w, setup_, layout_, task, engine_.stream_handle);
  if (!run(w, ArrayView<uint32_t>(storage))) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  // The feedback says whether there is a bitstream and how long it is.
  uint32_t feedback[amd_vce3::kFeedbackBytes / 4];
  uint8_t bytes[amd_vce3::kFeedbackBytes];
  mmio_read_bytes(feedback_.cpu, 0, ByteView(bytes));
  for (uint32_t i = 0; i < amd_vce3::kFeedbackBytes / 4; ++i) feedback[i] = bytes[i * 4] | (bytes[i * 4 + 1] << 8) | (bytes[i * 4 + 2] << 16) | (uint32_t{bytes[i * 4 + 3]} << 24);
  const amd_vce3::Feedback f = amd_vce3::read_feedback(ArrayView<const uint32_t>(feedback));
  if (!f.has_bitstream || f.bytes == 0 || f.bytes > bitstream_bytes_) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  out_.clear();
  if (!out_.put_zeros(f.bytes)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  mmio_read_bytes(bitstream_.cpu, 0, out_.writable_view());

  // The picture is in the coded picture buffer: the next P picture refers to it.
  last_.type = task.picture_type;
  last_.frame_num = task.frame_num;
  last_.pic_order_cnt = task.pic_order_cnt;
  last_.luma_offset = task.current_luma_offset;
  last_.chroma_offset = task.current_chroma_offset;
  last_slot_ = slot;
  ++pictures_;
  ++encoded_;
  ++stats_.frames;
  pts_ = frame.pts_us;
  key_ = idr;
  pending_ = true;
  return SubmitResult::Accepted;
}

bool AmdVce3Encoder::receive(VideoPacket* packet) {
  if (!pending_) return false;
  pending_ = false;
  packet->data = out_.view();
  packet->pts_us = pts_;
  packet->keyframe = key_;
  ++stats_.packets;
  stats_.bytes += packet->data.size();
  return true;
}

}  // namespace hardware
