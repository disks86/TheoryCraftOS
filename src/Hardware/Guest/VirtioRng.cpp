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

#include "Hardware/Guest/VirtioRng.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

bool VirtioRng::initialize() {
  if (!transport_.initialize() || !transport_.setup_queue(0, &queue_)) {
    shutdown();
    return false;
  }
  buffer_ = allocate_dma_view<uint8_t>(allocator_, kRequestBytes, 64);
  if (buffer_.empty() || !transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  return true;
}

bool VirtioRng::read(ByteView out) {
  if (!started_) return false;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint32_t want = out.size() - done < kRequestBytes ? static_cast<uint32_t>(out.size() - done) : kRequestBytes;
    queue_.post(0, buffer_.data(), want);
    transport_.notify(0);
    // The device may answer with fewer bytes than asked for; keep asking until the request is met.
    uint16_t id = 0;
    uint32_t got = 0;
    const uint64_t start = uptime_us();
    while (!queue_.take_used(&id, &got)) {
      if (uptime_us() - start > kTimeoutUs) return false;
      delay_us(10);
    }
    if (got > want) got = want;  // the length is the device's word
    for (uint32_t i = 0; i < got; ++i) out[done + i] = static_cast<const volatile uint8_t&>(buffer_[i]);
    done += got;
  }
  return true;
}

void VirtioRng::shutdown() {
  started_ = false;
  transport_.shutdown();  // reset first: the device must stop writing into the buffer
  queue_.release(allocator_);
  release_view(allocator_, buffer_);
}

}  // namespace hardware
