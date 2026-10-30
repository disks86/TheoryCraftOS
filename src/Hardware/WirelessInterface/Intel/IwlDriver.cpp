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


#include "Hardware/WirelessInterface/Intel/IwlDriver.hpp"
#include "Utility/String.hpp"

namespace hardware {

namespace {
constexpr uint64_t kMaxFirmwareBytes = 4u << 20;
constexpr uint16_t kPciCommand = 0x04;
}  // namespace

bool FileSystemFirmwareSource::read(const char* name, ByteBuffer& out) {
  String path(allocator_);
  if (!path.assign("/firmware/") || !path.append(StringView(name))) return false;
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(path.c_str()), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxFirmwareBytes) return false;
  out.clear();
  if (!out.put_zeros(size)) return false;
  uint64_t got = 0;
  if (file->read(0, out.writable_view(), &got) != filesystem::Status::Ok || got != size) {
    out.clear();
    return false;
  }
  return true;
}

const char* IwlDriver::outcome_name(Outcome outcome) {
  switch (outcome) {
    case Outcome::NotStarted: return "not started";
    case Outcome::Started: return "running";
    case Outcome::UnknownDevice: return "not a Wireless-AC 9000 series card";
    case Outcome::BarUnmapped: return "the card's registers cannot be mapped";
    case Outcome::UnsupportedHardware: return "no firmware for this hardware revision";
    case Outcome::FirmwareMissing: return "firmware file not found in /firmware";
    case Outcome::FirmwareInvalid: return "firmware file is not valid";
    case Outcome::StartFailed: return "the card did not start";
    case Outcome::InitFailed: return "the card's initialisation failed";
  }
  return "?";
}

IwlDriver::Outcome IwlDriver::fail(Outcome outcome, const char* message) {
  stop();
  outcome_ = outcome;
  message_ = message ? message : "";
  return outcome;
}

IwlDriver::Outcome IwlDriver::start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper, IIwlFirmwareSource& firmware) {
  const IwlPciDevice* info = iwl_find_pci_device(device.vendor_id, device.device_id);
  if (!info) return fail(Outcome::UnknownDevice, "");
  // memory space and bus mastering (the card writes the receive rings); the status bits above the command register clear when written, so only the low half is kept
  const uint32_t command = pci.read32(device.address, kPciCommand) & 0xFFFF;
  pci.write32(device.address, kPciCommand, command | (1u << 1) | (1u << 2));
  const uint64_t base = pci.memory_bar(device.address, 0);
  MmioRegion region;
  if (!base || !mapper.map_registers(base, kRegisterBytes, &region)) return fail(Outcome::BarUnmapped, "");
  mmio_registers_ = UniquePointer<MmioIwlRegisters>::make(allocator_, region);
  if (!mmio_registers_) return fail(Outcome::StartFailed, "out of memory");
  return start_device(*mmio_registers_, info->integrated, firmware);
}

IwlDriver::Outcome IwlDriver::start_device(IIwlRegisters& registers, bool integrated, IIwlFirmwareSource& source) {
  transport_ = UniquePointer<IwlTransport>();
  nvm_ = IwlNvmData();
  firmware_name_[0] = 0;
  IwlPcieDevice probe(registers);
  if (!probe.present()) return fail(Outcome::StartFailed, "the card does not answer");
  char prefix[48];
  if (!iwl_firmware_prefix(probe.hw_rev(), probe.hw_rf_id(), prefix, sizeof prefix)) return fail(Outcome::UnsupportedHardware, "");
  // the newest file the folder has
  bool found = false;
  for (uint32_t api = kIwl9000FirmwareApiMax; api >= kIwl9000FirmwareApiMin && !found; --api) {
    char name[64];
    if (!iwl_firmware_file_name(prefix, api, name, sizeof name)) continue;
    if (!source.read(name, file_)) continue;
    found = true;
    for (uint32_t i = 0; i < sizeof name && i < sizeof firmware_name_; ++i) firmware_name_[i] = name[i];
    firmware_name_[sizeof firmware_name_ - 1] = 0;
  }
  if (!found) return fail(Outcome::FirmwareMissing, "");
  const char* error = nullptr;
  if (!firmware_.parse(file_.view(), &error)) return fail(Outcome::FirmwareInvalid, error);
  transport_ = UniquePointer<IwlTransport>::make(allocator_, registers, allocator_, integrated);
  if (!transport_) return fail(Outcome::StartFailed, "out of memory");
  if (!transport_->start(firmware_, IwlImage::Regular, kTimeoutUs, &error)) return fail(Outcome::StartFailed, error);
  if (!iwl_run_unified_init(*transport_, firmware_, &nvm_, kTimeoutUs, &error)) return fail(Outcome::InitFailed, error);
  outcome_ = Outcome::Started;
  message_ = "";
  return outcome_;
}

void IwlDriver::stop() {
  if (transport_) transport_->stop();
  transport_ = UniquePointer<IwlTransport>();
  mmio_registers_ = UniquePointer<MmioIwlRegisters>();
  if (outcome_ == Outcome::Started) outcome_ = Outcome::NotStarted;
}

}  // namespace hardware
