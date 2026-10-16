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

#include "Hardware/StorageDevice/Ide/IdeStorageDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/IoPort.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"

namespace hardware {

namespace {
// Task-file registers (offsets from the command base).
constexpr uint32_t kData = 0, kFeatures = 1, kSectorCount = 2, kLbaLow = 3, kLbaMid = 4, kLbaHigh = 5,
                   kDevice = 6, kCommand = 7, kStatus = 7;
constexpr uint8_t kStatusError = 1, kStatusDrq = 1 << 3, kStatusFault = 1 << 5, kStatusBusy = 1 << 7;
constexpr uint8_t kControlInterruptsOff = 1 << 1;
constexpr uint8_t kReadSectors = 0x20, kReadSectorsExt = 0x24, kWriteSectors = 0x30, kWriteSectorsExt = 0x34;
constexpr uint8_t kFlushCache = 0xE7, kFlushCacheExt = 0xEA, kIdentify = 0xEC, kIdentifyPacket = 0xA1, kPacket = 0xA0;
constexpr uint32_t kPciCommand = 0x04;
constexpr uint32_t kPioSectorsPerCommand = 128;
}  // namespace

// The BAR for a channel when the controller is in native mode, the fixed legacy
// port range otherwise.
uint16_t IdeController::io_base(uint8_t bar_index, uint16_t legacy, bool native) const {
  if (!native) return legacy;
  const uint32_t bar = pci_.read32(device_.address, 0x10 + 4 * bar_index);
  return (bar & 1) ? static_cast<uint16_t>(bar & 0xFFFC) : legacy;
}

bool IdeController::initialize() {
  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | 1u);  // I/O space
  const bool native0 = device_.prog_if & 1, native1 = device_.prog_if & 4;
  channels_[0].command_base = io_base(0, 0x1F0, native0);
  channels_[0].control_base = io_base(1, 0x3F6, native0);
  if (native0) channels_[0].control_base += 2;
  channels_[1].command_base = io_base(2, 0x170, native1);
  channels_[1].control_base = io_base(3, 0x376, native1);
  if (native1) channels_[1].control_base += 2;
  return true;
}

