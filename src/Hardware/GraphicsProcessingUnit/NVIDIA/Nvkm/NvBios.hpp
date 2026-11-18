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

// The NVIDIA video BIOS: finding the image (nouveau's nvkm/subdev/bios/shadow*.c) and reading the tables the rest of the driver needs from it
// (bit.c, dcb.c, i2c.c, gpio.c, conn.c, image.c, pcir.c, npde.c). The image is untrusted input: every offset and length is checked, and a
// table that runs past the image is cut where it stops (BoardInfo::truncated).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace nv {

// --- Fetching the image ------------------------------------------------------------------------------------------------------------------

// The places an image can come from, in nouveau's order (nvbios_shadow; Open Firmware, which comes first there, does not exist on these
// machines).
enum class BiosSource : uint8_t { None, Pramin, Prom, AcpiFast, AcpiSlow, PciRom, Platform };
constexpr uint32_t kBiosSourceCount = 6;
const char* bios_source_name(BiosSource source);

// The ACPI _ROM method of the GPU's device (ACPI 6, "_ROM (Get ROM Data)": _ROM(offset, length) returns a buffer of the ROM's bytes). The ACPI
// layer supplies it; AmlNvAcpiRom finds it in the namespace.
class INvAcpiRom {
 public:
  // Fills `out` with the bytes from `offset` on; returns how many it wrote (fewer than asked when the method returns a short buffer, 0 if
  // it failed or returned something that is not a buffer).
  [[nodiscard]] virtual uint32_t read_rom(uint32_t offset, ByteView out) = 0;

 protected:
  INvAcpiRom() = default;
  ~INvAcpiRom() = default;
};

// The _ROM method of the device whose _ADR is the GPU's (device << 16 | function), through the AML interpreter (the interpreter's lock is
// held for each call and the copy out of its result). Mirrors AmdVbios::read_from_atrm's namespace walk, but takes no device whose _ADR does
// not match: _ROM is a standard name that other devices may have too.
class AmlNvAcpiRom final : public INvAcpiRom {
 public:
  // False if no such device has a _ROM method.
  [[nodiscard]] bool find(acpi::AmlInterpreter& aml, PciAddress function);
  bool valid() const { return method_ != nullptr; }
  [[nodiscard]] uint32_t read_rom(uint32_t offset, ByteView out) override;

 private:
  acpi::AmlInterpreter* aml_ = nullptr;
  acpi::Node* method_ = nullptr;
};

struct NvBiosSources {
  INvRegisters* registers = nullptr;   // BAR0: PRAMIN and PROM
  Chip chip;                           // which of nouveau's rules apply
  INvAcpiRom* acpi_rom = nullptr;      // the GPU's _ROM method, if it has one (laptops whose discrete GPU has no ROM of its own)
  IPciBus* pci = nullptr;              // the expansion ROM BAR
  PciAddress function{};
  IRegisterMapper* mapper = nullptr;   // mapping the expansion ROM window and the legacy shadow
  bool boot_display = false;           // the firmware's boot display: its video BIOS is the one shadowed at 0xC0000
  uint16_t device_id = 0;              // the PCI device ID: the legacy shadow's PCI data structure must name it (0: not checked)
  allocators::IAllocator* allocator = nullptr;
};

struct NvBiosAttempt {
  BiosSource source = BiosSource::None;
  bool tried = false;                  // nouveau's skip rule left it out when false (see fetch_bios)
  uint32_t score = 0;                  // nouveau's shadow_image score: 0 unusable, 4 per image with a good checksum, 2-3 when it is bad
  const char* reason = "not tried";    // why it was not usable, or "ok" / "checksum failed"
};

struct NvBiosFetchReport {
  NvBiosAttempt attempts[kBiosSourceCount];
  BiosSource chosen = BiosSource::None;
  uint32_t score = 0;
};

// What one image of a ROM chain is (nvbios_image).
struct NvBiosImageHeader {
  uint32_t base = 0, size = 0;
  uint8_t type = 0;                    // 0x00 x86 (the one with the tables), 0x03 EFI, 0x70 NVIDIA (no NPDE) ...
  bool last = false;
  uint16_t vendor_id = 0, device_id = 0;
};

