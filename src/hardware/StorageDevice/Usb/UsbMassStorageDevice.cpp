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

#include "hardware/StorageDevice/Usb/UsbMassStorageDevice.hpp"
#include "hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "hardware/StorageDevice/WaitFor.hpp"

namespace hardware {

namespace {
constexpr uint32_t kCbwSignature = 0x43425355, kCswSignature = 0x53425355;  // "USBC", "USBS"
constexpr uint8_t kClassMassStorage = 8, kSubclassScsi = 6, kProtocolBulkOnly = 0x50;
constexpr uint8_t kRequestSense = 0x03;
constexpr uint8_t kDescriptorConfiguration = 2, kDescriptorInterface = 4, kDescriptorEndpoint = 5;
constexpr uint8_t kClassInterfaceIn = 0xA1, kClassInterfaceOut = 0x21, kGetMaxLun = 0xFE, kBulkReset = 0xFF;
constexpr uint8_t kEndpointOut = 0x02;  // request type: standard, endpoint recipient
constexpr uint8_t kClearFeature = 1, kFeatureEndpointHalt = 0;
}  // namespace

void UsbMassStorageDevice::discover(IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st,
                                    List<UniquePointer<IStorageDevice>>& out) {
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    IUsbHostController* host = usb.host_controller(c);
    if (!host) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) {
      uint8_t config[256];
      uint16_t length = 0;
      if (!host->configuration_descriptor(port, config, sizeof(config), &length) || length < 9 ||
          config[1] != kDescriptorConfiguration)
        continue;
      if (length > sizeof(config)) length = sizeof(config);
      const uint8_t configuration_value = config[5];

      // Walk the descriptors for a Bulk-Only mass-storage interface and its endpoints.
      bool in_interface = false;
      uint8_t number = 0, in_ep = 0, out_ep = 0;
      uint16_t in_size = 0, out_size = 0;
      auto finish = [&] {
        if (!in_interface || !in_ep || !out_ep) return;
        in_interface = false;
        if (!host->set_configuration(port, configuration_value) || !host->start_bulk_endpoint(port, in_ep, in_size) ||
            !host->start_bulk_endpoint(port, out_ep, out_size))
          return;
        UniquePointer<UsbMassStorageDevice> device = UniquePointer<UsbMassStorageDevice>::make(
            allocator, *host, port, number, in_ep, in_size, out_ep, out_size, allocator, st);
        if (!device) return;
        if (!device->initialize()) {
          device->shutdown();
          return;
        }
        out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
      };
      for (uint16_t i = 0; i + 2 <= length && config[i] >= 2;) {
        const uint8_t size = config[i], type = config[i + 1];
        if (i + size > length) break;
        if (type == kDescriptorInterface && size >= 9) {
          finish();  // the previous interface is complete
          in_interface = config[i + 5] == kClassMassStorage && config[i + 6] == kSubclassScsi &&
                         config[i + 7] == kProtocolBulkOnly && config[i + 3] == 0;
          number = config[i + 2];
          in_ep = out_ep = 0;
        } else if (in_interface && type == kDescriptorEndpoint && size >= 7 && (config[i + 3] & 3) == 2) {
          const uint16_t packet = (config[i + 4] | (config[i + 5] << 8)) & 0x7FF;
          if (config[i + 2] & 0x80) {
            if (!in_ep) in_ep = config[i + 2], in_size = packet;
          } else if (!out_ep) {
            out_ep = config[i + 2], out_size = packet;
          }
        }
        i += size;
      }
      finish();
    }
  }
}

void UsbMassStorageDevice::recover() {
  // Reset recovery (Bulk-Only Transport 5.3.4): class reset, then clear both halts.
  host_.control_out(port_, kClassInterfaceOut, kBulkReset, 0, interface_);
  host_.reset_bulk_endpoint(port_, in_);
  host_.reset_bulk_endpoint(port_, out_);
  host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, in_);
  host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, out_);
}

