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

#include "Hardware/VideoCodec/Intel/IntelAvcSession.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
using intel_gt::GpuBuffer;
constexpr uint64_t kPage = intel_gt::kPageSize;
constexpr uint64_t kAddressAlignment = 0x10000;
constexpr uint64_t kAddressLimit = 0xFFFF0000ull;       // buffers stay below 4 GiB with a margin
constexpr uint64_t kBitstreamMinimum = 1u << 20;
constexpr uint64_t kBatchWords = intel_gt::VcsDriver::kBatchBytes / 4;

uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }
}  // namespace

IntelAvcDecoder::IntelAvcDecoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config)
    : allocator_(allocator), driver_(driver), config_(config), parser_(allocator), slices_(allocator), rows0_(allocator), rows1_(allocator), i420_(allocator), out_(allocator) {
  h264::PictureInfo* slots[h264::kMaxPictures];
  for (int i = 0; i < h264::kMaxPictures; ++i) slots[i] = &infos_[i];
  dpb_.attach(slots, h264::kMaxPictures);
}

IntelAvcDecoder::~IntelAvcDecoder() {
  release_resources();
  release_view(allocator_, batch_words_);
  if (leased_) driver_.release_lease();
}

bool IntelAvcDecoder::prepare() {
  if (!driver_.try_lease()) return false;
  leased_ = true;
  batch_words_ = allocate_view<uint32_t>(allocator_, kBatchWords);
  if (batch_words_.empty() || !(config_.codec_config.empty() || parser_.configure(config_.codec_config))) {
    release_view(allocator_, batch_words_);
    driver_.release_lease();
    leased_ = false;
    return false;
  }
  return true;
}

bool IntelAvcDecoder::place(GpuBuffer* buffer, uint64_t bytes, uint32_t* address) {
  bytes = round_up(bytes, kPage);
  const uint64_t at = round_up(next_address_, kAddressAlignment);
  if (bytes == 0 || at > kAddressLimit || bytes > kAddressLimit - at) return false;
  if (!driver_.memory().allocate(bytes, buffer)) return false;
  if (!driver_.map_buffer(*buffer, static_cast<uint32_t>(at))) {
    driver_.memory().free(buffer->cpu);
    *buffer = GpuBuffer();
    return false;
  }
  *address = static_cast<uint32_t>(at);
  next_address_ = at + bytes + kAddressAlignment;  // an unmapped gap behind every buffer
  return true;
}

void IntelAvcDecoder::drop(GpuBuffer* buffer, uint32_t address) {
  if (!buffer->valid()) return;
  // Only an engine that is idle and ours can have its buffers freed; a wedged one may still be writing to them.
  if (driver_.state() == intel_gt::VcsDriver::State::Ready) {
    driver_.unmap_buffer(*buffer, address);
    driver_.memory().free(buffer->cpu);
  }
  *buffer = GpuBuffer();
}

void IntelAvcDecoder::release_resources() {
  for (int i = 0; i < h264::kMaxPictures; ++i) {
    drop(&slots_[i].surface, slots_[i].surface_address);
    drop(&slots_[i].direct_mv, slots_[i].direct_mv_address);
    infos_[i].has_memory = false;
  }
  drop(&intra_row_, intra_row_address_);
  drop(&deblock_row_, deblock_row_address_);
  drop(&bsd_mpc_, bsd_mpc_address_);
  drop(&mpr_, mpr_address_);
  drop(&bitstream_, bitstream_address_);
  next_address_ = intel_gt::VcsDriver::kFirstBufferGpuAddress;
  configured_ = false;
}

bool IntelAvcDecoder::configure_stream(const h264::Sps& sps) {
  release_resources();
  if (sps.mb_w < 1 || sps.mb_h < 1 || static_cast<uint32_t>(sps.mb_w) > kMaxMbsPerSide || static_cast<uint32_t>(sps.mb_h) > kMaxMbsPerSide ||
      sps.chroma_format_idc != 1 || sps.mbaff)
    return false;
  mb_w_ = sps.mb_w;
  mb_h_ = sps.mb_h;
  if (!intel_mfx::nv12_tiled_layout(static_cast<uint32_t>(mb_w_) * 16, static_cast<uint32_t>(mb_h_) * 16, &layout_)) return false;
  const uint64_t w = static_cast<uint64_t>(mb_w_);
  if (!place(&intra_row_, w * 64, &intra_row_address_) || !place(&deblock_row_, w * 64 * 4, &deblock_row_address_) ||
      !place(&bsd_mpc_, w * 64 * 2, &bsd_mpc_address_) || !place(&mpr_, w * 64 * 2, &mpr_address_) ||
      !place(&bitstream_, kBitstreamMinimum, &bitstream_address_)) {
    release_resources();
    return false;
  }
  configured_ = true;
  return true;
}

