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

#include "Hardware/StorageDevice/Virtio/VirtioBlockDevice.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/ByteStream.hpp"
#if defined(__x86_64__)
#include "Kernel/Apic.hpp"
#endif

namespace hardware {

namespace {
constexpr uint32_t kFeatureReadOnly = 1u << 5, kFeatureBlockSize = 1u << 6, kFeatureFlush = 1u << 9, kFeatureMultiqueue = 1u << 12,
                   kFeatureDiscard = 1u << 13;
constexpr uint32_t kConfigCapacity = 0, kConfigBlockSize = 20, kConfigNumQueues = 34, kConfigMaxDiscardSectors = 36;
constexpr uint32_t kTypeRead = 0, kTypeWrite = 1, kTypeFlush = 4, kTypeDiscard = 11;
constexpr uint8_t kStatusOk = 0, kStatusPending = 0xFF;
constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kDescriptorsPerSlot = 3;       // header, data, status
constexpr uint32_t kHeaderStride = 32;
constexpr uint64_t kTimeoutUs = 10000000;         // 10 s
constexpr uint64_t kInterruptCheckUs = 20000;
}  // namespace

uint64_t VirtioBlockDevice::read_config64(uint32_t offset) const {
  uint64_t v = 0;
  for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{transport_.read_config8(offset + i)} << (8 * i);
  return v;
}

uint16_t VirtioBlockDevice::read_config16(uint32_t offset) const {
  return static_cast<uint16_t>(transport_.read_config8(offset) | (uint32_t{transport_.read_config8(offset + 1)} << 8));
}

bool VirtioBlockDevice::initialize() {
  transport_.request_features(kFeatureReadOnly | kFeatureBlockSize | kFeatureFlush | kFeatureMultiqueue | kFeatureDiscard);
  if (!transport_.initialize()) {
    transport_.shutdown();
    return false;
  }
  const uint32_t accepted = transport_.accepted_features();
  info_.read_only = accepted & kFeatureReadOnly;
  info_.media = StorageMedia::Virtual;
  has_flush_ = accepted & kFeatureFlush;
  info_.block_size = kSectorBytes;
  if (accepted & kFeatureBlockSize) {
    uint32_t size = 0;
    for (uint32_t i = 0; i < 4; ++i) size |= uint32_t{transport_.read_config8(kConfigBlockSize + i)} << (8 * i);
    if (size >= kSectorBytes && (size & (size - 1)) == 0 && size <= 4096) info_.block_size = size;
  }
  info_.block_count = read_config64(kConfigCapacity) / (info_.block_size / kSectorBytes);
  if (accepted & kFeatureDiscard) {
    for (uint32_t i = 0; i < 4; ++i) max_discard_sectors_ |= uint32_t{transport_.read_config8(kConfigMaxDiscardSectors + i)} << (8 * i);
    info_.discard = max_discard_sectors_ != 0 && !info_.read_only;
  }

  // A queue per online core, as many as the device has.
  uint32_t cores[kMaxQueues];
  uint32_t wanted = 0;
  const uint64_t online = kernel::running() ? kernel::online_core_mask() : 1;
  for (uint32_t c = 0; c < 64 && wanted < kMaxQueues; ++c)
    if (online & (uint64_t{1} << c)) cores[wanted++] = c;
  if (wanted == 0) cores[wanted++] = 0;
  uint32_t offered = (accepted & kFeatureMultiqueue) ? read_config16(kConfigNumQueues) : 1;
  if (offered == 0) offered = 1;
  if (offered > transport_.queue_count()) offered = transport_.queue_count();
  const uint32_t count = wanted < offered ? wanted : offered;

  uint16_t ring_size = 0xFFFF;
  for (uint32_t i = 0; i < count; ++i) {
    Queue& q = queues_[i];
    q.owner = this;
    q.index = static_cast<uint16_t>(i);
    q.core = cores[i];
    if (!transport_.setup_queue(q.index, &q.ring)) break;
    if (q.ring.size() < ring_size) ring_size = q.ring.size();
    queue_count_ = i + 1;
  }
  if (queue_count_ == 0 || ring_size < kDescriptorsPerSlot || !set_up_slots(ring_size)) {
    shutdown();
    return false;
  }
  uint32_t place = 0;
  for (uint32_t c = 0; c < 64; ++c) {
    const bool is_online = online & (uint64_t{1} << c);
    core_queue_[c] = static_cast<uint8_t>((is_online ? place++ : c) % queue_count_);
  }
  const bool interrupts = enable_interrupts();
  if (!transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  __builtin_memcpy(info_.model, "virtio block device", 20);
  if (interrupts) {
    interrupts_ = check_interrupts();
    if (!interrupts_) disable_interrupts();
  }
  info_.queue_depth = slot_count_;
  return info_.block_count > 0;
}

bool VirtioBlockDevice::set_up_slots(uint16_t ring_size) {
  slot_count_ = ring_size / kDescriptorsPerSlot;
  if (slot_count_ > SlotPool::kMaxSlots) slot_count_ = SlotPool::kMaxSlots;
  slot_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{slot_count_} * kSlotBytes, 4096);
  header_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{slot_count_} * kHeaderStride, 4096);
  if (slot_memory_.empty() || header_memory_.empty()) return false;
  header_memory_.fill(0);
  for (uint32_t i = 0; i < slot_count_; ++i) {
    Slot& s = slots_[i];
    s.buffer = slot_memory_.subview(uint64_t{i} * kSlotBytes, kSlotBytes);
    s.header = header_memory_.subview(uint64_t{i} * kHeaderStride, sizeof(Request));
    s.status = header_memory_.subview(uint64_t{i} * kHeaderStride + sizeof(Request), 1);
    s.completion.lost = false;
    s.completion.arm();
  }
  pool_.reset(slot_count_);
  return true;
}