class NvBiosShadow {
 public:
  static constexpr uint32_t kMaxImageBytes = 0x100000;    // the PRAMIN/PROM windows are 1 MiB; nouveau's readers refuse anything past it
  static constexpr uint32_t kMaxImages = 8;
  static constexpr uint32_t kFetchGranule = 0x1000;       // reads are made in 4 KiB pieces (the ACPI method's page)
  static constexpr uint16_t kNvidiaVendor = 0x10DE;
  // Registers (BAR0 offsets).
  static constexpr uint32_t kPraminWindow = 0x001700, kPraminBase = 0x700000, kPromBase = 0x300000;
  static constexpr uint32_t kDispFuseGf100 = 0x022500, kDispFuseGm100 = 0x021c04;
  static constexpr uint32_t kVbiosWindowNv50 = 0x619f04, kVbiosWindowGv100 = 0x625f04;
  static constexpr uint32_t kPciMirrorNv04 = 0x001800, kPciMirrorNv40 = 0x088000, kPciRomShadow = 0x50;
  // The PCI expansion ROM base register.
  static constexpr uint32_t kRomAddressRegister = 0x30, kRomEnable = 1, kRomAddressMask = 0xFFFFF800, kMaxRomWindowBytes = 16u << 20;
  static constexpr uint64_t kLegacyShadowBase = 0xC0000, kLegacyShadowBytes = 0x20000;

  // nvbios_imagen for the image at `base` of `data`: the ROM signature (0xAA55, and nouveau's 0xBB77 and "VN"), the PCI data structure
  // ("PCIR", "RGIS", "NPDS"), the NPDE extension's size and last flag (not for type 0x70). nullptr if it is good, otherwise why not.
  [[nodiscard]] static const char* parse_image_header(ConstByteView data, uint32_t base, NvBiosImageHeader* out);
  // nvbios_checksum: the bytes of an x86 image add up to 0.
  [[nodiscard]] static bool checksum_ok(ConstByteView image);

  // nvbios_shadow: every source is scored as nouveau scores it, and the best image is copied into `out` (the chain of images, up to the end
  // of the last good one). A source with a skip value is only tried while the best score so far is below it (ACPI one 4 KiB page at a time
  // only when nothing scored 4; the expansion ROM and the legacy shadow only when nothing was found). `report` (may be nullptr) says what
  // each source gave. None if no source has a usable image.
  [[nodiscard]] static BiosSource fetch(const NvBiosSources& sources, ByteBuffer* out, NvBiosFetchReport* report);
};

// --- Reading the tables ------------------------------------------------------------------------------------------------------------------

struct BitEntry {
  uint8_t id = 0, version = 0;
  uint16_t length = 0, offset = 0;
};

// An image (a view: the bytes must outlive it) with its BIT and BMP structures found.
class NvBios {
 public:
  NvBios() = default;
  explicit NvBios(ConstByteView image);
  ConstByteView data() const { return data_; }

  uint32_t bit_offset() const { return bit_offset_; }     // 0: no BIT (pre-NV40-era boards have the BMP)
  uint32_t bmp_offset() const { return bmp_offset_; }
  uint16_t bmp_version() const { return bmp_version_; }   // major << 8 | minor, 0 without a BMP
  // bit_entry: the BIT table's entry for `id` ('i' info, 'I' init, 'd' DP, 'P' perf, 'M' memory, 'p' falcon ucode ...). False if there is none.
  [[nodiscard]] bool bit_entry(uint8_t id, BitEntry* out) const;

  // Checked reads of the image (nvbios_rd08/16/32): false when the bytes are not all inside it.
  [[nodiscard]] bool rd08(uint64_t offset, uint8_t* out) const;
  [[nodiscard]] bool rd16(uint64_t offset, uint16_t* out) const;
  [[nodiscard]] bool rd32(uint64_t offset, uint32_t* out) const;

  static constexpr uint32_t kDcbSignature = 0x4edcbdcb;

 private:
  ConstByteView data_;
  uint32_t bit_offset_ = 0, bmp_offset_ = 0;
  uint16_t bmp_version_ = 0;
};

struct BoardParseResult {
  bool has_bit = false, has_bmp = false;
  bool has_dcb = false;              // a DCB with a valid header was found
  const char* dcb_problem = nullptr; // why there is none (or why it holds nothing usable)
};

// Fills `board` from the image: the version (BIT 'i', or the BMP), the DCB header and output entries (2.x to 4.1), the I2C table (DCB 1.5
// to 4.1, falling back to the BMP's two fixed ports), the GPIO table (2.x to 4.1) and the connector table (DCB 3.0+ with a long enough
// header). Never reads outside the image.
[[nodiscard]] BoardParseResult parse_board(const NvBios& bios, const Chip& chip, BoardInfo* board);

}  // namespace nv
}  // namespace hardware
