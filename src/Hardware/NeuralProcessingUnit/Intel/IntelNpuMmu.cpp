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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuMmu.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
constexpr uint64_t kPage = 4096;
constexpr uint32_t kQueueLog2 = 4;
constexpr uint32_t kQueueWrapMask = (2u << kQueueLog2) - 1;  // index bits plus the wrap bit
constexpr uint32_t kQueueIndexMask = (1u << kQueueLog2) - 1;
constexpr uint32_t kContextTableLog2 = 8;
constexpr uint32_t kStreamTableLog2 = 2;
constexpr uint32_t kConsValueMask = 0xFFFFFF;
constexpr uint32_t kConsErrorShift = 24, kConsErrorMask = 0x7F;
constexpr uint64_t kQueueBaseRwa = 1ull << 62, kQueueBaseAddress = ((1ull << 52) - 1) & ~uint64_t{0x1F};
constexpr uint64_t kStrtabBaseRa = 1ull << 62, kStrtabBaseAddress = ((1ull << 52) - 1) & ~uint64_t{0x3F};
constexpr uint32_t kStreamId0 = 0, kStreamId3 = 3;

// CR1: table and queue accesses are inner shareable write-back.
constexpr uint32_t kShIsh = 3, kCacheWb = 1;
constexpr uint32_t kCr1 = (kShIsh << 10) | (kCacheWb << 8) | (kCacheWb << 6) | (kShIsh << 4) | (kCacheWb << 2) | kCacheWb;

// Context descriptor (CD) fields, word 0.
constexpr uint64_t kCdT0sz48 = 16, kCdIps48 = 5ull << 32, kCdEpd1 = 1ull << 30, kCdValid = 1ull << 31, kCdAa64 = 1ull << 41, kCdR = 1ull << 45,
                   kCdA = 1ull << 46, kCdAset = 1ull << 47;
constexpr uint64_t kCdTtb0Mask = ((1ull << 52) - 1) & ~uint64_t{0xF};
constexpr uint64_t kCdWord3 = 0x7444;

// Stream table entry (STE) fields.
constexpr uint64_t kSteValid = 1, kSteCfgS1Trans = 5ull << 1, kSteS1CdMax = uint64_t{kContextTableLog2} << 59, kSteCtxPtrMask = ((1ull << 52) - 1) & ~uint64_t{0x3F};
constexpr uint64_t kSteW1 = (2ull << 48) | (2ull << 50) | (uint64_t{kStreamTableLog2} << 13) | (1ull << 19) | (1ull << 27);  // unpriv data, CONT, MEV, S1STALLD

// The sizes of the structures. Each starts on a page so every alignment the MMU wants holds.
constexpr uint64_t kContextTableBytes = uint64_t{IntelNpuMmu::kContextDescriptors} * 64;
constexpr uint64_t kStreamTableBytes = uint64_t{IntelNpuMmu::kStreams} * 64;
constexpr uint64_t kCommandQueueBytes = uint64_t{IntelNpuMmu::kQueueEntries} * IntelNpuMmu::kCommandBytes;
constexpr uint64_t kEventQueueBytes = uint64_t{IntelNpuMmu::kQueueEntries} * IntelNpuMmu::kEventBytes;

// Host DMA memory is identity mapped: the address of an allocation is its bus address (see IntelNpuPageTable).
uint64_t bus_address_of(const void* memory) { return reinterpret_cast<uint64_t>(memory); }

bool queue_full(uint32_t prod, uint32_t cons) {
  return (prod & kQueueIndexMask) == (cons & kQueueIndexMask) && (prod & (1u << kQueueLog2)) != (cons & (1u << kQueueLog2));
}
bool queue_empty(uint32_t prod, uint32_t cons) { return (prod & kQueueWrapMask) == (cons & kQueueWrapMask); }
}  // namespace

