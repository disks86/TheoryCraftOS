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


#include "Hardware/VideoCodec/Intel/IntelHevcVdencEncoder.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"

namespace hardware {

namespace {
// Scratch buffer indices of IntelEngineBuffers.
constexpr uint32_t kScratchDs4 = 0 /* and 1 */, kScratchDs8 = 2 /* and 3 */, kScratchArena = 4, kScratchBitstream = 5, kScratchStatus = 6, kScratchPakObjects = 7;
constexpr uint32_t kScratchDs4B = 8, kScratchDs8B = 9;   // the B picture's down-scaled reconstructions
uint32_t ds4_index(uint32_t slot) { return slot < 2 ? slot : kScratchDs4B; }
uint32_t ds8_index(uint32_t slot) { return slot < 2 ? 2 + slot : kScratchDs8B; }
constexpr uint32_t kMinBitstreamBytes = 64 * 1024;
constexpr uint32_t kLog2MaxPocLsb = 8;

// quality 1 .. 100 -> QP 51 .. 15
uint32_t quantiser_for(uint32_t quality) {
  const uint32_t q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  return 51 - q * 36 / 100;
}
}  // namespace

IntelHevcVdencEncoder::IntelHevcVdencEncoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const EncoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), yuv_{VideoFrameBuffer(allocator), VideoFrameBuffer(allocator)}, headers_(allocator), out_{ByteBuffer(allocator), ByteBuffer(allocator)} {}

bool IntelHevcVdencEncoder::prepare() {
  if (prepared_) return true;
  if (config_.codec != VideoCodec::Hevc) return false;
  if (config_.input != PixelFormat::I420 && config_.input != PixelFormat::Nv12 && config_.input != PixelFormat::Bgra8888) return false;
  if (config_.width < 16 || config_.height < 16 || config_.width > 4096 || config_.height > 4096 || (config_.width & 1) || (config_.height & 1)) return false;
  if (!buffers_.gen12()) return false;   // (Ice Lake builds two of the VDEnc commands with HuC firmware)
  if (!intel_enc::hevc_encode_layout((config_.width + 7) & ~7u, (config_.height + 7) & ~7u, &layout_)) return false;
  plan_ = intel_enc::hevc_scratch_plan(layout_);
  qp_ = quantiser_for(config_.quality);
  stream_.width = config_.width;
  stream_.height = config_.height;
  stream_.log2_max_poc_lsb = kLog2MaxPocLsb;
  // B pictures: only where the commands are written (Xe-LPG, Arc and Xe2).
  ra_mode_ = config_.b_frames > 0 && (buffers_.xe_lpg() || buffers_.xe_hpg() || buffers_.xe2());
  stream_.b_pictures = ra_mode_;
  if (!yuv_[0].allocate({config_.width, config_.height, PixelFormat::I420}) || !yuv_[1].allocate({config_.width, config_.height, PixelFormat::I420})) return false;
  if (!buffers_.lease()) return false;
  if (!buffers_.open(layout_.aligned_width, layout_.aligned_height)) {
    buffers_.release_lease();
    return false;
  }
  bitstream_bytes_ = layout_.aligned_width * layout_.aligned_height * 2 > kMinBitstreamBytes ? layout_.aligned_width * layout_.aligned_height * 2 : kMinBitstreamBytes;
  uint32_t source = 0, recon0 = 0, recon1 = 0, ignore = 0, arena = 0;
  bool ok = buffers_.surface(0, &source) && buffers_.surface(1, &recon0) && buffers_.surface(2, &recon1) && buffers_.picture_buffer(1, plan_.mv_temporal_bytes, &ignore) &&
            buffers_.picture_buffer(2, plan_.mv_temporal_bytes, &ignore) &&
            (!ra_mode_ || (buffers_.surface(3, &ignore) && buffers_.picture_buffer(3, plan_.mv_temporal_bytes, &ignore) && buffers_.scratch(kScratchDs4B, layout_.ds4.size, &ignore) &&
                           buffers_.scratch(kScratchDs8B, layout_.ds8.size, &ignore))) && buffers_.scratch(kScratchDs4, layout_.ds4.size, &ignore) &&
            buffers_.scratch(kScratchDs4 + 1, layout_.ds4.size, &ignore) && buffers_.scratch(kScratchDs8, layout_.ds8.size, &ignore) &&
            buffers_.scratch(kScratchDs8 + 1, layout_.ds8.size, &ignore) && buffers_.scratch(kScratchArena, plan_.total, &arena) &&
            buffers_.scratch(kScratchBitstream, bitstream_bytes_, &addresses_.bitstream) && buffers_.scratch(kScratchStatus, 4096, &addresses_.status) &&
            buffers_.scratch(kScratchPakObjects, plan_.pak_object_bytes, &addresses_.pak_objects);
  if (!ok) {
    buffers_.release_lease();
    return false;
  }
  addresses_.source = source;
  addresses_.bitstream_bytes = bitstream_bytes_;
  intel_enc::hevc_resolve_scratch(plan_, arena, &addresses_);
  prepared_ = true;
  return true;
}

