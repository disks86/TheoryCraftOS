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
constexpr uint32_t kScratchDs0 = 0, kScratchDs1 = 1, kScratchDsB = 10, kScratchVdencRows = 2, kScratchVdencStats = 3, kScratchPakStats = 4, kScratchIntraRows = 5, kScratchDeblockRows = 6,
                   kScratchMpcRows = 7, kScratchBitstream = 8, kScratchStatus = 9;
constexpr uint32_t kMinBitstreamBytes = 64 * 1024;
uint32_t ds_index(uint32_t slot) { return slot < 2 ? slot : kScratchDsB; }

// quality 1 .. 100 -> QP 51 .. 15
uint32_t quantiser_for(uint32_t quality) {
  const uint32_t q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  return 51 - q * 36 / 100;
}
}  // namespace

IntelAvcVdencEncoder::IntelAvcVdencEncoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const EncoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), yuv_{VideoFrameBuffer(allocator), VideoFrameBuffer(allocator)}, headers_(allocator), out_{ByteBuffer(allocator), ByteBuffer(allocator)} {}

bool IntelAvcVdencEncoder::prepare() {
  if (prepared_) return true;
  if (config_.codec != VideoCodec::H264) return false;
  if (config_.input != PixelFormat::I420 && config_.input != PixelFormat::Nv12 && config_.input != PixelFormat::Bgra8888) return false;
  if (config_.width < 16 || config_.height < 16 || config_.width > 4096 || config_.height > 4096) return false;
  width_mbs_ = (config_.width + 15) / 16;
  height_mbs_ = (config_.height + 15) / 16;
  if (!intel_enc::avc_encode_layout(width_mbs_, height_mbs_, &layout_)) return false;
  qp_ = quantiser_for(config_.quality);
  stream_.width = config_.width;
  stream_.height = config_.height;
  stream_.level_idc = 40;
  stream_.log2_max_frame_num = 8;
  stream_.chroma_qp_offset = 0;
  // B pictures: only where the commands are written (the engines that take the Xe_LPM_plus / Xe2 command set).
  b_mode_ = config_.b_frames > 0 && (buffers_.xe_lpg() || buffers_.xe_hpg() || buffers_.xe2());
  stream_.b_pictures = b_mode_;
  if (!yuv_[0].allocate({config_.width, config_.height, PixelFormat::I420}) || !yuv_[1].allocate({config_.width, config_.height, PixelFormat::I420})) return false;
  mv_bytes_ = uint64_t{((config_.width + 31) >> 4)} * height_mbs_ * 32;   // the colocated motion vectors: a cache line per two macroblocks
  if (!buffers_.lease()) return false;
  if (!buffers_.open(layout_.width, layout_.height)) {
    buffers_.release_lease();
    return false;
  }
  bitstream_bytes_ = layout_.width * layout_.height * 2 > kMinBitstreamBytes ? layout_.width * layout_.height * 2 : kMinBitstreamBytes;
  const intel_enc::AvcEncodeScratchSizes sizes = intel_enc::avc_encode_scratch_sizes(width_mbs_, height_mbs_);
  uint32_t source = 0, recon0 = 0, recon1 = 0, ignore_address = 0;
  bool ok = buffers_.surface(0, &source) && buffers_.surface(1, &recon0) && buffers_.surface(2, &recon1) && (!b_mode_ || buffers_.surface(3, &ignore_address));
  uint32_t ignore = 0;
  ok = ok && buffers_.scratch(kScratchDs0, layout_.quarter.size, &ignore) && buffers_.scratch(kScratchDs1, layout_.quarter.size, &ignore) && (!b_mode_ || buffers_.scratch(kScratchDsB, layout_.quarter.size, &ignore)) &&
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

bool IntelAvcVdencEncoder::build_batch(const Picture& pic, uint32_t* words) {
  using namespace intel_enc;
  AvcEncodeBuffers b = addresses_;
  uint32_t ds0 = 0;
  // The reconstruction of anchors alternates between surfaces 1 and 2; a B picture, which nothing refers to, writes into surface 3. The earlier anchor is list 0, the later one list 1.
  const uint32_t recon_slot = pic.b ? 2 : pic.recon_slot;
  if (!buffers_.surface(1 + static_cast<int>(recon_slot), &b.recon) || !buffers_.scratch(ds_index(recon_slot), layout_.quarter.size, &ds0)) return false;
  b.recon_ds4 = ds0;
  if (!pic.idr) {
    const uint32_t forward = pic.b ? pic.forward_slot : 1 - pic.recon_slot;
    if (!buffers_.surface(1 + static_cast<int>(forward), &b.reference) || !buffers_.scratch(ds_index(forward), layout_.quarter.size, &b.reference_ds4)) return false;
  }
  if (pic.b) {
    if (!buffers_.surface(1 + static_cast<int>(pic.backward_slot), &b.backward_reference) ||
        !buffers_.scratch(ds_index(pic.backward_slot), layout_.quarter.size, &b.backward_ds4) || !buffers_.picture_buffer(1 + static_cast<int>(pic.backward_slot), mv_bytes_, &b.colocated_mv_read))
      return false;
  } else if (b_mode_ && !pic.idr && !buffers_.picture_buffer(1 + static_cast<int>(pic.recon_slot), mv_bytes_, &b.colocated_mv_write)) {
    return false;
  }

  AvcEncodePicture p;
  p.generation = generation();
  p.width_mbs = width_mbs_;
  p.height_mbs = height_mbs_;
  p.p_picture = !pic.idr;
  p.b_picture = pic.b;
  p.b_mode = b_mode_;
  p.qp = static_cast<int32_t>(qp_);
  p.deblocking = true;
  p.level_idc = stream_.level_idc;
  p.reference_frame_store = pic.b ? pic.forward_slot : 1 - pic.recon_slot;
  p.backward_reference_frame_store = pic.backward_slot;
  p.xe_lpg = buffers_.xe_lpg() || buffers_.xe2();   // (Xe2 has Xe-LPG's sequence)
  p.xe2 = buffers_.xe3() ? 3 : buffers_.xe2_hpm() ? 2 : buffers_.xe2() ? 1 : 0;
  p.xe_hpg = buffers_.xe_hpg();
  p.poc = pic.poc;
  p.reference_poc = pic.reference_poc;
  p.backward_poc = pic.backward_poc;
  p.log2_max_frame_num = stream_.log2_max_frame_num;

  // The headers: the parameter sets in front of an IDR picture, then the slice header.
  headers_.clear();
  uint32_t sps_bits = 0, pps_bits = 0, slice_bits = 0;
  if (pic.idr && (!write_avc_sps(stream_, &headers_, &sps_bits) || !write_avc_pps(stream_, &headers_, &pps_bits))) return false;
  const uint64_t slice_at = headers_.size();
  AvcSliceHeader sh;
  sh.idr = pic.idr;
  sh.p_slice = !pic.idr && !pic.b;
  sh.b_slice = pic.b;
  sh.poc_lsb = pic.poc;
  sh.frame_num = pic.frame_num;
  sh.idr_pic_id = idr_id_;
  sh.qp = p.qp;
  sh.deblocking = p.deblocking;
  if (!write_avc_slice_header(stream_, sh, &headers_, &slice_bits)) return false;
  const ConstByteView all = headers_.view();
  AvcHeaderInsert inserts[3];
  uint32_t count = 0;
  if (pic.idr) {
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

// Codes one picture from `source` and appends its access unit to the output queue.
bool IntelAvcVdencEncoder::encode_picture(const VideoFrameBuffer& source, const Picture& pic, int64_t pts) {
  uint32_t source_address = 0;
  if (!buffers_.surface(0, &source_address) || !intel_mfx::tile_i420_to_nv12(source, layout_.width, layout_.height, layout_.full, buffers_.surface_buffer(0).cpu)) return false;
  uint32_t words = 0;
  if (!buffers_.clear_scratch(kScratchStatus) || !build_batch(pic, &words) || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), words))) return false;

  uint8_t size_bytes[4] = {};
  if (!buffers_.read_scratch(kScratchStatus, ByteView(size_bytes, 4))) return false;
  const uint32_t bytes = size_bytes[0] | (size_bytes[1] << 8) | (size_bytes[2] << 16) | (uint32_t{size_bytes[3]} << 24);
  if (bytes == 0 || bytes > bitstream_bytes_) return false;
  const uint32_t slot = (head_ + queued_) & 1u;
  out_[slot].clear();
  if (!out_[slot].put_zeros(bytes) || !buffers_.read_scratch(kScratchBitstream, out_[slot].writable_view())) return false;
  pts_[slot] = pts;
  key_[slot] = pic.idr;
  ++queued_;
  if (pic.idr) idr_id_ ^= 1u;   // consecutive IDR pictures name themselves differently
  ++encoded_;
  ++stats_.frames;
  return true;
}