const char* intel_npu_mmu_event_name(uint32_t op) {
  switch (op) {
    case 0x01: return "Unsupported Upstream Transaction";
    case 0x02: return "Transaction StreamID out of range";
    case 0x03: return "Fetch of STE caused external abort";
    case 0x04: return "Used STE invalid";
    case 0x05: return "Address Request disallowed for a StreamID";
    case 0x06: return "Transaction marks non-substream disabled";
    case 0x07: return "MMU bypass is disallowed for this StreamID";
    case 0x08: return "Invalid StreamID";
    case 0x09: return "Fetch of CD caused external abort";
    case 0x0A: return "Fetched CD invalid";
    case 0x0B: return "An external abort occurred fetching a TLB";
    case 0x10: return "Translation fault";
    case 0x11: return "Output address caused address size fault";
    case 0x12: return "Access flag fault";
    case 0x13: return "Permission fault occurred on page access";
    case 0x20: return "A TLB conflict";
    case 0x21: return "A configuration cache conflict";
    case 0x24: return "Page request hint from a client device";
    case 0x25: return "Fetch of VMS caused external abort";
    default: return "Unknown event";
  }
}

IntelNpuMmu::~IntelNpuMmu() {
  release_view(allocator_, context_table_);
  release_view(allocator_, stream_table_);
  release_view(allocator_, command_queue_);
  release_view(allocator_, event_queue_);
}

void IntelNpuMmu::flush(const void* memory, uint64_t bytes) {
  if (!snoop_) intel_npu_flush_cache(ConstByteView(static_cast<const uint8_t*>(memory), bytes));
}

bool IntelNpuMmu::poll(uint64_t offset, uint32_t mask, uint32_t expected) {
  for (uint32_t i = 0; i < IntelNpuHw::kPollAttempts; ++i)
    if ((ip_.read32(offset) & mask) == expected) return true;
  return false;
}

bool IntelNpuMmu::allocate() {
  if (allocated_) return true;
  context_table_ = allocate_dma_view<uint64_t>(allocator_, kContextTableBytes / 8, kPage);
  stream_table_ = allocate_dma_view<uint64_t>(allocator_, kStreamTableBytes / 8, kPage);
  command_queue_ = allocate_dma_view<uint64_t>(allocator_, kCommandQueueBytes / 8, kPage);
  event_queue_ = allocate_dma_view<uint32_t>(allocator_, kEventQueueBytes / 4, kPage);
  if (context_table_.empty() || stream_table_.empty() || command_queue_.empty() || event_queue_.empty()) return false;
  context_table_.fill(0);
  stream_table_.fill(0);
  command_queue_.fill(0);
  event_queue_.fill(0);
  allocated_ = true;
  return true;
}

bool IntelNpuMmu::write_cr0(uint32_t value) {
  ip_.write32(kRegCr0, value);
  return poll(kRegCr0Ack, 0xFFFFFFFFu, value);
}

bool IntelNpuMmu::write_irq_ctrl(uint32_t value) {
  ip_.write32(kRegIrqCtrl, value);
  return poll(kRegIrqCtrlAck, 0xFFFFFFFFu, value);
}

bool IntelNpuMmu::queue_command(uint64_t word0, uint64_t word1) {
  if (queue_full(cmdq_prod_, cmdq_cons_)) return false;
  const uint64_t at = uint64_t{cmdq_prod_ & kQueueIndexMask} * (kCommandBytes / 8);
  command_queue_[at] = word0;
  command_queue_[at + 1] = word1;
  cmdq_prod_ = (cmdq_prod_ + 1) & kQueueWrapMask;
  return true;
}

bool IntelNpuMmu::sync() {
  if (!queue_command(kCmdSync, 0)) return false;
  flush(command_queue_.data(), kCommandQueueBytes);
  ip_.write32(kRegCmdqProd, cmdq_prod_);
  if (poll(kRegCmdqCons, kConsValueMask, cmdq_prod_)) {
    cmdq_cons_ = cmdq_prod_;
    command_error_ = 0;
    return true;
  }
  command_error_ = (ip_.read32(kRegCmdqCons) >> kConsErrorShift) & kConsErrorMask;
  return false;
}

