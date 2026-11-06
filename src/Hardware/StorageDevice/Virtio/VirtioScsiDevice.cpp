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

#include "Hardware/StorageDevice/Virtio/VirtioScsiDevice.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/Apic.hpp"
#endif

namespace hardware {

namespace {
constexpr uint32_t kConfigNumQueues = 0, kConfigSenseSize = 20, kConfigCdbSize = 24, kConfigMaxTarget = 30;
constexpr uint16_t kControlQueue = 0, kEventQueue = 1, kFirstRequestQueue = 2;
constexpr uint32_t kMaxProbedTargets = 16;
constexpr uint32_t kDescriptorsPerSlot = 3;   // request, data, response
constexpr uint32_t kHeaderStride = 256;       // per slot: request header, then at 128 the response header
constexpr uint64_t kTimeoutUs = 10000000;
constexpr uint64_t kInterruptCheckUs = 20000;
constexpr uint8_t kTestUnitReady = 0x00;
}  // namespace

bool VirtioScsiController::initialize() {
  if (!transport_.initialize()) {
    transport_.shutdown();
    return false;
  }
  auto config32 = [&](uint32_t offset) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v |= uint32_t{transport_.read_config8(offset + i)} << (8 * i);
    return v;
  };
  cdb_size_ = config32(kConfigCdbSize);
  sense_size_ = config32(kConfigSenseSize);
  max_targets_ = static_cast<uint32_t>(transport_.read_config8(kConfigMaxTarget) |
                                       (transport_.read_config8(kConfigMaxTarget + 1) << 8)) + 1;
  if (cdb_size_ != 32 || sense_size_ != 96) {  // the layout used below is the one QEMU (and the spec default) has
    transport_.shutdown();
    return false;
  }
  if (!transport_.setup_queue(kControlQueue, &control_) || !transport_.setup_queue(kEventQueue, &event_)) {
    shutdown();
    return false;
  }
  // A request queue per online core, as many as the device has.
  uint32_t cores[kMaxQueues];
  uint32_t wanted = 0;
  const uint64_t online = kernel::running() ? kernel::online_core_mask() : 1;
  for (uint32_t c = 0; c < 64 && wanted < kMaxQueues; ++c)
    if (online & (uint64_t{1} << c)) cores[wanted++] = c;
  if (wanted == 0) cores[wanted++] = 0;
  uint32_t offered = config32(kConfigNumQueues);
  if (offered == 0) offered = 1;
  const uint32_t available = transport_.queue_count() > kFirstRequestQueue ? transport_.queue_count() - kFirstRequestQueue : 0;
  if (offered > available) offered = available;
  const uint32_t count = wanted < offered ? wanted : offered;
  uint16_t ring_size = 0xFFFF;
  for (uint32_t i = 0; i < count; ++i) {
    Queue& q = queues_[i];
    q.owner = this;
    q.index = static_cast<uint16_t>(kFirstRequestQueue + i);
    q.core = cores[i];
    if (!transport_.setup_queue(q.index, &q.ring)) break;
    if (q.ring.size() < ring_size) ring_size = q.ring.size();
    queue_count_ = i + 1;
  }
  if (queue_count_ == 0 || ring_size < kDescriptorsPerSlot) {
    shutdown();
    return false;
  }
  uint32_t place = 0;
  for (uint32_t c = 0; c < 64; ++c) {
    const bool is_online = online & (uint64_t{1} << c);
    core_queue_[c] = static_cast<uint8_t>((is_online ? place++ : c) % queue_count_);
  }
  slot_count_ = ring_size / kDescriptorsPerSlot;
  if (slot_count_ > SlotPool::kMaxSlots) slot_count_ = SlotPool::kMaxSlots;
  slot_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{slot_count_} * kSlotBytes, 4096);
  header_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{slot_count_} * kHeaderStride, 4096);
  if (slot_memory_.empty() || header_memory_.empty()) {
    shutdown();
    return false;
  }
  header_memory_.fill(0);
  for (uint32_t i = 0; i < slot_count_; ++i) {
    Slot& s = slots_[i];
    s.buffer = slot_memory_.subview(uint64_t{i} * kSlotBytes, kSlotBytes);
    s.request = header_memory_.subview(uint64_t{i} * kHeaderStride, sizeof(RequestHeader));
    s.response = header_memory_.subview(uint64_t{i} * kHeaderStride + 128, sizeof(ResponseHeader));
    s.completion.lost = false;
    s.completion.arm();
  }
  pool_.reset(slot_count_);
  const bool interrupts = enable_interrupts();
  if (!transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  if (interrupts) {
    interrupts_ = check_interrupts();
    if (!interrupts_) disable_interrupts();
  }
  return true;
}

