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
constexpr uint32_t kMinBitstreamBytes = 64 * 1024;
constexpr uint32_t kLog2MaxPocLsb = 8;

// quality 1 .. 100 -> QP 51 .. 15
uint32_t quantiser_for(uint32_t quality) {
  const uint32_t q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  return 51 - q * 36 / 100;
}
}  // namespace

IntelHevcVdencEncoder::IntelHevcVdencEncoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const EncoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), yuv_(allocator), headers_(allocator), out_(allocator) {}

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
  if (!yuv_.allocate({config_.width, config_.height, PixelFormat::I420})) return false;
  if (!buffers_.lease()) return false;
  if (!buffers_.open(layout_.aligned_width, layout_.aligned_height)) {
    buffers_.release_lease();
    return false;
  }
  bitstream_bytes_ = layout_.aligned_width * layout_.aligned_height * 2 > kMinBitstreamBytes ? layout_.aligned_width * layout_.aligned_height * 2 : kMinBitstreamBytes;
  uint32_t source = 0, recon0 = 0, recon1 = 0, ignore = 0, arena = 0;
  bool ok = buffers_.surface(0, &source) && buffers_.surface(1, &recon0) && buffers_.surface(2, &recon1) && buffers_.picture_buffer(1, plan_.mv_temporal_bytes, &ignore) &&
            buffers_.picture_buffer(2, plan_.mv_temporal_bytes, &ignore) && buffers_.scratch(kScratchDs4, layout_.ds4.size, &ignore) &&
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

bool IntelHevcVdencEncoder::build_batch(bool idr, uint32_t poc, uint32_t* words) {
  using namespace intel_enc;
  const uint32_t current = recon_slot_, previous = 1 - recon_slot_;   // the reconstruction alternates between two surfaces
  HevcEncodeBuffers b = addresses_;
  if (!buffers_.surface(1 + static_cast<int>(current), &b.recon) || !buffers_.surface(1 + static_cast<int>(previous), &b.reference) ||
      !buffers_.picture_buffer(1 + static_cast<int>(current), plan_.mv_temporal_bytes, &b.mv_temporal) ||
      !buffers_.picture_buffer(1 + static_cast<int>(previous), plan_.mv_temporal_bytes, &b.reference_mv_temporal) ||
      !buffers_.scratch(kScratchDs4 + current, layout_.ds4.size, &b.recon_ds4) || !buffers_.scratch(kScratchDs4 + previous, layout_.ds4.size, &b.reference_ds4) ||
      !buffers_.scratch(kScratchDs8 + current, layout_.ds8.size, &b.recon_ds8) || !buffers_.scratch(kScratchDs8 + previous, layout_.ds8.size, &b.reference_ds8))
    return false;

  HevcEncodePicture p;
  p.p_picture = !idr;
  p.qp = static_cast<int32_t>(qp_);
  p.pps_qp = 26;
  p.poc = poc;
  p.reference_poc = poc ? poc - 1 : 0;
  p.deblocking = true;

  // The headers: the parameter sets in front of an IDR picture, then the slice header.
  headers_.clear();
  uint32_t vps_bits = 0, sps_bits = 0, pps_bits = 0, slice_bits = 0;
  if (idr && (!write_hevc_vps(stream_, &headers_, &vps_bits) || !write_hevc_sps(stream_, &headers_, &sps_bits) || !write_hevc_pps(stream_, &headers_, &pps_bits))) return false;
  const uint64_t slice_at = headers_.size();
  HevcSliceHeader sh;
  sh.idr = idr;
  sh.b_slice = !idr;
  sh.poc_lsb = poc & ((1u << kLog2MaxPocLsb) - 1);
  sh.qp = p.qp;
  sh.deblocking = p.deblocking;
  if (!write_hevc_slice_header(stream_, sh, &headers_, &slice_bits)) return false;
  const ConstByteView all = headers_.view();
  HevcHeaderInsert inserts[4];
  uint32_t count = 0;
  if (idr) {
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

SubmitResult IntelHevcVdencEncoder::submit(const VideoFrame& frame) {
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
  if (!buffers_.surface(0, &source_address) || !intel_mfx::tile_i420_to_nv12(yuv_, layout_.aligned_width, layout_.aligned_height, layout_.full, buffers_.surface_buffer(0).cpu)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }

  const uint32_t interval = config_.keyframe_interval < 1 ? 1 : config_.keyframe_interval;
  const bool idr = pictures_ == 0 || gop_position_ + 1 >= interval;
  if (idr) {
    gop_position_ = 0;
    poc_ = 0;
  } else {
    ++gop_position_;
    ++poc_;
  }
  uint32_t words = 0;
  if (!buffers_.clear_scratch(kScratchStatus) || !build_batch(idr, poc_, &words) || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), words))) {
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

  recon_slot_ = 1 - recon_slot_;
  ++pictures_;
  ++encoded_;
  ++stats_.frames;
  pts_ = frame.pts_us;
  key_ = idr;
  pending_ = true;
  return SubmitResult::Accepted;
}

bool IntelHevcVdencEncoder::receive(VideoPacket* packet) {
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
