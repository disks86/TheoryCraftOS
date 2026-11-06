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


#include "Hardware/VideoCodec/Intel/IntelAvcVdencEncoder.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"

namespace hardware {

namespace {
// Scratch buffer indices of IntelEngineBuffers.
constexpr uint32_t kScratchDs0 = 0, kScratchDs1 = 1, kScratchVdencRows = 2, kScratchVdencStats = 3, kScratchPakStats = 4, kScratchIntraRows = 5, kScratchDeblockRows = 6,
                   kScratchMpcRows = 7, kScratchBitstream = 8, kScratchStatus = 9;
constexpr uint32_t kMinBitstreamBytes = 64 * 1024;

// quality 1 .. 100 -> QP 51 .. 15
uint32_t quantiser_for(uint32_t quality) {
  const uint32_t q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  return 51 - q * 36 / 100;
}
}  // namespace

IntelAvcVdencEncoder::IntelAvcVdencEncoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const EncoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), yuv_(allocator), headers_(allocator), out_(allocator) {}

bool IntelAvcVdencEncoder::prepare() {
  if (prepared_) return true;
  if (config_.codec != VideoCodec::H264) return false;
  if (config_.input != PixelFormat::I420 && config_.input != PixelFormat::Nv12 && config_.input != PixelFormat::Bgra8888) return false;
  if (config_.width < 16 || config_.height < 16 || config_.width > 4096 || config_.height > 4096) return false;
  if (buffers_.xe2()) return false;   // (its VDEnc commands are not written yet; Xe-LP, Xe-LPG and Arc's are)
  width_mbs_ = (config_.width + 15) / 16;
  height_mbs_ = (config_.height + 15) / 16;
  if (!intel_enc::avc_encode_layout(width_mbs_, height_mbs_, &layout_)) return false;
  qp_ = quantiser_for(config_.quality);
  stream_.width = config_.width;
  stream_.height = config_.height;
  stream_.level_idc = 40;
  stream_.log2_max_frame_num = 8;
  stream_.chroma_qp_offset = 0;
  if (!yuv_.allocate({config_.width, config_.height, PixelFormat::I420})) return false;
  if (!buffers_.lease()) return false;
  if (!buffers_.open(layout_.width, layout_.height)) {
    buffers_.release_lease();
    return false;
  }
  bitstream_bytes_ = layout_.width * layout_.height * 2 > kMinBitstreamBytes ? layout_.width * layout_.height * 2 : kMinBitstreamBytes;
  const intel_enc::AvcEncodeScratchSizes sizes = intel_enc::avc_encode_scratch_sizes(width_mbs_, height_mbs_);
  uint32_t source = 0, recon0 = 0, recon1 = 0;
  bool ok = buffers_.surface(0, &source) && buffers_.surface(1, &recon0) && buffers_.surface(2, &recon1);
  uint32_t ignore = 0;
  ok = ok && buffers_.scratch(kScratchDs0, layout_.quarter.size, &ignore) && buffers_.scratch(kScratchDs1, layout_.quarter.size, &ignore) &&
       buffers_.scratch(kScratchVdencRows, sizes.vdenc_row_store, &addresses_.vdenc_row_store) &&
       buffers_.scratch(kScratchVdencStats, sizes.vdenc_statistics, &addresses_.vdenc_statistics) &&
       buffers_.scratch(kScratchPakStats, sizes.pak_statistics, &addresses_.pak_statistics) &&
       buffers_.scratch(kScratchIntraRows, sizes.intra_row_store, &addresses_.intra_row_store) &&
       buffers_.scratch(kScratchDeblockRows, sizes.deblocking_row_store, &addresses_.deblocking_row_store) &&
       buffers_.scratch(kScratchMpcRows, sizes.bsd_mpc_row_store, &addresses_.bsd_mpc_row_store) &&
       buffers_.scratch(kScratchBitstream, bitstream_bytes_, &addresses_.bitstream) && buffers_.scratch(kScratchStatus, 4096, &addresses_.status);
  if (!ok) {
    buffers_.release_lease();
    return false;
  }
  addresses_.source = source;
  addresses_.bitstream_bytes = bitstream_bytes_;
  prepared_ = true;
  return true;
}