bool IntelHevcVdencEncoder::build_batch(const Picture& pic, uint32_t* words) {
  using namespace intel_enc;
  HevcEncodeBuffers b = addresses_;
  // Anchors alternate between surfaces 1 and 2; a B picture, which nothing refers to, uses surface 3 (and its own down-scaled copies and motion vector buffer).
  const uint32_t recon_slot = pic.b ? 2 : pic.recon_slot;
  if (!buffers_.surface(1 + static_cast<int>(recon_slot), &b.recon) || !buffers_.picture_buffer(1 + static_cast<int>(recon_slot), plan_.mv_temporal_bytes, &b.mv_temporal) ||
      !buffers_.scratch(ds4_index(recon_slot), layout_.ds4.size, &b.recon_ds4) || !buffers_.scratch(ds8_index(recon_slot), layout_.ds8.size, &b.recon_ds8))
    return false;
  if (!pic.idr) {
    const uint32_t forward = pic.b ? pic.forward_slot : 1 - pic.recon_slot;
    if (!buffers_.surface(1 + static_cast<int>(forward), &b.reference) || !buffers_.picture_buffer(1 + static_cast<int>(forward), plan_.mv_temporal_bytes, &b.reference_mv_temporal) ||
        !buffers_.scratch(ds4_index(forward), layout_.ds4.size, &b.reference_ds4) || !buffers_.scratch(ds8_index(forward), layout_.ds8.size, &b.reference_ds8))
      return false;
  }
  if (pic.b) {
    if (!buffers_.surface(1 + static_cast<int>(pic.backward_slot), &b.backward_reference) ||
        !buffers_.picture_buffer(1 + static_cast<int>(pic.backward_slot), plan_.mv_temporal_bytes, &b.backward_mv_temporal) ||
        !buffers_.scratch(ds4_index(pic.backward_slot), layout_.ds4.size, &b.backward_ds4) || !buffers_.scratch(ds8_index(pic.backward_slot), layout_.ds8.size, &b.backward_ds8))
      return false;
  }

  HevcEncodePicture p;
  p.platform = buffers_.xe_lpg() ? HevcVdencPlatform::XeLpg : buffers_.xe_hpg() ? HevcVdencPlatform::XeHpg : buffers_.xe2_hpm() ? HevcVdencPlatform::Xe2Hpm : buffers_.xe2() ? HevcVdencPlatform::Xe2Lpm : HevcVdencPlatform::XeLp;
  p.p_picture = !pic.idr;
  p.b_picture = pic.b;
  p.ra_mode = ra_mode_;
  p.reference_store = !ra_mode_ ? 0 : pic.b ? pic.forward_slot : 1 - pic.recon_slot;   // (frame store 0 for every reference of a stream without B pictures)
  p.backward_store = pic.backward_slot;
  p.qp = static_cast<int32_t>(qp_);
  p.pps_qp = 26;
  p.poc = pic.poc;
  p.reference_poc = pic.reference_poc;
  p.backward_poc = pic.backward_poc;
  p.deblocking = true;

  // The headers: the parameter sets in front of an IDR picture, then the slice header.
  headers_.clear();
  uint32_t vps_bits = 0, sps_bits = 0, pps_bits = 0, slice_bits = 0;
  if (pic.idr && (!write_hevc_vps(stream_, &headers_, &vps_bits) || !write_hevc_sps(stream_, &headers_, &sps_bits) || !write_hevc_pps(stream_, &headers_, &pps_bits))) return false;
  const uint64_t slice_at = headers_.size();
  HevcSliceHeader sh;
  sh.idr = pic.idr;
  sh.b_slice = !pic.idr;
  sh.poc_lsb = pic.poc & ((1u << kLog2MaxPocLsb) - 1);
  sh.qp = p.qp;
  sh.deblocking = p.deblocking;
  if (!pic.idr) {
    sh.negative_delta = pic.poc - pic.reference_poc;
    sh.positive_delta = pic.b ? pic.backward_poc - pic.poc : 0;
    sh.non_reference = pic.b;
  }
  if (!write_hevc_slice_header(stream_, sh, &headers_, &slice_bits)) return false;
  const ConstByteView all = headers_.view();
  HevcHeaderInsert inserts[4];
  uint32_t count = 0;
  if (pic.idr) {
    const uint32_t set_bits[3] = {vps_bits, sps_bits, pps_bits};
    uint64_t at = 0;
    for (const uint32_t bits : set_bits) {
      inserts[count].data = all.subview(at, bits / 8);
      inserts[count++].bits = bits;
      at += bits / 8;
    }
  }
  inserts[count].data = all.subview(slice_at, headers_.size() - slice_at);
  inserts[count].bits = slice_bits;
  inserts[count].emulation_prevention = true;
  inserts[count].skip_emulation_bytes = 6;   // the start code and the two-byte NAL unit header
  ++count;

  DwordWriter w(buffers_.batch_words());
  intel_mfx::emit_mi_flush(w);
  emit_hevc_vdenc_picture(w, p, b, layout_);
  if (!emit_hevc_vdenc_slice(w, p, layout_, ArrayView<const HevcHeaderInsert>(inserts, count))) return false;
  emit_hevc_vdenc_finish(w, b);
  if (!w.ok()) return false;
  *words = static_cast<uint32_t>(w.count());
  return true;
}