bool VirtioBlockDevice::enable_interrupts() {
#if defined(__x86_64__)
  if (!kernel::running()) return false;
  uint8_t vectors[kMaxQueues];
  uint32_t apic_ids[kMaxQueues];
  bool ok = true;
  for (uint32_t i = 0; i < queue_count_ && ok; ++i) {
    Queue& q = queues_[i];
    const uint64_t apic = kernel::core_hardware_id(q.core);
    q.vector = claim_storage_vector();
    if (q.vector == 0 || apic > 0xFF) {
      ok = false;
      break;
    }
    q.io_device = kernel::register_io_device("virtio-blk", static_cast<int32_t>(q.core));
    if (!kernel::set_device_interrupt_handler(q.vector, q.io_device, &VirtioBlockDevice::on_queue_interrupt, &q))
      kernel::set_interrupt_handler(q.vector, &VirtioBlockDevice::on_queue_interrupt, &q);
    vectors[i] = q.vector;
    apic_ids[i] = static_cast<uint32_t>(apic);
  }
  ok = ok && transport_.enable_msix_queues(vectors, apic_ids, static_cast<uint16_t>(queue_count_));
  if (!ok) disable_interrupts();
  return ok;
#else
  return false;
#endif
}

bool VirtioBlockDevice::check_interrupts() {
  for (uint32_t i = 0; i < queue_count_; ++i) {
    Queue& q = queues_[i];
    const uint64_t before = __atomic_load_n(&q.interrupts, __ATOMIC_ACQUIRE);
    const uint32_t slot = pool_.acquire();
    const bool read = execute(q, slot, kTypeRead, 0, info_.block_size);
    if (!slots_[slot].completion.lost) pool_.release(slot);
    if (!read) return false;
    const uint64_t start = storage_now_us();
    while (__atomic_load_n(&q.interrupts, __ATOMIC_ACQUIRE) == before) {
      if (storage_now_us() - start > kInterruptCheckUs) return false;
      kernel::cpu_relax();
    }
  }
  return true;
}

void VirtioBlockDevice::disable_interrupts() {
  interrupts_ = false;
  transport_.disable_msix();
#if defined(__x86_64__)
  for (Queue& q : queues_) {
    if (q.vector) {
      kernel::set_device_interrupt_handler(q.vector, q.io_device, nullptr, nullptr);
      kernel::set_interrupt_handler(q.vector, nullptr, nullptr);
      release_storage_vector(q.vector);
      q.vector = 0;
    }
    if (q.io_device >= 0) kernel::unregister_io_device(q.io_device);
    q.io_device = -1;
  }
#endif
}