bool IntelNpuMmu::cfgi_all_sync() { return queue_command(kCmdCfgiAll, 0x1F) && sync(); }

void IntelNpuMmu::link_stream(uint32_t sid) {
  const uint64_t at = uint64_t{sid} * 8;  // 64 bytes per entry
  const uint64_t w0 = kSteCfgS1Trans | kSteS1CdMax | kSteValid | (bus_address_of(context_table_.data()) & kSteCtxPtrMask);
  stream_table_[at + 1] = kSteW1;
  stream_table_[at] = w0;
  flush(&stream_table_[at], 64);
}

bool IntelNpuMmu::reset() {
  command_queue_.fill(0);
  flush(command_queue_.data(), kCommandQueueBytes);
  cmdq_prod_ = cmdq_cons_ = 0;
  event_queue_.fill(0);
  evtq_prod_ = evtq_cons_ = 0;

  if (!write_cr0(0)) return false;
  ip_.write32(kRegCr1, kCr1);
  ip_.write64(kRegStrtabBase, kStrtabBaseRa | (bus_address_of(stream_table_.data()) & kStrtabBaseAddress));
  ip_.write32(kRegStrtabBaseCfg, kStreamTableLog2);
  ip_.write64(kRegCmdqBase, kQueueBaseRwa | (bus_address_of(command_queue_.data()) & kQueueBaseAddress) | kQueueLog2);
  ip_.write32(kRegCmdqProd, 0);
  ip_.write32(kRegCmdqCons, 0);
  uint32_t cr0 = kCr0CmdqEn;
  if (!write_cr0(cr0)) return false;
  if (!queue_command(kCmdCfgiAll, 0x1F) || !queue_command(kCmdTlbiNsnhAll, 0) || !sync()) return false;

  ip_.write64(kRegEvtqBase, kQueueBaseRwa | (bus_address_of(event_queue_.data()) & kQueueBaseAddress) | kQueueLog2);
  ip_.write32(kRegEvtqProdSec, 0);
  ip_.write32(kRegEvtqConsSec, 0);
  cr0 |= kCr0EvtqEn;
  if (!write_cr0(cr0)) return false;
  cr0 |= kCr0AtsChk;
  if (!write_cr0(cr0)) return false;
  if (!write_irq_ctrl(0) || !write_irq_ctrl(kIrqEvtq | kIrqGerror)) return false;
  cr0 |= kCr0SmmuEn;
  return write_cr0(cr0);
}

bool IntelNpuMmu::enable() {
  if (!allocated_) return false;
  on_ = true;
  if (!reset() || !cfgi_all_sync() ) {
    on_ = false;
    return false;
  }
  if (!queue_command(kCmdTlbiNsnhAll, 0) || !sync()) {
    on_ = false;
    return false;
  }
  return true;
}

bool IntelNpuMmu::init() {
  if (!allocate()) return false;
  link_stream(kStreamId0);
  link_stream(kStreamId3);
  return enable();
}

bool IntelNpuMmu::set_context(uint32_t ssid, uint64_t root) {
  // ivpu allows ssid == 256, one past the table; refused here.
  if (!allocated_ || ssid >= kContextDescriptors) return false;
  if ((root & ~kCdTtb0Mask) != 0 || root == 0) return false;
  const uint64_t at = uint64_t{ssid} * 8;
  uint64_t w0 = kCdT0sz48 | kCdIps48 | (uint64_t{ssid} << 48) | kCdEpd1 | kCdAa64 | kCdR | kCdAset | kCdValid;
  if (ssid == IntelNpuMmuContext::kGlobalSsid || ssid == IntelNpuMmuContext::kReservedSsid) w0 |= kCdA;  // fault loudly on the firmware's own contexts
  context_table_[at + 1] = root & kCdTtb0Mask;
  context_table_[at + 2] = 0;
  context_table_[at + 3] = kCdWord3;
  context_table_[at] = w0;
  flush(&context_table_[at], 64);
  if (!on_) return true;
  if (!cfgi_all_sync()) {
    context_table_[at] = 0;  // do not leave a descriptor the MMU may not have seen
    flush(&context_table_[at], 64);
    return false;
  }
  return true;
}

