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
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The memory a decoding session keeps on the Intel video engine and the plumbing around it, shared by the sessions of the codecs that run behind a CPU decoder's offload
// interface (MPEG-2, VC-1, H.265, VP9): the engine's lease, a command batch buffer, one tiled NV12 surface per picture of the CPU decoder's pool (the engine decodes into
// it and predicts from it), a few scratch buffers the engine keeps state in, and the bitstream buffer. Every buffer is mapped in the engine's address space at an
// address below 4 GiB with an unmapped gap behind it; they are freed only while the engine is idle and ours (a wedged engine may still be writing).
class IntelEngineBuffers {
 public:
  static constexpr uint32_t kMaxSurfaces = 17;
  static constexpr uint32_t kMaxScratch = 12;
  static constexpr uint32_t kMaxMbsPerSide = 256;   // 4096 pixels

  IntelEngineBuffers(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver) : allocator_(allocator), driver_(driver) {}
  ~IntelEngineBuffers();
  IntelEngineBuffers(const IntelEngineBuffers&) = delete;
  IntelEngineBuffers& operator=(const IntelEngineBuffers&) = delete;

  // Takes the engine's lease and the batch buffer. False when the engine is not ready or busy with another session.
  bool lease();
  bool leased() const { return leased_; }
  // Gives the lease back (and the batch buffer); nothing must be open.
  void release_lease();
  bool engine_ready() const { return driver_.state() == intel_gt::VcsDriver::State::Ready; }
  ArrayView<uint32_t> batch_words() const { return batch_words_; }

  // Starts (or restarts) the picture buffers for pictures of `width` x `height` luma samples (frees what there was). False for a size the engine does not take.
  bool open(uint32_t width, uint32_t height);
  bool is_open() const { return open_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  const intel_mfx::Nv12TiledLayout& layout() const { return layout_; }
  void release_all();

  // The surface of picture `slot` of the CPU decoder's pool, made on first use; its graphics address.
  bool surface(int slot, uint32_t* address);
  const intel_gt::GpuBuffer& surface_buffer(int slot) const { return surfaces_[slot].buffer; }
  // Scratch buffer `index`, at least `bytes` big (made, or made bigger, when needed: its contents are then zero); its address.
  bool scratch(uint32_t index, uint64_t bytes, uint32_t* address);
  // Copies `bytes` into the start of scratch buffer `index` (made before with scratch()).
  bool upload_scratch(uint32_t index, ConstByteView bytes);
  // Fills scratch buffer `index` (made before with scratch()) with zeros.
  bool clear_scratch(uint32_t index);
  // Copies the start of scratch buffer `index` (made before) into `out`.
  bool read_scratch(uint32_t index, ByteView out) const;
  // A buffer tied to picture `slot` (a direct motion vector buffer), at least `bytes` big; its address. Made on first use.
  bool picture_buffer(int slot, uint64_t bytes, uint32_t* address);
  // Copies picture `from` over picture `to` (both made; a skipped picture repeats the one before it). False if either is missing.
  bool copy_surface(int from, int to);
  // Copies `bits` into the bitstream buffer (growing it as needed) and gives its address.
  bool upload_bitstream(ConstByteView bits, uint32_t* address);

  // Runs a batch on the engine. True when it completed.
  bool run(ArrayView<const uint32_t> words);
  // Reads picture `slot` back into planes of the CPU decoder (see detile_nv12_to_planes).
  bool read_back(int slot, ByteView y, uint32_t y_stride, ByteView u, uint32_t u_stride, ByteView v, uint32_t v_stride, uint32_t first_line, uint32_t line_step);
  // Fills picture `slot` with mid-grey (a picture the engine could not decode).
  void grey(int slot);

 private:
  struct Block {
    intel_gt::GpuBuffer buffer;
    uint32_t address = 0;
  };
  bool place(Block* block, uint64_t bytes);
  void drop(Block* block);

  allocators::IAllocator& allocator_;
  intel_gt::VcsDriver& driver_;
  bool leased_ = false, open_ = false;
  uint32_t width_ = 0, height_ = 0;
  intel_mfx::Nv12TiledLayout layout_;
  Block surfaces_[kMaxSurfaces], aux_[kMaxSurfaces], scratch_[kMaxScratch], bitstream_;
  uint64_t next_address_ = intel_gt::VcsDriver::kFirstBufferGpuAddress;
  ArrayView<uint32_t> batch_words_;
};

}  // namespace hardware
