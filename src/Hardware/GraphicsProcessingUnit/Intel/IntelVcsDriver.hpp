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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGtt.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelLrc.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderCommands.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {

// Memory the GPU reads and writes: physically contiguous, zeroed, 4 KiB aligned, and in reach of the GPU (below 2^39).
struct GpuBuffer {
  ByteView cpu;          // the CPU's view
  uint64_t dma = 0;      // the address the GPU uses for the first byte
  bool valid() const { return !cpu.empty(); }
};

class IGpuMemory {
 public:
  virtual bool allocate(uint64_t bytes, GpuBuffer* out) = 0;
  // Gives back a buffer by its CPU view. Never called for memory the GPU may still be writing to (see VcsDriver).
  virtual void free(ByteView cpu) = 0;

 protected:
  constexpr IGpuMemory() = default;
  ~IGpuMemory() = default;
};

// GPU memory from the OS allocator's DMA pool. The kernel maps memory 1:1, so the address the CPU uses is the address the GPU uses.
class AllocatorGpuMemory final : public IGpuMemory {
 public:
  explicit AllocatorGpuMemory(allocators::IAllocator& allocator) : allocator_(allocator) {}
  bool allocate(uint64_t bytes, GpuBuffer* out) override;
  void free(ByteView cpu) override { allocator_.deallocate(cpu.data()); }

 private:
  allocators::IAllocator& allocator_;
};

// The first video engine of a Gen9 GPU (VCS0; Skylake through Coffee Lake and Comet Lake graphics) or of a Gen12 one (VD0; Xe-LP: Tiger Lake, Rocket Lake, Alder Lake
// and Raptor Lake graphics, integrated parts), or the render engine of a Gen9 one (RCS0, Config::engine), driven through its execlist: one context, one
// engine, one job at a time, polled to completion. It sets up everything the engine needs (global GTT entries for the ring, status page and context,
// the context's own page tables, the register state image), takes a command batch and runs it.
//
// This is not wired into anything: nothing creates it yet, and the video device does not use it until the decode path on top is verified on hardware. It
// is written to fail safe:
//  - every entry point checks its arguments, and a failure at any step leaves the GPU the way it was found (before the engine is enabled nothing the GPU
//    can reach has been given to it);
//  - the driver only touches the part of the global GTT it was given, so the display's mappings stay;
//  - a job that does not finish in time wedges the engine for good: the ring is stopped if the engine says it can be, and memory the engine might
//    still be writing to is never freed or reused;
//  - batches are checked before they run: only the commands of a decode batch are accepted (of a 3D batch, for the render engine: see IntelRenderCommands).
class VcsDriver {
 public:
  enum class State : uint8_t { Created, Ready, Failed, Wedged };
  enum class Engine : uint8_t { Video, Render };
  enum class Result : uint8_t { Done, Rejected, Timeout };

  // Pages of the global GTT the driver needs: ring (4), status page (1), context (2; 22 for the render engine), and one more kept as the scratch page entry.
  static constexpr uint64_t kRingBytes = 4 * kPageSize;
  static constexpr uint64_t kGgttPagesNeeded = 8;
  static constexpr uint64_t kRenderGgttPagesNeeded = 5 + kLrcRenderPages + 1;
  static constexpr uint64_t ggtt_pages_for(Engine engine) { return engine == Engine::Render ? kRenderGgttPagesNeeded : kGgttPagesNeeded; }
  // Graphics addresses (in the context's own address space) of the batch buffer and the lowest address the caller may map buffers at.
  static constexpr uint32_t kBatchGpuAddress = 0x00010000;
  static constexpr uint64_t kBatchBytes = 16 * kPageSize;
  static constexpr uint32_t kFirstBufferGpuAddress = 0x00100000;
  static constexpr uint32_t kContextId = 1;
  static constexpr uint32_t kSeqnoDword = 0x40;  // I915_GEM_HWS_SEQNO: where the batch writes its marker in the status page

  struct Config {
    IRegisterBank* registers = nullptr;   // the GPU's registers
    MmioRegion ggtt_table;                // the whole global GTT table
    uint64_t ggtt_first_page = 0;         // the first of the ggtt_pages_for(engine) pages of graphics address space this driver may use; it touches no others
    Generation generation = Generation::Gen9;
    Engine engine = Engine::Video;        // the render engine is Gen9 only; it needs ggtt_pages_for(Engine::Render) pages
    uint16_t device_id = 0;               // the GPU's PCI device ID and revision: which Gen9.5 workarounds the render engine gets (IntelGen9Setup.hpp)
    uint8_t revision = 0;
    uint32_t engine_base = 0;             // 0: the first video engine of the generation (VCS0 at 0x12000, VD0 at 0x1C0000)
    uint32_t poll_limit = 5'000'000;      // polls of the completion marker (and then the status buffer) before a job is given up
  };

  explicit VcsDriver(IGpuMemory& memory) : memory_(memory), page_source_(memory), ppgtt_(page_source_) {}
  ~VcsDriver() { shutdown(); }
  VcsDriver(const VcsDriver&) = delete;
  VcsDriver& operator=(const VcsDriver&) = delete;

  bool initialize(const Config& config);
  // Stops the engine and gives its memory back (unless it cannot be shown to be idle, then nothing is freed). Safe to call again.
  void shutdown();
  State state() const { return state_; }

  // Makes the pages of `buffer` visible to the engine at `gpu_address` (4 KiB aligned, at least kFirstBufferGpuAddress, below 4 GiB).
  bool map_buffer(const GpuBuffer& buffer, uint32_t gpu_address);
  bool unmap_buffer(const GpuBuffer& buffer, uint32_t gpu_address);

  // Runs one batch: copies it to the batch buffer, queues it on the ring and waits for it. The batch may only hold the commands a decode batch holds, and must
  // end with MI_BATCH_BUFFER_END. (Render engine: the commands of gen9_3d, an end marker, and then any state the commands point to.)
  Result run_batch(ArrayView<const uint32_t> commands);
  // Checks a video batch the way run_batch does (render batches: gen9_3d::render_batch_is_acceptable).
  static bool batch_is_acceptable(ArrayView<const uint32_t> commands);

  uint32_t last_sequence() const { return seqno_; }

  // Only one user at a time can have buffers in the context's address space: whoever starts a decode session takes the lease and gives it back at the end.
  bool try_lease() {
    if (state_ != State::Ready || leased_) return false;
    leased_ = true;
    return true;
  }
  void release_lease() { leased_ = false; }
  IGpuMemory& memory() { return memory_; }

 private:
  // Page table pages come out of the same GPU memory.
  class PageSource final : public IGpuPageSource {
   public:
    explicit PageSource(IGpuMemory& memory) : memory_(memory) {}
    bool allocate_page(ArrayView<uint64_t>* entries, uint64_t* dma_address) override;
    void free_page(ArrayView<uint64_t> entries) override;

   private:
    IGpuMemory& memory_;
  };

  bool allocate(uint64_t bytes, GpuBuffer* out);
  bool render() const { return config_.engine == Engine::Render; }
  ForcewakeDomain engine_domain() const {
    if (render()) return ForcewakeDomain::Render;
    return config_.generation == Generation::Gen12 ? ForcewakeDomain::MediaVdbox0 : ForcewakeDomain::Media;
  }
  uint32_t engine_base() const {
    if (config_.engine_base) return config_.engine_base;
    return render() ? kRcs0MmioBase : config_.generation == Generation::Gen12 ? kVd0MmioBase : kVcs0MmioBase;
  }
  void write_job(uint64_t status_address, uint32_t sequence);
  void fail_init();
  bool wedge();
  void write_ring(uint32_t dword);

  IGpuMemory& memory_;
  PageSource page_source_;
  Ppgtt32 ppgtt_;
  State state_ = State::Created;
  Config config_;
  GpuBuffer scratch_, ring_, status_, context_, batch_;
  ArrayView<uint32_t> ring_dwords_, status_dwords_, context_state_, batch_dwords_;
  uint64_t ring_ggtt_ = 0, status_ggtt_ = 0, context_ggtt_ = 0;
  bool ggtt_mapped_ = false;
  uint64_t ggtt_pages_ = kGgttPagesNeeded;
  uint32_t lrc_pages_ = kLrcPages;
  uint64_t saved_entries_[kRenderGgttPagesNeeded] = {};  // what the driver's global GTT entries held before it changed them
  uint64_t ring_tail_ = 0;  // bytes
  uint32_t seqno_ = 0;
  uint64_t write_position_ = 0;
  bool leased_ = false;
  ExeclistPort port_;
};

}  // namespace intel_gt
}  // namespace hardware
