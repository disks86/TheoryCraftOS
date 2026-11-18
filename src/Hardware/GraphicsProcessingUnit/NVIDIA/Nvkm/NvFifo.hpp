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

// The host's channels (nouveau's nvkm/engine/fifo: gk104.c, gk110.c, gv100.c, tu102.c, runl.c, chan.c), Kepler to Turing.
//
// A channel is an instance block (RAMFC: the channel's saved host state, first 0x200 bytes; the page directory base at 0x200), a
// USERD page (where the CPU writes GP_PUT and the host writes GP_GET back), a GPFIFO ring of 8-byte entries each pointing at a run
// of push-buffer words, and a VMM the host and engines translate those addresses with. Channels run when they are bound (0x800000 +
// id * 8), enabled and on their engine's runlist. Kepler..Pascal: the host sees GP_PUT through BAR1 (USERD mapped there, 0x002254);
// Volta and Turing: GP_PUT is written to USERD and the usermode doorbell (0x810090) is rung, and every channel sits in a TSG.
//
// Ampere and later are recognized and refused: channels, runlists and engines are owned by the GSP firmware there (nouveau requires it).
// Turing without GSP is what nouveau did before GSP; the copy engines work without firmware (GR would need NVIDIA's signed firmware).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBar.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMmu.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/Optional.hpp"

namespace hardware {
namespace nv {

// The device topology (PTOP, 0x022700: gk104_top_oneinit), which says what engines exist and which runlist feeds each.
struct TopEngine {
  uint32_t type = 0xFFFFFFFFu;   // 0 GR, 1..3 CE0..CE2 (Kepler/Maxwell), 0x13 LCE (Pascal+), 8 MSPDEC, 0x0e NVENC, 0x10 NVDEC ...
  uint8_t instance = 0;
  int8_t runlist = -1;
  int8_t reset = -1;             // the PMC_ENABLE bit
  int8_t intr = -1;
  int8_t fault = -1;
  uint32_t addr = 0;             // the engine's BAR0 base
};

struct Topology {
  static constexpr uint32_t kMaxEngines = 32;
  TopEngine engines[kMaxEngines];
  uint32_t count = 0;
  bool truncated = false;
};

constexpr uint32_t kTopTypeGr = 0x00, kTopTypeCe0 = 0x01, kTopTypeCe2 = 0x03, kTopTypeLce = 0x13;

[[nodiscard]] bool read_topology(INvRegisters& regs, Topology* out);
// The `index`th copy engine that has a runlist.
Optional<TopEngine> find_copy_engine(const Topology& topology, uint32_t index);

enum class FifoStatus : uint8_t { Ok, Unsupported, NeedsGsp, NoMemory, NoEngine, NotReady, Full, Busy, Timeout, BadArgument };
const char* fifo_status_name(FifoStatus status);

// The push-buffer encoding of Fermi and later (nouveau's PUSH_* macros / NVC0_FIFO_PKHDR_*).
constexpr uint32_t method_incr(uint32_t subc, uint32_t mthd, uint32_t count) { return 0x20000000u | count << 16 | subc << 13 | mthd >> 2; }
constexpr uint32_t method_nonincr(uint32_t subc, uint32_t mthd, uint32_t count) { return 0x60000000u | count << 16 | subc << 13 | mthd >> 2; }
constexpr uint32_t method_immd(uint32_t subc, uint32_t mthd, uint32_t data) { return 0x80000000u | (data & 0x1FFF) << 16 | subc << 13 | mthd >> 2; }

// A fixed-size push-buffer under construction.
class PushBuilder {
 public:
  static constexpr uint32_t kMaxWords = 256;
  void method(uint32_t subc, uint32_t mthd, ArrayView<const uint32_t> data);
  void method1(uint32_t subc, uint32_t mthd, uint32_t value) { const uint32_t v[1] = {value}; method(subc, mthd, ArrayView<const uint32_t>(v)); }
  ArrayView<const uint32_t> words() const { return ArrayView<const uint32_t>(words_).first(count_); }
  bool overflow() const { return overflow_; }
  void clear() { count_ = 0; overflow_ = false; }

