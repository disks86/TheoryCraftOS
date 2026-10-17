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

namespace hardware {

namespace {
constexpr uint32_t kCap = 0x00, kCc = 0x14, kCsts = 0x1C, kAqa = 0x24, kAsq = 0x28, kAcq = 0x30, kDoorbells = 0x1000;
constexpr uint32_t kCcEnable = 1u;
constexpr uint32_t kCcIoSqEntrySize = 6u << 16, kCcIoCqEntrySize = 4u << 20;  // 64-byte and 16-byte entries
constexpr uint32_t kCstsReady = 1u, kCstsFatal = 1u << 1;
constexpr uint8_t kAdminCreateIoSq = 0x01, kAdminCreateIoCq = 0x05, kAdminIdentify = 0x06;
constexpr uint8_t kIoFlush = 0x00, kIoWrite = 0x01, kIoRead = 0x02;
constexpr uint32_t kPciCommand = 0x04;
constexpr uint32_t kPage = 4096;
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

bool NvmeStorageDevice::setup_queue(Queue* q, uint16_t id) {
  q->submission = allocate_dma_view<Command>(allocator_, kPage / sizeof(Command), kPage);
  q->completion = allocate_dma_view<Completion>(allocator_, kPage / sizeof(Completion), kPage);
  if (q->submission.empty() || q->completion.empty()) return false;
  __builtin_memset(q->submission.data(), 0, q->submission.size_bytes());
  __builtin_memset(q->completion.data(), 0, q->completion.size_bytes());
  q->tail = q->head = 0;
  q->phase = 1;
  q->id = id;
  return true;
}

bool NvmeStorageDevice::submit(Queue& q, Command& command, uint32_t* result) {
  const uint16_t id = next_command_id_++;
  command.cdw0 = (command.cdw0 & 0xFFFF) | (uint32_t{id} << 16);
  q.submission[q.tail] = command;  // the tail is always below kQueueEntries
  q.tail = (q.tail + 1) % kQueueEntries;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write32(kDoorbells + (2 * q.id) * doorbell_stride_, q.tail);

  volatile Completion* c = &q.completion[q.head];
  if (!wait_for(st_, timeout_ms_, [&] { return (c->status & 1) == q.phase; })) return false;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint16_t status = c->status >> 1;
  if (result) *result = c->result;
  q.head = (q.head + 1) % kQueueEntries;
  if (q.head == 0) q.phase ^= 1;
  write32(kDoorbells + (2 * q.id + 1) * doorbell_stride_, q.head);
  return status == 0 && c->command_id == id;
}

bool NvmeStorageDevice::identify() {
  Command cmd = {};
  cmd.cdw0 = kAdminIdentify;
  cmd.prp1 = reinterpret_cast<uint64_t>(scratch_.data());

  cmd.cdw10 = 1;  // the controller
  if (!submit(admin_, cmd)) return false;
  copy_device_string(ArrayView<char>(info_.serial), ConstByteView(scratch_).subview(4, 20), false);
  copy_device_string(ArrayView<char>(info_.model), ConstByteView(scratch_).subview(24, 40), false);

  // First active namespace (falls back to 1 on controllers without the list command).
  cmd = {};
  cmd.cdw0 = kAdminIdentify;
  cmd.prp1 = reinterpret_cast<uint64_t>(scratch_.data());
  cmd.cdw10 = 2;
  scratch_.first(kPage).fill(0);
  if (submit(admin_, cmd)) {
    ByteReader list(scratch_);
    const uint32_t first = list.le32();
    if (first) namespace_id_ = first;
  }

  cmd = {};
  cmd.cdw0 = kAdminIdentify;
  cmd.nsid = namespace_id_;
  cmd.prp1 = reinterpret_cast<uint64_t>(scratch_.data());
  cmd.cdw10 = 0;
  if (!submit(admin_, cmd)) return false;
  ByteReader ns(scratch_);
  info_.block_count = ns.le64();                     // NSZE
  const uint8_t format = scratch_[26] & 0x0F;        // FLBAS: index of the LBA format in use (0-15)
  const uint8_t data_size_log2 = scratch_[128 + 4 * format + 2];  // LBAF[format].LBADS
  if (data_size_log2 < 9 || data_size_log2 > 12) return false;
  info_.block_size = 1u << data_size_log2;
  return info_.block_count > 0;
}

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
  if (!(cap & (1ull << 37))) return false;                              // NVM command set

