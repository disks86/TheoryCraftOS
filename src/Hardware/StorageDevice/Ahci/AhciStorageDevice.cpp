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

#include "Hardware/StorageDevice/Ahci/AhciStorageDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"

namespace hardware {

namespace {
// HBA registers.
constexpr uint32_t kCap = 0x00, kGhc = 0x04, kPi = 0x0C;
constexpr uint32_t kGhcReset = 1u, kGhcAhciEnable = 1u << 31;
constexpr uint32_t kCapS64A = 1u << 31;
// Port registers.
constexpr uint32_t kClb = 0x00, kClbu = 0x04, kFb = 0x08, kFbu = 0x0C, kIs = 0x10, kIe = 0x14, kCmd = 0x18,
                   kTfd = 0x20, kSig = 0x24, kSsts = 0x28, kSerr = 0x30, kSact = 0x34, kCi = 0x38;
constexpr uint32_t kCmdStart = 1u, kCmdFisReceive = 1u << 4, kCmdFisRunning = 1u << 14, kCmdListRunning = 1u << 15;
constexpr uint32_t kTfdError = 1u, kTfdDrq = 1u << 3, kTfdBusy = 1u << 7;
constexpr uint32_t kIsTaskFileError = 1u << 30;
constexpr uint32_t kSigAta = 0x00000101, kSigAtapi = 0xEB140101;
// ATA commands.
constexpr uint8_t kReadDmaExt = 0x25, kWriteDmaExt = 0x35, kReadDma = 0xC8, kWriteDma = 0xCA;
constexpr uint8_t kFlushCache = 0xE7, kFlushCacheExt = 0xEA, kIdentify = 0xEC, kPacket = 0xA0;
constexpr uint32_t kPciCommand = 0x04;

// Layout of the per-port page.
constexpr uint32_t kCommandListOffset = 0, kFisOffset = 1024, kTableOffset = 2048;
constexpr uint32_t kTablePacketOffset = 0x40, kTablePrdtOffset = 0x80;

struct CommandHeader {
  uint16_t flags;  // bits 4:0 FIS length in dwords, 5 ATAPI, 6 write
  uint16_t prdt_length;
  uint32_t bytes_transferred;
  uint32_t table_low, table_high;
  uint32_t reserved[4];
};
struct PrdEntry {
  uint32_t low, high, reserved;
  uint32_t byte_count;  // minus one
};
}  // namespace

bool AhciController::initialize() {
  registers_ = pci_.memory_bar(device_.address, 5);
  if (!registers_) return false;
  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));  // memory space + bus master

  volatile uint32_t* hba = reinterpret_cast<volatile uint32_t*>(registers_);
  hba[kGhc / 4] = hba[kGhc / 4] | kGhcAhciEnable;  // interrupts stay off: everything is polled
  hba[kGhc / 4] = hba[kGhc / 4] & ~2u;
  addresses_64_ = hba[kCap / 4] & kCapS64A;
  ports_ = hba[kPi / 4];
  return ports_ != 0;
}

