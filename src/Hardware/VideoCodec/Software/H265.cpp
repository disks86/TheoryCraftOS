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


#include "Hardware/VideoCodec/Software/H265Core.hpp"

namespace hardware {

H265Decoder::~H265Decoder() = default;

bool H265Decoder::configure(ConstByteView hvcc) {
  if (!core_) core_ = UniquePointer<h265::Core>::make(allocator_, allocator_);
  return core_ && core_->configure(hvcc);
}

bool H265Decoder::decode(ConstByteView access_unit, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<h265::Core>::make(allocator_, allocator_);
  return core_ && core_->decode(access_unit, pts_us);
}

bool H265Decoder::receive(VideoFrame* frame) { return core_ && core_->receive(frame); }

void H265Decoder::flush() {
  if (core_) core_->flush();
}

void H265Decoder::reset() {
  if (core_) core_->reset();
}

VideoFormat H265Decoder::output_format() const { return core_ ? core_->output_format() : VideoFormat(); }

bool H265Decoder::ready() const { return core_ && core_->ready(); }

H265Decoder::HashStatistics H265Decoder::hash_statistics() const { return core_ ? core_->hash_statistics() : HashStatistics(); }

void H265Decoder::set_plane_hasher(IPlaneHasher* hasher) {
  if (!core_) core_ = UniquePointer<h265::Core>::make(allocator_, allocator_);
  if (core_) core_->set_plane_hasher(hasher);
}

void H265Decoder::set_offload(h265::IOffload* offload) {
  if (!core_) core_ = UniquePointer<h265::Core>::make(allocator_, allocator_);
  if (core_) core_->set_offload(offload);
}

}  // namespace hardware
