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

#include "Hardware/Bluetooth/IntelBluetooth.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace hci;

namespace {
constexpr uint16_t kOpIntelReadVersion = opcode(kOgfVendor, 0x05), kOpIntelSecureSend = opcode(kOgfVendor, 0x09), kOpIntelReadBootParams = opcode(kOgfVendor, 0x0D),
                   kOpIntelReset = opcode(kOgfVendor, 0x01), kWriteBootParams = opcode(kOgfVendor, 0x0E);
constexpr uint8_t kVendorBootup = 0x02, kVendorSecureSendResult = 0x06;
constexpr uint32_t kSfiHeader = 644, kCssHeader = 128, kPublicKeyAt = 128, kPublicKey = 256, kSignatureAt = 388, kSignature = 256;
constexpr uint8_t kFragmentInit = 0x00, kFragmentData = 0x01, kFragmentSign = 0x02, kFragmentPublicKey = 0x03;
constexpr uint32_t kBootParamsSize = 23;

void put_decimal(char* out, uint32_t capacity, uint32_t& at, uint32_t value) {
  char digits[10];
  uint32_t n = 0;
  do { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; } while (value && n < sizeof digits);
  while (n && at + 1 < capacity) out[at++] = digits[--n];
}
void put_text(char* out, uint32_t capacity, uint32_t& at, const char* text) {
  while (*text && at + 1 < capacity) out[at++] = *text++;
}
}

bool IntelBluetooth::hook(void* self, const Event& event) {
  IntelBluetooth& s = *static_cast<IntelBluetooth*>(self);
  if (event.code != kEvVendor || event.params.empty()) return false;
  const uint8_t sub = event.params[0];
  const ConstByteView body = event.params.dropped(1);
  if (sub == kVendorBootup) {
    if (body.size() == 6) s.booted_ = true;
    return true;
  }
  if (sub == kVendorSecureSendResult) {
    if (body.size() == 4) {
      if (body[0] != 0) s.download_failed_ = true;
      s.download_done_ = true;
    }
    return true;
  }
  return false;
}

IntelSetup IntelBluetooth::identify() {
  version_ = IntelVersion();
  name_[0] = 0;
  const uint8_t any = 0xFF;                                        // (the parameter newer controllers want; the older ones answer in the old format anyway)
  if (controller_.command(kOpIntelReadVersion, ConstByteView(&any, 1)) != HciResult::Ok) return IntelSetup::Failed;
  const ConstByteView r = controller_.reply();
  if (r.size() != 10 || r[1] != 0x37) return IntelSetup::UnsupportedVariant;      // (the platform is 0x37; the other answer is the TLV list of the newer controllers)
  version_.hw_platform = r[1];
  version_.hw_variant = r[2];
  version_.hw_revision = r[3];
  version_.fw_variant = r[4];
  version_.fw_revision = r[5];
  version_.fw_build_num = r[6];
  version_.fw_build_ww = r[7];
  version_.fw_build_yy = r[8];
  version_.fw_patch_num = r[9];
  switch (version_.hw_variant) {
    case 0x0B: case 0x0C: case 0x11: case 0x12: case 0x13: case 0x14:
      break;
    default:
      return IntelSetup::UnsupportedVariant;
  }
  if (operational()) {
    const HciResult a = controller_.command(kOpReadBdAddr);
    if (a == HciResult::Ok && controller_.reply().size() >= 7) {
      const ConstByteView b = controller_.reply();
      static const uint8_t kIntelDefault[6] = {0x00, 0x8B, 0x9E, 0x19, 0x03, 0x00};
      bool same = true, zero = true;
      for (uint32_t i = 0; i < 6; ++i) { same = same && b[1 + i] == kIntelDefault[i]; zero = zero && b[1 + i] == 0; }
      address_valid_ = !same && !zero;
    }
    return IntelSetup::AlreadyRunning;
  }
  // The bootloader: which image does it want?
  if (controller_.command(kOpIntelReadBootParams) != HciResult::Ok) return IntelSetup::Failed;
  const ConstByteView p = controller_.reply();
  if (p.size() != kBootParamsSize) return IntelSetup::Failed;
  dev_revid_ = p[4] | (uint32_t{p[5]} << 8);
  bool zero = true;
  for (uint32_t i = 0; i < 6; ++i) zero = zero && p[12 + i] == 0;
  address_valid_ = !zero;
  if (p[21] != 0) return IntelSetup::UnsupportedVariant;             // limited_cce: a bootloader that does not acknowledge every fragment
  uint32_t at = 0;
  put_text(name_, sizeof name_, at, "intel/ibt-");
  put_decimal(name_, sizeof name_, at, version_.hw_variant);
  put_text(name_, sizeof name_, at, "-");
  if (version_.hw_variant == 0x0B || version_.hw_variant == 0x0C) {
    put_decimal(name_, sizeof name_, at, dev_revid_);
  } else {
    put_decimal(name_, sizeof name_, at, version_.hw_revision);
    put_text(name_, sizeof name_, at, "-");
    put_decimal(name_, sizeof name_, at, version_.fw_revision);
  }
  put_text(name_, sizeof name_, at, ".sfi");
  name_[at] = 0;
  return IntelSetup::NeedFirmware;
}