bool IntelAvcVdencEncoder::build_batch(bool idr, uint32_t frame_num, uint32_t* words) {
  using namespace intel_enc;
  const uint32_t current = recon_slot_, previous = 1 - recon_slot_;   // the reconstruction alternates between two surfaces
  AvcEncodeBuffers b = addresses_;
  uint32_t ds0 = 0, ds1 = 0;
  if (!buffers_.surface(1 + static_cast<int>(current), &b.recon) || !buffers_.surface(1 + static_cast<int>(previous), &b.reference) ||
      !buffers_.scratch(current == 0 ? kScratchDs0 : kScratchDs1, layout_.quarter.size, &ds0) ||
      !buffers_.scratch(previous == 0 ? kScratchDs0 : kScratchDs1, layout_.quarter.size, &ds1))
    return false;
  b.recon_ds4 = ds0;
  b.reference_ds4 = ds1;

  AvcEncodePicture p;
  p.generation = generation();
  p.width_mbs = width_mbs_;
  p.height_mbs = height_mbs_;
  p.p_picture = !idr;
  p.qp = static_cast<int32_t>(qp_);
  p.deblocking = true;
  p.level_idc = stream_.level_idc;
  p.reference_frame_store = previous;
  p.xe_lpg = buffers_.xe_lpg();
  p.xe_hpg = buffers_.xe_hpg();
  p.poc = 2 * frame_num;                        // pic_order_cnt_type 2: twice the frame number
  p.reference_poc = 2 * (frame_num - 1);
  p.log2_max_frame_num = stream_.log2_max_frame_num;

  // The headers: the parameter sets in front of an IDR picture, then the slice header.
  headers_.clear();
  uint32_t sps_bits = 0, pps_bits = 0, slice_bits = 0;
  if (idr && (!write_avc_sps(stream_, &headers_, &sps_bits) || !write_avc_pps(stream_, &headers_, &pps_bits))) return false;
  const uint64_t slice_at = headers_.size();
  AvcSliceHeader sh;
  sh.idr = idr;
  sh.p_slice = !idr;
  sh.frame_num = frame_num;
  sh.idr_pic_id = idr_id_;
  sh.qp = p.qp;
  sh.deblocking = p.deblocking;
  if (!write_avc_slice_header(stream_, sh, &headers_, &slice_bits)) return false;
  const ConstByteView all = headers_.view();
  AvcHeaderInsert inserts[3];
  uint32_t count = 0;
  if (idr) {
    const uint64_t sps_bytes = sps_bits / 8;
    inserts[count].data = all.subview(0, sps_bytes);
    inserts[count++].bits = sps_bits;
    inserts[count].data = all.subview(sps_bytes, pps_bits / 8);
    inserts[count++].bits = pps_bits;
  }
  inserts[count].data = all.subview(slice_at, headers_.size() - slice_at);
  inserts[count].bits = slice_bits;
  inserts[count].emulation_prevention = true;
  inserts[count].skip_emulation_bytes = 4;
  inserts[count].slice_header = true;
  ++count;

  DwordWriter w(buffers_.batch_words());
  intel_mfx::emit_mi_flush(w);
  emit_avc_vdenc_picture(w, p, b, layout_);
  if (!emit_avc_vdenc_slice(w, p, ArrayView<const AvcHeaderInsert>(inserts, count))) return false;
  emit_avc_vdenc_finish(w, b, p.generation);
  if (!w.ok()) return false;
  *words = static_cast<uint32_t>(w.count());
  return true;
}

SubmitResult IntelAvcVdencEncoder::submit(const VideoFrame& frame) {
  if (!prepared_) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (pending_) return SubmitResult::Full;
  if (frame.format.width != config_.width || frame.format.height != config_.height || frame.format.pixel_format != config_.input) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (!buffers_.engine_ready() || !convert_frame(frame, yuv_)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  uint32_t source_address = 0;
  if (!buffers_.surface(0, &source_address) || !intel_mfx::tile_i420_to_nv12(yuv_, layout_.width, layout_.height, layout_.full, buffers_.surface_buffer(0).cpu)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  const uint32_t interval = config_.keyframe_interval < 1 ? 1 : config_.keyframe_interval;
  const bool idr = pictures_ == 0 || gop_position_ + 1 >= interval;
  if (idr) {
    gop_position_ = 0;
    frame_num_ = 0;
  } else {
    ++gop_position_;
    frame_num_ = (frame_num_ + 1) & ((1u << stream_.log2_max_frame_num) - 1);
  }
  uint32_t words = 0;
  if (!buffers_.clear_scratch(kScratchStatus) || !build_batch(idr, frame_num_, &words) || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), words))) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  uint8_t size_bytes[4] = {};
  if (!buffers_.read_scratch(kScratchStatus, ByteView(size_bytes, 4))) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  const uint32_t bytes = size_bytes[0] | (size_bytes[1] << 8) | (size_bytes[2] << 16) | (uint32_t{size_bytes[3]} << 24);
  if (bytes == 0 || bytes > bitstream_bytes_) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  out_.clear();
  if (!out_.put_zeros(bytes) || !buffers_.read_scratch(kScratchBitstream, out_.writable_view())) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  if (idr) idr_id_ ^= 1u;   // consecutive IDR pictures name themselves differently
  recon_slot_ = 1 - recon_slot_;
  ++pictures_;
  ++encoded_;
  ++stats_.frames;
  pts_ = frame.pts_us;
  key_ = idr;
  pending_ = true;
  return SubmitResult::Accepted;
}

bool IntelAvcVdencEncoder::receive(VideoPacket* packet) {
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
