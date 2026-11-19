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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuProbe.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVbios.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04;
constexpr uint32_t kMemorySpace = 1u << 1;
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kMaxVideoMemory = 128ull << 30;   // more than any Radeon this probes; a bigger answer is not the register it should be
constexpr int32_t kSmu7MaximumReading = 255;          // SMU7_THERMAL_MAXIMUM_TEMP_READING
constexpr int32_t kNaviHighest = 150;                 // the Navi reading is believed up to this (the driver does not read it)

// atomfirmware.h
constexpr uint32_t kNumberOfStrings = 0x2F;   // OFFSET_TO_GET_ATOMBIOS_NUMBER_OF_STRINGS
constexpr uint32_t kStringStart = 0x6E;       // OFFSET_TO_GET_ATOMBIOS_STRING_START
constexpr uint32_t kPartNumber = 0x80;        // OFFSET_TO_VBIOS_PART_NUMBER
constexpr char kAtomPrefix[] = "ATOMBIOS";    // BIOS_ATOM_PREFIX
constexpr uint32_t kPartNumberLength = 43;    // BIOS_STRING_LENGTH

uint8_t byte_at(ConstByteView image, uint64_t offset) {
  uint8_t b = 0;
  return image.try_get(offset, &b) ? b : 0;
}

uint32_t le16_at(ConstByteView image, uint64_t offset) { return byte_at(image, offset) | (uint32_t{byte_at(image, offset + 1)} << 8); }

void clear(ArrayView<char> out) {
  if (!out.empty()) out[0] = 0;
}

// atom_find_str_in_rom(ctx, BIOS_ATOM_PREFIX, 3, 1024, 64): the first offset in [3, 1024] where the prefix is; false if none (or the image ends first).
bool find_atom_prefix(ConstByteView image, uint64_t* at) {
  constexpr uint64_t kLength = sizeof(kAtomPrefix) - 1;
  for (uint64_t start = 3; start <= 1024; ++start) {
    ConstByteView here;
    if (!image.try_subview(start, kLength, &here)) return false;
    bool same = true;
    for (uint64_t i = 0; i < kLength && same; ++i) same = here[i] == static_cast<uint8_t>(kAtomPrefix[i]);
    if (same) {
      *at = start;
      return true;
    }
  }
  return false;
}
}  // namespace

AmdGpuProbe::Layout AmdGpuProbe::layout_of(uint16_t device_id) {
  const AmdGpuInfo info = amd_gpu_identify(device_id);
  if (!info.discrete) return Layout::None;
  switch (info.architecture) {
    case AmdGpuArchitecture::Polaris: return Layout::Vi;   // (Vega M is a Polaris-family chip)
    case AmdGpuArchitecture::Vega: return device_id >= 0x66A0 && device_id <= 0x66AF ? Layout::Vega20 : Layout::Vega10;
    case AmdGpuArchitecture::Rdna1:
    case AmdGpuArchitecture::Rdna2: return Layout::Navi;
    case AmdGpuArchitecture::Rdna3:
    case AmdGpuArchitecture::Rdna4: return Layout::Discovery;
    default: return Layout::None;   // Instinct (CDNA) and the APUs' graphics
  }
}

int32_t AmdGpuProbe::smu7_temperature(uint32_t value) {
  if (value == 0xFFFFFFFFu) return -1;
  const uint32_t temp = (value & kCtfTempMask) >> kCtfTempShift;
  // "Bit 9 means the reading is lower than the lowest usable value" (the field is 9 bits wide, so this never happens; kept as the driver has it).
  const int32_t degrees = (temp & 0x200) ? kSmu7MaximumReading : static_cast<int32_t>(temp & 0x1FF);
  return degrees == 0 ? -1 : degrees * 1000;
}

int32_t AmdGpuProbe::thm_temperature(uint32_t value) {
  if (value == 0xFFFFFFFFu) return -1;
  const int32_t degrees = static_cast<int32_t>(((value & kCtfTempMask) >> kCtfTempShift) & 0x1FF);
  return degrees == 0 ? -1 : degrees * 1000;
}

uint64_t AmdGpuProbe::memsize_bytes(uint32_t raw, bool vi) {
  if (raw == 0xFFFFFFFFu) return 0;
  // gmc_v8_0_mc_init: "some boards may have garbage in the upper 16 bits"
  if (vi && (raw & 0xFFFF0000u) && (raw & 0xFFFF)) raw &= 0xFFFF;
  const uint64_t bytes = uint64_t{raw} * kMiB;
  return bytes > kMaxVideoMemory ? 0 : bytes;
}

void AmdGpuProbe::vbios_part_number(ConstByteView image, ArrayView<char> out) {
  clear(out);
  if (out.empty() || image.empty()) return;
  uint64_t at = byte_at(image, kNumberOfStrings) != 0 ? le16_at(image, kStringStart) : kPartNumber;
  bool found = at < image.size();
  if (!found || image[at] == 0) found = find_atom_prefix(image, &at);   // (the driver goes on with a null pointer when the prefix is missing: no string)
  if (!found) return;
  if (image[at] == 0) ++at;
  uint64_t n = 0;
  for (; n < kPartNumberLength && n + 1 < out.size() && at + n < image.size(); ++n) {
    const uint8_t c = image[at + n];
    if (c < ' ' || c > 'z') break;
    out[n] = static_cast<char>(c);
  }
  out[n] = 0;
}