bool VirtioScsiController::enable_interrupts() {
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
    q.io_device = kernel::register_io_device("virtio-scsi", static_cast<int32_t>(q.core));
    if (!kernel::set_device_interrupt_handler(q.vector, q.io_device, &VirtioScsiController::on_queue_interrupt, &q))
      kernel::set_interrupt_handler(q.vector, &VirtioScsiController::on_queue_interrupt, &q);
    vectors[i] = q.vector;
    apic_ids[i] = static_cast<uint32_t>(apic);
  }
  ok = ok && transport_.enable_msix_queues(vectors, apic_ids, static_cast<uint16_t>(queue_count_), kFirstRequestQueue);
  if (!ok) disable_interrupts();
  return ok;
#else
  return false;
#endif
}

// TEST UNIT READY on every queue (to target 0: an absent target still gets an answer); its interrupt must come.
bool VirtioScsiController::check_interrupts() {
  const uint8_t cdb[6] = {kTestUnitReady};
  for (uint32_t i = 0; i < queue_count_; ++i) {
    Queue& q = queues_[i];
    const uint64_t before = __atomic_load_n(&q.interrupts, __ATOMIC_ACQUIRE);
    const uint32_t slot = pool_.acquire();
    const bool sent = submit(q, slot, 0, 0, ConstByteView(cdb), 0, false);
    const Result r = sent ? finish(q, slot) : Result{};
    if (!slots_[slot].completion.lost) pool_.release(slot);
    if (!r.delivered) return false;
    const uint64_t start = storage_now_us();
    while (__atomic_load_n(&q.interrupts, __ATOMIC_ACQUIRE) == before) {
      if (storage_now_us() - start > kInterruptCheckUs) return false;
      kernel::cpu_relax();
    }
  }
  return true;
}