 private:
  uint32_t words_[kMaxWords] = {};
  uint32_t count_ = 0;
  bool overflow_ = false;
};

class NvFifo;

class NvChannel {
 public:
  // Copies `words` into the push buffer, adds a GPFIFO entry and kicks the channel. One submitter at a time (a second one gets
  // Busy). Waits (bounded) for the ring to drain when the push buffer has to wrap.
  [[nodiscard]] FifoStatus submit(ArrayView<const uint32_t> words);
  // Waits until the host has fetched every GPFIFO entry (GP_GET == GP_PUT); Timeout if it does not.
  [[nodiscard]] FifoStatus wait_idle();
  uint32_t gp_get();
  uint32_t gp_put() const { return gp_put_; }
  // The kernel's delay (through the register interface), for callers polling memory the channel's engine writes.
  void pause_us(uint32_t us);

  uint32_t id() const { return id_; }
  uint32_t runlist() const { return runlist_; }
  const GpuMemory& instance() const { return inst_; }
  uint64_t gpfifo_va() const { return gpfifo_va_; }
  uint64_t userd_address() const;
  NvVmm& vmm() { return *vmm_; }
  bool active() const { return active_; }

 private:
  friend class NvFifo;
  FifoStatus submit_locked(ArrayView<const uint32_t> words);
  NvFifo* fifo_ = nullptr;
  NvVmm* vmm_ = nullptr;
  uint32_t id_ = 0;
  uint32_t runlist_ = 0;
  GpuMemory inst_, gpfifo_, push_;
  uint64_t gpfifo_va_ = 0, push_va_ = 0;
  uint32_t gp_put_ = 0;
  uint32_t push_put_ = 0;
  bool active_ = false;
  bool busy_ = false;            // atomically: a submit is running
};

class NvFifo {
 public:
  static constexpr uint32_t kMaxChannels = 32;
  static constexpr uint32_t kGpfifoEntries = 512;          // limit2 = 9
  static constexpr uint32_t kPushBytes = 64 * 1024;
  static constexpr uint32_t kUserdStride = 0x200;
  static constexpr uint32_t kUserdGpGet = 0x88, kUserdGpPut = 0x8c;
  static constexpr uint32_t kMaxRunlists = 16;

  // `memory` reaches VRAM (NvVramMemory over NvInstmem). Kepler..Pascal also need `bar` (BAR1's VMM, for USERD) and `bar1` (the CPU's
  // mapping of PCI BAR1).
  NvFifo(INvRegisters& regs, const Chip& chip, INvMemory& memory, NvBar* bar, INvAperture* bar1, INvLock& lock, NvTimeouts timeouts = {});
  ~NvFifo();
  NvFifo(const NvFifo&) = delete;
  NvFifo& operator=(const NvFifo&) = delete;

  [[nodiscard]] FifoStatus init();
  // A channel on `runlist` translating through `vmm` (which must stay alive until the channel is destroyed).
  [[nodiscard]] FifoStatus create_channel(NvVmm& vmm, uint32_t runlist, NvChannel** out);
  // Disables, preempts and unbinds the channel and gives its memory back. Timeout if the preempt did not finish (the channel is
  // still taken off the runlist and unbound, but its memory is kept: the engine may still be using it).
  [[nodiscard]] FifoStatus destroy_channel(NvChannel& channel);

  const Topology& topology() const { return topology_; }
  bool uses_doorbell() const { return chip_.family >= Family::Volta; }
  bool uses_tsg() const { return chip_.family >= Family::Volta; }

 private:
  friend class NvChannel;
  bool poll_clear(uint32_t offset, uint32_t mask, uint32_t timeout_us);
  FifoStatus write_ramfc(NvChannel& ch);
  FifoStatus update_runlist(uint32_t runlist);     // lock held
  FifoStatus preempt(NvChannel& ch);
  bool write_gp_put(NvChannel& ch);
  uint32_t read_gp_get(NvChannel& ch);

  INvRegisters& regs_;
  Chip chip_;
  INvMemory& memory_;
  NvBar* bar_;
  INvAperture* bar1_;
  INvLock& lock_;
  NvTimeouts timeouts_;
  Topology topology_;
  GpuMemory userd_;
  uint64_t userd_bar1_va_ = 0;
  struct RunlistState {
    GpuMemory buffer[2];         // written alternately, so the one the host is reading is never changed
    uint8_t next = 0;
  };
  RunlistState runlists_[kMaxRunlists];
  NvChannel channels_[kMaxChannels];
  bool ready_ = false;
};

}  // namespace nv
}  // namespace hardware