void AmdGpuProbe::vbios_name(ConstByteView image, ArrayView<char> out) {
  clear(out);
  if (out.empty() || image.size() <= kStringStart + 1) return;
  const uint32_t strings = byte_at(image, kNumberOfStrings);
  if (strings == 0) return;
  uint64_t c = le16_at(image, kStringStart);
  if (c >= image.size()) return;
  // Past the strings (usually four: part number, ASIC, PCI type, memory type), then the 0x0D 0x0A.
  for (uint32_t i = 0; i < strings; ++i) {
    while (c < image.size() && image[c] != 0) ++c;
    ++c;
  }
  c += 2;
  if (c >= image.size()) return;
  // strnlen(c, STRLEN_LONG - 1), the trailing spaces taken off. (A character outside printable ASCII also ends it here: the name goes to the log.)
  uint64_t n = 0;
  for (; n + 1 < out.size() && n < kNameChars - 1 && c + n < image.size(); ++n) {
    const uint8_t ch = image[c + n];
    if (ch < ' ' || ch > '~') break;
    out[n] = static_cast<char>(ch);
  }
  while (n > 0 && out[n - 1] == ' ') --n;
  out[n] = 0;
}

AmdGpuProbe::AmdGpuProbe(const GpuContext& context, allocators::IAllocator& allocator)
    : context_(context), allocator_(allocator), device_id_(context.device.device_id), info_(amd_gpu_identify(device_id_)),
      layout_(layout_of(device_id_)), vbios_(allocator) {}

AmdGpuProbe::AmdGpuProbe(IVcnRegisters& registers, uint16_t device_id, allocators::IAllocator& allocator, ConstByteView vbios)
    : allocator_(allocator), device_id_(device_id), info_(amd_gpu_identify(device_id)), layout_(layout_of(device_id)), registers_(&registers),
      vbios_(allocator) {
  vbios_given_ = !vbios.empty() && vbios_.append(vbios);
}

bool AmdGpuProbe::map_registers() {
  if (registers_) return true;
  if (!context_.registers || !context_.pci) return fail("no register mapper");
  const PciAddress address = context_.device.address;
  if (!(context_.pci->read32(address, kPciCommand) & kMemorySpace)) return fail("memory decoding is off");
  const uint64_t bar5 = context_.pci->memory_bar(address, 5);
  MmioRegion window;
  const uint64_t bytes = layout_ == Layout::Vi ? kViRegisterBytes : kSoc15RegisterBytes;
  if (bar5 == 0 || !context_.registers->map_registers(bar5, bytes, &window)) return fail("BAR 5 not mapped");
  mmio_ = MmioVcnRegisters(window);
  registers_ = &mmio_;
  return true;
}

uint32_t AmdGpuProbe::read_memsize() { return registers_->read(layout_ == Layout::Vi ? kConfigMemsize : kRccConfigMemsize); }

uint32_t AmdGpuProbe::smc_read(uint32_t address) {
  // vi_smc_rreg
  registers_->write(kSmcIndex, address);
  return registers_->read(kSmcData);
}

void AmdGpuProbe::read_vbios() {
  if (!vbios_given_) {
    vbios_.clear();
    // vi_read_bios_from_rom. (The later generations read the ROM through SMUIO registers whose place differs by version: not done, see docs/MultiGpu.md.)
    if (layout_ == Layout::Vi && !AmdVbios::read_from_rom_registers(*registers_, allocator_, &vbios_)) vbios_.clear();
  }
  vbios_part_number(vbios_.view(), ArrayView<char>(part_number_));
  vbios_name(vbios_.view(), ArrayView<char>(vbios_name_));
}

bool AmdGpuProbe::start() {
  started_ = false;
  if (layout_ == Layout::None) return fail("not a discrete Radeon this probe knows");
  if (!map_registers()) return false;
  const uint32_t memsize = read_memsize();
  if (memsize == 0xFFFFFFFFu) return fail("the GPU does not answer (powered down?)");
  vram_bytes_ = memsize_bytes(memsize, layout_ == Layout::Vi);
  read_vbios();
  status_ = vram_bytes_ ? "identified" : "identified; the memory size reads 0 (not POSTed?)";
  started_ = true;
  return true;
}

bool AmdGpuProbe::resume() {
  if (!started_) return fail("not started");
  const uint32_t memsize = read_memsize();
  if (memsize == 0xFFFFFFFFu) return fail("the GPU does not answer (powered down?)");
  // After a power cut nothing POSTs the card again, so the register may read 0: the size from the first look stays.
  const uint64_t bytes = memsize_bytes(memsize, layout_ == Layout::Vi);
  if (bytes) {
    vram_bytes_ = bytes;
    status_ = "identified";
  } else {
    status_ = "identified; the memory size reads 0 since the power cut (not POSTed)";
  }
  return true;
}

int32_t AmdGpuProbe::temperature_millicelsius() {
  if (!started_) return -1;
  switch (layout_) {
    case Layout::Vi: return smu7_temperature(smc_read(kSmcCgMultThermalStatus));
    case Layout::Vega10: return thm_temperature(registers_->read(kThm9CgMultThermalStatus));
    case Layout::Vega20: return thm_temperature(registers_->read(kThm11CgMultThermalStatus));
    case Layout::Navi: {
      const int32_t t = thm_temperature(registers_->read(kThm11CgMultThermalStatus));
      return t > kNaviHighest * 1000 ? -1 : t;
    }
    default: return -1;
  }
}

}  // namespace hardware