UsbMassStorageDevice::Result UsbMassStorageDevice::command(const uint8_t* cdb, uint32_t cdb_length, void* data,
                                                           uint32_t length, bool data_in) {
  Cbw* cbw = reinterpret_cast<Cbw*>(wire_);
  Csw* csw = reinterpret_cast<Csw*>(wire_ + 64);
  __builtin_memset(cbw, 0, sizeof(Cbw));
  const uint32_t tag = tag_++;
  cbw->signature = kCbwSignature;
  cbw->tag = tag;
  cbw->data_length = length;
  cbw->flags = data_in ? 0x80 : 0;
  cbw->lun = 0;
  cbw->cb_length = static_cast<uint8_t>(cdb_length);
  __builtin_memcpy(cbw->cb, cdb, cdb_length);

  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, out_, cbw, sizeof(Cbw), &moved) || moved != sizeof(Cbw)) {
    recover();
    return Result::Error;
  }
  if (length) {
    const bool ok = host_.bulk_transfer(port_, data_in ? in_ : out_, data, length, &moved);
    if (!ok) {  // a stall here is normal when the command failed: clear it and read the status
      host_.reset_bulk_endpoint(port_, data_in ? in_ : out_);
      host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, data_in ? in_ : out_);
    }
  }
  __builtin_memset(csw, 0, sizeof(Csw));
  if (!host_.bulk_transfer(port_, in_, csw, sizeof(Csw), &moved) || moved != sizeof(Csw)) {
    // One more try after un-stalling, as the specification asks.
    host_.reset_bulk_endpoint(port_, in_);
    host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, in_);
    __builtin_memset(csw, 0, sizeof(Csw));
    if (!host_.bulk_transfer(port_, in_, csw, sizeof(Csw), &moved) || moved != sizeof(Csw)) {
      recover();
      return Result::Error;
    }
  }
  if (csw->signature != kCswSignature || csw->tag != tag || csw->status == 2) {
    recover();
    return Result::Error;
  }
  return csw->status == 0 ? Result::Good : Result::Failed;
}

// Runs a command; when it fails with CHECK CONDITION the sense data is fetched
// (required to clear the condition) and the command is tried again a few times.
bool UsbMassStorageDevice::run(const uint8_t* cdb, uint32_t cdb_length, void* data, uint32_t length, bool data_in) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const Result r = command(cdb, cdb_length, data, length, data_in);
    if (r == Result::Good) return true;
    if (r == Result::Error) return false;
    uint8_t sense_cdb[6] = {kRequestSense, 0, 0, 0, 18, 0};
    uint8_t* sense = wire_ + 128;
    if (command(sense_cdb, 6, sense, 18, true) != Result::Good) return false;
    const uint8_t key = sense[2] & 0x0F;
    if (key == 0x02) wait_for(st_, 50, [] { return false; });  // NOT READY: give it a moment
    else if (key != 0x06) return false;                          // anything but UNIT ATTENTION is final
  }
  return false;
}

bool UsbMassStorageDevice::initialize() {
  bounce_alignment_ = 65536;  // a bulk transfer must not cross a 64 KiB boundary
  wire_ = static_cast<uint8_t*>(allocator_.allocate_aligned(4096, 65536));
  if (!wire_) return false;
  __builtin_memset(wire_, 0, 4096);

  uint8_t max_lun = 0;
  host_.control_in(port_, kClassInterfaceIn, kGetMaxLun, 0, interface_, &max_lun, 1);  // a stall means "one LUN"

  uint8_t* data = wire_ + 256;
  uint8_t inquiry[6] = {scsi::kInquiry, 0, 0, 0, 36, 0};
  if (!run(inquiry, 6, data, 36, true)) return false;
  const uint8_t type = data[0] & 0x1F;
  if (type != scsi::kDeviceTypeDisk && type != scsi::kDeviceTypeOptical) return false;
  optical_ = type == scsi::kDeviceTypeOptical;
  info_.kind = optical_ ? StorageKind::Optical : StorageKind::Disk;
  info_.read_only = optical_;
  info_.removable = data[1] & 0x80;
  copy_device_string(info_.model, sizeof(info_.model), data + 8, 24, false);

  uint8_t ready[6] = {scsi::kTestUnitReady};
  for (int i = 0; i < 10 && !run(ready, 6, nullptr, 0, false); ++i) {}
  uint8_t capacity10[10] = {scsi::kReadCapacity10};
  if (!run(capacity10, 10, data, 8, true)) return false;
  uint64_t last = scsi::be32(data);
  info_.block_size = scsi::be32(data + 4);
  if (last == 0xFFFFFFFF) {
    uint8_t capacity16[16] = {scsi::kServiceActionIn16, scsi::kReadCapacity16Action, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    if (!run(capacity16, 16, data, 32, true)) return false;
    last = scsi::be64(data);
    info_.block_size = scsi::be32(data + 8);
  }
  info_.block_count = last + 1;
  if (info_.block_size < 512 || info_.block_size > 4096) return false;
  return prepare_bounce();
}

void UsbMassStorageDevice::shutdown() {
  release_bounce();
  allocator_.deallocate(wire_);
  wire_ = nullptr;
}

bool UsbMassStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) {
  uint8_t cdb[16];
  const uint32_t length = scsi::build_read_write(cdb, write, lba, blocks);
  return run(cdb, length, dma, blocks * info_.block_size, !write);
}

bool UsbMassStorageDevice::flush_cache() {
  if (optical_) return true;
  uint8_t cdb[10] = {scsi::kSynchronizeCache10};
  return run(cdb, 10, nullptr, 0, false);
}

}  // namespace hardware
