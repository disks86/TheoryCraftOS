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

#include "Hardware/Bus/Thunderbolt/ThunderboltNvm.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

TbNvmResult TbDmaPort::read_dwords(uint32_t offset, ArrayView<uint32_t> out, uint32_t timeout_us) {
  TbControl::ConfigError error;
  const TbControl::Result r = control_.read_config(route_, port_, TbConfigSpace::Port, offset, out, timeout_us, &error, events_);
  return r == TbControl::Result::Ok ? TbNvmResult::Ok : r == TbControl::Result::Timeout ? TbNvmResult::Timeout : TbNvmResult::Failed;
}

TbNvmResult TbDmaPort::write_dwords(uint32_t offset, ArrayView<const uint32_t> data, uint32_t timeout_us) {
  TbControl::ConfigError error;
  const TbControl::Result r = control_.write_config(route_, port_, TbConfigSpace::Port, offset, data, timeout_us, &error, events_);
  return r == TbControl::Result::Ok ? TbNvmResult::Ok : r == TbControl::Result::Timeout ? TbNvmResult::Timeout : TbNvmResult::Failed;
}

TbNvmResult TbDmaPort::find() {
  static const uint32_t kPorts[] = {3, 5, 7};
  for (const uint32_t candidate : kPorts) {
    port_ = candidate;
    uint32_t type = 0;
    if (read_dwords(2, ArrayView<uint32_t>(&type, 1), tb_nvm::kTimeoutUs) == TbNvmResult::Ok && (type & 0xFFFFFF) == tb_nvm::kPortType) return TbNvmResult::Ok;
  }
  port_ = 0;
  return TbNvmResult::NoDmaPort;
}

// One command: write MAIL_IN, wait for the request bit to clear, read the status in MAIL_OUT.
TbNvmResult TbDmaPort::request(uint32_t in, uint32_t timeout_us) {
  using namespace tb_nvm;
  if (port_ == 0) return TbNvmResult::NoDmaPort;
  TbNvmResult r = write_dwords(kDmaPortCap + kMailIn, ArrayView<const uint32_t>(&in, 1), kTimeoutUs);
  if (r != TbNvmResult::Ok) return r;
  bool done = false;
  for (uint32_t waited = 0; waited < timeout_us && !done; waited += 100) {
    uint32_t value = 0;
    r = read_dwords(kDmaPortCap + kMailIn, ArrayView<uint32_t>(&value, 1), 50000);
    if (r == TbNvmResult::Ok && !(value & kInRequest)) done = true;
    else if (r == TbNvmResult::Failed) return r;
    else delay_us(100);
  }
  if (!done) return TbNvmResult::Timeout;
  uint32_t out = 0;
  r = read_dwords(kDmaPortCap + kMailOut, ArrayView<uint32_t>(&out, 1), kTimeoutUs);
  if (r != TbNvmResult::Ok) return r;
  switch (out & kOutStatusMask) {
    case kStatusCompleted: return TbNvmResult::Ok;
    case kStatusErrAuth: return TbNvmResult::Auth;
    case kStatusErrAccess: return TbNvmResult::Access;
    default: return TbNvmResult::Failed;
  }
}

TbNvmResult TbDmaPort::read_block(uint32_t dword_address, ArrayView<uint32_t> out) {
  using namespace tb_nvm;
  if (out.size() == 0 || out.size() > kMailDataDwords || dword_address > kInAddressMask) return TbNvmResult::BadRequest;
  uint32_t in = kCmdFlashRead << kInCmdShift;
  if (out.size() < kMailDataDwords) in |= (static_cast<uint32_t>(out.size()) & kInDwordsMask) << kInDwordsShift;   // (16 is a zero field)
  in |= (dword_address & kInAddressMask) << kInAddressShift;
  in |= kInRequest;
  const TbNvmResult r = request(in, kTimeoutUs);
  if (r != TbNvmResult::Ok) return r;
  return read_dwords(kDmaPortCap + kMailData, out, kTimeoutUs);
}