void VirtioScsiController::disable_interrupts() {
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

void VirtioScsiController::shutdown() {
  started_ = false;
  transport_.shutdown();
  disable_interrupts();
  control_.release(allocator_);
  event_.release(allocator_);
  for (Queue& q : queues_) q.ring.release(allocator_);
  queue_count_ = 0;
  pool_.close();
  for (Slot& s : slots_) s.buffer = s.request = s.response = ByteView();
  slot_count_ = 0;
  release_view(allocator_, slot_memory_);
  release_view(allocator_, header_memory_);
}

VirtioScsiController::Queue& VirtioScsiController::queue_for_this_core() {
  const uint32_t core = kernel::current_core();
  const uint32_t index = core < 64 ? core_queue_[core] : core % queue_count_;
  return queues_[index < queue_count_ ? index : 0];
}

void VirtioScsiController::reap(Queue& q) {
  uint32_t woken[SlotPool::kMaxSlots];
  uint32_t n = 0;
  const uint64_t flags = kernel::detail::lock_list(&q.lock);
  uint16_t id;
  uint32_t written;
  while (n < SlotPool::kMaxSlots && q.ring.take_used(&id, &written)) {
    if (id % kDescriptorsPerSlot != 0 || id / kDescriptorsPerSlot >= slot_count_) continue;
    Slot& s = slots_[id / kDescriptorsPerSlot];
    const volatile ResponseHeader* r = reinterpret_cast<const volatile ResponseHeader*>(s.response.data());   // (written before the used entry)
    s.completion.record(static_cast<uint16_t>((uint32_t{r->response} << 8) | r->status));
    woken[n++] = id / kDescriptorsPerSlot;
  }
  kernel::detail::unlock_list(&q.lock, flags);
  for (uint32_t i = 0; i < n; ++i) slots_[woken[i]].completion.wake();
}

kernel::Frame* VirtioScsiController::on_queue_interrupt(kernel::Frame* frame, void* queue) {
  Queue* q = static_cast<Queue*>(queue);
#if defined(__x86_64__)
  kernel::apic::eoi();
#endif
  __atomic_fetch_add(&q->interrupts, 1, __ATOMIC_ACQ_REL);
  q->owner->reap(*q);
  return frame;
}

bool VirtioScsiController::submit(Queue& q, uint32_t slot, uint32_t target, uint32_t lun, ConstByteView cdb, uint64_t bytes,
                                  bool device_writes) {
  if (!started_ || cdb.size() > 32 || bytes > kSlotBytes) return false;
  Slot& s = slots_[slot];
  s.request.fill(0);
  s.response.fill(0xFF);
  RequestHeader& request = s.request.cast<RequestHeader>()[0];
  request.lun[0] = 1;
  request.lun[1] = static_cast<uint8_t>(target);
  request.lun[2] = static_cast<uint8_t>(0x40 | (lun >> 8));
  request.lun[3] = static_cast<uint8_t>(lun);
  request.id = slot;
  ByteView(request.cdb).copy_from(cdb);
  Virtqueue::Segment chain[3];
  uint16_t n = 0;
  const uint32_t length = static_cast<uint32_t>(bytes);
  chain[n++] = {s.request.data(), sizeof(RequestHeader), false};
  if (length && !device_writes) chain[n++] = {s.buffer.data(), length, false};
  chain[n++] = {s.response.data(), sizeof(ResponseHeader), true};
  if (length && device_writes) chain[n++] = {s.buffer.data(), length, true};
  s.completion.arm();
  const uint64_t flags = kernel::detail::lock_list(&q.lock);
  q.ring.post_chain(static_cast<uint16_t>(slot * kDescriptorsPerSlot), chain, n);
  transport_.notify(q.index);
  kernel::detail::unlock_list(&q.lock, flags);
  return true;
}

VirtioScsiController::Result VirtioScsiController::finish(Queue& q, uint32_t slot) {
  Result result;
  CommandCompletion& c = slots_[slot].completion;
  if (!c.wait(may_sleep_for_interrupt(interrupts_), kTimeoutUs, [&] { reap(q); })) return result;
  result.delivered = true;
  result.response = static_cast<uint8_t>(c.status >> 8);
  result.status = static_cast<uint8_t>(c.status);
  return result;
}

VirtioScsiController::Result VirtioScsiController::command(uint32_t target, uint32_t lun, ConstByteView cdb, ByteView data,
                                                           bool device_writes) {
  if (!started_ || data.size() > kSlotBytes) return Result{};
  const uint32_t slot = pool_.acquire();
  Queue& q = queue_for_this_core();
  if (!device_writes && !data.empty()) slots_[slot].buffer.first(data.size()).copy_from(data);
  Result r;
  if (submit(q, slot, target, lun, cdb, data.size(), device_writes)) r = finish(q, slot);
  if (r.delivered && device_writes && !data.empty()) data.copy_from(slots_[slot].buffer.first(data.size()));
  if (!slots_[slot].completion.lost) pool_.release(slot);
  return r;
}

bool VirtioScsiController::transfer(uint32_t target, bool write, uint64_t lba, uint32_t block_size, ByteView into, ConstByteView from) {
  const uint64_t bytes = write ? from.size() : into.size();
  if (!started_ || block_size == 0 || bytes == 0 || bytes % block_size != 0) return false;
  const uint64_t count = bytes / block_size;
  const uint32_t max_blocks = kSlotBytes / block_size;
  const uint64_t chunks = (count + max_blocks - 1) / max_blocks;
  uint32_t slots[kMaxParallelChunks];
  const uint32_t got = acquire_slots(pool_, slots, chunks < kMaxParallelChunks ? static_cast<uint32_t>(chunks) : kMaxParallelChunks);
  Queue& q = queue_for_this_core();
  auto send = [&](uint32_t slot, uint64_t first, uint32_t n) {
    uint8_t cdb[16];
    const uint32_t length = scsi::build_read_write(ByteView(cdb), write, lba + first, n);
    if (write) slots_[slot].buffer.first(uint64_t{n} * block_size).copy_from(from.subview(first * block_size, uint64_t{n} * block_size));
    return submit(q, slot, target, 0, ConstByteView(cdb).first(length), uint64_t{n} * block_size, !write);
  };
  const bool ok = run_chunks(slots, got, count, max_blocks, send, [&](uint32_t slot, uint64_t first, uint32_t n) {
    Result r = finish(q, slot);
    for (int attempt = 0; attempt < 3 && r.delivered && r.response == 0 && r.status == scsi::kStatusCheckCondition; ++attempt)
      r = send(slot, first, n) ? finish(q, slot) : Result{};   // a unit attention (reset, media change): once more
    if (!r.delivered || r.response != 0 || r.status != scsi::kStatusGood) return false;
    if (!write) into.subview(first * block_size, uint64_t{n} * block_size).copy_from(slots_[slot].buffer.first(uint64_t{n} * block_size));
    return true;
  });
  for (uint32_t i = 0; i < got; ++i)
    if (!slots_[slots[i]].completion.lost) pool_.release(slots[i]);
  return ok;
}

void VirtioScsiController::describe(IPropertySink& sink) const {
  QueueReport report[kMaxQueues];
  for (uint32_t i = 0; i < queue_count_; ++i) report[i] = {queues_[i].core, __atomic_load_n(&queues_[i].interrupts, __ATOMIC_RELAXED)};
  describe_queues(sink, queue_count_, slot_count_, interrupts_, report);
}

bool VirtioScsiDevice::run(ConstByteView cdb, ByteView data, bool device_writes) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const VirtioScsiController::Result r = controller_->command(target_, 0, cdb, data, device_writes);
    if (!r.delivered || r.response != 0) return false;
    if (r.status == scsi::kStatusGood) return true;
    if (r.status != scsi::kStatusCheckCondition) return false;  // busy, reservation conflict, ...
    // CHECK CONDITION: unit attention after a reset or media change; try again.
  }
  return false;
}

