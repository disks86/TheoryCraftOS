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

#include "Hardware/StorageDevice/NVMe/NvmeStorageDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/Apic.hpp"
#endif

namespace hardware {

namespace {
constexpr uint32_t kCap = 0x00, kIntms = 0x0C, kCc = 0x14, kCsts = 0x1C, kAqa = 0x24, kAsq = 0x28, kAcq = 0x30, kDoorbells = 0x1000;
constexpr uint32_t kCcEnable = 1u;
constexpr uint32_t kCcIoSqEntrySize = 6u << 16, kCcIoCqEntrySize = 4u << 20;  // 64-byte and 16-byte entries
constexpr uint32_t kCstsReady = 1u, kCstsFatal = 1u << 1;
constexpr uint8_t kAdminCreateIoSq = 0x01, kAdminCreateIoCq = 0x05, kAdminIdentify = 0x06, kAdminSetFeatures = 0x09;
constexpr uint32_t kFeatureNumberOfQueues = 0x07;
constexpr uint8_t kIoFlush = 0x00, kIoWrite = 0x01, kIoRead = 0x02, kIoDatasetManagement = 0x09;
constexpr uint32_t kDsmDeallocate = 1u << 2;
constexpr uint32_t kDsmRangeBytes = 16, kDsmMaxRanges = 256;
constexpr uint32_t kPciCommand = 0x04;
constexpr uint32_t kPage = 4096;
constexpr uint32_t kPrpListBytes = 128;          // per slot: 16 entries, enough for 64 KiB (and 128-byte aligned, so a list never crosses a page)
constexpr uint64_t kInterruptCheckUs = 20000;    // how long start-up waits for a queue's first interrupt

uint64_t physical(const void* p) { return reinterpret_cast<uint64_t>(p); }   // (the kernel's memory is identity mapped)
}  // namespace

uint32_t NvmeStorageDevice::read32(uint32_t reg) const { return *reinterpret_cast<volatile uint32_t*>(registers_ + reg); }
void NvmeStorageDevice::write32(uint32_t reg, uint32_t v) const { *reinterpret_cast<volatile uint32_t*>(registers_ + reg) = v; }
uint64_t NvmeStorageDevice::read64(uint32_t reg) const { return *reinterpret_cast<volatile uint64_t*>(registers_ + reg); }
void NvmeStorageDevice::write64(uint32_t reg, uint64_t v) const { *reinterpret_cast<volatile uint64_t*>(registers_ + reg) = v; }

bool NvmeStorageDevice::wait_ready(bool ready) {
  return wait_for(st_, timeout_ms_, [&] { return ((read32(kCsts) & kCstsReady) != 0) == ready; });
}

bool NvmeStorageDevice::disable_controller() {
  write32(kCc, read32(kCc) & ~kCcEnable);
  return wait_ready(false);
}

// ---- the admin queue: one command at a time, polled ----

bool NvmeStorageDevice::admin(Command& command, uint32_t* result) {
  kernel::ScopedMutex guard(admin_mutex_);
  const uint16_t id = next_admin_id_++;
  command.cdw0 = (command.cdw0 & 0xFFFF) | (uint32_t{id} << 16);
  admin_.submission[admin_.tail] = command;  // the tail is always below kAdminEntries
  admin_.tail = (admin_.tail + 1) % kAdminEntries;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write32(kDoorbells, admin_.tail);

  volatile Completion* c = &admin_.completion[admin_.head];
  if (!wait_for(st_, timeout_ms_, [&] { return (c->status & 1) == admin_.phase; })) return false;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint16_t status = c->status >> 1;
  if (result) *result = c->result;
  admin_.head = (admin_.head + 1) % kAdminEntries;
  if (admin_.head == 0) admin_.phase ^= 1;
  write32(kDoorbells + doorbell_stride_, admin_.head);
  return status == 0 && c->command_id == id;
}

bool NvmeStorageDevice::identify() {
  Command cmd = {};
  cmd.cdw0 = kAdminIdentify;
  cmd.prp1 = physical(scratch_.data());
  cmd.cdw10 = 1;  // the controller
  if (!admin(cmd)) return false;
  copy_device_string(ArrayView<char>(info_.serial), ConstByteView(scratch_).subview(4, 20), false);
  copy_device_string(ArrayView<char>(info_.model), ConstByteView(scratch_).subview(24, 40), false);
  info_.discard = (scratch_[520] & (1u << 2)) != 0;   // ONCS bit 2: Dataset Management
  const uint8_t mdts = scratch_[77];   // the largest transfer, as a power of two of the minimum page size (4 KiB here); 0: no limit
  if (mdts != 0 && mdts < 32 && (uint64_t{kPage} << mdts) < chunk_bytes_) chunk_bytes_ = kPage << mdts;

  // First active namespace (falls back to 1 on controllers without the list command).
  cmd = {};
  cmd.cdw0 = kAdminIdentify;
  cmd.prp1 = physical(scratch_.data());
  cmd.cdw10 = 2;
  scratch_.first(kPage).fill(0);
  if (admin(cmd)) {
    ByteReader list(scratch_);
    const uint32_t first = list.le32();
    if (first) namespace_id_ = first;
  }

  cmd = {};
  cmd.cdw0 = kAdminIdentify;
  cmd.nsid = namespace_id_;
  cmd.prp1 = physical(scratch_.data());
  cmd.cdw10 = 0;
  if (!admin(cmd)) return false;
  ByteReader ns(scratch_);
  info_.block_count = ns.le64();                     // NSZE
  const uint8_t format = scratch_[26] & 0x0F;        // FLBAS: index of the LBA format in use (0-15)
  const uint8_t data_size_log2 = scratch_[128 + 4 * format + 2];  // LBAF[format].LBADS
  if (data_size_log2 < 9 || data_size_log2 > 12) return false;
  info_.block_size = 1u << data_size_log2;
  info_.discard_zeroes = info_.discard && (scratch_[33] & 0x7) == 0x1;   // DLFEAT: deallocated blocks read as zeros
  return info_.block_count > 0 && chunk_bytes_ >= info_.block_size;
}

// ---- start-up ----

bool NvmeStorageDevice::initialize() {
  registers_ = pci_.memory_bar(device_.address, 0);
  if (!registers_) return false;
  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));  // memory space + bus master

  const uint64_t cap = read64(kCap);
  const uint32_t min_page_shift = 12 + ((cap >> 48) & 0xF);
  if (min_page_shift > 12) return false;  // controller needs bigger pages than 4 KiB
  doorbell_stride_ = 4u << ((cap >> 32) & 0xF);
  timeout_ms_ = static_cast<uint32_t>(((cap >> 24) & 0xFF) + 1) * 500;  // CAP.TO is in 500 ms units
  max_queue_entries_ = static_cast<uint32_t>(cap & 0xFFFF) + 1;         // CAP.MQES is zero-based
  if (!(cap & (1ull << 37)) || max_queue_entries_ < 2) return false;     // NVM command set

  if (!disable_controller()) return false;
  scratch_ = allocate_dma_view<uint8_t>(allocator_, 2 * kPage, kPage);
  admin_.submission = allocate_dma_view<Command>(allocator_, kPage / sizeof(Command), kPage);
  admin_.completion = allocate_dma_view<Completion>(allocator_, kPage / sizeof(Completion), kPage);
  if (scratch_.empty() || admin_.submission.empty() || admin_.completion.empty()) return false;
  scratch_.fill(0);
  for (Command& c : admin_.submission) c = Command{};
  for (Completion& c : admin_.completion) c = Completion{};
  admin_.tail = admin_.head = 0;
  admin_.phase = 1;

  write32(kAqa, ((kAdminEntries - 1) << 16) | (kAdminEntries - 1));
  write64(kAsq, physical(admin_.submission.data()));
  write64(kAcq, physical(admin_.completion.data()));
  // 4 KiB pages (MPS 0), NVM command set, round-robin arbitration, 64/16-byte entries.
  write32(kCc, kCcIoSqEntrySize | kCcIoCqEntrySize | kCcEnable);
  if (!wait_ready(true) || (read32(kCsts) & kCstsFatal)) return false;
  enabled_ = true;

  if (!identify() || !set_up_slots() || !set_up_queues()) return false;
  info_.media = StorageMedia::Nvme;
  info_.queue_depth = slot_count_;
  return true;
}