// An anchor (IDR or P) picture at display position `display` (in pictures since the IDR picture): the reconstruction goes to the surface not holding the previous anchor.
bool IntelAvcVdencEncoder::encode_anchor(const VideoFrameBuffer& source, bool idr, uint32_t display, int64_t pts) {
  const uint32_t mask = (1u << stream_.log2_max_frame_num) - 1;
  Picture pic;
  pic.idr = idr;
  pic.b = false;
  if (idr) {
    pic.frame_num = 0;
    pic.recon_slot = 0;
  } else {
    pic.frame_num = (ref_frame_num_ + 1) & mask;
    pic.recon_slot = 1 - anchor_slot_;
  }
  pic.poc = b_mode_ ? 2 * display : 2 * pic.frame_num;   // without B pictures: pic_order_cnt_type 2 (twice the frame number)
  pic.reference_poc = anchor_poc_;
  if (!encode_picture(source, pic, pts)) return false;
  ref_frame_num_ = pic.frame_num;
  previous_anchor_poc_ = anchor_poc_;
  anchor_poc_ = pic.poc;
  previous_anchor_slot_ = anchor_slot_;
  anchor_slot_ = pic.recon_slot;
  return true;
}

// The B picture between the two latest anchors, at display position `display`.
bool IntelAvcVdencEncoder::encode_b(const VideoFrameBuffer& source, uint32_t display, int64_t pts) {
  const uint32_t mask = (1u << stream_.log2_max_frame_num) - 1;
  Picture pic;
  pic.b = true;
  pic.frame_num = (ref_frame_num_ + 1) & mask;   // (not a reference picture: the next anchor has the same frame number)
  pic.recon_slot = 2;
  pic.forward_slot = previous_anchor_slot_;
  pic.backward_slot = anchor_slot_;
  pic.poc = 2 * display;
  pic.reference_poc = previous_anchor_poc_;
  pic.backward_poc = anchor_poc_;
  return encode_picture(source, pic, pts);
}