TbNvmResult TbDmaPort::write_block(uint32_t dword_address, ArrayView<const uint32_t> data) {
  using namespace tb_nvm;
  if (data.size() == 0 || data.size() > kMailDataDwords || dword_address > kInAddressMask) return TbNvmResult::BadRequest;
  const TbNvmResult r = write_dwords(kDmaPortCap + kMailData, data, kTimeoutUs);   // the block into MAIL_DATA, then the command
  if (r != TbNvmResult::Ok) return r;
  uint32_t in = kCmdFlashWrite << kInCmdShift;
  in |= ((static_cast<uint32_t>(data.size()) - 1) & kInDwordsMask) << kInDwordsShift;
  in |= (dword_address & kInAddressMask) << kInAddressShift;
  in |= kInRequest;
  return request(in, kTimeoutUs);
}

TbNvmResult TbDmaPort::flash_read(uint32_t address, ByteView out) {
  using namespace tb_nvm;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint32_t offset = address & 3;
    const uint64_t want = out.size() - done;
    const uint32_t nbytes = static_cast<uint32_t>(want + offset < kBlockBytes ? want + offset : kBlockBytes);
    const uint32_t dwords = (nbytes + 3) / 4;
    uint32_t block[kMailDataDwords];
    TbNvmResult r = TbNvmResult::Failed;
    for (uint32_t attempt = 0; attempt <= kRetries; ++attempt) {
      r = read_block(address / 4, ArrayView<uint32_t>(block, dwords));
      if (r == TbNvmResult::Ok || r == TbNvmResult::BadRequest || r == TbNvmResult::NoDmaPort) break;
    }
    if (r != TbNvmResult::Ok) return r;
    uint8_t bytes[kBlockBytes];
    for (uint32_t i = 0; i < dwords; ++i)
      for (uint32_t k = 0; k < 4; ++k) bytes[i * 4 + k] = static_cast<uint8_t>(block[i] >> (8 * k));   // (little endian words)
    const uint32_t take = nbytes - offset;
    for (uint32_t i = 0; i < take; ++i) out[done + i] = bytes[offset + i];
    done += take;
    if (address > 0xFFFFFFFFu - take) return TbNvmResult::BadRequest;
    address += take;
  }
  return TbNvmResult::Ok;
}

TbNvmResult TbDmaPort::flash_write(uint32_t address, ConstByteView data) {
  using namespace tb_nvm;
  if ((address & 3) != 0 || (data.size() & 3) != 0) return TbNvmResult::BadRequest;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t left = data.size() - done;
    const uint32_t nbytes = static_cast<uint32_t>(left < kBlockBytes ? left : kBlockBytes);
    uint32_t block[kMailDataDwords];
    for (uint32_t i = 0; i < nbytes / 4; ++i) block[i] = data[done + i * 4] | (uint32_t{data[done + i * 4 + 1]} << 8) | (uint32_t{data[done + i * 4 + 2]} << 16) | (uint32_t{data[done + i * 4 + 3]} << 24);
    TbNvmResult r = TbNvmResult::Failed;
    for (uint32_t attempt = 0; attempt <= kRetries; ++attempt) {
      r = write_block(address / 4, ArrayView<const uint32_t>(block, nbytes / 4));
      if (r != TbNvmResult::Timeout) break;                    // (only a timeout is tried again; a refusal would be refused again)
    }
    if (r == TbNvmResult::Timeout) r = TbNvmResult::Failed;
    if (r != TbNvmResult::Ok) return r;
    done += nbytes;
    if (address > 0xFFFFFFFFu - nbytes) return TbNvmResult::BadRequest;
    address += nbytes;
  }
  return TbNvmResult::Ok;
}