// Codes one picture from `source` and appends its access unit to the output queue.
bool IntelHevcVdencEncoder::encode_picture(const VideoFrameBuffer& source, const Picture& pic, int64_t pts) {
  uint32_t source_address = 0;
  if (!buffers_.surface(0, &source_address) || !intel_mfx::tile_i420_to_nv12(source, layout_.aligned_width, layout_.aligned_height, layout_.full, buffers_.surface_buffer(0).cpu)) return false;
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
  ++encoded_;
  ++stats_.frames;
  return true;
}

// An anchor (IDR or P) picture at output position `poc`: the reconstruction goes to the surface not holding the previous anchor.
bool IntelHevcVdencEncoder::encode_anchor(const VideoFrameBuffer& source, bool idr, uint32_t poc, int64_t pts) {
  Picture pic;
  pic.idr = idr;
  pic.poc = idr ? 0 : poc;
  pic.recon_slot = idr ? 0 : 1 - anchor_slot_;
  pic.reference_poc = anchor_poc_;
  if (!encode_picture(source, pic, pts)) return false;
  previous_anchor_slot_ = anchor_slot_;
  previous_anchor_poc_ = anchor_poc_;
  anchor_slot_ = pic.recon_slot;
  anchor_poc_ = pic.poc;
  return true;
}

// The B picture between the two latest anchors, at output position `poc`.
bool IntelHevcVdencEncoder::encode_b(const VideoFrameBuffer& source, uint32_t poc, int64_t pts) {
  Picture pic;
  pic.b = true;
  pic.poc = poc;
  pic.recon_slot = 2;
  pic.forward_slot = previous_anchor_slot_;
  pic.backward_slot = anchor_slot_;
  pic.reference_poc = previous_anchor_poc_;
  pic.backward_poc = anchor_poc_;
  return encode_picture(source, pic, pts);
}

SubmitResult IntelHevcVdencEncoder::submit(const VideoFrame& frame) {
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
    if (ra_mode_ && !has_held_) {
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

void IntelHevcVdencEncoder::flush() {
  if (!has_held_ || queued_ != 0) return;
  // A last picture without a later anchor is coded as an anchor itself.
  has_held_ = false;
  if (!encode_anchor(yuv_[held_], false, gop_position_, held_pts_)) {
    queued_ = 0;
    ++stats_.errors;
  }
}

bool IntelHevcVdencEncoder::receive(VideoPacket* packet) {
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