bool IntelNpuMmu::clear_context(uint32_t ssid) {
  if (!allocated_ || ssid >= kContextDescriptors) return false;
  const uint64_t at = uint64_t{ssid} * 8;
  context_table_[at] &= ~kCdValid;
  flush(&context_table_[at], 64);
  return !on_ || cfgi_all_sync();
}

bool IntelNpuMmu::invalidate_tlb(uint32_t ssid) {
  if (!on_) return true;
  return queue_command(uint64_t{kCmdTlbiNhAsid} | (uint64_t{ssid & 0xFFFF} << 48), 0) && sync();
}

bool IntelNpuMmu::disable_context_events(uint32_t ssid) {
  if (!allocated_ || ssid >= kContextDescriptors) return false;
  const uint64_t at = uint64_t{ssid} * 8;
  context_table_[at] &= ~kCdR;
  flush(&context_table_[at], 64);
  return cfgi_all_sync();
}

bool IntelNpuMmu::next_event(IntelNpuMmuEvent* event) {
  if (!allocated_) return false;
  evtq_prod_ = ip_.read32(kRegEvtqProdSec) & kQueueWrapMask;
  if (queue_empty(evtq_prod_, evtq_cons_)) return false;
  const uint64_t at = uint64_t{evtq_cons_ & kQueueIndexMask} * (kEventBytes / 4);
  uint32_t w[8];
  for (uint32_t i = 0; i < 8; ++i) w[i] = event_queue_[at + i];
  evtq_cons_ = (evtq_cons_ + 1) & kQueueWrapMask;
  ip_.write32(kRegEvtqConsSec, evtq_cons_);
  event->op = w[0] & 0xFF;
  event->ssid = (w[0] >> 12) & 0xFFFFF;
  event->sid = w[1];
  event->word2 = w[2];
  event->word3 = w[3];
  event->in_address = w[4] | (uint64_t{w[5]} << 32);
  event->fetch_address = w[6] | (uint64_t{w[7]} << 32);
  return true;
}

bool IntelNpuMmu::evtq_set(bool enable) {
  uint32_t cr0 = ip_.read32(kRegCr0);
  cr0 = enable ? (cr0 | kCr0EvtqEn) : (cr0 & ~kCr0EvtqEn);
  return write_cr0(cr0);
}

bool IntelNpuMmu::discard_events() {
  if (!allocated_) return false;
  // Stop the MMU from moving the producer, so that the two indexes can be made equal.
  const bool stopped = evtq_set(false);
  evtq_cons_ = ip_.read32(kRegEvtqProdSec) & kQueueWrapMask;
  ip_.write32(kRegEvtqConsSec, evtq_cons_);
  evtq_prod_ = evtq_cons_;
  const bool restarted = evtq_set(true);
  return stopped && restarted;
}

uint32_t IntelNpuMmu::acknowledge_global_errors() {
  const uint32_t gerror = ip_.read32(kRegGerror);
  const uint32_t active = (gerror ^ ip_.read32(kRegGerrorn)) & kGerrorErrors;
  if (active == 0) return 0;
  ip_.write32(kRegGerrorn, gerror);
  return active;
}

// ---- the address space ----

IntelNpuAddressSpace::IntelNpuAddressSpace(allocators::IAllocator& allocator, uint64_t start, uint64_t end) : free_(allocator), start_(start), end_(end) {
  if (start < end) free_.push_back(Range{start, end});
}

