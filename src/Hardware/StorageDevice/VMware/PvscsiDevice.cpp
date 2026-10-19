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

#include "Hardware/StorageDevice/VMware/PvscsiDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"

namespace hardware {

namespace {
constexpr uint32_t kRegCommand = 0x0, kRegCommandData = 0x4, kRegIntrMask = 0x2010, kRegKickNonRw = 0x3014, kRegKickRw = 0x4018;
constexpr uint32_t kCmdAdapterReset = 1, kCmdSetupRings = 3;
constexpr uint32_t kFlagDirNone = 1u << 2, kFlagDirToHost = 1u << 3, kFlagDirToDevice = 1u << 4;
constexpr uint32_t kRequestBytes = 128, kCompletionBytes = 32, kRequestEntries = 32, kCompletionEntries = 128;
constexpr uint32_t kStateOffset = 0, kRequestOffset = 4096, kCompletionOffset = 8192, kSenseOffset = 12288;
constexpr uint32_t kMaxProbedTargets = 16;
constexpr uint32_t kSenseBytes = 32;
constexpr uint16_t kHostDataUnderrun = 0x12;

uint32_t rd32(const uint8_t* p) { return *reinterpret_cast<const volatile uint32_t*>(p); }
void wr32(uint8_t* p, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(p) = v; }
void wr64(uint8_t* p, uint64_t v) { wr32(p, static_cast<uint32_t>(v)); wr32(p + 4, static_cast<uint32_t>(v >> 32)); }
}  // namespace

uint32_t PvscsiController::reg_read(uint32_t o) const { return *reinterpret_cast<volatile uint32_t*>(registers_ + o); }
void PvscsiController::reg_write(uint32_t o, uint32_t v) const { *reinterpret_cast<volatile uint32_t*>(registers_ + o) = v; }

// A command is its number written to the command register, then its parameters, word by word, to the data register.
void PvscsiController::write_command(uint32_t command, const uint32_t* data, uint32_t words) const {
  reg_write(kRegCommand, command);
  for (uint32_t i = 0; i < words; ++i) reg_write(kRegCommandData, data[i]);
}

bool PvscsiController::initialize() {
  registers_ = pci_.memory_bar(device_.address, 0);
  if (!registers_) return false;
  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  pci_.write32(device_.address, 0x04, (pci_.read32(device_.address, 0x04) & 0xFFFF) | 0x6);   // memory and bus master

  memory_ = allocate_dma_view<uint8_t>(allocator_, 4 * 4096, 4096);
  if (memory_.empty()) {
    shutdown();
    return false;
  }
  memory_.fill(0);
  reg_write(kRegIntrMask, 0);   // polled
  write_command(kCmdAdapterReset, nullptr, 0);

  // SETUP_RINGS: ring sizes in pages, the page number of the ring state, then those of the request and completion rings (32 each).
  const uint64_t base = reinterpret_cast<uint64_t>(memory_.data());
  uint32_t words[2 + 2 + 64 + 64] = {};
  words[0] = 1;
  words[1] = 1;
  words[2] = static_cast<uint32_t>((base + kStateOffset) >> 12);
  words[3] = static_cast<uint32_t>((base + kStateOffset) >> 44);
  words[4] = static_cast<uint32_t>((base + kRequestOffset) >> 12);
  words[5] = static_cast<uint32_t>((base + kRequestOffset) >> 44);
  words[4 + 64] = static_cast<uint32_t>((base + kCompletionOffset) >> 12);
  words[5 + 64] = static_cast<uint32_t>((base + kCompletionOffset) >> 44);
  write_command(kCmdSetupRings, words, sizeof words / sizeof words[0]);
  started_ = true;
  return true;
}

void PvscsiController::shutdown() {
  if (started_ || owns_device_) {
    started_ = false;
    if (registers_) write_command(kCmdAdapterReset, nullptr, 0);   // the adapter stops using the rings
  }
  release_view(allocator_, memory_);
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

PvscsiController::Result PvscsiController::command(uint32_t target, uint32_t lun, ConstByteView cdb, ByteView data, bool device_writes) {
  Result result;
  if (!started_ || cdb.size() > 16) return result;
  kernel::ScopedMutex guard(mutex_);
  uint8_t* base = memory_.data();
  uint8_t* state = base + kStateOffset;
  const uint32_t produced = rd32(state + 0);   // reqProdIdx
  const uint32_t consumed = rd32(state + 4);   // reqConsIdx
  if (produced - consumed >= kRequestEntries) return result;   // (the ring is full: cannot be, one command at a time)

  uint8_t* entry = base + kRequestOffset + (produced % kRequestEntries) * kRequestBytes;
  for (uint32_t i = 0; i < kRequestBytes; ++i) entry[i] = 0;
  uint8_t* sense = base + kSenseOffset;
  for (uint32_t i = 0; i < kSenseBytes; ++i) sense[i] = 0;
  const uint64_t context = next_context_++;
  wr64(entry + 0, context);
  wr64(entry + 8, data.size() ? reinterpret_cast<uint64_t>(data.data()) : 0);
  wr64(entry + 16, data.size());
  wr64(entry + 24, reinterpret_cast<uint64_t>(sense));
  wr32(entry + 32, kSenseBytes);
  wr32(entry + 36, data.size() == 0 ? kFlagDirNone : device_writes ? kFlagDirToHost : kFlagDirToDevice);
  for (uint32_t i = 0; i < cdb.size(); ++i) entry[40 + i] = cdb[i];
  entry[56] = static_cast<uint8_t>(cdb.size());
  entry[58] = static_cast<uint8_t>(lun);   // LUN, single-level addressing (lun[1])
  entry[66] = 0;                           // bus
  entry[67] = static_cast<uint8_t>(target);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  wr32(state + 0, produced + 1);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  const uint8_t opcode = cdb.empty() ? 0 : cdb[0];
  const bool read_write = opcode == 0x08 || opcode == 0x0A || opcode == 0x28 || opcode == 0x2A || opcode == 0x88 || opcode == 0x8A;
  reg_write(read_write ? kRegKickRw : kRegKickNonRw, 0);

  // The completion ring: the device advances its producer index; this driver is the only consumer.
  const bool done = wait_for(st_, 10000, [&] { return rd32(state + 12) != rd32(state + 16); });   // cmpProdIdx != cmpConsIdx
  if (!done) return result;
  const uint32_t consumer = rd32(state + 16);
  const uint8_t* c = base + kCompletionOffset + (consumer % kCompletionEntries) * kCompletionBytes;
  uint64_t completed_context = 0;
  completed_context = static_cast<uint64_t>(rd32(c)) | (static_cast<uint64_t>(rd32(c + 4)) << 32);
  result.host_status = *reinterpret_cast<const volatile uint16_t*>(c + 20);
  result.status = static_cast<uint8_t>(*reinterpret_cast<const volatile uint16_t*>(c + 22));
  wr32(state + 16, consumer + 1);
  result.delivered = completed_context == context;
  return result;
}

bool PvscsiDevice::run(ConstByteView cdb, ByteView data, bool device_writes) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const PvscsiController::Result r = controller_->command(target_, 0, cdb, data, device_writes);
    // Host status 0x12 is the adapter noting that the unit sent less data than asked for (a CHECK CONDITION answer, say): the SCSI status says more.
    if (!r.delivered || (r.host_status != 0 && r.host_status != kHostDataUnderrun)) return false;
    if (r.status == scsi::kStatusGood) return true;
    if (r.status != scsi::kStatusCheckCondition) return false;
    // CHECK CONDITION: unit attention after a reset or media change; try again.
  }
  return false;
}