// The command slots: as many as fit in a queue at once (so a queue can never overflow, even with every slot on it), up to kMaxSlots.
bool NvmeStorageDevice::set_up_slots() {
  const uint32_t entries = max_queue_entries_ < kMaxQueueEntries ? max_queue_entries_ : kMaxQueueEntries;
  slot_count_ = entries - 1 < kMaxSlots ? entries - 1 : kMaxSlots;
  slot_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{slot_count_} * chunk_bytes_, kPage);
  prp_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{slot_count_} * kPrpListBytes, kPage);
  if (slot_memory_.empty() || prp_memory_.empty()) return false;
  for (uint32_t i = 0; i < slot_count_; ++i) {
    Slot& s = slots_[i];
    s.buffer = slot_memory_.subview(uint64_t{i} * chunk_bytes_, chunk_bytes_);
    s.prp = prp_memory_.subview(uint64_t{i} * kPrpListBytes, kPrpListBytes).cast<uint64_t>();
    s.completion.lost = false;
    s.completion.arm();
  }
  pool_.reset(slot_count_);
  return true;
}

// A queue pair per online core (as many as the controller grants and the MSI-X table holds), each core mapped to one.
bool NvmeStorageDevice::set_up_queues() {
  uint32_t cores[kMaxQueues];
  uint32_t wanted = 0;
  const uint64_t online = kernel::running() ? kernel::online_core_mask() : 1;
  for (uint32_t c = 0; c < 64 && wanted < kMaxQueues; ++c)
    if (online & (uint64_t{1} << c)) cores[wanted++] = c;
  if (wanted == 0) cores[wanted++] = 0;
#if defined(__x86_64__)
  if (kernel::running()) {
    msix_ = UniquePointer<PciMsix>::make(allocator_, pci_, bar_mapper_);
    const uint32_t table = msix_ ? msix_->table_size(device_.address) : 0;   // (entry 0 is the admin queue's)
    if (table >= 2 && table - 1 < wanted) wanted = table - 1;
  }
#endif

  Command cmd = {};
  cmd.cdw0 = kAdminSetFeatures;
  cmd.cdw10 = kFeatureNumberOfQueues;
  cmd.cdw11 = ((wanted - 1) << 16) | (wanted - 1);
  uint32_t granted = 1;
  uint32_t result = 0;
  if (admin(cmd, &result)) {
    const uint32_t submission = (result & 0xFFFF) + 1, completion = (result >> 16) + 1;
    granted = wanted;
    if (submission < granted) granted = submission;
    if (completion < granted) granted = completion;
  }

  const uint32_t entries = max_queue_entries_ < kMaxQueueEntries ? max_queue_entries_ : kMaxQueueEntries;
  for (uint32_t i = 0; i < granted; ++i) {
    IoQueue& q = queues_[i];
    q.owner = this;
    q.id = static_cast<uint16_t>(i + 1);
    q.core = cores[i];
    q.entries = entries;
    q.submission = allocate_dma_view<Command>(allocator_, entries, kPage);
    q.completion = allocate_dma_view<Completion>(allocator_, entries, kPage);
    if (q.submission.empty() || q.completion.empty()) {
      release_view(allocator_, q.submission);
      release_view(allocator_, q.completion);
      granted = i;
      break;
    }
    for (Command& c : q.submission) c = Command{};
    for (Completion& c : q.completion) c = Completion{};
    q.tail = q.head = 0;
    q.phase = 1;
  }
  if (granted == 0) return false;
  queue_count_ = granted;

  const bool interrupts = enable_interrupts();
  for (uint32_t i = 0; i < queue_count_; ++i) {
    if (create_queue(queues_[i], interrupts)) continue;
    if (i == 0) return false;
    for (uint32_t j = i; j < queue_count_; ++j) {   // the controller took fewer than it granted: use those
      release_view(allocator_, queues_[j].submission);
      release_view(allocator_, queues_[j].completion);
    }
    queue_count_ = i;
    break;
  }
  // Each online core submits on the queue of its place among the online cores (wrapping when there are fewer queues).
  uint32_t place = 0;
  for (uint32_t c = 0; c < 64; ++c) {
    const bool is_online = online & (uint64_t{1} << c);
    core_queue_[c] = static_cast<uint8_t>((is_online ? place++ : c) % queue_count_);
  }
  if (interrupts) {
    interrupts_ = check_interrupts();
    if (!interrupts_) disable_interrupts();
  }
  if (!interrupts_) write32(kIntms, 0xFFFFFFFFu);   // (no pin interrupts either: waiters poll)
  return true;
}

