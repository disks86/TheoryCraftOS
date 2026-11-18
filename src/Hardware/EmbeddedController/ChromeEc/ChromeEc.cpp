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

#include "Hardware/EmbeddedController/ChromeEc/ChromeEc.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace chromeec;

namespace {

constexpr uint32_t kHelloIn = 0xA0B0C0D0u, kHelloAdd = 0x01020304u;

// Text of a fixed-size field the EC filled: up to the first NUL, anything unprintable shown as '?'.
template <uint64_t N>
void copy_text(ConstByteView field, FixedString<N>* out) {
  out->clear();
  for (uint8_t c : field) {
    if (c == 0 || out->full()) break;
    const char ch = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    (void)out->append(StringView(&ch, 1));
  }
}

bool refused_version(uint16_t result) {
  return result == static_cast<uint16_t>(EcResult::InvalidVersion) || result == static_cast<uint16_t>(EcResult::InvalidParam) ||
         result == static_cast<uint16_t>(EcResult::InvalidCommand);
}

}  // namespace

uint8_t EcBatteryInfo::percent() const {
  if (full_charge_mah == 0) return 0;
  const uint64_t p = uint64_t{remaining_mah} * 100 / full_charge_mah;
  return static_cast<uint8_t>(p > 100 ? 100 : p);
}

