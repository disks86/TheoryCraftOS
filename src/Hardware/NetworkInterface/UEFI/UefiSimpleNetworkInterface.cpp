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

#include "Kernel/Delay.hpp"
#include "Kernel/Firmware.hpp"
#include "Hardware/NetworkInterface/UEFI/UefiSimpleNetworkInterface.hpp"

namespace hardware {

bool UefiSimpleNetworkInterface::initialize() {
  efi::SimpleNetworkMode* mode = snp_->mode;
  if (mode->hw_address_size != 6) return false;  // not Ethernet

  if (mode->state == static_cast<uint32_t>(efi::SimpleNetworkState::Stopped) && snp_->start(snp_) != efi::kSuccess)
    return false;
  if (mode->state != static_cast<uint32_t>(efi::SimpleNetworkState::Initialized)) {
    if (snp_->initialize(snp_, 0, 0) != efi::kSuccess) return false;
    we_initialized_ = true;
  }
  // Our unicast address plus broadcast, where the NIC allows choosing.
  const uint32_t wanted = (efi::kReceiveFilterUnicast | efi::kReceiveFilterBroadcast) & mode->receive_filter_mask;
  snp_->receive_filters(snp_, wanted, 0, 0, 0, nullptr);

  for (int i = 0; i < 6; ++i) mac_.bytes[i] = mode->current_address[i];
  started_ = true;
  return true;
}

void UefiSimpleNetworkInterface::shutdown() {
  if (started_ && we_initialized_) snp_->shutdown(snp_);  // leave the NIC as we found it
  started_ = false;
  we_initialized_ = false;
}

bool UefiSimpleNetworkInterface::link_up() const {
  const efi::SimpleNetworkMode* mode = snp_->mode;
  return started_ && (!mode->media_present_supported || mode->media_present);
}

bool UefiSimpleNetworkInterface::transmit(const uint8_t* frame, uint32_t length) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !frame || length == 0 || length > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  const uint32_t padded = length < kEthernetMinFrame ? kEthernetMinFrame : length;
  __builtin_memset(staging_, 0, padded);
  __builtin_memcpy(staging_, frame, length);
  // Header size 0: the buffer already holds a complete Ethernet frame.
  efi::Status sent = efi::kSuccess;
  kernel::firmware([&] { sent = snp_->transmit(snp_, 0, padded, staging_, nullptr, nullptr, nullptr); });
  if (sent != efi::kSuccess) {
    ++stats_.send_failures;
    return false;
  }
  // Wait (briefly) until the firmware hands the buffer back, so staging_ can be reused.
  for (int i = 0; i < 1000; ++i) {
    uint32_t interrupts = 0;
    void* recycled = nullptr;
    efi::Status status = efi::kSuccess;
    kernel::firmware([&] { status = snp_->get_status(snp_, &interrupts, &recycled); });
    if (status == efi::kSuccess && recycled) break;
    hardware::delay_us(1000);
  }
  ++stats_.frames_sent;
  return true;
}

bool UefiSimpleNetworkInterface::receive(uint8_t* buffer, uint32_t capacity, uint32_t* length) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !buffer || !length) return false;
  uint8_t frame[kEthernetMaxFrame + 64];
  efi::Uintn size = sizeof(frame), header = 0;
  efi::Status received = efi::kSuccess;
  kernel::firmware([&] { received = snp_->receive(snp_, &header, &size, frame, nullptr, nullptr, nullptr); });
  if (received != efi::kSuccess) return false;
  __builtin_memcpy(buffer, frame, size < capacity ? size : capacity);
  *length = static_cast<uint32_t>(size);
  ++stats_.frames_received;
  return true;
}

}  // namespace hardware
