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

#include "Hardware/StorageDevice/Usb/UsbMassStorageDevice.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"

namespace hardware {

namespace {
constexpr uint32_t kCbwSignature = 0x43425355, kCswSignature = 0x53425355;  // "USBC", "USBS"
constexpr uint8_t kClassMassStorage = 8, kSubclassScsi = 6, kProtocolBulkOnly = 0x50;
constexpr uint8_t kRequestSense = 0x03;
constexpr uint8_t kClassInterfaceIn = 0xA1, kClassInterfaceOut = 0x21, kGetMaxLun = 0xFE, kBulkReset = 0xFF;
constexpr uint8_t kEndpointOut = 0x02;  // request type: standard, endpoint recipient
constexpr uint8_t kClearFeature = 1, kFeatureEndpointHalt = 0;
constexpr uint32_t kCbwOffset = 0, kCswOffset = 64, kSenseOffset = 128, kDataOffset = 256;  // inside wire_
}  // namespace

void UsbMassStorageDevice::discover(IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st,
                                    List<UniquePointer<IStorageDevice>>& out) {
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    IUsbHostController* host = usb.host_controller(c);
    if (!host) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) {
      uint8_t storage[256];
      uint16_t length = 0;
      if (!host->configuration_descriptor(port, ByteView(storage), &length)) continue;  // length is the full size
      const ConstByteView config = ConstByteView(storage).at_most(length);

      // Walk the descriptors for a Bulk-Only mass-storage interface and its endpoints.
      UsbDescriptorWalker walker(config);
      ConstByteView descriptor;
      UsbConfigurationInfo configuration;
      if (!walker.next(&descriptor) || !parse_usb_configuration(descriptor, &configuration)) continue;

      bool in_interface = false;
      uint8_t number = 0, in_ep = 0, out_ep = 0;
      uint16_t in_size = 0, out_size = 0;
      auto finish = [&] {
        if (!in_interface || !in_ep || !out_ep) return;
        in_interface = false;
        if (!host->set_configuration(port, configuration.value) || !host->start_bulk_endpoint(port, in_ep, in_size) ||
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
      while (walker.next(&descriptor)) {
        UsbInterfaceInfo interface;
        UsbEndpointInfo endpoint;
        if (parse_usb_interface(descriptor, &interface)) {
          finish();  // the previous interface is complete
          in_interface = interface.device_class == kClassMassStorage && interface.subclass == kSubclassScsi &&
                         interface.protocol == kProtocolBulkOnly && interface.alternate_setting == 0;
          number = interface.number;
          in_ep = out_ep = 0;
        } else if (in_interface && parse_usb_endpoint(descriptor, &endpoint) && endpoint.transfer_type() == 2) {
          if (endpoint.is_in()) {
            if (!in_ep) in_ep = endpoint.address, in_size = endpoint.max_packet;
          } else if (!out_ep) {
            out_ep = endpoint.address, out_size = endpoint.max_packet;
          }
        }
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

UsbMassStorageDevice::Result UsbMassStorageDevice::command(ConstByteView cdb, ByteView data, bool data_in) {
  if (cdb.size() > 16) return Result::Error;
  const ByteView cbw = wire_.subview(kCbwOffset, kCbwSize);
  const ByteView csw = wire_.subview(kCswOffset, kCswSize);
  const uint32_t tag = tag_++;
  ByteWriter command_block(cbw);
  command_block.le32(kCbwSignature);
  command_block.le32(tag);
  command_block.le32(static_cast<uint32_t>(data.size()));
  command_block.u8(data_in ? 0x80 : 0);
  command_block.u8(0);  // LUN
  command_block.u8(static_cast<uint8_t>(cdb.size()));
  command_block.bytes(cdb);
  command_block.zeros(16 - cdb.size());  // the CDB field is always 16 bytes
  if (!command_block.ok()) return Result::Error;

  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, out_, cbw, &moved) || moved != kCbwSize) {
    recover();
    return Result::Error;
  }
  if (!data.empty()) {
    const uint8_t endpoint = data_in ? in_ : out_;
    if (!host_.bulk_transfer(port_, endpoint, data, &moved)) {  // a stall here is normal when the command failed: clear it and read the status
      host_.reset_bulk_endpoint(port_, endpoint);
      host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, endpoint);
    }
  }
  csw.fill(0);
  if (!host_.bulk_transfer(port_, in_, csw, &moved) || moved != kCswSize) {
    // One more try after un-stalling, as the specification asks.
    host_.reset_bulk_endpoint(port_, in_);
    host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, in_);
    csw.fill(0);
    if (!host_.bulk_transfer(port_, in_, csw, &moved) || moved != kCswSize) {
      recover();
      return Result::Error;
    }
  }
  ByteReader status(csw);
  const uint32_t signature = status.le32();
  const uint32_t answered_tag = status.le32();
  status.skip(4);  // residue
  const uint8_t code = status.u8();
  if (!status.ok() || signature != kCswSignature || answered_tag != tag || code == 2) {
    recover();
    return Result::Error;
  }
  return code == 0 ? Result::Good : Result::Failed;
}

// Runs a command; when it fails with CHECK CONDITION the sense data is fetched
// (required to clear the condition) and the command is tried again a few times.
bool UsbMassStorageDevice::run(ConstByteView cdb, ByteView data, bool data_in) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const Result r = command(cdb, data, data_in);
    if (r == Result::Good) return true;
    if (r == Result::Error) return false;
    const uint8_t sense_cdb[6] = {kRequestSense, 0, 0, 0, 18, 0};
    const ByteView sense = wire_.subview(kSenseOffset, 18);
    if (command(ConstByteView(sense_cdb), sense, true) != Result::Good) return false;
    const uint8_t key = sense[2] & 0x0F;
    if (key == 0x02) wait_for(st_, 50, [] { return false; });  // NOT READY: give it a moment
    else if (key != 0x06) return false;                          // anything but UNIT ATTENTION is final
  }
  return false;
}