// Fragments of up to 252 bytes, each acknowledged by a command complete.
bool IntelBluetooth::secure_send(uint8_t fragment_type, ConstByteView data) {
  for (uint64_t done = 0; done < data.size();) {
    const uint32_t n = static_cast<uint32_t>(data.size() - done < 252 ? data.size() - done : 252);
    uint8_t params[253];
    params[0] = fragment_type;
    for (uint32_t i = 0; i < n; ++i) params[1 + i] = data[done + i];
    if (controller_.command(kOpIntelSecureSend, ConstByteView(params, n + 1), 3000) != HciResult::Ok) return false;
    done += n;
  }
  return true;
}

bool IntelBluetooth::wait_flag(const volatile bool* flag, uint32_t timeout_ms) {
  const uint64_t limit = uint64_t{timeout_ms} * 10;
  for (uint64_t round = 0; round < limit && !*flag; ++round)
    if (controller_.poll() == 0) delay_us(100);
  return *flag;
}

IntelSetup IntelBluetooth::load_firmware(ConstByteView image) {
  if (operational()) return IntelSetup::AlreadyRunning;
  if (name_[0] == 0) return IntelSetup::Failed;                          // identify() has not found a bootloader
  if (image.size() < kSfiHeader) return IntelSetup::BadFirmware;
  // The boot address is in the image's Write Boot Params command (0xFC0E), which the Linux driver finds by walking the commands of the payload; the payload is a run of HCI commands.
  uint32_t boot_address = 0;
  bool found = false;
  for (ByteReader walk(image.dropped(kSfiHeader)); walk.remaining() >= 3 && !found;) {
    const uint16_t op = walk.le16();
    const uint8_t length = walk.u8();
    const ConstByteView body = walk.take(length);
    if (!walk.ok()) return IntelSetup::BadFirmware;                      // a command that runs past the end of the file
    if (op == kWriteBootParams) {
      if (body.size() < 4) return IntelSetup::BadFirmware;
      boot_address = body[0] | (uint32_t{body[1]} << 8) | (uint32_t{body[2]} << 16) | (uint32_t{body[3]} << 24);
      found = true;
    }
  }
  if (!found) return IntelSetup::BadFirmware;

  download_done_ = download_failed_ = booted_ = false;
  controller_.set_event_hook(&IntelBluetooth::hook, this);
  struct Unhook { HciController& c; ~Unhook() { c.set_event_hook(nullptr, nullptr); } } unhook{controller_};

  if (!secure_send(kFragmentInit, image.subview(0, kCssHeader)) || !secure_send(kFragmentPublicKey, image.subview(kPublicKeyAt, kPublicKey)) ||
      !secure_send(kFragmentSign, image.subview(kSignatureAt, kSignature)))
    return IntelSetup::Failed;
  // The payload goes in as data fragments that end on a 4 byte boundary: the file has Intel NOP commands where the commands need padding to get there.
  uint32_t fragment_start = 0, fragment = 0;
  const ConstByteView all = image.dropped(kSfiHeader);
  while (fragment_start < all.size()) {
    fragment = 0;
    bool aligned = false;
    while (!aligned) {
      if (fragment_start + fragment + 3 > all.size()) return IntelSetup::BadFirmware;
      const uint32_t plen = all[fragment_start + fragment + 2];
      fragment += 3 + plen;
      if (fragment_start + fragment > all.size()) return IntelSetup::BadFirmware;
      aligned = fragment % 4 == 0;
    }
    if (!secure_send(kFragmentData, all.subview(fragment_start, fragment))) return IntelSetup::Failed;
    fragment_start += fragment;
  }
  if (!wait_flag(&download_done_, 5000)) return IntelSetup::Timeout;
  if (download_failed_) return IntelSetup::Failed;
  const uint8_t reset[8] = {0x00, 0x01, 0x00, 0x01, static_cast<uint8_t>(boot_address), static_cast<uint8_t>(boot_address >> 8), static_cast<uint8_t>(boot_address >> 16),
                            static_cast<uint8_t>(boot_address >> 24)};
  if (controller_.command(kOpIntelReset, ConstByteView(reset, 8), 3000) != HciResult::Ok) return IntelSetup::Failed;
  if (!wait_flag(&booted_, 5000)) return IntelSetup::Timeout;
  version_.fw_variant = 0x23;
  return IntelSetup::Ok;
}

}  // namespace hardware