void VirtioScsiDevice::discover(const SharedPointer<VirtioScsiController>& controller, allocators::IAllocator& allocator,
                                List<UniquePointer<IStorageDevice>>& out) {
  const uint32_t limit = controller->max_targets() < kMaxProbedTargets ? controller->max_targets() : kMaxProbedTargets;
  for (uint32_t target = 0; target < limit; ++target) {
    UniquePointer<VirtioScsiDevice> device = UniquePointer<VirtioScsiDevice>::make(allocator, controller, target, allocator);
    if (!device) continue;
    if (!device->initialize()) continue;
    out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
  }
}

bool VirtioScsiDevice::initialize() {
  alignas(16) uint8_t data[64] = {};
  const uint8_t inquiry[6] = {scsi::kInquiry, 0, 0, 0, 36, 0};
  if (!run(ConstByteView(inquiry), ByteView(data).first(36), true)) return false;
  scsi::InquiryInfo unit;
  if (!scsi::parse_inquiry(ConstByteView(data), &unit) || !unit.present) return false;  // no logical unit here
  if (unit.device_type != scsi::kDeviceTypeDisk && unit.device_type != scsi::kDeviceTypeOptical) return false;
  optical_ = unit.device_type == scsi::kDeviceTypeOptical;
  info_.kind = optical_ ? StorageKind::Optical : StorageKind::Disk;
  info_.read_only = optical_;
  info_.removable = unit.removable;
  info_.media = StorageMedia::Virtual;
  copy_device_string(ArrayView<char>(info_.model), unit.vendor_and_product, false);

  const uint8_t capacity10[10] = {scsi::kReadCapacity10};
  uint64_t last = 0;
  if (!run(ConstByteView(capacity10), ByteView(data).first(8), true) ||  // also fails for an empty drive
      !scsi::parse_capacity10(ConstByteView(data), &last, &info_.block_size))
    return false;
  if (last == 0xFFFFFFFF) {  // more than 2 TiB: needs READ CAPACITY(16)
    const uint8_t capacity16[16] = {scsi::kServiceActionIn16, scsi::kReadCapacity16Action, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    uint8_t big[32] = {};
    if (!run(ConstByteView(capacity16), ByteView(big), true) || !scsi::parse_capacity16(ConstByteView(big), &last, &info_.block_size))
      return false;
  }
  info_.block_count = last + 1;
  if (info_.block_size < 512 || info_.block_size > 4096) return false;
  info_.queue_depth = controller_->depth();
  if (!optical_) {
    uint8_t scratch[64];
    unmap_ = scsi::probe_unmap([this](ConstByteView cdb, ByteView data, bool in) { return run(cdb, data, in); }, ByteView(scratch));
    info_.discard = unmap_.unmap && !info_.read_only;
    info_.discard_zeroes = info_.discard && unmap_.reads_zeros;
    write_same_max_ = scsi::probe_write_same([this](ConstByteView cdb, ByteView data, bool in) { return run(cdb, data, in); }, ByteView(scratch));
    info_.write_zeroes = write_same_max_ != 0 && !info_.read_only;
  }
  return true;
}

// UNMAP, through the SCSI helpers (scsi::unmap_range).
bool VirtioScsiDevice::discard(uint64_t lba, uint64_t blocks) {
  if (!info_.discard || blocks == 0 || lba >= info_.block_count || blocks > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  uint8_t parameters[scsi::kUnmapParameterBytes];
  const bool ok = scsi::unmap_range([this](ConstByteView cdb, ByteView data, bool in) { return run(cdb, data, in); }, unmap_, ByteView(parameters),
                                    lba, blocks);
  if (ok) {
    __atomic_fetch_add(&stats_.discards, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&stats_.blocks_discarded, blocks, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  }
  return ok;
}

// WRITE SAME(16) with a block of zeros (scsi::write_same_zeroes). A unit that turns it down once is not asked again.
bool VirtioScsiDevice::write_zeroes(uint64_t lba, uint64_t blocks) {
  if (!info_.write_zeroes || __atomic_load_n(&write_same_refused_, __ATOMIC_ACQUIRE) || blocks == 0 || lba >= info_.block_count ||
      blocks > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  uint8_t zeros[4096];
  const ByteView zero_block = ByteView(zeros).first(info_.block_size);   // (copied into a slot by the controller)
  const bool ok = scsi::write_same_zeroes([this](ConstByteView cdb, ByteView data, bool in) { return run(cdb, data, in); }, zero_block, write_same_max_, lba, blocks);
  if (ok) {
    __atomic_fetch_add(&stats_.blocks_zeroed, blocks, __ATOMIC_RELAXED);
  } else {
    __atomic_store_n(&write_same_refused_, true, __ATOMIC_RELEASE);
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  }
  return ok;
}

void VirtioScsiDevice::shutdown() { controller_->shutdown(); }

bool VirtioScsiDevice::read(uint64_t lba, ByteView buffer) {
  if (buffer.empty() || buffer.size() % info_.block_size != 0 || lba >= info_.block_count ||
      buffer.size() / info_.block_size > info_.block_count - lba || !controller_->transfer(target_, false, lba, info_.block_size, buffer, ConstByteView())) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  __atomic_fetch_add(&stats_.reads, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats_.blocks_read, buffer.size() / info_.block_size, __ATOMIC_RELAXED);
  return true;
}

bool VirtioScsiDevice::write(uint64_t lba, ConstByteView buffer) {
  if (info_.read_only || buffer.empty() || buffer.size() % info_.block_size != 0 || lba >= info_.block_count ||
      buffer.size() / info_.block_size > info_.block_count - lba || !controller_->transfer(target_, true, lba, info_.block_size, ByteView(), buffer)) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  __atomic_fetch_add(&stats_.writes, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&stats_.blocks_written, buffer.size() / info_.block_size, __ATOMIC_RELAXED);
  return true;
}

bool VirtioScsiDevice::flush() {
  if (optical_) return true;
  const uint8_t cdb[10] = {scsi::kSynchronizeCache10};
  return run(ConstByteView(cdb), ByteView(), false);
}

}  // namespace hardware
