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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace intel_gt {

namespace {
constexpr uint64_t kGpuReach = uint64_t{1} << 39;       // Gen9 addresses system memory with 39 bits
constexpr uint64_t kGgttReach = uint64_t{1} << 32;      // graphics addresses of the global GTT that fit a context descriptor and the ring registers

// Commands of the video ring.
constexpr uint32_t kMiNoop = 0;
constexpr uint32_t kMiUserInterrupt = 0x02u << 23;
constexpr uint32_t kMiArbCheck = 0x05u << 23;
constexpr uint32_t kMiArbOnOff = 0x08u << 23;
constexpr uint32_t kMiArbEnable = 1;
constexpr uint32_t kMiBatchBufferEnd = 0x0Au << 23;
constexpr uint32_t kMiBatchBufferStartGen8 = (0x31u << 23) | 1;
constexpr uint32_t kBatchInPpgtt = 1u << 8;
constexpr uint32_t kMiFlushDw = (0x26u << 23) | 1;
constexpr uint32_t kFlushDwStoreIndex = 1u << 21, kFlushDwInvalidateTlb = 1u << 18, kFlushDwOpStoreDw = 1u << 14, kFlushDwInvalidateBsd = 1u << 7, kFlushDwUseGtt = 1u << 2;
constexpr uint32_t kPphwspScratchAddress = 0x34 * 4;   // LRC_PPHWSP_SCRATCH_ADDR

// Ring bytes of one job: invalidating flush (4), batch start (1 + 3 + 2), marker write (4), interrupt and wait-for-arbitration padding (1 + 1 + 2).
constexpr uint64_t kJobDwords = 4 + 6 + 4 + 4;

// The status page is written by the GPU: read it from memory every time.
uint32_t load(const uint32_t& word) {
  const volatile uint32_t& v = word;
  const uint32_t value = v;
  __asm__ volatile("" ::: "memory");
  return value;
}

// The kernel maps memory 1:1, so a CPU address is the address the GPU uses. This is the one place that turns a pointer into a device address.
uint64_t device_address_of(const void* memory) { return reinterpret_cast<uint64_t>(memory); }

void memory_fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
}  // namespace

bool AllocatorGpuMemory::allocate(uint64_t bytes, GpuBuffer* out) {
  if (bytes == 0 || (bytes & (kPageSize - 1))) return false;
  ByteView memory = allocate_dma_view<uint8_t>(allocator_, bytes, kPageSize);
  if (memory.empty()) return false;
  for (uint8_t& b : memory) b = 0;
  const uint64_t address = device_address_of(memory.data());
  if (address & (kPageSize - 1) || address > kGpuReach || bytes > kGpuReach - address) {
    release_view(allocator_, memory);
    return false;
  }
  out->cpu = memory;
  out->dma = address;
  return true;
}

bool VcsDriver::PageSource::allocate_page(ArrayView<uint64_t>* entries, uint64_t* dma_address) {
  GpuBuffer b;
  if (!memory_.allocate(kPageSize, &b)) return false;
  ArrayView<uint64_t> view;
  if (b.dma & (kPageSize - 1) || b.cpu.size() != kPageSize || !b.cpu.try_cast<uint64_t>(&view)) {
    memory_.free(b.cpu);
    return false;
  }
  *entries = view;
  *dma_address = b.dma;
  return true;
}

void VcsDriver::PageSource::free_page(ArrayView<uint64_t> entries) {
  ByteView bytes;
  if (entries.try_cast<uint8_t>(&bytes)) memory_.free(bytes);
}

bool VcsDriver::allocate(uint64_t bytes, GpuBuffer* out) {
  GpuBuffer b;
  if (!memory_.allocate(bytes, &b)) return false;
  if (b.cpu.size() != bytes || (b.dma & (kPageSize - 1)) || b.dma > kGpuReach || bytes > kGpuReach - b.dma) {
    memory_.free(b.cpu);
    return false;
  }
  *out = b;
  return true;
}

void VcsDriver::fail_init() {
  // Nothing has been given to the engine yet (the port is enabled last), so everything can go back.
  if (ggtt_mapped_) {
    const Ggtt ggtt(config_.ggtt_table, config_.ggtt_first_page, kGgttPagesNeeded);
    for (uint64_t i = 0; i < kGgttPagesNeeded; ++i) ggtt.write_raw(i, saved_entries_[i]);
    ggtt_invalidate(*config_.registers);
    ggtt_mapped_ = false;
  }
  ppgtt_.shutdown();
  GpuBuffer* const all[] = {&batch_, &context_, &status_, &ring_, &scratch_};
  for (GpuBuffer* b : all)
    if (b->valid()) memory_.free(b->cpu);
  batch_ = context_ = status_ = ring_ = scratch_ = GpuBuffer();
  ring_dwords_ = status_dwords_ = context_state_ = batch_dwords_ = ArrayView<uint32_t>();
  state_ = State::Failed;
}

bool VcsDriver::initialize(const Config& config) {
  if (state_ != State::Created) return false;
  config_ = config;
  if (!config.registers || !config.ggtt_table.valid()) { state_ = State::Failed; return false; }
  const uint64_t entries = config.ggtt_table.length() / 8;
  if (config.ggtt_first_page > entries || kGgttPagesNeeded > entries - config.ggtt_first_page ||
      (config.ggtt_first_page + kGgttPagesNeeded) * kPageSize > kGgttReach) {
    state_ = State::Failed;
    return false;
  }
  IRegisterBank& regs = *config.registers;

  if (!Forcewake::acquire(regs, ForcewakeDomain::Gt)) { state_ = State::Failed; return false; }
  if (!Forcewake::acquire(regs, media_domain())) {
    Forcewake::release(regs, ForcewakeDomain::Gt);
    state_ = State::Failed;
    return false;
  }
  auto release_wells = [this, &regs] {
    Forcewake::release(regs, media_domain());
    Forcewake::release(regs, ForcewakeDomain::Gt);
  };

  if (config.generation == Generation::Gen12) program_gen12_pat(regs);
  else program_private_pat(regs, gen9_private_pat());

  if (!allocate(kPageSize, &scratch_) || !allocate(kRingBytes, &ring_) || !allocate(kPageSize, &status_) || !allocate(kLrcPages * kPageSize, &context_) ||
      !allocate(kBatchBytes, &batch_)) {
    fail_init();
    release_wells();
    return false;
  }
  ByteView state_page;
  if (!ring_.cpu.try_cast<uint32_t>(&ring_dwords_) || !status_.cpu.try_cast<uint32_t>(&status_dwords_) || !batch_.cpu.try_cast<uint32_t>(&batch_dwords_) ||
      !context_.cpu.subview(uint64_t{kLrcStatePage} * kPageSize, kPageSize).try_cast<uint32_t>(&context_state_)) {
    fail_init();
    release_wells();
    return false;
  }

  // The global GTT: the driver's eight entries are remembered, point at the scratch page, then the ring, the status page and the context get theirs.
  const Ggtt ggtt(config.ggtt_table, config.ggtt_first_page, kGgttPagesNeeded);
  if (!ggtt.valid()) { fail_init(); release_wells(); return false; }
  for (uint64_t i = 0; i < kGgttPagesNeeded; ++i) ggtt.read(i, &saved_entries_[i]);
  if (!ggtt.clear(0, kGgttPagesNeeded, scratch_.dma)) { fail_init(); release_wells(); return false; }
  ggtt_mapped_ = true;
  bool ok = true;
  for (uint64_t i = 0; i < kRingBytes / kPageSize; ++i) ok = ok && ggtt.map(i, ring_.dma + i * kPageSize);
  ok = ok && ggtt.map(4, status_.dma);
  for (uint64_t i = 0; i < kLrcPages; ++i) ok = ok && ggtt.map(5 + i, context_.dma + i * kPageSize);
  ring_ggtt_ = ggtt.address_of(0);
  status_ggtt_ = ggtt.address_of(4);
  context_ggtt_ = ggtt.address_of(5);
  ggtt_invalidate(regs);
  if (!ok) { fail_init(); release_wells(); return false; }

  // The context's own address space, with the batch buffer in it.
  if (!ppgtt_.initialize(config.generation == Generation::Gen12)) { fail_init(); release_wells(); return false; }
  for (uint64_t i = 0; i < kBatchBytes / kPageSize; ++i)
    if (!ppgtt_.map(kBatchGpuAddress + i * kPageSize, batch_.dma + i * kPageSize)) { fail_init(); release_wells(); return false; }

  // The context image: registers, page directories, ring.
  const uint64_t directories[4] = {ppgtt_.page_directory_dma(0), ppgtt_.page_directory_dma(1), ppgtt_.page_directory_dma(2), ppgtt_.page_directory_dma(3)};
  const bool gen12 = config.generation == Generation::Gen12;
  const bool image = gen12 ? lrc_init_gen12_xcs(context_state_, engine_base(), true) && lrc_set_pml4(context_state_, ppgtt_.root_dma())
                           : lrc_init_gen9_xcs(context_state_, engine_base(), true) && lrc_set_page_directories(context_state_, directories);
  if (!image || !lrc_set_ring(context_state_, static_cast<uint32_t>(ring_ggtt_), static_cast<uint32_t>(kRingBytes), 0, 0)) {
    fail_init();
    release_wells();
    return false;
  }
  ring_tail_ = 0;
  memory_fence();

  // From here the engine knows our status page.
  port_.configure(regs, engine_base(), status_dwords_, static_cast<uint32_t>(status_ggtt_), gen12 ? ExeclistPort::Mode::Elsq : ExeclistPort::Mode::Elsp);
  if (!port_.enable()) { fail_init(); release_wells(); return false; }
  release_wells();
  state_ = State::Ready;
  return true;
}

bool VcsDriver::wedge() {
  // Try to stop the ring so the engine stops reading and writing; whatever the answer, nothing it could touch is ever freed or reused.
  port_.stop(100000);
  ppgtt_.abandon();
  state_ = State::Wedged;
  return false;
}

void VcsDriver::shutdown() {
  if (state_ != State::Ready) return;
  IRegisterBank& regs = *config_.registers;
  if (!Forcewake::acquire(regs, media_domain())) { wedge(); return; }
  if (!port_.stop(100000)) {
    wedge();
    Forcewake::release(regs, media_domain());
    return;
  }
  // Idle. The engine's status page address still names our page: point the whole range at the scratch page, which is never freed (the engine may still
  // write there when it wakes), and free the rest.
  const Ggtt ggtt(config_.ggtt_table, config_.ggtt_first_page, kGgttPagesNeeded);
  ggtt.clear(0, kGgttPagesNeeded, scratch_.dma);
  ggtt_invalidate(regs);
  ggtt_mapped_ = false;
  Forcewake::release(regs, media_domain());
  ppgtt_.shutdown();
  GpuBuffer* const freed[] = {&batch_, &context_, &status_, &ring_};  // not the scratch page
  for (GpuBuffer* b : freed)
    if (b->valid()) memory_.free(b->cpu);
  batch_ = context_ = status_ = ring_ = scratch_ = GpuBuffer();
  ring_dwords_ = status_dwords_ = context_state_ = batch_dwords_ = ArrayView<uint32_t>();
  state_ = State::Created;
}

bool VcsDriver::map_buffer(const GpuBuffer& buffer, uint32_t gpu_address) {
  const uint64_t bytes = buffer.cpu.size();
  if (state_ != State::Ready || !buffer.valid() || (bytes & (kPageSize - 1)) || (buffer.dma & (kPageSize - 1)) || (gpu_address & (kPageSize - 1)) ||
      gpu_address < kFirstBufferGpuAddress || bytes > kGgttReach - gpu_address || buffer.dma > kGpuReach || bytes > kGpuReach - buffer.dma)
    return false;
  const uint64_t pages = bytes / kPageSize;
  for (uint64_t i = 0; i < pages; ++i) {
    if (!ppgtt_.map(gpu_address + i * kPageSize, buffer.dma + i * kPageSize)) {
      for (uint64_t k = 0; k < i; ++k) ppgtt_.unmap(gpu_address + k * kPageSize);
      return false;
    }
  }
  return true;
}

bool VcsDriver::unmap_buffer(const GpuBuffer& buffer, uint32_t gpu_address) {
  const uint64_t bytes = buffer.cpu.size();
  if (state_ != State::Ready || !buffer.valid() || (bytes & (kPageSize - 1)) || (gpu_address & (kPageSize - 1)) || gpu_address < kFirstBufferGpuAddress ||
      bytes > kGgttReach - gpu_address)
    return false;
  for (uint64_t i = 0; i < bytes / kPageSize; ++i) ppgtt_.unmap(gpu_address + i * kPageSize);
  return true;
}

bool VcsDriver::batch_is_acceptable(ArrayView<const uint32_t> commands) {
  if (commands.empty() || commands.size() > kBatchBytes / 4) return false;
  uint64_t i = 0;
  const uint64_t n = commands.size();
  while (i < n) {
    const uint32_t h = commands[i];
    if (h == kMiNoop) { ++i; continue; }
    if (h == kMiBatchBufferEnd) return i + 1 == n;  // the end must be the last dword
    uint64_t length;
    if ((h >> 29) == 3) {
      if (((h >> 27) & 3) != 2) return false;                     // only the MFX pipeline
      length = (h & 0xFF) + 2;
    } else if ((h >> 23) == 0x26) {                               // MI_FLUSH_DW: a flush, no write
      if (((h >> 14) & 3) != 0 || (h & 0x3F) != 2) return false;
      length = 4;
    } else {
      return false;
    }
    if (length > n - i) return false;
    i += length;
  }
  return false;  // no end marker
}

void VcsDriver::write_ring(uint32_t dword) {
  ring_dwords_[write_position_ / 4] = dword;
  write_position_ = (write_position_ + 4) % kRingBytes;
}

VcsDriver::Result VcsDriver::run_batch(ArrayView<const uint32_t> commands) {
  if (state_ != State::Ready || !batch_is_acceptable(commands)) return Result::Rejected;
  IRegisterBank& regs = *config_.registers;
  if (!Forcewake::acquire(regs, media_domain())) return Result::Rejected;

  // The batch.
  for (uint64_t i = 0; i < commands.size(); ++i) batch_dwords_[i] = commands[i];

  // The ring: pad to the end if the job does not fit before it, then the job.
  const uint64_t job_bytes = kJobDwords * 4;
  const uint64_t old_tail = ring_tail_;
  write_position_ = old_tail;
  if (write_position_ + job_bytes > kRingBytes)
    while (write_position_ != 0) write_ring(kMiNoop);
  ++seqno_;
  write_ring((kMiFlushDw + 1) | kFlushDwStoreIndex | kFlushDwOpStoreDw | kFlushDwInvalidateTlb | kFlushDwInvalidateBsd);  // (MI_FLUSH_DW + 1)
  write_ring(kPphwspScratchAddress);
  write_ring(0);
  write_ring(0);
  write_ring(kMiArbOnOff | kMiArbEnable);
  write_ring(kMiBatchBufferStartGen8 | kBatchInPpgtt);
  write_ring(kBatchGpuAddress);
  write_ring(0);
  write_ring(kMiArbOnOff);
  write_ring(kMiNoop);
  write_ring((kMiFlushDw + 1) | kFlushDwOpStoreDw);  // the completion marker: store the sequence number in the status page
  write_ring(static_cast<uint32_t>(status_ggtt_ + uint64_t{kSeqnoDword} * 4) | kFlushDwUseGtt);
  write_ring(0);
  write_ring(seqno_);
  write_ring(kMiUserInterrupt);
  write_ring(kMiArbOnOff | kMiArbEnable);
  write_ring(kMiArbCheck);
  write_ring(kMiNoop);
  const uint64_t new_tail = write_position_;

  lrc_set_ring(context_state_, static_cast<uint32_t>(ring_ggtt_), static_cast<uint32_t>(kRingBytes), static_cast<uint32_t>(old_tail), static_cast<uint32_t>(new_tail));
  memory_fence();
  const uint64_t descriptor = config_.generation == Generation::Gen12
                                  ? lrc_descriptor_gen12(static_cast<uint32_t>(context_ggtt_), kContextId, 0, kVideoDecodeClass)
                                  : lrc_descriptor(static_cast<uint32_t>(context_ggtt_), kContextId);
  port_.submit(descriptor | kDescriptorForceRestore);

  bool marker = false, idle = false;
  for (uint32_t i = 0; i < config_.poll_limit && !(marker && idle); ++i) {
    if (!marker && load(status_dwords_[kSeqnoDword]) == seqno_) marker = true;
    const ExeclistPort::Events e = port_.poll();
    if (e.completed) idle = true;
    if (!(marker && idle)) __builtin_ia32_pause();
  }
  if (!(marker && idle)) {
    wedge();  // while the well is still awake: its registers are only reliable then
    Forcewake::release(regs, media_domain());
    return Result::Timeout;
  }
  ring_tail_ = new_tail;
  Forcewake::release(regs, media_domain());
  return Result::Done;
}

}  // namespace intel_gt
}  // namespace hardware