void AhciController::shutdown() {
  if (registers_ && owns_device_) {
    volatile uint32_t* hba = reinterpret_cast<volatile uint32_t*>(registers_);
    hba[kGhc / 4] = hba[kGhc / 4] | kGhcReset;  // the HBA stops using our memory
    wait_for(st_, 1000, [&] { return !(hba[kGhc / 4] & kGhcReset); });
  }
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

void AhciStorageDevice::discover(const SharedPointer<AhciController>& controller, allocators::IAllocator& allocator,
                                 List<UniquePointer<IStorageDevice>>& out) {
  for (uint32_t port = 0; port < 32; ++port) {
    if (!(controller->ports_implemented() & (1u << port))) continue;
    const uint32_t ssts = controller->port_register(port, kSsts);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;  // no device, or link asleep
    const uint32_t signature = controller->port_register(port, kSig);
    if (signature != kSigAta && signature != kSigAtapi) continue;  // port multiplier, enclosure
    UniquePointer<AhciStorageDevice> device = UniquePointer<AhciStorageDevice>::make(allocator, controller, port, allocator);
    if (!device) continue;
    device->atapi_ = signature == kSigAtapi;
    if (!device->initialize()) {
      device->release_resources();
      continue;
    }
    out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
  }
}

bool AhciStorageDevice::stop_port() {
  volatile uint32_t& cmd = controller_->port_register(port_, kCmd);
  cmd = cmd & ~kCmdStart;
  if (!wait_for(st_, 500, [&] { return !(cmd & kCmdListRunning); })) return false;
  cmd = cmd & ~kCmdFisReceive;
  return wait_for(st_, 500, [&] { return !(cmd & kCmdFisRunning); });
}

bool AhciStorageDevice::start_port() {
  if (!stop_port()) return false;
  const uint64_t list = reinterpret_cast<uint64_t>(memory_.subview(kCommandListOffset).data());
  const uint64_t fis = reinterpret_cast<uint64_t>(memory_.subview(kFisOffset).data());
  if (!controller_->addresses_64_bit() && ((list | fis) >> 32)) return false;
  controller_->port_register(port_, kClb) = static_cast<uint32_t>(list);
  controller_->port_register(port_, kClbu) = static_cast<uint32_t>(list >> 32);
  controller_->port_register(port_, kFb) = static_cast<uint32_t>(fis);
  controller_->port_register(port_, kFbu) = static_cast<uint32_t>(fis >> 32);
  controller_->port_register(port_, kSerr) = 0xFFFFFFFF;
  controller_->port_register(port_, kIs) = 0xFFFFFFFF;
  controller_->port_register(port_, kIe) = 0;
  volatile uint32_t& cmd = controller_->port_register(port_, kCmd);
  cmd = cmd | kCmdFisReceive;
  if (!wait_for(st_, 1000, [&] { return !(controller_->port_register(port_, kTfd) & (kTfdBusy | kTfdDrq)); }))
    return false;
  cmd = cmd | kCmdStart;
  return true;
}

// Runs one command through slot 0. `fis` is the host-to-device register FIS;
// `packet` (12 bytes, may be null) turns it into an ATAPI command.
bool AhciStorageDevice::issue(ConstByteView fis, ConstByteView packet, bool write, ByteView data) {
  if (!started_ || fis.size() > 64 || packet.size() > 16) return false;
  const ByteView table = memory_.subview(kTableOffset, kTablePrdtOffset + sizeof(PrdEntry));
  table.fill(0);
  table.copy_from(fis);                                     // the command FIS fills the front of the table
  table.subview(kTablePacketOffset, 16).copy_from(packet);  // ATAPI packet, if any
  uint32_t prdt = 0;
  if (!data.empty()) {
    PrdEntry& prd = table.subview(kTablePrdtOffset, sizeof(PrdEntry)).cast<PrdEntry>()[0];
    prd.low = static_cast<uint32_t>(reinterpret_cast<uint64_t>(data.data()));
    prd.high = static_cast<uint32_t>(reinterpret_cast<uint64_t>(data.data()) >> 32);
    prd.byte_count = static_cast<uint32_t>(data.size() - 1);
    prdt = 1;
  }
  CommandHeader& header = memory_.subview(kCommandListOffset, sizeof(CommandHeader)).cast<CommandHeader>()[0];
  const uint64_t table_address = reinterpret_cast<uint64_t>(table.data());
  header.flags = static_cast<uint16_t>((fis.size() / 4) | (!packet.empty() ? 1u << 5 : 0) | (write ? 1u << 6 : 0));
  header.prdt_length = static_cast<uint16_t>(prdt);
  header.bytes_transferred = 0;
  header.table_low = static_cast<uint32_t>(table_address);
  header.table_high = static_cast<uint32_t>(table_address >> 32);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  controller_->port_register(port_, kIs) = 0xFFFFFFFF;
  controller_->port_register(port_, kCi) = 1;
  volatile uint32_t& ci = controller_->port_register(port_, kCi);
  const bool done = wait_for(st_, 10000, [&] {
    return !(ci & 1) || (controller_->port_register(port_, kIs) & kIsTaskFileError);
  });
  const bool failed = !done || (controller_->port_register(port_, kIs) & kIsTaskFileError) ||
                      (controller_->port_register(port_, kTfd) & kTfdError);
  if (failed) {
    started_ = start_port();  // recover: restart the port so later commands can run
    return false;
  }
  return true;
}

bool AhciStorageDevice::identify_ata() {
  const uint8_t fis[20] = {0x27, 0x80, kIdentify};
  const ByteView identify_data = memory_.subview(3072, 512);  // IDENTIFY DEVICE data, 256 words
  if (!issue(ConstByteView(fis), ConstByteView(), false, identify_data)) return false;
  const ArrayView<uint16_t> id = identify_data.cast<uint16_t>();
  copy_device_string(ArrayView<char>(info_.serial), ConstByteView(identify_data).subview(20, 20), true);  // words 10-19
  copy_device_string(ArrayView<char>(info_.model), ConstByteView(identify_data).subview(54, 40), true);   // words 27-46
  lba48_ = id[83] & (1u << 10);
  info_.block_count = lba48_ ? (uint64_t{id[103]} << 48) | (uint64_t{id[102]} << 32) | (uint64_t{id[101]} << 16) | id[100]
                             : (uint64_t{id[61]} << 16) | id[60];
  info_.block_size = 512;
  if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {  // logical sector longer than 256 words
    const uint32_t words = (uint32_t{id[118]} << 16) | id[117];
    if (words >= 256 && words <= 2048) info_.block_size = words * 2;
  }
  info_.removable = id[0] & (1u << 7);
  return info_.block_count > 0;
}

bool AhciStorageDevice::atapi_command(ConstByteView cdb, ByteView data) {
  uint8_t fis[20] = {0x27, 0x80, kPacket, 1 /* DMA */};
  fis[7] = 0xA0;
  fis[5] = 0xFE;  // byte count limit, unused with DMA
  fis[6] = 0xFF;
  return issue(ConstByteView(fis), cdb, false, data);
}

bool AhciStorageDevice::identify_atapi() {
  info_.kind = StorageKind::Optical;
  info_.read_only = true;
  info_.removable = true;
  const ByteView buffer = memory_.subview(3072, 512);
  const uint8_t inquiry[12] = {scsi::kInquiry, 0, 0, 0, 36};
  if (!atapi_command(ConstByteView(inquiry), buffer.first(36))) return false;
  scsi::InquiryInfo unit;
  if (!scsi::parse_inquiry(buffer, &unit)) return false;
  copy_device_string(ArrayView<char>(info_.model), unit.vendor_and_product, false);
  for (int attempt = 0; attempt < 3; ++attempt) {  // the first command after attaching may report a media change
    const uint8_t capacity[12] = {scsi::kReadCapacity10};
    uint64_t last = 0;
    if (atapi_command(ConstByteView(capacity), buffer.first(8)) &&
        scsi::parse_capacity10(buffer, &last, &info_.block_size)) {
      info_.block_count = last + 1;
      return info_.block_size >= 512 && info_.block_size <= 4096;
    }
  }
  return false;  // no medium
}

bool AhciStorageDevice::initialize() {
  memory_ = allocate_view<uint8_t>(allocator_, 4096, 4096);
  if (memory_.empty()) return false;
  memory_.fill(0);
  started_ = start_port();
  if (!started_) return false;
  if (!(atapi_ ? identify_atapi() : identify_ata())) return false;
  return prepare_bounce();
}

void AhciStorageDevice::shutdown() {
  release_resources();
  controller_->shutdown();
}

void AhciStorageDevice::release_resources() {
  if (!memory_.empty() && started_) stop_port();
  started_ = false;
  release_bounce();
  release_view(allocator_, memory_);
}

bool AhciStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  if (atapi_) {
    if (write) return false;
    uint8_t cdb[16];
    // A CD-ROM is read with READ(10) in a 12-byte packet; 16-byte commands do not fit.
    if (scsi::build_read_write(ByteView(cdb), false, lba, blocks) != 10) return false;
    return atapi_command(ConstByteView(cdb).first(12), dma);
  }
  uint8_t fis[20] = {0x27, 0x80};
  fis[2] = lba48_ ? (write ? kWriteDmaExt : kReadDmaExt) : (write ? kWriteDma : kReadDma);
  fis[7] = 0x40;  // LBA mode
  fis[4] = static_cast<uint8_t>(lba);
  fis[5] = static_cast<uint8_t>(lba >> 8);
  fis[6] = static_cast<uint8_t>(lba >> 16);
  if (lba48_) {
    fis[8] = static_cast<uint8_t>(lba >> 24);
    fis[9] = static_cast<uint8_t>(lba >> 32);
    fis[10] = static_cast<uint8_t>(lba >> 40);
  } else {
    fis[7] |= (lba >> 24) & 0x0F;
  }
  fis[12] = static_cast<uint8_t>(blocks);
  fis[13] = static_cast<uint8_t>(blocks >> 8);
  return issue(ConstByteView(fis), ConstByteView(), write, dma);
}

bool AhciStorageDevice::flush_cache() {
  if (atapi_) return true;
  uint8_t fis[20] = {0x27, 0x80, lba48_ ? kFlushCacheExt : kFlushCache};
  fis[7] = 0x40;
  return issue(ConstByteView(fis), ConstByteView(), false, ByteView());
}

}  // namespace hardware