bool NvmeStorageDevice::create_queue(IoQueue& q, bool interrupt) {
  Command cmd = {};
  cmd.cdw0 = kAdminCreateIoCq;
  cmd.prp1 = physical(q.completion.data());
  cmd.cdw10 = ((q.entries - 1) << 16) | q.id;
  cmd.cdw11 = (interrupt ? (uint32_t{q.id} << 16) | 2u : 0u) | 1u;   // interrupt vector = MSI-X entry q.id, interrupts on; physically contiguous
  if (!admin(cmd)) return false;
  cmd = {};
  cmd.cdw0 = kAdminCreateIoSq;
  cmd.prp1 = physical(q.submission.data());
  cmd.cdw10 = ((q.entries - 1) << 16) | q.id;
  cmd.cdw11 = (uint32_t{q.id} << 16) | 1u;  // completes on CQ q.id, physically contiguous
  return admin(cmd);
}

// MSI-X: entry 0 (the admin queue's, which is polled) shares queue 1's vector; entry n goes to queue n's core with a vector of its own.
bool NvmeStorageDevice::enable_interrupts() {
#if defined(__x86_64__)
  if (!kernel::running() || !msix_) return false;
  uint8_t vectors[kMaxQueues + 1];
  uint32_t apic_ids[kMaxQueues + 1];
  bool ok = true;
  for (uint32_t i = 0; i < queue_count_ && ok; ++i) {
    IoQueue& q = queues_[i];
    const uint64_t apic = kernel::core_hardware_id(q.core);
    q.vector = claim_storage_vector();
    if (q.vector == 0 || apic > 0xFF) {
      ok = false;
      break;
    }
    q.io_device = kernel::register_io_device("nvme", static_cast<int32_t>(q.core));
    if (!kernel::set_device_interrupt_handler(q.vector, q.io_device, &NvmeStorageDevice::on_queue_interrupt, &q))
      kernel::set_interrupt_handler(q.vector, &NvmeStorageDevice::on_queue_interrupt, &q);
    vectors[i + 1] = q.vector;
    apic_ids[i + 1] = static_cast<uint32_t>(apic);
  }
  if (ok) {
    vectors[0] = vectors[1];
    apic_ids[0] = apic_ids[1];
    ok = msix_->enable(device_.address, vectors, apic_ids, queue_count_ + 1);
  }
  if (!ok) disable_interrupts();
  return ok;
#else
  return false;
#endif
}