void IdeController::shutdown() {
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

uint8_t IdeStorageDevice::reg_read(uint32_t reg) const { return IoPort(channel_.command_base + reg).read8(); }
void IdeStorageDevice::reg_write(uint32_t reg, uint8_t v) const { IoPort(channel_.command_base + reg).write8(v); }
uint8_t IdeStorageDevice::status() const { return IoPort(channel_.control_base).read8(); }  // alternate status: no side effects

void IdeStorageDevice::select(uint8_t lba_high_bits) const {
  reg_write(kDevice, static_cast<uint8_t>(0xE0 | (drive_ << 4) | (lba_high_bits & 0x0F)));
  for (int i = 0; i < 4; ++i) status();  // 400 ns for the selection to settle
}

bool IdeStorageDevice::wait_not_busy(uint32_t timeout_ms) {
  return wait_for(st_, timeout_ms, [&] { return !(status() & kStatusBusy); });
}

// Waits for the device to be ready to transfer a block of data (or to fail).
bool IdeStorageDevice::wait_data_ready() {
  if (!wait_not_busy(5000)) return false;
  const uint8_t s = status();
  return (s & kStatusDrq) && !(s & (kStatusError | kStatusFault));
}

void IdeStorageDevice::discover(const SharedPointer<IdeController>& controller, allocators::IAllocator& allocator,
                                List<UniquePointer<IStorageDevice>>& out) {
  for (uint32_t channel = 0; channel < 2; ++channel) {
    if (!controller->channel(channel).command_base) continue;
    for (uint32_t drive = 0; drive < 2; ++drive) {
      UniquePointer<IdeStorageDevice> device = UniquePointer<IdeStorageDevice>::make(allocator, controller, channel, drive, allocator);
      if (!device) continue;
      if (!device->initialize()) {
        device->release_resources();
        continue;
      }
      out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
    }
  }
}

bool IdeStorageDevice::identify(uint8_t command, uint16_t* data) {
  reg_write(kSectorCount, 0);
  reg_write(kLbaLow, 0);
  reg_write(kLbaMid, 0);
  reg_write(kLbaHigh, 0);
  reg_write(kCommand, command);
  for (int i = 0; i < 4; ++i) status();
  if (status() == 0) return false;  // nothing there
  if (!wait_not_busy(2000)) return false;
  if (!wait_data_ready()) return false;
  const IoPort port(channel_.command_base + kData);
  for (int i = 0; i < 256; ++i) data[i] = port.read16();
  return true;
}

bool IdeStorageDevice::initialize() {
  IoPort(channel_.control_base).write8(kControlInterruptsOff);
  select(0);
  if (reg_read(kStatus) == 0xFF) return false;  // floating bus: no controller channel
  // Signature after selection: ATAPI drives answer 0x14/0xEB.
  uint16_t id[256];
  if (!identify(kIdentify, id)) {
    if (reg_read(kLbaMid) != 0x14 || reg_read(kLbaHigh) != 0xEB) return false;
    atapi_ = true;
    select(0);
    if (!identify(kIdentifyPacket, id)) return false;
  }
  copy_device_string(info_.serial, sizeof(info_.serial), reinterpret_cast<uint8_t*>(id + 10), 20, true);
  copy_device_string(info_.model, sizeof(info_.model), reinterpret_cast<uint8_t*>(id + 27), 40, true);
  info_.removable = id[0] & (1u << 7);

  if (atapi_) {
    info_.kind = StorageKind::Optical;
    info_.read_only = true;
    info_.removable = true;
    uint8_t buffer[8];
    for (int attempt = 0; attempt < 3; ++attempt) {
      uint8_t capacity[12] = {scsi::kReadCapacity10};
      if (atapi_command(capacity, buffer, 8)) {
        info_.block_count = uint64_t{scsi::be32(buffer)} + 1;
        info_.block_size = scsi::be32(buffer + 4);
        break;
      }
    }
    if (info_.block_size < 512 || info_.block_size > 4096 || info_.block_count < 2) return false;  // no medium
  } else {
    lba48_ = id[83] & (1u << 10);
    info_.block_count = lba48_ ? (uint64_t{id[103]} << 48) | (uint64_t{id[102]} << 32) | (uint64_t{id[101]} << 16) | id[100]
                               : (uint64_t{id[61]} << 16) | id[60];
    info_.block_size = 512;
    if (info_.block_count == 0) return false;
  }
  return prepare_bounce();
}

void IdeStorageDevice::shutdown() {
  release_bounce();
  controller_->shutdown();
}

bool IdeStorageDevice::atapi_command(const uint8_t* cdb, void* data, uint32_t bytes) {
  select(0);
  if (!wait_not_busy(2000)) return false;
  reg_write(kFeatures, 0);  // PIO
  reg_write(kLbaMid, static_cast<uint8_t>(bytes > 0xFFFE ? 0xFE : bytes));
  reg_write(kLbaHigh, static_cast<uint8_t>((bytes > 0xFFFE ? 0xFFFE : bytes) >> 8));
  reg_write(kCommand, kPacket);
  for (int i = 0; i < 4; ++i) status();
  if (!wait_data_ready()) return false;
  const IoPort port(channel_.command_base + kData);
  for (int i = 0; i < 6; ++i) port.write16(static_cast<uint16_t>(cdb[2 * i] | (cdb[2 * i + 1] << 8)));

  uint8_t* out = static_cast<uint8_t*>(data);
  uint32_t left = bytes;
  while (true) {
    if (!wait_not_busy(10000)) return false;
    const uint8_t s = status();
    if (s & (kStatusError | kStatusFault)) return false;
    if (!(s & kStatusDrq)) break;  // command finished
    uint32_t chunk = reg_read(kLbaMid) | (uint32_t{reg_read(kLbaHigh)} << 8);
    for (uint32_t i = 0; i < chunk; i += 2) {
      const uint16_t w = port.read16();
      if (left >= 2) {
        out[0] = static_cast<uint8_t>(w);
        out[1] = static_cast<uint8_t>(w >> 8);
        out += 2;
        left -= 2;
      }
    }
  }
  return left == 0;
}

bool IdeStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) {
  kernel::ScopedMutex guard(controller_->channel_mutex(channel_index_));
  uint16_t* words = static_cast<uint16_t*>(dma);
  if (atapi_) {
    if (write) return false;
    uint8_t cdb[12];
    scsi::build_read_write(cdb, false, lba, blocks);
    return atapi_command(cdb, dma, blocks * info_.block_size);
  }
  const IoPort port(channel_.command_base + kData);
  while (blocks > 0) {
    const uint32_t n = blocks < kPioSectorsPerCommand ? blocks : kPioSectorsPerCommand;
    if (!lba48_ && lba + n > 0x0FFFFFFF) return false;
    select(lba48_ ? 0 : static_cast<uint8_t>(lba >> 24));
    if (!wait_not_busy(2000)) return false;
    if (lba48_) {  // high bytes first, then low bytes
      reg_write(kSectorCount, 0);
      reg_write(kLbaLow, static_cast<uint8_t>(lba >> 24));
      reg_write(kLbaMid, static_cast<uint8_t>(lba >> 32));
      reg_write(kLbaHigh, static_cast<uint8_t>(lba >> 40));
    }
    reg_write(kSectorCount, static_cast<uint8_t>(n));
    reg_write(kLbaLow, static_cast<uint8_t>(lba));
    reg_write(kLbaMid, static_cast<uint8_t>(lba >> 8));
    reg_write(kLbaHigh, static_cast<uint8_t>(lba >> 16));
    reg_write(kCommand, lba48_ ? (write ? kWriteSectorsExt : kReadSectorsExt) : (write ? kWriteSectors : kReadSectors));
    for (uint32_t s = 0; s < n; ++s) {
      if (!wait_data_ready()) return false;
      if (write) for (int i = 0; i < 256; ++i) port.write16(*words++);
      else for (int i = 0; i < 256; ++i) *words++ = port.read16();
    }
    if (write && !wait_not_busy(5000)) return false;
    lba += n;
    blocks -= n;
  }
  return true;
}

bool IdeStorageDevice::flush_cache() {
  kernel::ScopedMutex guard(controller_->channel_mutex(channel_index_));
  if (atapi_) return true;
  select(0);
  if (!wait_not_busy(2000)) return false;
  reg_write(kCommand, lba48_ ? kFlushCacheExt : kFlushCache);
  for (int i = 0; i < 4; ++i) status();
  if (!wait_not_busy(10000)) return false;
  return !(status() & (kStatusError | kStatusFault));
}

}  // namespace hardware
