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

namespace hardware {

namespace {
constexpr uint32_t kConfigSenseSize = 20, kConfigCdbSize = 24, kConfigMaxTarget = 30;
constexpr uint16_t kControlQueue = 0, kEventQueue = 1, kRequestQueue = 2;
constexpr uint32_t kMaxProbedTargets = 16;
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
  memory_ = allocate_dma_view<uint8_t>(allocator_, 4096, 4096);
  if (memory_.empty() || !transport_.setup_queue(kControlQueue, &control_) || !transport_.setup_queue(kEventQueue, &event_) ||
      !transport_.setup_queue(kRequestQueue, &request_) || request_.size() < 4 || !transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  return true;
}

void VirtioScsiController::shutdown() {
  started_ = false;
  transport_.shutdown();
  control_.release(allocator_);
  event_.release(allocator_);
  request_.release(allocator_);
  release_view(allocator_, memory_);
}

VirtioScsiController::Result VirtioScsiController::command(uint32_t target, uint32_t lun, ConstByteView cdb,
                                                           ByteView data, bool device_writes) {
  Result result;
  if (!started_ || cdb.size() > 32) return result;
  kernel::ScopedMutex guard(mutex_);
  // The request and response headers live in the controller's page, one after the other.
  RequestHeader* request = reinterpret_cast<RequestHeader*>(&memory_[0]);
  ResponseHeader* response = reinterpret_cast<ResponseHeader*>(&memory_[256]);
  memory_.subview(0, sizeof(RequestHeader)).fill(0);
  memory_.subview(256, sizeof(ResponseHeader)).fill(0xFF);
  request->lun[0] = 1;
  request->lun[1] = static_cast<uint8_t>(target);
  request->lun[2] = static_cast<uint8_t>(0x40 | (lun >> 8));
  request->lun[3] = static_cast<uint8_t>(lun);
  ByteView(request->cdb).copy_from(cdb);

  Virtqueue::Segment chain[4];
  uint16_t n = 0;
  const uint32_t length = static_cast<uint32_t>(data.size());
  chain[n++] = {request, sizeof(RequestHeader), false};
  if (length && !device_writes) chain[n++] = {data.data(), length, false};
  chain[n++] = {response, sizeof(ResponseHeader), true};
  if (length && device_writes) chain[n++] = {data.data(), length, true};
  request_.post_chain(0, chain, n);
  transport_.notify(kRequestQueue);

  uint16_t id;
  uint32_t written;
  if (!wait_for(st_, 10000, [&] { return request_.take_used(&id, &written); })) return result;
  const volatile ResponseHeader* r = response;
  result.delivered = true;
  result.response = r->response;
  result.status = r->status;
  return result;
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
    if (!device->initialize()) {
      device->release_bounce();
      continue;
    }
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
  return prepare_bounce();
}

void VirtioScsiDevice::shutdown() {
  release_bounce();
  controller_->shutdown();
}

bool VirtioScsiDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  uint8_t cdb[16];
  const uint32_t length = scsi::build_read_write(ByteView(cdb), write, lba, blocks);
  return run(ConstByteView(cdb).first(length), dma, !write);
}

bool VirtioScsiDevice::flush_cache() {
  if (optical_) return true;
  const uint8_t cdb[10] = {scsi::kSynchronizeCache10};
  return run(ConstByteView(cdb), ByteView(), false);
}

}  // namespace hardware