  if (!disable_controller()) return false;
  scratch_ = allocate_dma_view<uint8_t>(allocator_, 2 * kPage, kPage);
  if (scratch_.empty() || !setup_queue(&admin_, 0) || !setup_queue(&io_, 1)) return false;
  scratch_.fill(0);

  write32(kAqa, ((kQueueEntries - 1) << 16) | (kQueueEntries - 1));
  write64(kAsq, reinterpret_cast<uint64_t>(admin_.submission.data()));
  write64(kAcq, reinterpret_cast<uint64_t>(admin_.completion.data()));
  // 4 KiB pages (MPS 0), NVM command set, round-robin arbitration, 64/16-byte entries.
  write32(kCc, kCcIoSqEntrySize | kCcIoCqEntrySize | kCcEnable);
  if (!wait_ready(true) || (read32(kCsts) & kCstsFatal)) return false;
  enabled_ = true;

  Command cmd = {};
  cmd.cdw0 = kAdminCreateIoCq;
  cmd.prp1 = reinterpret_cast<uint64_t>(io_.completion.data());
  cmd.cdw10 = ((kQueueEntries - 1) << 16) | 1;
  cmd.cdw11 = 1;  // physically contiguous, interrupts off
  if (!submit(admin_, cmd)) return false;
  cmd = {};
  cmd.cdw0 = kAdminCreateIoSq;
  cmd.prp1 = reinterpret_cast<uint64_t>(io_.submission.data());
  cmd.cdw10 = ((kQueueEntries - 1) << 16) | 1;
  cmd.cdw11 = (1u << 16) | 1;  // completes on CQ 1, physically contiguous
  if (!submit(admin_, cmd)) return false;

  if (!identify() || !prepare_bounce()) return false;
  return true;
}

void NvmeStorageDevice::shutdown() {
  if (registers_ && enabled_) disable_controller();  // the controller stops using our memory
  enabled_ = false;
  release_view(allocator_, admin_.submission);
  release_view(allocator_, admin_.completion);
  release_view(allocator_, io_.submission);
  release_view(allocator_, io_.completion);
  release_view(allocator_, scratch_);
  admin_ = Queue{};
  io_ = Queue{};
  release_bounce();
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

bool NvmeStorageDevice::io(uint8_t opcode, uint64_t lba, uint32_t blocks, ByteView dma) {
  Command cmd = {};
  cmd.cdw0 = opcode;
  cmd.nsid = namespace_id_;
  if (opcode != kIoFlush) {
    const uint64_t bytes = dma.size();
    const uint64_t data = reinterpret_cast<uint64_t>(dma.data());
    cmd.prp1 = data;
    if (bytes > kPage) {
      if (bytes <= 2 * kPage) {
        cmd.prp2 = data + kPage;
      } else {  // a list of the remaining pages
        const ArrayView<uint64_t> list = scratch_.subview(kPage).cast<uint64_t>();  // 512 entries
        const uint32_t pages = static_cast<uint32_t>((bytes + kPage - 1) / kPage);
        for (uint32_t i = 1; i < pages; ++i) list[i - 1] = data + uint64_t{i} * kPage;  // checked against the list page
        cmd.prp2 = reinterpret_cast<uint64_t>(list.data());
      }
    }
    cmd.cdw10 = static_cast<uint32_t>(lba);
    cmd.cdw11 = static_cast<uint32_t>(lba >> 32);
    cmd.cdw12 = blocks - 1;  // zero-based count
  }
  return submit(io_, cmd);
}

bool NvmeStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  return enabled_ && io(write ? kIoWrite : kIoRead, lba, blocks, dma);
}

bool NvmeStorageDevice::flush_cache() { return enabled_ && io(kIoFlush, 0, 0, ByteView()); }

}  // namespace hardware