void VirtioBlockDevice::shutdown() {
  started_ = false;
  transport_.shutdown();  // reset first: the device must stop using our memory
  disable_interrupts();
  for (Queue& q : queues_) q.ring.release(allocator_);
  queue_count_ = 0;
  pool_.close();
  for (Slot& s : slots_) s.buffer = s.header = s.status = ByteView();
  slot_count_ = 0;
  release_view(allocator_, slot_memory_);
  release_view(allocator_, header_memory_);
}

VirtioBlockDevice::Queue& VirtioBlockDevice::queue_for_this_core() {
  const uint32_t core = kernel::current_core();
  const uint32_t index = core < 64 ? core_queue_[core] : core % queue_count_;
  return queues_[index < queue_count_ ? index : 0];
}

void VirtioBlockDevice::reap(Queue& q) {
  uint32_t woken[SlotPool::kMaxSlots];
  uint32_t n = 0;
  const uint64_t flags = kernel::detail::lock_list(&q.lock);
  uint16_t id;
  uint32_t written;
  while (n < SlotPool::kMaxSlots && q.ring.take_used(&id, &written)) {
    if (id % kDescriptorsPerSlot != 0 || id / kDescriptorsPerSlot >= slot_count_) continue;   // (not a request head: the device's error)
    Slot& s = slots_[id / kDescriptorsPerSlot];
    s.completion.record(*reinterpret_cast<volatile uint8_t*>(s.status.data()));   // (the device wrote it before the used entry)
    woken[n++] = id / kDescriptorsPerSlot;
  }
  kernel::detail::unlock_list(&q.lock, flags);
  for (uint32_t i = 0; i < n; ++i) slots_[woken[i]].completion.wake();
}

kernel::Frame* VirtioBlockDevice::on_queue_interrupt(kernel::Frame* frame, void* queue) {
  Queue* q = static_cast<Queue*>(queue);
#if defined(__x86_64__)
  kernel::apic::eoi();
#endif
  __atomic_fetch_add(&q->interrupts, 1, __ATOMIC_ACQ_REL);
  q->owner->reap(*q);
  return frame;
}

bool VirtioBlockDevice::execute(Queue& q, uint32_t slot, uint32_t type, uint64_t sector, uint64_t bytes) {
  return submit(q, slot, type, sector, bytes) && finish(q, slot);
}

bool VirtioBlockDevice::submit(Queue& q, uint32_t slot, uint32_t type, uint64_t sector, uint64_t bytes) {
  if (!started_) return false;
  Slot& s = slots_[slot];
  Request& header = s.header.cast<Request>()[0];
  header.type = type;
  header.reserved = 0;
  header.sector = sector;
  s.status[0] = kStatusPending;
  Virtqueue::Segment chain[3];
  uint16_t n = 0;
  chain[n++] = {s.header.data(), sizeof(Request), false};
  if (bytes) chain[n++] = {s.buffer.data(), static_cast<uint32_t>(bytes), type == kTypeRead};
  chain[n++] = {s.status.data(), 1, true};
  s.completion.arm();

  const uint64_t flags = kernel::detail::lock_list(&q.lock);
  q.ring.post_chain(static_cast<uint16_t>(slot * kDescriptorsPerSlot), chain, n);
  transport_.notify(q.index);
  kernel::detail::unlock_list(&q.lock, flags);
  return true;
}

bool VirtioBlockDevice::finish(Queue& q, uint32_t slot) {
  CommandCompletion& c = slots_[slot].completion;
  if (!c.wait(may_sleep_for_interrupt(interrupts_), kTimeoutUs, [&] { reap(q); })) return false;
  return c.status == kStatusOk;
}