// The transaction lock: the mutex, then the firmware's lock.
struct ChromeEc::Lock {
  explicit Lock(ChromeEc& ec) : ec_(ec) {
    ec_.mutex_.lock();
    fw_ = ec_.firmware_lock_;
    if (fw_) fw_->lock();
  }
  ~Lock() {
    if (fw_) fw_->unlock();
    ec_.mutex_.unlock();
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  ChromeEc& ec_;
  IEcFirmwareLock* fw_ = nullptr;
};

void ChromeEc::set_firmware_lock(IEcFirmwareLock* lock) {
  kernel::ScopedMutex guard(mutex_);
  firmware_lock_ = lock;
}

EcReply ChromeEc::command_locked(uint16_t command, uint8_t version, ConstByteView params, ByteView response) {
  EcReply reply;
  if (params.size() > max_params_) {
    reply.transfer = EcTransferStatus::RequestTooLarge;
    return reply;
  }
  uint8_t packet[kHostPacketSize];
  const uint32_t length = build_request(command, version, params, ByteView(packet));
  if (length == 0) {
    reply.transfer = EcTransferStatus::RequestTooLarge;
    return reply;
  }
  const EcTransfer t = transport_.transfer(ConstByteView(packet).first(length), response.at_most(max_response_));
  reply.transfer = t.status;
  reply.result = t.result;
  reply.data_bytes = t.data_bytes;
  return reply;
}

EcReply ChromeEc::command(uint16_t command, uint8_t version, ConstByteView params, ByteView response) {
  Lock lock(*this);
  return command_locked(command, version, params, response);
}

bool ChromeEc::read_memmap(uint32_t offset, ByteView out) {
  Lock lock(*this);
  return transport_.read_memmap(offset, out);
}

bool ChromeEc::start() {
  started_ = false;
  uint8_t id[2] = {};
  if (!read_memmap(memmap::kId, ByteView(id)) || id[0] != 'E' || id[1] != 'C') {
    problem_ = "no \"EC\" signature in the memory map";
    return false;
  }
  uint8_t flags = 0, thermal = 0;
  if (!read_memmap(memmap::kHostCommandFlags, ByteView(&flags, 1)) || !(flags & memmap::kHostCommandFlagVersion3)) {
    problem_ = "the controller does not offer host command protocol 3";
    return false;
  }
  const Optional<uint32_t> echo = hello(kHelloIn);
  if (!echo || *echo != kHelloIn + kHelloAdd) {
    problem_ = "the controller did not answer the hello command";
    return false;
  }
  if (Optional<EcProtocolInfo> info = read_protocol_info()) {
    protocol_ = *info;
    // (sizes the EC claims are believed only where they make the limits smaller)
    if (info->max_request_bytes > kRequestHeaderBytes && info->max_request_bytes - kRequestHeaderBytes < max_params_)
      max_params_ = info->max_request_bytes - kRequestHeaderBytes;
    if (info->max_response_bytes > kResponseHeaderBytes && info->max_response_bytes - kResponseHeaderBytes < max_response_)
      max_response_ = info->max_response_bytes - kResponseHeaderBytes;
  }
  if (Optional<EcVersionInfo> v = read_version()) version_ = *v;
  if (Optional<EcChipInfo> c = read_chip_info()) chip_ = *c;
  if (Optional<uint64_t> f = read_features()) features_ = *f;
  if (read_memmap(memmap::kThermalVersion, ByteView(&thermal, 1))) thermal_version_ = thermal;
  started_ = true;
  problem_ = nullptr;
  return true;
}

// --- Commands --------------------------------------------------------------------------------------------------------------------------------

Optional<uint32_t> ChromeEc::hello(uint32_t value) {
  uint8_t in[4], out[4] = {};
  ByteWriter w{ByteView(in)};
  w.le32(value);
  const EcReply r = command(command::kHello, 0, ConstByteView(in), ByteView(out));
  if (!r.ok() || r.data_bytes < 4) return {};
  return ByteReader(ConstByteView(out)).le32();
}

Optional<EcVersionInfo> ChromeEc::read_version() {
  uint8_t out[100] = {};
  const EcReply r = command(command::kGetVersion, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 64) return {};
  EcVersionInfo v;
  copy_text(ConstByteView(out).subview(0, 32), &v.read_only);
  copy_text(ConstByteView(out).subview(32, 32), &v.read_write);
  if (r.data_bytes >= 100) {
    const uint32_t image = ByteReader(ConstByteView(out).subview(96, 4)).le32();
    v.current = image == 1 ? EcImage::ReadOnly : image == 2 ? EcImage::ReadWrite : EcImage::Unknown;
  }
  return v;
}

Optional<EcChipInfo> ChromeEc::read_chip_info() {
  uint8_t out[96] = {};
  const EcReply r = command(command::kGetChipInfo, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 96) return {};
  EcChipInfo c;
  copy_text(ConstByteView(out).subview(0, 32), &c.vendor);
  copy_text(ConstByteView(out).subview(32, 32), &c.name);
  copy_text(ConstByteView(out).subview(64, 32), &c.revision);
  return c;
}

Optional<EcProtocolInfo> ChromeEc::read_protocol_info() {
  uint8_t out[12] = {};
  const EcReply r = command(command::kGetProtocolInfo, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 12) return {};
  ByteReader in{ConstByteView(out)};
  EcProtocolInfo p;
  p.versions = in.le32();
  p.max_request_bytes = in.le16();
  p.max_response_bytes = in.le16();
  p.flags = in.le32();
  return p;
}

Optional<uint64_t> ChromeEc::read_features() {
  uint8_t out[8] = {};
  const EcReply r = command(command::kGetFeatures, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 4) return {};
  ByteReader in{ConstByteView(out).first(r.data_bytes >= 8 ? 8 : 4)};
  const uint64_t low = in.le32();
  const uint64_t high = r.data_bytes >= 8 ? in.le32() : 0;
  return low | (high << 32);
}

Optional<uint32_t> ChromeEc::command_versions(uint16_t which) {
  uint8_t out[4] = {};
  uint8_t v1[2];
  ByteWriter w{ByteView(v1)};
  w.le16(which);
  EcReply r = command(command::kGetCommandVersions, 1, ConstByteView(v1), ByteView(out));
  if (r.transfer == EcTransferStatus::Ok && refused_version(r.result) && which < 0x100) {
    const uint8_t v0 = static_cast<uint8_t>(which);
    r = command(command::kGetCommandVersions, 0, ConstByteView(&v0, 1), ByteView(out));
  }
  if (!r.ok() || r.data_bytes < 4) return {};
  return ByteReader(ConstByteView(out)).le32();
}

uint8_t ChromeEc::best_version(uint16_t which, uint8_t highest) {
  uint32_t mask = 0;
  bool cached = false;
  {
    kernel::ScopedMutex guard(mutex_);
    for (const VersionCache& v : versions_)
      if (v.used && v.command == which) {
        mask = v.mask;
        cached = true;
        break;
      }
  }
  if (!cached) {
    mask = command_versions(which).value_or(1u);   // (an EC that cannot say: assume version 0 only)
    kernel::ScopedMutex guard(mutex_);
    for (VersionCache& v : versions_)
      if (!v.used) {
        v = VersionCache{which, mask, true};
        break;
      }
  }
  for (int32_t version = highest; version > 0; --version)
    if (mask & (1u << version)) return static_cast<uint8_t>(version);
  return 0;
}

Optional<uint32_t> ChromeEc::fan_target_rpm() {
  uint8_t out[4] = {};
  const EcReply r = command(command::kPwmGetFanTargetRpm, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 4) return {};
  return ByteReader(ConstByteView(out)).le32();
}

bool ChromeEc::set_fan_target_rpm(Optional<uint8_t> fan, uint32_t rpm) {
  uint8_t in[5];
  ByteWriter w{ByteView(in)};
  w.le32(rpm);
  uint8_t version = 0;
  if (fan) {
    if (best_version(command::kPwmSetFanTargetRpm, 1) < 1) return false;
    w.u8(*fan);
    version = 1;
  }
  return command(command::kPwmSetFanTargetRpm, version, ConstByteView(w.written()), ByteView()).ok();
}

bool ChromeEc::set_fan_duty(Optional<uint8_t> fan, uint8_t percent) {
  if (percent > 100) return false;
  uint8_t in[5];
  ByteWriter w{ByteView(in)};
  w.le32(percent);
  uint8_t version = 0;
  if (fan) {
    if (best_version(command::kPwmSetFanDuty, 1) < 1) return false;
    w.u8(*fan);
    version = 1;
  }
  return command(command::kPwmSetFanDuty, version, ConstByteView(w.written()), ByteView()).ok();
}

bool ChromeEc::set_fan_automatic(Optional<uint8_t> fan) {
  if (!fan) return command(command::kThermalAutoFanControl, 0, ConstByteView(), ByteView()).ok();
  if (best_version(command::kThermalAutoFanControl, 1) < 1) return false;
  const uint8_t index = *fan;
  return command(command::kThermalAutoFanControl, 1, ConstByteView(&index, 1), ByteView()).ok();
}

Optional<EcKeyboardBacklight> ChromeEc::keyboard_backlight() {
  uint8_t out[2] = {};
  const EcReply r = command(command::kPwmGetKeyboardBacklight, 0, ConstByteView(), ByteView(out));
  if (!r.ok() || r.data_bytes < 2 || out[0] > 100) return {};
  EcKeyboardBacklight k;
  k.percent = out[0];
  k.enabled = out[1] != 0;
  return k;
}

bool ChromeEc::set_keyboard_backlight(uint8_t percent) {
  if (percent > 100) return false;
  return command(command::kPwmSetKeyboardBacklight, 0, ConstByteView(&percent, 1), ByteView()).ok();
}

Optional<EcLedRange> ChromeEc::led_range(EcLed led) {
  uint8_t in[2 + kLedColorCount] = {static_cast<uint8_t>(led), kLedFlagQuery};
  uint8_t out[kLedColorCount] = {};
  const EcReply r = command(command::kLedControl, 1, ConstByteView(in), ByteView(out));
  if (!r.ok() || r.data_bytes < kLedColorCount) return {};
  EcLedRange range;
  ArrayView<uint8_t>(range.max).copy_from(ConstByteView(out));
  return range;
}

bool ChromeEc::set_led(EcLed led, const uint8_t (&brightness)[kLedColorCount]) {
  uint8_t in[2 + kLedColorCount] = {static_cast<uint8_t>(led), 0};
  ByteView(in).subview(2).copy_from(ConstByteView(brightness));
  uint8_t out[kLedColorCount];
  return command(command::kLedControl, 1, ConstByteView(in), ByteView(out)).ok();
}

bool ChromeEc::set_led_automatic(EcLed led) {
  uint8_t in[2 + kLedColorCount] = {static_cast<uint8_t>(led), kLedFlagAuto};
  uint8_t out[kLedColorCount];
  return command(command::kLedControl, 1, ConstByteView(in), ByteView(out)).ok();
}

Optional<EcChargeState> ChromeEc::charge_state() {
  uint8_t in[9] = {};   // cmd 0 (GET_STATE) and the unused union
  uint8_t out[20] = {};
  const EcReply r = command(command::kChargeState, 0, ConstByteView(in), ByteView(out));
  if (!r.ok() || r.data_bytes < 20) return {};
  ByteReader rd{ConstByteView(out)};
  EcChargeState s;
  s.ac = rd.le32() != 0;
  s.charge_voltage_mv = rd.le32();
  s.charge_current_ma = rd.le32();
  s.input_current_ma = rd.le32();
  s.state_of_charge = rd.le32();
  return s;
}

bool ChromeEc::temperature_sensor_name(uint8_t sensor, FixedString<33>* out) {
  uint8_t response[33] = {};
  const EcReply r = command(command::kTempSensorGetInfo, 0, ConstByteView(&sensor, 1), ByteView(response));
  if (!r.ok() || r.data_bytes < 32) return false;
  copy_text(ConstByteView(response).first(32), out);
  return !out->empty();
}

// --- The memory map --------------------------------------------------------------------------------------------------------------------------

Optional<EcTemperatureReading> ChromeEc::temperature(uint32_t sensor) {
  uint32_t offset = 0;
  if (sensor < memmap::kTempSensorEntries) offset = memmap::kTempSensor + sensor;
  else if (sensor < kMaxTemperatureSensors && thermal_version_ >= 2) offset = memmap::kTempSensorB + (sensor - memmap::kTempSensorEntries);
  else return {};
  uint8_t raw = 0;
  if (!read_memmap(offset, ByteView(&raw, 1))) return {};
  EcTemperatureReading t;
  if (raw == memmap::kTempNotPresent) return t;
  t.present = true;
  if (raw >= memmap::kTempNotCalibrated) return t;   // error, not powered, not calibrated
  t.valid = true;
  t.millicelsius = (static_cast<int32_t>(raw) + memmap::kTempOffsetKelvin) * 1000 - 273150;
  return t;
}

Optional<uint32_t> ChromeEc::fan_rpm(uint32_t fan) {
  if (fan >= kMaxFans) return {};
  uint8_t raw[2] = {};
  if (!read_memmap(memmap::kFan + 2 * fan, ByteView(raw))) return {};
  const uint16_t rpm = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
  if (rpm == memmap::kFanNotPresent) return {};
  if (rpm == memmap::kFanStalled) return 0u;
  return uint32_t{rpm};
}

Optional<EcBatteryInfo> ChromeEc::battery() {
  uint8_t version = 0;
  if (!read_memmap(memmap::kBatteryVersion, ByteView(&version, 1)) || version == 0) return {};
  uint8_t block[0x30] = {};   // 0x40..0x6F
  if (!read_memmap(memmap::kBatteryVoltage, ByteView(block))) return {};
  ByteReader r{ConstByteView(block)};
  EcBatteryInfo b;
  b.voltage_mv = r.le32();
  b.rate_ma = r.le32();
  b.remaining_mah = r.le32();
  const uint8_t flags = r.u8();
  r.skip(3);
  b.design_mah = r.le32();
  b.design_mv = r.le32();
  b.full_charge_mah = r.le32();
  b.cycle_count = r.le32();
  copy_text(r.take(8), &b.manufacturer);
  copy_text(r.take(8), &b.model);
  if (!r.ok()) return {};
  b.ac_present = flags & memmap::kBatteryAcPresent;
  b.present = flags & memmap::kBatteryPresent;
  b.discharging = flags & memmap::kBatteryDischarging;
  b.charging = flags & memmap::kBatteryCharging;
  b.critical = flags & memmap::kBatteryCritical;
  return b;
}

Optional<uint8_t> ChromeEc::switches() {
  uint8_t version = 0, value = 0;
  if (!read_memmap(memmap::kSwitchesVersion, ByteView(&version, 1)) || version == 0) return {};
  if (!read_memmap(memmap::kSwitches, ByteView(&value, 1))) return {};
  return value;
}

// --- Detection -------------------------------------------------------------------------------------------------------------------------------

namespace {
bool plain_signature(IChromeEcPorts& ports, uint16_t base) {
  return ports.in8(static_cast<uint16_t>(base + memmap::kId)) == 'E' && ports.in8(static_cast<uint16_t>(base + memmap::kId + 1)) == 'C';
}
}  // namespace

EcDetection detect_chrome_ec(IChromeEcPorts& ports, bool acpi_id_present) {
  EcDetection d;
  // Reads only.
  const bool at_900 = plain_signature(ports, kPortMemmap);
  const bool at_e00 = !at_900 && plain_signature(ports, kPortFrameworkMemmap);
  if (!acpi_id_present && !at_900 && !at_e00) {
    d.reason = "no Chrome EC device in the ACPI tables and no memory map signature";
    return d;
  }
  if (at_900) {   // the map at its usual place as plain I/O: an LPC EC (a MEC keeps the window behind its EMI)
    d.found = true;
    d.kind = EcTransportKind::Lpc;
    d.lpc_memmap_base = kPortMemmap;
    d.reason = "LPC, memory map at 0x900";
    return d;
  }
  // The controller is identified: the EMI address registers may be written to look for the map behind them.
  MecEmi emi(ports);
  uint8_t id[2] = {};
  if (emi.read(kPortMemmap - kMecWindowStart + memmap::kId, ByteView(id)) && id[0] == 'E' && id[1] == 'C') {
    d.found = true;
    d.kind = EcTransportKind::Mec;
    d.mec_plain_memmap_base = 0;
    d.lpc_memmap_base = at_e00 ? kPortFrameworkMemmap : 0;   // (the fallback should the EMI answer turn out to be chance)
    d.reason = "Microchip EMI, memory map behind it";
    return d;
  }
  if (at_e00) {
    d.found = true;
    d.kind = EcTransportKind::Lpc;
    d.lpc_memmap_base = kPortFrameworkMemmap;
    d.reason = "LPC, memory map at 0xE00";
    return d;
  }
  d.reason = "the ACPI tables name a Chrome EC but no memory map answers";
  return d;
}

}  // namespace hardware