// One read on every queue; its interrupt must arrive on the queue's core (on KVM it can land a little after the completion is seen).
bool NvmeStorageDevice::check_interrupts() {
  for (uint32_t i = 0; i < queue_count_; ++i) {
    IoQueue& q = queues_[i];
    const uint64_t before = __atomic_load_n(&q.interrupts, __ATOMIC_ACQUIRE);
    const uint32_t slot = pool_.acquire();
    const bool read = execute(q, slot, kIoRead, 0, 1, info_.block_size);
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

void NvmeStorageDevice::disable_interrupts() {
  interrupts_ = false;
#if defined(__x86_64__)
  if (msix_) msix_->disable();
  for (IoQueue& q : queues_) {
    if (q.vector) {
      kernel::set_device_interrupt_handler(q.vector, q.io_device, nullptr, nullptr);   // (clears the vector either way)
      kernel::set_interrupt_handler(q.vector, nullptr, nullptr);
      release_storage_vector(q.vector);
      q.vector = 0;
    }
    if (q.io_device >= 0) kernel::unregister_io_device(q.io_device);
    q.io_device = -1;
  }
#endif
}

void NvmeStorageDevice::shutdown() {
  if (registers_ && enabled_) disable_controller();  // the controller stops using our memory (and stops interrupting)
  enabled_ = false;
  disable_interrupts();
  msix_ = UniquePointer<PciMsix>();
  release_view(allocator_, admin_.submission);
  release_view(allocator_, admin_.completion);
  release_view(allocator_, scratch_);
  admin_ = AdminQueue{};
  for (IoQueue& q : queues_) {
    release_view(allocator_, q.submission);
    release_view(allocator_, q.completion);
    q.entries = 0;
  }
  queue_count_ = 0;
  pool_.close();
  for (Slot& s : slots_) s.buffer = ByteView(), s.prp = ArrayView<uint64_t>();
  slot_count_ = 0;
  release_view(allocator_, slot_memory_);
  release_view(allocator_, prp_memory_);
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

// ---- requests ----

NvmeStorageDevice::IoQueue& NvmeStorageDevice::queue_for_this_core() {
  const uint32_t core = kernel::current_core();
  const uint32_t index = core < 64 ? core_queue_[core] : core % queue_count_;
  return queues_[index < queue_count_ ? index : 0];
}

// Takes every new completion off the queue, records it in its slot and wakes the slot's waiter (after the queue's lock is let go).
void NvmeStorageDevice::reap(IoQueue& q) {
  uint32_t woken[kMaxQueueEntries];
  uint32_t n = 0;
  const uint64_t flags = kernel::detail::lock_list(&q.lock);
  while (n < kMaxQueueEntries) {
    volatile Completion* c = &q.completion[q.head];
    if ((c->status & 1) != q.phase) break;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint16_t id = c->command_id;
    const uint16_t status = c->status >> 1;
    q.head = (q.head + 1) % q.entries;
    if (q.head == 0) q.phase ^= 1;
    if (id < slot_count_) {
      slots_[id].completion.record(status);
      woken[n++] = id;
    }
  }
  if (n) write32(kDoorbells + (2 * q.id + 1) * doorbell_stride_, q.head);
  kernel::detail::unlock_list(&q.lock, flags);
  for (uint32_t i = 0; i < n; ++i) slots_[woken[i]].completion.wake();
}

kernel::Frame* NvmeStorageDevice::on_queue_interrupt(kernel::Frame* frame, void* queue) {
  IoQueue* q = static_cast<IoQueue*>(queue);
#if defined(__x86_64__)
  kernel::apic::eoi();
#endif
  __atomic_fetch_add(&q->interrupts, 1, __ATOMIC_ACQ_REL);
  q->owner->reap(*q);
  return frame;
}

bool NvmeStorageDevice::execute(IoQueue& q, uint32_t slot, uint8_t opcode, uint64_t lba, uint32_t blocks, uint64_t bytes) {
  submit(q, slot, opcode, lba, blocks, bytes);
  return finish(q, slot);
}

void NvmeStorageDevice::submit(IoQueue& q, uint32_t slot, uint8_t opcode, uint64_t lba, uint32_t blocks, uint64_t bytes) {
  Slot& s = slots_[slot];
  Command cmd = {};
  cmd.cdw0 = opcode | (slot << 16);   // the command id is the slot
  cmd.nsid = namespace_id_;
  if (opcode != kIoFlush) {
    const uint64_t data = physical(s.buffer.data());
    cmd.prp1 = data;
    if (bytes > kPage) {
      if (bytes <= 2 * kPage) {
        cmd.prp2 = data + kPage;
      } else {  // a list of the remaining pages
        const uint32_t pages = static_cast<uint32_t>((bytes + kPage - 1) / kPage);
        for (uint32_t i = 1; i < pages; ++i) s.prp[i - 1] = data + uint64_t{i} * kPage;   // (checked against the slot's list)
        cmd.prp2 = physical(s.prp.data());
      }
    }
    cmd.cdw10 = static_cast<uint32_t>(lba);
    cmd.cdw11 = static_cast<uint32_t>(lba >> 32);
    cmd.cdw12 = blocks - 1;  // zero-based count
  }
  submit_command(q, slot, cmd);
}

void NvmeStorageDevice::submit_command(IoQueue& q, uint32_t slot, Command cmd) {
  Slot& s = slots_[slot];
  cmd.cdw0 = (cmd.cdw0 & 0xFFFF) | (slot << 16);
  cmd.nsid = namespace_id_;
  s.completion.arm();

  const uint64_t flags = kernel::detail::lock_list(&q.lock);
  q.submission[q.tail] = cmd;
  q.tail = (q.tail + 1) % q.entries;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write32(kDoorbells + (2 * q.id) * doorbell_stride_, q.tail);
  kernel::detail::unlock_list(&q.lock, flags);
}

bool NvmeStorageDevice::finish(IoQueue& q, uint32_t slot) {
  CommandCompletion& c = slots_[slot].completion;
  if (!c.wait(may_sleep_for_interrupt(interrupts_), uint64_t{timeout_ms_} * 1000, [&] { reap(q); })) return false;
  return c.status == 0;
}

// Moves a whole request, chunk_bytes_ at a time, several chunks at once when slots are free.
bool NvmeStorageDevice::run(bool write, uint64_t lba, ByteView into, ConstByteView from) {
  const uint64_t bytes = write ? from.size() : into.size();
  if (!enabled_ || slot_count_ == 0 || bytes == 0 || bytes % info_.block_size != 0) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint64_t count = bytes / info_.block_size;
  if (lba >= info_.block_count || count > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint32_t max_blocks = chunk_bytes_ / info_.block_size;
  const uint64_t chunks = (count + max_blocks - 1) / max_blocks;
  uint32_t slots[kMaxParallelChunks];
  const uint32_t got = acquire_slots(pool_, slots, chunks < kMaxParallelChunks ? static_cast<uint32_t>(chunks) : kMaxParallelChunks);
  IoQueue& q = queue_for_this_core();
  const uint32_t bs = info_.block_size;
  const bool ok = run_chunks(
      slots, got, count, max_blocks,
      [&](uint32_t slot, uint64_t first, uint32_t n) {
        if (write) slots_[slot].buffer.first(uint64_t{n} * bs).copy_from(from.subview(first * bs, uint64_t{n} * bs));
        submit(q, slot, write ? kIoWrite : kIoRead, lba + first, n, uint64_t{n} * bs);
        return true;
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

bool NvmeStorageDevice::read(uint64_t lba, ByteView buffer) { return run(false, lba, buffer, ConstByteView()); }

bool NvmeStorageDevice::write(uint64_t lba, ConstByteView buffer) {
  if (info_.read_only) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  return run(true, lba, ByteView(), buffer);
}

// Dataset Management with the deallocate attribute: up to 256 ranges a command, each at most 2^32 - 1 blocks.
bool NvmeStorageDevice::discard(uint64_t lba, uint64_t blocks) {
  if (!enabled_ || !info_.discard || blocks == 0 || lba >= info_.block_count || blocks > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint64_t requested = blocks;
  const uint32_t slot = pool_.acquire();
  IoQueue& q = queue_for_this_core();
  bool ok = true;
  while (blocks > 0 && ok) {
    ByteWriter ranges(slots_[slot].buffer.first(uint64_t{kDsmMaxRanges} * kDsmRangeBytes));
    uint32_t count = 0;
    for (; count < kDsmMaxRanges && blocks > 0; ++count) {
      const uint32_t n = blocks < 0xFFFFFFFFull ? static_cast<uint32_t>(blocks) : 0xFFFFFFFFu;
      ranges.le32(0);       // context attributes
      ranges.le32(n);       // length in blocks
      ranges.le64(lba);     // starting LBA
      lba += n;
      blocks -= n;
    }
    Command cmd = {};
    cmd.cdw0 = kIoDatasetManagement;
    cmd.prp1 = physical(slots_[slot].buffer.data());
    cmd.cdw10 = count - 1;   // zero-based number of ranges
    cmd.cdw11 = kDsmDeallocate;
    submit_command(q, slot, cmd);
    ok = finish(q, slot);
  }
  if (!slots_[slot].completion.lost) pool_.release(slot);
  if (ok) {
    __atomic_fetch_add(&stats_.discards, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&stats_.blocks_discarded, requested, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  }
  return ok;
}

bool NvmeStorageDevice::flush() {
  if (!enabled_ || slot_count_ == 0) return false;
  const uint32_t slot = pool_.acquire();
  const bool ok = execute(queue_for_this_core(), slot, kIoFlush, 0, 0, 0);
  if (!slots_[slot].completion.lost) pool_.release(slot);
  if (!ok) __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  return ok;
}

StorageStatistics NvmeStorageDevice::statistics() const {
  StorageStatistics s;
  s.reads = __atomic_load_n(&stats_.reads, __ATOMIC_RELAXED);
  s.writes = __atomic_load_n(&stats_.writes, __ATOMIC_RELAXED);
  s.blocks_read = __atomic_load_n(&stats_.blocks_read, __ATOMIC_RELAXED);
  s.blocks_written = __atomic_load_n(&stats_.blocks_written, __ATOMIC_RELAXED);
  s.discards = __atomic_load_n(&stats_.discards, __ATOMIC_RELAXED);
  s.blocks_discarded = __atomic_load_n(&stats_.blocks_discarded, __ATOMIC_RELAXED);
  s.errors = __atomic_load_n(&stats_.errors, __ATOMIC_RELAXED);
  return s;
}

uint64_t NvmeStorageDevice::queue_interrupts(uint32_t queue) const {
  return queue < queue_count_ ? __atomic_load_n(&queues_[queue].interrupts, __ATOMIC_RELAXED) : 0;
}

void NvmeStorageDevice::describe(IPropertySink& sink) const {
  QueueReport report[kMaxQueues];
  for (uint32_t i = 0; i < queue_count_; ++i) report[i] = {queues_[i].core, __atomic_load_n(&queues_[i].interrupts, __ATOMIC_RELAXED)};
  describe_queues(sink, queue_count_, slot_count_, interrupts_, report);
  sink.add_number("Largest transfer", chunk_bytes_ / 1024, "KiB");
}

}  // namespace hardware
