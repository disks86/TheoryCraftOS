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

#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/NVIDIA/NvidiaVideoEngine.hpp"

namespace hardware {

namespace {
template <class T, class... Args>
void try_add(List<UniquePointer<IVideoCodecDevice>>& list, allocators::IAllocator& allocator, Args... args) {
  UniquePointer<T> device = UniquePointer<T>::make(allocator, allocator, args...);
  if (!device) return;
  if (!device->initialize()) {
    device->shutdown();
    return;
  }
  list.push_back(UniquePointer<IVideoCodecDevice>(util::move(device)));
}
}  // namespace

void SystemVideoCodecs::initialize() {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassDisplay) continue;
    if (d.vendor_id == NvidiaVideoEngine::kPciVendor) try_add<NvidiaVideoEngine>(devices_, allocator_);
    else if (d.vendor_id == AmdVcnVideoEngine::kPciVendor) try_add<AmdVcnVideoEngine>(devices_, allocator_);
    else if (d.vendor_id == IntelMediaVideoEngine::kPciVendor) try_add<IntelMediaVideoEngine>(devices_, allocator_, d.device_id);
  }
  try_add<SoftwareVideoCodec>(devices_, allocator_);
}

bool SystemVideoCodecs::add_device(UniquePointer<IVideoCodecDevice> device) {
  if (!device) return false;
  if (!device->initialize()) {
    device->shutdown();
    return false;
  }
  return devices_.insert(0, util::move(device));
}

void SystemVideoCodecs::shutdown() {
  for (uint64_t i = devices_.count(); i > 0; --i) devices_[i - 1]->shutdown();
  devices_.clear();
}

uint32_t SystemVideoCodecs::decode_codecs() const {
  uint32_t mask = 0;
  for (const UniquePointer<IVideoCodecDevice>& d : devices_) mask |= d->info().decode_codecs;
  return mask;
}

uint32_t SystemVideoCodecs::encode_codecs() const {
  uint32_t mask = 0;
  for (const UniquePointer<IVideoCodecDevice>& d : devices_) mask |= d->info().encode_codecs;
  return mask;
}

UniquePointer<IVideoDecoder> SystemVideoCodecs::create_decoder(const DecoderConfig& config) {
  for (UniquePointer<IVideoCodecDevice>& d : devices_) {
    if (!d->can_decode(config.codec)) continue;
    UniquePointer<IVideoDecoder> session = d->create_decoder(config);
    if (session) return session;
  }
  return UniquePointer<IVideoDecoder>();
}

UniquePointer<IVideoEncoder> SystemVideoCodecs::create_encoder(const EncoderConfig& config) {
  for (UniquePointer<IVideoCodecDevice>& d : devices_) {
    if (!d->can_encode(config.codec)) continue;
    UniquePointer<IVideoEncoder> session = d->create_encoder(config);
    if (session) return session;
  }
  return UniquePointer<IVideoEncoder>();
}

}  // namespace hardware