bool UsbMassStorageDevice::initialize() {
  bounce_alignment_ = 65536;  // a bulk transfer must not cross a 64 KiB boundary
  wire_ = allocate_dma_view<uint8_t>(allocator_, 4096, 65536);
  if (wire_.empty()) return false;
  wire_.fill(0);

  uint8_t max_lun = 0;
  host_.control_in(port_, kClassInterfaceIn, kGetMaxLun, 0, interface_, ByteView(&max_lun, 1));  // a stall means "one LUN"

  const ByteView data = wire_.subview(kDataOffset, 64);
  const uint8_t inquiry[6] = {scsi::kInquiry, 0, 0, 0, 36, 0};
  if (!run(ConstByteView(inquiry), data.first(36), true)) return false;
  scsi::InquiryInfo unit;
  if (!scsi::parse_inquiry(data, &unit)) return false;
  if (unit.device_type != scsi::kDeviceTypeDisk && unit.device_type != scsi::kDeviceTypeOptical) return false;
  optical_ = unit.device_type == scsi::kDeviceTypeOptical;
  info_.kind = optical_ ? StorageKind::Optical : StorageKind::Disk;
  info_.read_only = optical_;
  info_.removable = unit.removable;
  copy_device_string(ArrayView<char>(info_.model), unit.vendor_and_product, false);

  const uint8_t ready[6] = {scsi::kTestUnitReady};
  for (int i = 0; i < 10 && !run(ConstByteView(ready), ByteView(), false); ++i) {}
  const uint8_t capacity10[10] = {scsi::kReadCapacity10};
  uint64_t last = 0;
  if (!run(ConstByteView(capacity10), data.first(8), true) || !scsi::parse_capacity10(data, &last, &info_.block_size))
    return false;
  if (last == 0xFFFFFFFF) {
    const uint8_t capacity16[16] = {scsi::kServiceActionIn16, scsi::kReadCapacity16Action, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    if (!run(ConstByteView(capacity16), wire_.subview(kDataOffset, 32), true) ||
        !scsi::parse_capacity16(wire_.subview(kDataOffset, 32), &last, &info_.block_size))
      return false;
  }
  info_.block_count = last + 1;
  if (info_.block_size < 512 || info_.block_size > 4096) return false;
  return prepare_bounce();
}

void UsbMassStorageDevice::shutdown() {
  release_bounce();
  release_view(allocator_, wire_);
}

bool UsbMassStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  uint8_t cdb[16];
  const uint32_t length = scsi::build_read_write(ByteView(cdb), write, lba, blocks);
  return run(ConstByteView(cdb).first(length), dma, !write);
}

bool UsbMassStorageDevice::flush_cache() {
  if (optical_) return true;
  const uint8_t cdb[10] = {scsi::kSynchronizeCache10};
  return run(ConstByteView(cdb), ByteView(), false);
}

}  // namespace hardware