bool VirtioBlockDevice::run(bool write, uint64_t lba, ByteView into, ConstByteView from) {
  const uint64_t bytes = write ? from.size() : into.size();
  if (!started_ || bytes == 0 || bytes % info_.block_size != 0) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  uint64_t count = bytes / info_.block_size;
  if (lba >= info_.block_count || count > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint32_t scale = info_.block_size / kSectorBytes;
  const uint32_t max_blocks = kSlotBytes / info_.block_size;
  const uint64_t chunks = (count + max_blocks - 1) / max_blocks;
  uint32_t slots[kMaxParallelChunks];
  const uint32_t got = acquire_slots(pool_, slots, chunks < kMaxParallelChunks ? static_cast<uint32_t>(chunks) : kMaxParallelChunks);
  Queue& q = queue_for_this_core();
  const uint32_t bs = info_.block_size;
  const bool ok = run_chunks(
      slots, got, count, max_blocks,
      [&](uint32_t slot, uint64_t first, uint32_t n) {
        if (write) slots_[slot].buffer.first(uint64_t{n} * bs).copy_from(from.subview(first * bs, uint64_t{n} * bs));
        return submit(q, slot, write ? kTypeWrite : kTypeRead, (lba + first) * scale, uint64_t{n} * bs);
      },
      [&](uint32_t slot, uint64_t first, uint32_t n) {
        if (!finish(q, slot)) return false;
        if (!write) into.subview(first * bs, uint64_t{n} * bs).copy_from(slots_[slot].buffer.first(uint64_t{n} * bs));
        __atomic_fetch_add(write ? &stats_.blocks_written : &stats_.blocks_read, n, __ATOMIC_RELAXED);
        return true;
      });
  for (uint32_t i = 0; i < got; ++i)
    if (!slots_[slots[i]].completion.lost) pool_.release(slots[i]);
  __atomic_fetch_add(ok ? (write ? &stats_.writes : &stats_.reads) : &stats_.errors, 1, __ATOMIC_RELAXED);
  return ok;
}

bool VirtioBlockDevice::read(uint64_t lba, ByteView buffer) { return run(false, lba, buffer, ConstByteView()); }

bool VirtioBlockDevice::write(uint64_t lba, ConstByteView buffer) {
  if (info_.read_only) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  return run(true, lba, ByteView(), buffer);
}

// One segment {sector, sectors, flags} a request, at most max_discard_sectors each.
bool VirtioBlockDevice::discard(uint64_t lba, uint64_t blocks) {
  if (!started_ || !info_.discard || blocks == 0 || lba >= info_.block_count || blocks > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint32_t scale = info_.block_size / kSectorBytes;
  uint64_t sector = lba * scale, sectors = blocks * scale;
  const uint32_t limit = max_discard_sectors_ / scale * scale;   // (whole blocks)
  const uint32_t slot = pool_.acquire();
  Queue& q = queue_for_this_core();
  bool ok = limit != 0;
  while (sectors > 0 && ok) {
    const uint32_t n = sectors < limit ? static_cast<uint32_t>(sectors) : limit;
    ByteWriter segment(slots_[slot].buffer.first(16));
    segment.le64(sector);
    segment.le32(n);
    segment.le32(0);   // flags: no unmap guarantee asked for
    ok = submit(q, slot, kTypeDiscard, 0, 16) && finish(q, slot);
    sector += n;
    sectors -= n;
  }
  if (!slots_[slot].completion.lost) pool_.release(slot);
  if (!ok) __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  return ok;
}

bool VirtioBlockDevice::flush() {
  if (info_.read_only || !has_flush_) return true;   // nothing can be dirty / the device has no cache to flush
  if (!started_) return false;
  const uint32_t slot = pool_.acquire();
  const bool ok = execute(queue_for_this_core(), slot, kTypeFlush, 0, 0);
  if (!slots_[slot].completion.lost) pool_.release(slot);
  if (!ok) __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  return ok;
}

StorageStatistics VirtioBlockDevice::statistics() const {
  StorageStatistics s;
  s.reads = __atomic_load_n(&stats_.reads, __ATOMIC_RELAXED);
  s.writes = __atomic_load_n(&stats_.writes, __ATOMIC_RELAXED);
  s.blocks_read = __atomic_load_n(&stats_.blocks_read, __ATOMIC_RELAXED);
  s.blocks_written = __atomic_load_n(&stats_.blocks_written, __ATOMIC_RELAXED);
  s.errors = __atomic_load_n(&stats_.errors, __ATOMIC_RELAXED);
  return s;
}

void VirtioBlockDevice::describe(IPropertySink& sink) const {
  QueueReport report[kMaxQueues];
  for (uint32_t i = 0; i < queue_count_; ++i) report[i] = {queues_[i].core, __atomic_load_n(&queues_[i].interrupts, __ATOMIC_RELAXED)};
  describe_queues(sink, queue_count_, slot_count_, interrupts_, report);
}

}  // namespace hardware