SubmitResult IntelAvcVdencEncoder::submit(const VideoFrame& frame) {
  if (!prepared_) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (queued_ != 0) return SubmitResult::Full;
  if (frame.format.width != config_.width || frame.format.height != config_.height || frame.format.pixel_format != config_.input) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (!buffers_.engine_ready() || !convert_frame(frame, yuv_[current_])) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  const uint32_t interval = config_.keyframe_interval < 1 ? 1 : config_.keyframe_interval;
  const bool idr = pictures_ == 0 || gop_position_ + 1 >= interval;
  bool ok = true;
  if (idr) {
    if (has_held_) ok = encode_anchor(yuv_[held_], false, gop_position_, held_pts_);   // the end of a group: the picture waiting for a later anchor becomes one itself
    has_held_ = false;
    gop_position_ = 0;
    ok = ok && encode_anchor(yuv_[current_], true, 0, frame.pts_us);
  } else {
    ++gop_position_;
    if (b_mode_ && !has_held_) {
      // The first of two pictures: it waits for the next one, the anchor it will be predicted from.
      held_ = current_;
      current_ ^= 1u;
      has_held_ = true;
      held_pts_ = frame.pts_us;
      ++pictures_;
      return SubmitResult::Accepted;
    }
    ok = encode_anchor(yuv_[current_], false, gop_position_, frame.pts_us);
    if (ok && has_held_) {
      ok = encode_b(yuv_[held_], gop_position_ - 1, held_pts_);
      has_held_ = false;
    }
  }
  if (!ok) {
    queued_ = 0;
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  ++pictures_;
  return SubmitResult::Accepted;
}

void IntelAvcVdencEncoder::flush() {
  if (!has_held_ || queued_ != 0) return;
  // A last picture without a later anchor is coded as an anchor itself.
  has_held_ = false;
  if (!encode_anchor(yuv_[held_], false, gop_position_, held_pts_)) {
    queued_ = 0;
    ++stats_.errors;
  }
}

bool IntelAvcVdencEncoder::receive(VideoPacket* packet) {
  if (queued_ == 0) return false;
  const uint32_t slot = head_;
  head_ = (head_ + 1) & 1u;
  --queued_;
  packet->data = out_[slot].view();
  packet->pts_us = pts_[slot];
  packet->keyframe = key_[slot];
  ++stats_.packets;
  stats_.bytes += packet->data.size();
  return true;
}

}  // namespace hardware