TbNvmResult TbDmaPort::update_auth() { return request((tb_nvm::kCmdFlashUpdateAuth << tb_nvm::kInCmdShift) | tb_nvm::kInRequest, tb_nvm::kAuthTimeoutUs); }

TbNvmResult TbDmaPort::power_cycle() { return request((tb_nvm::kCmdPowerCycle << tb_nvm::kInCmdShift) | tb_nvm::kInRequest, tb_nvm::kAuthTimeoutUs); }

int TbDmaPort::update_auth_status(uint32_t* status) {
  using namespace tb_nvm;
  uint32_t out = 0;
  if (port_ == 0 || read_dwords(kDmaPortCap + kMailOut, ArrayView<uint32_t>(&out, 1), kTimeoutUs) != TbNvmResult::Ok) return -1;
  if (((out >> kOutCmdShift) & kOutCmdMask) != kCmdFlashUpdateAuth) return 0;   // (the status belongs to another command)
  if (status) *status = out & kOutStatusMask;
  return 1;
}

namespace {
uint32_t load_le32(ConstByteView image, uint64_t offset) {
  return image[offset] | (uint32_t{image[offset + 1]} << 8) | (uint32_t{image[offset + 2]} << 16) | (uint32_t{image[offset + 3]} << 24);
}
}  // namespace

bool tb_nvm_validate_image(ConstByteView image, uint16_t device_id, bool safe_mode, uint32_t* data_start) {
  using namespace tb_nvm;
  if (image.size() < kMinImage || image.size() > kMaxImage) return false;
  // The first dword's low 24 bits point to the digital section; the section's device id and its size have to be inside the image.
  const uint32_t header = load_le32(image, 0) & 0xFFFFFF;
  if (header > image.size() || image.size() - header <= kDeviceId + 2) return false;
  if ((header & 0xFFF) != 0) return false;                                    // (4 KiB aligned)
  const uint32_t section_size = image[header] | (uint32_t{image[header + 1]} << 8);
  if (section_size >= image.size()) return false;
  if (!safe_mode) {
    const uint32_t image_device = image[header + kDeviceId] | (uint32_t{image[header + kDeviceId + 1]} << 8);
    if (image_device != device_id) return false;                              // an image for another controller
  }
  if (data_start) *data_start = header;
  return true;
}

TbNvmResult tb_nvm_read_version(TbDmaPort& port, TbNvmVersion* out) {
  using namespace tb_nvm;
  uint8_t word[4];
  TbNvmResult r = port.flash_read(kFlashSize, ByteView(word, sizeof word));
  if (r != TbNvmResult::Ok) return r;
  const uint32_t size = word[0] | (uint32_t{word[1]} << 8);
  const uint32_t area = ((1024u * 1024u) << (size & 7)) / 8;
  const uint32_t active = area > kHeaderBytes ? (area - kHeaderBytes) / 2 : 0;
  r = port.flash_read(kVersion, ByteView(word, sizeof word));
  if (r != TbNvmResult::Ok) return r;
  const uint32_t value = word[0] | (uint32_t{word[1]} << 8) | (uint32_t{word[2]} << 16) | (uint32_t{word[3]} << 24);
  out->major = static_cast<uint8_t>(value >> 16);
  out->minor = static_cast<uint8_t>(value >> 8);
  out->active_size = active;
  return TbNvmResult::Ok;
}

TbNvmResult tb_nvm_update(TbDmaPort& port, ConstByteView image, uint16_t device_id, bool safe_mode) {
  uint32_t start = 0;
  if (!tb_nvm_validate_image(image, device_id, safe_mode, &start)) return TbNvmResult::BadImage;
  const TbNvmResult written = port.flash_write(0, image.subview(start));
  if (written != TbNvmResult::Ok) return written;
  // A timeout is what a controller that is restarting looks like: the update is on its way.
  const TbNvmResult auth = port.update_auth();
  return auth == TbNvmResult::Timeout ? TbNvmResult::Ok : auth;
}

}  // namespace hardware