void PvscsiDevice::discover(const SharedPointer<PvscsiController>& controller, allocators::IAllocator& allocator, List<UniquePointer<IStorageDevice>>& out) {
  const uint32_t limit = controller->max_targets() < kMaxProbedTargets ? controller->max_targets() : kMaxProbedTargets;
  for (uint32_t target = 0; target < limit; ++target) {
    UniquePointer<PvscsiDevice> device = UniquePointer<PvscsiDevice>::make(allocator, controller, target, allocator);
    if (!device) continue;
    if (!device->initialize()) {
      device->release_bounce();
      continue;
    }
    out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
  }
}

bool PvscsiDevice::initialize() {
  alignas(16) uint8_t data[64] = {};
  const uint8_t inquiry[6] = {scsi::kInquiry, 0, 0, 0, 36, 0};
  if (!run(ConstByteView(inquiry), ByteView(data).first(36), true)) return false;
  scsi::InquiryInfo unit;
  if (!scsi::parse_inquiry(ConstByteView(data), &unit) || !unit.present) return false;
  if (unit.device_type != scsi::kDeviceTypeDisk && unit.device_type != scsi::kDeviceTypeOptical) return false;
  optical_ = unit.device_type == scsi::kDeviceTypeOptical;
  info_.kind = optical_ ? StorageKind::Optical : StorageKind::Disk;
  info_.read_only = optical_;
  info_.removable = unit.removable;
  copy_device_string(ArrayView<char>(info_.model), unit.vendor_and_product, false);

  const uint8_t capacity10[10] = {scsi::kReadCapacity10};
  uint64_t last = 0;
  if (!run(ConstByteView(capacity10), ByteView(data).first(8), true) ||
      !scsi::parse_capacity10(ConstByteView(data), &last, &info_.block_size))
    return false;
  if (last == 0xFFFFFFFF) {   // more than 2 TiB: READ CAPACITY(16)
    const uint8_t capacity16[16] = {scsi::kServiceActionIn16, scsi::kReadCapacity16Action, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    uint8_t big[32] = {};
    if (!run(ConstByteView(capacity16), ByteView(big), true) || !scsi::parse_capacity16(ConstByteView(big), &last, &info_.block_size))
      return false;
  }
  info_.block_count = last + 1;
  if (info_.block_size < 512 || info_.block_size > 4096) return false;
  return prepare_bounce();
}

void PvscsiDevice::shutdown() {
  release_bounce();
  controller_->shutdown();
}

bool PvscsiDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  uint8_t cdb[16];
  const uint32_t length = scsi::build_read_write(ByteView(cdb), write, lba, blocks);
  return run(ConstByteView(cdb).first(length), dma, !write);
}

bool PvscsiDevice::flush_cache() {
  if (optical_) return true;
  const uint8_t cdb[10] = {scsi::kSynchronizeCache10};
  return run(ConstByteView(cdb), ByteView(), false);
}

}  // namespace hardware