uint64_t IntelNpuAddressSpace::free_bytes() const {
  uint64_t total = 0;
  for (const Range& r : free_) total += r.end - r.start;
  return total;
}

bool IntelNpuAddressSpace::allocate_aligned(uint64_t lo, uint64_t hi, uint64_t size, uint64_t alignment, uint64_t* address) {
  uint64_t best = free_.count();
  uint64_t best_start = 0, best_size = UINT64_MAX;
  for (uint64_t i = 0; i < free_.count(); ++i) {
    const Range r = free_[i];
    const uint64_t from = r.start > lo ? r.start : lo;
    const uint64_t to = r.end < hi ? r.end : hi;
    if (from >= to) continue;
    const uint64_t aligned = (from + (alignment - 1)) & ~(alignment - 1);
    if (aligned < from || aligned >= to || size > to - aligned) continue;  // `aligned < from`: the rounding wrapped
    if (r.end - r.start < best_size) {
      best = i;
      best_start = aligned;
      best_size = r.end - r.start;
    }
  }
  if (best == free_.count()) return false;
  const Range r = free_[best];
  const uint64_t used_end = best_start + size;
  // Split the free range around [best_start, used_end).
  if (best_start == r.start && used_end == r.end) {
    free_.remove_at(best);
  } else if (best_start == r.start) {
    free_[best].start = used_end;
  } else if (used_end == r.end) {
    free_[best].end = best_start;
  } else {
    if (!free_.insert(best + 1, Range{used_end, r.end})) return false;  // out of memory: nothing changed
    free_[best].end = best_start;
  }
  *address = best_start;
  return true;
}

bool IntelNpuAddressSpace::allocate(uint64_t lo, uint64_t hi, uint64_t size, uint64_t* address) {
  if (size == 0 || (size & (kPage - 1)) != 0) return false;
  if (size >= IntelNpuPageTable::kContiguousSize && allocate_aligned(lo, hi, size, IntelNpuPageTable::kContiguousSize, address)) return true;
  return allocate_aligned(lo, hi, size, kPage, address);
}

bool IntelNpuAddressSpace::release(uint64_t address, uint64_t size) {
  if (size == 0 || address < start_ || address > end_ || size > end_ - address) return false;
  const uint64_t end = address + size;
  uint64_t at = 0;
  while (at < free_.count() && free_[at].start < address) ++at;
  // Anything already free that overlaps the range means it was not (fully) allocated.
  if (at > 0 && free_[at - 1].end > address) return false;
  if (at < free_.count() && free_[at].start < end) return false;
  const bool join_before = at > 0 && free_[at - 1].end == address;
  const bool join_after = at < free_.count() && free_[at].start == end;
  if (join_before && join_after) {
    free_[at - 1].end = free_[at].end;
    free_.remove_at(at);
  } else if (join_before) {
    free_[at - 1].end = end;
  } else if (join_after) {
    free_[at].start = address;
  } else if (!free_.insert(at, Range{address, end})) {
    return false;
  }
  return true;
}

// ---- a context ----

namespace {
IntelNpuAddressSpace make_space(allocators::IAllocator& allocator, const IntelNpuMemoryMap& map, uint32_t ssid) {
  if (ssid == IntelNpuMmuContext::kGlobalSsid) return IntelNpuAddressSpace(allocator, map.runtime.start, map.shave.end);
  const uint64_t start = map.user.start < map.shave.start ? map.user.start : map.shave.start;
  const uint64_t end = map.user.end > map.dma.end ? map.user.end : map.dma.end;
  return IntelNpuAddressSpace(allocator, start, end);
}
}  // namespace

IntelNpuMmuContext::IntelNpuMmuContext(allocators::IAllocator& allocator, IntelNpuMmu& mmu, const IntelNpuMemoryMap& map, uint32_t ssid, bool snoop)
    : allocator_(allocator), mmu_(mmu), ssid_(ssid), table_(allocator, snoop), space_(make_space(allocator, map, ssid)) {}

