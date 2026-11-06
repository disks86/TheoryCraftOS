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

#include "Hardware/Chipset/Smbus/SmbusHost.hpp"

namespace hardware {

namespace {
using namespace smbus_reg;
constexpr uint8_t kMaxBlock = 32;
constexpr uint32_t kSemaphoreTries = 5, kSemaphorePauseUs = 1000;
}  // namespace

const char* smbus_status_name(SmbusStatus status) {
  switch (status) {
    case SmbusStatus::Ok: return "ok";
    case SmbusStatus::NoController: return "no SMBus controller";
    case SmbusStatus::NoDevice: return "no device acknowledged";
    case SmbusStatus::Busy: return "SMBus in use by the firmware";
    case SmbusStatus::Collision: return "bus collision";
    case SmbusStatus::Failed: return "transaction failed";
    case SmbusStatus::Timeout: return "timed out";
    case SmbusStatus::BadArgument: return "bad argument";
  }
  return "unknown SMBus status";
}

SmbusStatus SmbusHost::claim() {
  if (flavor_ == SmbusHostFlavor::IntelI801) {
    // Reading the status register with INUSE_STS clear sets it: the read itself is the claim. Set on the read means someone else (ACPI code,
    // the firmware's SMM handler) holds it.
    const uint8_t status = ports_->read8(kHostStatus);
    if (status & kStatusInUse) return SmbusStatus::Busy;
    if (status & kStatusBusy) {
      release();
      return SmbusStatus::Busy;
    }
  } else {
    // The host semaphore shared with the platform's microcontroller: ask, and read back whether it was granted.
    bool granted = false;
    for (uint32_t attempt = 0; attempt < kSemaphoreTries && !granted; ++attempt) {
      const uint8_t control = ports_->read8(kAmdSlaveControl);
      ports_->write8(kAmdSlaveControl, control | kAmdSemaphoreRequest);
      granted = ports_->read8(kAmdSlaveControl) & kAmdSemaphoreRequest;
      if (!granted) ports_->pause_us(kSemaphorePauseUs);
    }
    if (!granted) return SmbusStatus::Busy;
    if (ports_->read8(kHostStatus) & kStatusBusy) {
      release();
      return SmbusStatus::Busy;
    }
  }
  ports_->write8(kHostStatus, kStatusClear);   // stale completion and error bits from an earlier user
  return SmbusStatus::Ok;
}

void SmbusHost::release() {
  if (flavor_ == SmbusHostFlavor::IntelI801) {
    ports_->write8(kHostStatus, kStatusClear | kStatusInUse);
  } else {
    ports_->write8(kHostStatus, kStatusClear);
    ports_->write8(kAmdSlaveControl, ports_->read8(kAmdSlaveControl) | kAmdSemaphoreRelease);
  }
}

SmbusStatus SmbusHost::execute(const Request& request) {
  ports_->write8(kHostAddress, static_cast<uint8_t>((request.address << 1) | (request.read ? 1 : 0)));
  ports_->write8(kHostCommand, request.command);
  if (!request.read) {
    ports_->write8(kHostData0, request.data0);
    ports_->write8(kHostData1, request.data1);
  }
  ports_->write8(kHostControl, static_cast<uint8_t>(request.protocol | kControlStart));

  uint8_t status = 0;
  bool done = false;
  for (uint32_t poll = 0; poll < kMaxPolls && !done; ++poll) {
    ports_->pause_us(kPollUs);
    status = ports_->read8(kHostStatus);
    done = !(status & kStatusBusy) && (status & (kStatusInterrupt | kStatusErrors));
  }
  if (!done) {
    ports_->write8(kHostControl, kControlKill);
    ports_->pause_us(kPollUs);
    ports_->write8(kHostControl, 0);
    ports_->write8(kHostStatus, kStatusClear);
    return SmbusStatus::Timeout;
  }
  if (status & kStatusDeviceError) return SmbusStatus::NoDevice;
  if (status & kStatusBusError) return SmbusStatus::Collision;
  if (status & kStatusFailed) return SmbusStatus::Failed;
  return SmbusStatus::Ok;
}

SmbusStatus SmbusHost::run(const Request& request, uint8_t* data0, uint8_t* data1, ArrayView<uint8_t> block, uint8_t* block_length) {
  if (!ports_) return SmbusStatus::NoController;
  if (request.address > 0x7F) return SmbusStatus::BadArgument;
  const SmbusStatus claimed = claim();
  if (claimed != SmbusStatus::Ok) return claimed;

  const bool block_read = request.protocol == kProtocolBlockData;
  uint8_t aux = 0;
  if (block_read && flavor_ == SmbusHostFlavor::IntelI801) {
    aux = ports_->read8(kIntelAuxControl);
    ports_->write8(kIntelAuxControl, aux | kIntelAuxE32b);   // the whole block in the controller's buffer instead of a byte at a time
  }

  SmbusStatus result = execute(request);
  if (result == SmbusStatus::Ok && request.read) {
    if (data0) *data0 = ports_->read8(kHostData0);
    if (data1) *data1 = ports_->read8(kHostData1);
    if (block_read) {
      const uint8_t count = ports_->read8(kHostData0);   // the count byte the device sent
      if (count == 0 || count > kMaxBlock || count > block.size()) {
        result = SmbusStatus::Failed;
      } else {
        (void)ports_->read8(kHostControl);   // reading the control register rewinds the block buffer's index
        for (uint8_t i = 0; i < count; ++i) block[i] = ports_->read8(kBlockData);
        *block_length = count;
      }
    }
  }

  if (block_read && flavor_ == SmbusHostFlavor::IntelI801) ports_->write8(kIntelAuxControl, aux);
  release();
  return result;
}

SmbusStatus SmbusHost::quick(uint8_t address, bool read) {
  return run({address, read, 0, kProtocolQuick, 0, 0}, nullptr, nullptr, ArrayView<uint8_t>(), nullptr);
}

SmbusStatus SmbusHost::read_byte(uint8_t address, uint8_t* value) {
  return run({address, true, 0, kProtocolByte, 0, 0}, value, nullptr, ArrayView<uint8_t>(), nullptr);
}

SmbusStatus SmbusHost::read_byte_data(uint8_t address, uint8_t command, uint8_t* value) {
  return run({address, true, command, kProtocolByteData, 0, 0}, value, nullptr, ArrayView<uint8_t>(), nullptr);
}

SmbusStatus SmbusHost::write_byte_data(uint8_t address, uint8_t command, uint8_t value) {
  return run({address, false, command, kProtocolByteData, value, 0}, nullptr, nullptr, ArrayView<uint8_t>(), nullptr);
}

SmbusStatus SmbusHost::read_word_data(uint8_t address, uint8_t command, uint16_t* value) {
  uint8_t low = 0, high = 0;
  const SmbusStatus status = run({address, true, command, kProtocolWordData, 0, 0}, &low, &high, ArrayView<uint8_t>(), nullptr);
  if (status == SmbusStatus::Ok && value) *value = static_cast<uint16_t>(low | (high << 8));
  return status;
}

SmbusStatus SmbusHost::read_block_data(uint8_t address, uint8_t command, ArrayView<uint8_t> out, uint8_t* length) {
  if (!length) return SmbusStatus::BadArgument;
  *length = 0;
  return run({address, true, command, kProtocolBlockData, 0, 0}, nullptr, nullptr, out, length);
}

}  // namespace hardware