bool IntelAvcDecoder::ensure_slot(int slot) {
  Slot& s = slots_[slot];
  if (s.surface.valid() && s.direct_mv.valid()) return true;
  drop(&s.surface, s.surface_address);
  drop(&s.direct_mv, s.direct_mv_address);
  const uint64_t dmv_bytes = static_cast<uint64_t>(mb_w_) * static_cast<uint64_t>(mb_h_) * 128;
  if (!place(&s.surface, layout_.size, &s.surface_address) || !place(&s.direct_mv, dmv_bytes, &s.direct_mv_address)) {
    drop(&s.surface, s.surface_address);
    drop(&s.direct_mv, s.direct_mv_address);
    return false;
  }
  return true;
}

bool IntelAvcDecoder::ensure_bitstream(uint64_t bytes) {
  if (bitstream_.valid() && bitstream_.cpu.size() >= bytes) return true;
  uint64_t wanted = bitstream_.valid() ? bitstream_.cpu.size() * 2 : kBitstreamMinimum;
  if (wanted < bytes) wanted = bytes;
  drop(&bitstream_, bitstream_address_);  // the engine is idle between jobs
  return place(&bitstream_, wanted, &bitstream_address_);
}

SubmitResult IntelAvcDecoder::submit(const VideoPacket& packet) {
  if (driver_.state() != intel_gt::VcsDriver::State::Ready || batch_words_.empty()) {
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
      // Nothing is decoded before the first IDR picture (an I picture will do), and a new picture size needs new buffers: first let everything of the old size out.
      if (!configured_ && !first.header.idr() && first.header.type != h264::kSliceI) {
        ++stats_.errors;
        return SubmitResult::Invalid;
      }
      dpb_.bump_output(true);
      if (dpb_.output_ready()) {
        parser_.reset();  // the packet will come again
        return SubmitResult::Full;
      }
      dpb_.set_sps(sps);
      dpb_.clear_pictures();
      if (!configure_stream(*sps)) {
        ++stats_.errors;
        return SubmitResult::Invalid;
      }
    }
    if (!ensure_bitstream(packet.data.size()) || !bitstream_.cpu.first(packet.data.size()).copy_from(packet.data)) {
      ++stats_.errors;
      return SubmitResult::Invalid;
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

bool IntelAvcDecoder::decode_picture(ConstByteView packet, ArrayView<const H264Slice> group, int64_t pts) {
  const H264Slice& first = group[0];
  const h264::Sps* sps = parser_.sps_for(first.header);
  const h264::Pps* pps = parser_.pps_for(first.header);
  if (!sps || !pps) { ++stats_.errors; return false; }
  // Like the CPU decoder: nothing decodes before an IDR picture (an I picture will do to start with).
  if (waiting_for_idr_ && !first.header.idr() && first.header.type != h264::kSliceI) return false;
  if (!configured_ || mb_w_ != sps->mb_w || mb_h_ != sps->mb_h) { ++stats_.errors; return false; }  // a size change in the middle of a packet
  sps_ = sps;
  dpb_.set_sps(sps);
  h264::PictureInfo* cur = dpb_.begin_picture(first.header, pts);
  if (!cur) { ++stats_.errors; return false; }
  if (!ensure_slot(cur->slot)) {
    dpb_.abort_picture();
    ++stats_.errors;
    return false;
  }
  cur->has_memory = true;
  waiting_for_idr_ = false;

  // The reference frames the engine is told about: the pictures marked for reference now, in slot order.
  intel_mfx::AvcPictureJob job;
  int frame_store[h264::kMaxPictures];
  for (int& f : frame_store) f = -1;
  bool ok = true;
  uint32_t used = 0;
  for (int slot = 0; slot < h264::kMaxPictures; ++slot) {
    const h264::PictureInfo& p = infos_[slot];
    if (&p == cur || !p.is_ref) continue;
    if (used >= intel_mfx::kMaxAvcReferenceFrames || !slots_[slot].surface.valid() || !slots_[slot].direct_mv.valid()) { ok = false; break; }
    intel_mfx::AvcFrameStore& f = job.frames[used];
    f.used = true;
    f.picture = slots_[slot].surface_address;
    f.direct_mv = slots_[slot].direct_mv_address;
    f.poc[0] = p.top_poc;
    f.poc[1] = p.bottom_poc;
    f.long_term = p.is_long;
    frame_store[slot] = static_cast<int>(used++);
  }

  // The reference lists of every slice, as indices into those frame stores.
  rows0_.clear();
  rows1_.clear();
  for (const H264Slice& slice : group) {
    RefRow r0, r1;
    for (uint8_t& e : r0.entry) e = 0xFF;
    for (uint8_t& e : r1.entry) e = 0xFF;
    dpb_.build_ref_lists(slice.header);
    for (int l = 0; l < 2; ++l) {
      const int count = dpb_.ref_count(l) < static_cast<int>(intel_mfx::kMaxAvcRefIdx) ? dpb_.ref_count(l) : static_cast<int>(intel_mfx::kMaxAvcRefIdx);
      for (int k = 0; k < count; ++k) {
        const h264::PictureInfo* r = dpb_.ref(l, k);
        const int idx = r ? frame_store[r->slot] : -1;
        (l == 0 ? r0 : r1).entry[k] = idx < 0 ? 0xFF : static_cast<uint8_t>(idx);
      }
    }
    if (!rows0_.push_back(r0) || !rows1_.push_back(r1)) ok = false;
  }

  bool decoded = false;
  if (ok) {
    job.sets = parser_.parameter_sets();
    job.sps = sps;
    job.pps = pps;
    job.slices = group;
    job.access_unit = packet;
    job.bitstream_address = bitstream_address_;
    job.ref_list0 = &rows0_[0].entry;
    job.ref_list1 = &rows1_[0].entry;
    job.current_poc[0] = cur->top_poc;
    job.current_poc[1] = cur->bottom_poc;
    intel_mfx::AvcBuffers& b = job.buffers;
    b.current_picture = slots_[cur->slot].surface_address;
    b.direct_mv_current = slots_[cur->slot].direct_mv_address;
    b.intra_row_store = intra_row_address_;
    b.deblocking_row_store = deblock_row_address_;
    b.bsd_mpc_row_store = bsd_mpc_address_;
    b.mpr_row_store = mpr_address_;
    uint64_t words = 0;
    if (intel_mfx::build_avc_decode_batch(job, batch_words_, &words) == intel_mfx::AvcBatchError::None) {
      decoded = driver_.run_batch(ArrayView<const uint32_t>(batch_words_.data(), words)) == intel_gt::VcsDriver::Result::Done;
    }
  }
  if (!decoded) {
    // A picture the engine could not decode (damaged stream, or the engine failed): show mid-grey, as the CPU decoder does for what it cannot decode, and
    // keep the buffer in order.
    ++stats_.errors;
    if (driver_.state() == intel_gt::VcsDriver::State::Ready)
      for (uint8_t& b : slots_[cur->slot].surface.cpu) b = 128;
  }
  dpb_.finish_picture(group[group.size() - 1].header);
  return decoded;
}

bool IntelAvcDecoder::receive(VideoFrame* frame) {
  h264::PictureInfo* p = dpb_.take_output();
  if (!p || !sps_) return false;
  const h264::Sps& s = *sps_;
  const uint32_t width = static_cast<uint32_t>(s.mb_w * 16 - s.crop_l - s.crop_r);
  const uint32_t height = static_cast<uint32_t>(s.mb_h * 16 - s.crop_t - s.crop_b);
  const GpuBuffer& surface = slots_[p->slot].surface;
  if (!surface.valid() || !i420_.allocate({width, height, PixelFormat::I420}) ||
      !intel_mfx::detile_nv12_to_i420(surface.cpu, layout_, width, height, i420_, static_cast<uint32_t>(s.crop_l), static_cast<uint32_t>(s.crop_t))) {
    ++stats_.errors;
    return false;
  }
  VideoFrame in = i420_.frame(p->pts, p->keyframe);
  in.limited_range = !s.full_range;
  if (!out_.allocate({width, height, config_.output}) || !convert_frame(in, out_)) {
    ++stats_.errors;
    return false;
  }
  *frame = out_.frame(p->pts, p->keyframe);
  frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
  ++stats_.frames;
  return true;
}

void IntelAvcDecoder::reset() {
  dpb_.reset();
  waiting_for_idr_ = true;
  parser_.reset();
}

VideoFormat IntelAvcDecoder::output_format() const {
  VideoFormat f;
  if (sps_ && configured_) {
    f.width = static_cast<uint32_t>(sps_->mb_w * 16 - sps_->crop_l - sps_->crop_r);
    f.height = static_cast<uint32_t>(sps_->mb_h * 16 - sps_->crop_t - sps_->crop_b);
    f.pixel_format = config_.output;
  }
  return f;
}

}  // namespace hardware
