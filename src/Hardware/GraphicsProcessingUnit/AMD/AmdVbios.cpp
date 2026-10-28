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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdVbios.hpp"

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): amdgpu_bios.c (check_atom_bios, amdgpu_atrm_get_bios, amdgpu_acpi_vfct_bios, amdgpu_read_bios,
// amdgpu_read_bios_from_rom, amdgpu_get_bios_dgpu), vi.c (vi_read_bios_from_rom), atombios.h (UEFI_ACPI_VFCT, VFCT_IMAGE_HEADER), and the PCI core's pci_get_rom_size.
// scripts/check_vbios_port.py checks the numbers against them.

namespace hardware {

namespace {
constexpr char kVbiosSignature[] = " 761295520";   // AMD_VBIOS_SIGNATURE
constexpr uint8_t kAtom[4] = {'A', 'T', 'O', 'M'}, kMota[4] = {'M', 'O', 'T', 'A'};
constexpr uint8_t kSignatureBytes[10] = {' ', '7', '6', '1', '2', '9', '5', '5', '2', '0'};
constexpr uint32_t kVfctHeaderBytes = 0x4C;         // sizeof(UEFI_ACPI_VFCT): the ACPI header (36), the UUID (16), the two image offsets and 16 reserved bytes
constexpr uint32_t kVfctVbiosOffsetField = 0x34;    // VBIOSImageOffset
constexpr uint32_t kVfctImageHeaderBytes = 28;      // sizeof(VFCT_IMAGE_HEADER)

uint16_t le16(ConstByteView v, uint64_t at) { return static_cast<uint16_t>(v[at] | (v[at + 1] << 8)); }
uint32_t le32(ConstByteView v, uint64_t at) { return v[at] | (v[at + 1] << 8) | (v[at + 2] << 16) | (uint32_t{v[at + 3]} << 24); }
}  // namespace

const char* AmdVbios::source_name(Source source) {
  switch (source) {
    case Source::None: return "none";
    case Source::Atrm: return "ACPI ATRM method";
    case Source::Vfct: return "ACPI VFCT";
    case Source::Vram: return "the copy in video memory";
    case Source::LegacyShadow: return "legacy shadow at 0xC0000";
    case Source::RomBar: return "PCI expansion ROM";
    case Source::RomRegisters: return "the card's ROM registers";
    case Source::DisabledRom: return "the expansion ROM, switched on again";
  }
  return "unknown";
}

bool AmdVbios::is_atom_bios(ConstByteView image) {
  if (image.size() < 0x49) return false;                         // vbios mem is null or mem size is wrong
  if (image[0] != 0x55 || image[1] != 0xAA) return false;        // BIOS signature incorrect
  const uint32_t header = image[0x48] | (image[0x49] << 8);
  if (header == 0) return false;                                 // Can't locate bios header
  const uint64_t tmp = uint64_t{header} + 4;
  if (image.size() < tmp) return false;                          // BIOS header is broken
  if (image.size() - tmp < 4) return false;                      // (the driver compares 4 bytes at tmp whatever the size)
  const ConstByteView magic = image.subview(tmp, 4);
  return magic.equals(ConstByteView(kAtom, 4)) || magic.equals(ConstByteView(kMota, 4));
}

ConstByteView AmdVbios::find_in_vfct(ConstByteView table, PciAddress function, uint16_t vendor_id, uint16_t device_id) {
  const uint64_t tbl_size = table.size();
  if (tbl_size < kVfctHeaderBytes) return ConstByteView();     // present but broken (too short)
  uint64_t offset = le32(table, kVfctVbiosOffsetField);
  while (offset < tbl_size) {
    const uint64_t header_at = offset;
    offset += kVfctImageHeaderBytes;
    if (offset > tbl_size) return ConstByteView();             // image header truncated
    const uint32_t bus = le32(table, header_at), device = le32(table, header_at + 4), fn = le32(table, header_at + 8);
    const uint16_t vendor = le16(table, header_at + 12), dev_id = le16(table, header_at + 14);
    const uint32_t length = le32(table, header_at + 24);
    offset += length;
    if (offset > tbl_size) return ConstByteView();             // image truncated
    if (length && bus == function.bus && device == function.device && fn == function.function && vendor == vendor_id && dev_id == device_id) {
      const ConstByteView image = table.subview(offset - length, length);
      return is_atom_bios(image) ? image : ConstByteView();
    }
  }
  return ConstByteView();
}

bool AmdVbios::rom_ids(ConstByteView image, uint16_t* vendor_id, uint16_t* device_id) {
  if (image.size() < 26 || le16(image, 0) != 0xAA55) return false;
  const uint64_t pds = le16(image, 24);
  if (pds + 8 > image.size() || le32(image, pds) != 0x52494350) return false;   // "PCIR"
  *vendor_id = le16(image, pds + 4);
  *device_id = le16(image, pds + 6);
  return true;
}

bool AmdVbios::read_from_vram(IVcnRegisters& registers, IRegisterMapper& mapper, uint64_t bar0_physical, uint64_t bar0_bytes, allocators::IAllocator& allocator, ByteBuffer* out) {
  if (!(registers.read(kBiosScratch7) & kAsicInitComplete)) return false;   // the card needs a POST: nothing is there
  if (bar0_physical == 0 || bar0_bytes < kVramBiosBytes) return false;     // FB BAR not enabled
  MmioRegion region;
  if (!mapper.map_registers(bar0_physical, kVramBiosBytes, &region)) return false;
  ByteBuffer copy(allocator);
  for (uint64_t at = 0; at < kVramBiosBytes; at += 4)
    if (!copy.put32(region.read32(at))) return false;
  if (!is_atom_bios(copy.view())) return false;
  out->clear();
  return out->append(copy.view());
}

bool AmdVbios::read_from_legacy_shadow(IRegisterMapper& mapper, allocators::IAllocator& allocator, uint16_t vendor_id, uint16_t device_id, ByteBuffer* out) {
  MmioRegion region;
  if (!mapper.map_registers(kLegacyShadowBase, kLegacyShadowBytes, &region)) return false;
  ByteBuffer copy(allocator);
  for (uint64_t at = 0; at < kLegacyShadowBytes; at += 4)
    if (!copy.put32(region.read32(at))) return false;
  uint16_t vendor = 0, device = 0;
  if (!rom_ids(copy.view(), &vendor, &device) || vendor != vendor_id || device != device_id) return false;
  if (!is_atom_bios(copy.view())) return false;
  out->clear();
  return out->append(copy.view());
}

uint64_t AmdVbios::rom_size(ConstByteView window) {
  uint64_t size = 0, image = 0;
  uint32_t length = 0;
  bool last = false;
  do {
    if (image + 26 > window.size()) break;
    if (le16(window, image) != 0xAA55) break;                  // Invalid PCI ROM header signature
    const uint32_t pds = static_cast<uint32_t>(image) + le16(window, image + 24);
    if (uint64_t{pds} + 22 > window.size()) break;
    if (le32(window, pds) != 0x52494350) break;                // "PCIR": Invalid PCI ROM data signature
    last = (window[pds + 21] & 0x80) != 0;
    length = le16(window, pds + 16);
    image += uint64_t{length} * 512;
    size += uint64_t{length} * 512;
  } while (length && !last && size < window.size());
  return size < window.size() ? size : window.size();         // never more than the window
}

bool AmdVbios::read_from_rom_bar(IPciBus& pci, PciAddress function, IRegisterMapper& mapper, allocators::IAllocator& allocator, ByteBuffer* out) {
  const uint32_t original = pci.read32(function, kRomAddressRegister);
  const uint64_t base = original & kRomAddressMask;
  if (base == 0) return false;                                 // no ROM base assigned
  // The size: the bits of the base register that stick when it is written with ones (the usual probe), the register put back.
  pci.write32(function, kRomAddressRegister, kRomAddressMask);
  const uint32_t sized = pci.read32(function, kRomAddressRegister) & kRomAddressMask;
  pci.write32(function, kRomAddressRegister, original & ~kRomEnable);
  if (sized == 0) return false;
  const uint64_t window_bytes = (~uint64_t{sized} + 1) & 0xFFFFFFFFull;
  if (window_bytes == 0 || window_bytes > kMaxRomWindowBytes) return false;
  MmioRegion region;
  if (!mapper.map_registers(base, window_bytes, &region)) return false;
  pci.write32(function, kRomAddressRegister, static_cast<uint32_t>(base) | kRomEnable);   // pci_enable_rom
  ByteBuffer copy(allocator);
  bool copied = true;
  for (uint64_t at = 0; at < window_bytes && copied; at += 4) copied = copy.put32(region.read32(at));
  pci.write32(function, kRomAddressRegister, static_cast<uint32_t>(base));               // pci_disable_rom
  if (!copied) return false;
  const uint64_t size = rom_size(copy.view());
  if (size == 0) return false;
  copy.truncate(size);
  if (!is_atom_bios(copy.view())) return false;
  out->clear();
  return out->append(copy.view());
}

bool AmdVbios::read_from_rom_registers(IVcnRegisters& registers, allocators::IAllocator& allocator, ByteBuffer* out) {
  // vi_read_bios_from_rom: set the ROM index to 0, then read the data register for the dwords in a row (the index advances by itself)
  auto read = [&](uint32_t bytes, ByteBuffer* into) {
    registers.write(kSmcIndex, kRomIndex);
    registers.write(kSmcData, 0);
    registers.write(kSmcIndex, kRomData);
    const uint32_t dwords = (bytes + 3) / 4;
    for (uint32_t i = 0; i < dwords; ++i)
      if (!into->put32(registers.read(kSmcData))) return false;
    return true;
  };
  // the header: AMD_VBIOS_SIGNATURE_END + 1 bytes
  ByteBuffer header(allocator);
  const uint32_t header_bytes = kSignatureOffset + sizeof(kVbiosSignature) + 1;
  if (!read(header_bytes, &header)) return false;
  const ConstByteView h = header.view();
  if (h[0] != 0x55 || h[1] != 0xAA) return false;
  if (!h.subview(kSignatureOffset, sizeof(kSignatureBytes)).equals(ConstByteView(kSignatureBytes, sizeof(kSignatureBytes)))) return false;
  const uint32_t length = uint32_t{h[2]} << 9;
  const uint32_t aligned = (length + 3) & ~3u;
  if (aligned < 0x4A || aligned > kMaxImageBytes) return false;
  ByteBuffer image(allocator);
  if (!read(aligned, &image)) return false;
  if (!is_atom_bios(image.view())) return false;
  out->clear();
  return out->append(image.view());
}

namespace {
constexpr uint32_t kMaxAtrmWalk = 4096, kMaxAtrmDepth = 16;

// A device of the namespace with a method called ATRM of its own
acpi::Node* atrm_method_of(acpi::AmlInterpreter& aml, acpi::Node* device) {
  for (acpi::Node* n = aml.first_child(device); n; n = aml.next_sibling(n)) {
    const char* name = aml.name_of(n);
    if (aml.is_method(n) && name[0] == 'A' && name[1] == 'T' && name[2] == 'R' && name[3] == 'M') return n;
  }
  return nullptr;
}

// The ATRM method of the first device (in namespace order) that has one and, if `adr` is given, whose _ADR is it
acpi::Node* find_atrm(acpi::AmlInterpreter& aml, acpi::Node* node, uint32_t depth, bool match_adr, uint64_t adr, uint32_t* visited) {
  for (acpi::Node* n = aml.first_child(node); n; n = aml.next_sibling(n)) {
    if (++*visited > kMaxAtrmWalk) return nullptr;
    if (!aml.is_device(n)) {
      if (depth < kMaxAtrmDepth) {   // (scopes hold devices too)
        if (acpi::Node* found = find_atrm(aml, n, depth + 1, match_adr, adr, visited)) return found;
      }
      continue;
    }
    if (acpi::Node* method = atrm_method_of(aml, n)) {
      uint64_t device_adr = 0;
      acpi::Node* adr_node = aml.find("_ADR", n);
      const bool has_adr = adr_node && aml.parent(adr_node) == n && aml.evaluate_integer(adr_node, &device_adr);
      if (!match_adr || (has_adr && device_adr == adr)) return method;
    }
    if (depth < kMaxAtrmDepth) {
      if (acpi::Node* found = find_atrm(aml, n, depth + 1, match_adr, adr, visited)) return found;
    }
  }
  return nullptr;
}
}  // namespace

bool AmdVbios::read_from_atrm(acpi::AmlInterpreter& aml, PciAddress function, allocators::IAllocator& allocator, ByteBuffer* out) {
  uint32_t visited = 0;
  const uint64_t adr = (uint64_t{function.device} << 16) | function.function;
  acpi::Node* method = find_atrm(aml, aml.root(), 0, true, adr, &visited);
  if (!method) {
    visited = 0;
    method = find_atrm(aml, aml.root(), 0, false, 0, &visited);
  }
  if (!method) return false;
  ByteBuffer image(allocator);
  if (!image.put_zeros(kAtrmBiosBytes)) return false;
  const ByteView bytes = image.writable_view();
  for (uint32_t page = 0; page < kAtrmBiosBytes / kAtrmBiosPage; ++page) {
    acpi::Value arguments[2];
    arguments[0].kind = acpi::ValueKind::Integer;
    arguments[0].integer = uint64_t{page} * kAtrmBiosPage;
    arguments[1].kind = acpi::ValueKind::Integer;
    arguments[1].integer = kAtrmBiosPage;
    acpi::Value result;
    if (!aml.evaluate(method, arguments, 2, &result)) break;                     // failed to evaluate ATRM
    if (result.kind != acpi::ValueKind::Buffer || result.length > kAtrmBiosPage) break;
    if (result.length != 0) bytes.subview(uint64_t{page} * kAtrmBiosPage, result.length).copy_from(ConstByteView(result.bytes, result.length));
    if (result.length < kAtrmBiosPage) break;
  }
  if (!is_atom_bios(image.view())) return false;
  out->clear();
  return out->append(image.view());
}

bool AmdVbios::read_from_disabled_rom(IVcnRegisters& registers, bool display_engine, IPciBus& pci, PciAddress function, IRegisterMapper& mapper, allocators::IAllocator& allocator, ByteBuffer* out) {
  auto smc_read = [&](uint32_t reg) {   // vi_smc_rreg
    registers.write(kSmcIndex, reg);
    return registers.read(kSmcData);
  };
  auto smc_write = [&](uint32_t reg, uint32_t value) {   // vi_smc_wreg
    registers.write(kSmcIndex, reg);
    registers.write(kSmcData, value);
  };
  const uint32_t bus_cntl = registers.read(kBusCntl);
  uint32_t d1vga = 0, d2vga = 0, vga_render = 0;
  if (display_engine) {
    d1vga = registers.read(kD1VgaControl);
    d2vga = registers.read(kD2VgaControl);
    vga_render = registers.read(kVgaRenderControl);
  }
  const uint32_t rom_cntl = smc_read(kRomCntl);

  // enable the rom
  registers.write(kBusCntl, bus_cntl & ~kBiosRomDis);
  if (display_engine) {
    // Disable VGA mode
    registers.write(kD1VgaControl, d1vga & ~(kVgaModeEnable | kVgaTimingSelect));
    registers.write(kD2VgaControl, d2vga & ~(kVgaModeEnable | kVgaTimingSelect));
    registers.write(kVgaRenderControl, vga_render & ~kVgaVstatusCntl);
  }
  smc_write(kRomCntl, rom_cntl | kSckOverwrite);

  const bool found = read_from_rom_bar(pci, function, mapper, allocator, out);

  // restore regs
  registers.write(kBusCntl, bus_cntl);
  if (display_engine) {
    registers.write(kD1VgaControl, d1vga);
    registers.write(kD2VgaControl, d2vga);
    registers.write(kVgaRenderControl, vga_render);
  }
  smc_write(kRomCntl, rom_cntl);
  return found;
}

AmdVbios::Source AmdVbios::find(const Sources& s, ByteBuffer* out) {
  if (s.aml && s.allocator && read_from_atrm(*s.aml, s.function, *s.allocator, out)) return Source::Atrm;
  if (s.vfct.size() != 0) {
    const ConstByteView image = find_in_vfct(s.vfct, s.function, s.vendor_id, s.device_id);
    if (image.size() != 0) {
      out->clear();
      if (out->append(image)) return Source::Vfct;
    }
  }
  if (!s.allocator) return Source::None;
  if (s.registers && s.mapper && read_from_vram(*s.registers, *s.mapper, s.bar0_physical, s.bar0_bytes, *s.allocator, out)) return Source::Vram;
  if (s.mapper && read_from_legacy_shadow(*s.mapper, *s.allocator, s.vendor_id, s.device_id, out)) return Source::LegacyShadow;
  if (s.pci && s.mapper && read_from_rom_bar(*s.pci, s.function, *s.mapper, *s.allocator, out)) return Source::RomBar;
  if (s.registers && read_from_rom_registers(*s.registers, *s.allocator, out)) return Source::RomRegisters;
  if (s.registers && s.pci && s.mapper && read_from_disabled_rom(*s.registers, s.display_engine, *s.pci, s.function, *s.mapper, *s.allocator, out)) return Source::DisabledRom;
  return Source::None;
}

}  // namespace hardware