IntelNpuMmuContext::~IntelNpuMmuContext() {
  if (descriptor_valid_) mmu_.clear_context(ssid_);
}

bool IntelNpuMmuContext::init_reserved() {
  if (!table_.ensure_root()) return false;
  if (!mmu_.set_context(ssid_, table_.root_bus_address())) return false;
  descriptor_valid_ = true;
  return true;
}

bool IntelNpuMmuContext::map(uint64_t vpu_addr, ArrayView<const IntelNpuDmaExtent> extents, uint64_t size, IntelNpuPageTable::MapFlags flags) {
  if (ssid_ == kReservedSsid || !IntelNpuPageTable::range_ok(vpu_addr, size)) return false;
  uint64_t mapped = 0;
  bool ok = true;
  for (const IntelNpuDmaExtent& e : extents) {
    if (e.length == 0 || e.length > size - mapped || !table_.map(vpu_addr + mapped, e.bus_addr, e.length, flags)) {
      ok = false;
      break;
    }
    mapped += e.length;
  }
  if (ok && mapped != size) ok = false;  // the extents do not cover the buffer
  if (ok && !descriptor_valid_) {
    ok = table_.ensure_root() && mmu_.set_context(ssid_, table_.root_bus_address());
    if (ok) descriptor_valid_ = true;
  }
  if (ok) ok = mmu_.invalidate_tlb(ssid_);
  if (!ok) table_.unmap(vpu_addr, mapped);
  return ok;
}

void IntelNpuMmuContext::unmap(uint64_t vpu_addr, ArrayView<const IntelNpuDmaExtent> extents) {
  uint64_t at = vpu_addr;
  for (const IntelNpuDmaExtent& e : extents) {
    if (e.length == 0 || e.length > IntelNpuPageTable::kAddressLimit) return;
    table_.unmap(at, e.length);
    at += e.length;
  }
  mmu_.invalidate_tlb(ssid_);
}

bool IntelNpuMmuContext::set_read_only(uint64_t vpu_addr, uint64_t size) {
  if (!table_.set_read_only(vpu_addr, size)) return false;
  return mmu_.invalidate_tlb(ssid_);
}

bool intel_npu_map_firmware(IntelNpuMmuContext& global, const IntelNpuFirmware& fw, const IntelNpuFirmwareMemory& memory) {
  if (!fw.valid() || global.ssid() != IntelNpuMmuContext::kGlobalSsid) return false;
  if (memory.boot_params.length != fw.boot_params_size || memory.fw_version.length != fw.fw_version_size || memory.runtime.length != fw.runtime_size) return false;
  const IntelNpuDmaExtent bp[1] = {memory.boot_params}, ver[1] = {memory.fw_version}, rt[1] = {memory.runtime};
  IntelNpuPageTable::MapFlags flags;
  if (!global.map(fw.boot_params_addr, ArrayView<const IntelNpuDmaExtent>(bp), fw.boot_params_size, flags)) return false;
  if (!global.map(fw.fw_version_addr, ArrayView<const IntelNpuDmaExtent>(ver), fw.fw_version_size, flags)) {
    global.unmap(fw.boot_params_addr, ArrayView<const IntelNpuDmaExtent>(bp));
    return false;
  }
  bool ok = global.map(fw.runtime_addr, ArrayView<const IntelNpuDmaExtent>(rt), fw.runtime_size, flags);
  if (ok && fw.read_only_size != 0) ok = global.set_read_only(fw.read_only_addr, fw.read_only_size);
  if (!ok) {
    global.unmap(fw.boot_params_addr, ArrayView<const IntelNpuDmaExtent>(bp));
    global.unmap(fw.fw_version_addr, ArrayView<const IntelNpuDmaExtent>(ver));
    global.unmap(fw.runtime_addr, ArrayView<const IntelNpuDmaExtent>(rt));
  }
  return ok;
}

}  // namespace hardware
