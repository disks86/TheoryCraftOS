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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// Finding the video BIOS image of an AMD card, for the AtomBIOS interpreter (AtomBios) that POSTs it. The sources and their checks follow the Linux amdgpu driver's amdgpu_bios.c, in
// the order of amdgpu_get_bios_dgpu: the card's ACPI ATRM method (an AML method of the card's device that returns the ROM a page at a time), the ACPI VFCT table (the firmware's copy of the images of the video BIOSes of its AMD cards, what most laptops with a discrete card use), the copy at the start of video memory, the legacy shadow at 0xC0000, the PCI
// expansion ROM, and the card's own ROM read through the SMC's ROM index and data registers (the Volcanic Islands cards' amdgpu_read_bios_from_rom). Every image found must pass
// check_atom_bios: the 0xAA55 signature, a pointer at 0x48 to a header that has "ATOM" (or "MOTA") at offset 4.
//
// The last resort is the expansion ROM switched on again when the firmware left it disabled (read_from_disabled_rom). Not done: the copy in video memory (for SR-IOV).
class AmdVbios {
 public:
  static constexpr uint32_t kSignatureOffset = 0x30;       // AMD_VBIOS_SIGNATURE_OFFSET
  static constexpr uint32_t kMaxImageBytes = 255 * 512;    // the length is a byte of 512-byte units
  static constexpr uint32_t kMaxRomWindowBytes = 8u << 20; // the biggest expansion ROM window read
  static constexpr uint32_t kRomAddressRegister = 0x30;    // PCI_ROM_ADDRESS
  static constexpr uint32_t kRomEnable = 1;                // PCI_ROM_ADDRESS_ENABLE
  static constexpr uint32_t kRomAddressMask = 0xFFFFF800;  // PCI_ROM_ADDRESS_MASK
  // vi_read_bios_from_rom: the SMC indirect index and data registers (mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11) and the ROM's index and data (ixROM_INDEX, ixROM_DATA)
  static constexpr uint32_t kSmcIndex = 0x1AC, kSmcData = 0x1AD;
  static constexpr uint32_t kRomIndex = 0xc0600010, kRomData = 0xc0600014;

  enum class Source : uint8_t { None, Atrm, Vfct, Vram, LegacyShadow, RomBar, RomRegisters, DisabledRom };
  static const char* source_name(Source source);

  // check_atom_bios
  static bool is_atom_bios(ConstByteView image);

  // amdgpu_acpi_vfct_bios: `table` is the whole VFCT table. Returns the image of the card at `function` with this vendor and device ID, as a view into the table (empty if there is none,
  // the table is broken, or the image is not an AtomBIOS image).
  static ConstByteView find_in_vfct(ConstByteView table, PciAddress function, uint16_t vendor_id, uint16_t device_id);

  // amdgpu_atrm_get_bios: the ATRM method (ATRM(offset, length) returns a buffer of the ROM's bytes) of a device in the ACPI namespace, called for 4096 byte pages from 0 until one comes back
  // short, into an image of kAtrmBiosBytes (zeros where nothing came). The driver takes the first display device that has the method; this prefers the device whose _ADR is the card's
  // (device << 16 | function) and then takes any, in the order of the namespace. A page that comes back longer than asked, or not a buffer, ends the reading (the driver copies it
  // whatever its length). False if there is no such method or the image is no good. `aml` has the tables loaded.
  static constexpr uint32_t kAtrmBiosPage = 4096, kAtrmBiosBytes = 256 * 1024;
  static bool read_from_atrm(acpi::AmlInterpreter& aml, PciAddress function, allocators::IAllocator& allocator, ByteBuffer* out);

  // amdgpu_read_bios_from_vram: a card that has been POSTed has the video BIOS image at the start of video memory (the memory BAR, BAR 0), 256 KiB of it (the driver's guess at the size). Only
  // read when the card needs no POST (amdgpu_device_need_post for CIK and later is amdgpu_atombios_scratch_need_asic_init: BIOS_SCRATCH_7 has ATOM_S7_ASIC_INIT_COMPLETE), as a card that
  // has not been POSTed has nothing there. Needs a BAR of at least the size (the driver maps 256 KiB whatever the BAR says). False if not, the range cannot be mapped or the image is no good.
  static constexpr uint32_t kBiosScratch7 = 0x5d0, kAsicInitComplete = 0x200;   // mmBIOS_SCRATCH_7, ATOM_S7_ASIC_INIT_COMPLETE_MASK
  static constexpr uint32_t kVramBiosBytes = 256 * 1024;
  static bool read_from_vram(IVcnRegisters& registers, IRegisterMapper& mapper, uint64_t bar0_physical, uint64_t bar0_bytes, allocators::IAllocator& allocator, ByteBuffer* out);

  // amdgpu_read_platform_bios: the video BIOS the firmware shadowed at the legacy address 0xC0000, 128 KiB of it (what the PCI core sets as the ROM of the boot display, `pdev->rom` and
  // `pdev->romlen`, in pci_fixup_video). The driver relies on the PCI core to say that this is the card's; there is no such knowledge here, so the image must also say it is this card's:
  // the PCI data structure it points to at 0x18 has the vendor and device ID asked for. That is stricter than the driver, and a shadow of another card (the integrated graphics, which is
  // normally the boot display of a laptop with a second card) is refused. False if the range cannot be mapped or the image is no good.
  static constexpr uint64_t kLegacyShadowBase = 0xC0000, kLegacyShadowBytes = 0x20000;
  static bool read_from_legacy_shadow(IRegisterMapper& mapper, allocators::IAllocator& allocator, uint16_t vendor_id, uint16_t device_id, ByteBuffer* out);

  // amdgpu_read_disabled_bios with vi_read_disabled_bios: a card whose firmware switched its ROM off. BUS_CNTL.BIOS_ROM_DIS is cleared, the VGA modes of the first two display controllers and
  // VGA_RENDER_CONTROL's status select are cleared (only if the card has display controllers: `display_engine`), and ROM_CNTL (an SMC register) gets SCK_OVERWRITE, then the expansion
  // ROM is read as read_from_rom_bar does and every register put back (the same order as the driver, whatever the result).
  static constexpr uint32_t kBusCntl = 0x1508, kD1VgaControl = 0xcc, kD2VgaControl = 0xce, kVgaRenderControl = 0xc0, kRomCntl = 0xc0600000;
  static constexpr uint32_t kBiosRomDis = 0x2, kVgaModeEnable = 0x1, kVgaTimingSelect = 0x100, kVgaVstatusCntl = 0x30000, kSckOverwrite = 0x2;
  static bool read_from_disabled_rom(IVcnRegisters& registers, bool display_engine, IPciBus& pci, PciAddress function, IRegisterMapper& mapper, allocators::IAllocator& allocator, ByteBuffer* out);

  // The vendor and device ID in the PCI data structure of an expansion ROM image (false if it has none).
  static bool rom_ids(ConstByteView image, uint16_t* vendor_id, uint16_t* device_id);

  // pci_get_rom_size: how much of an expansion ROM window is images - the images are chained, each says how long it is in 512 byte units and whether it is the last. 0 if the first is not an image.
  static uint64_t rom_size(ConstByteView window);

  // amdgpu_read_bios (pci_map_rom): switches the card's expansion ROM on at the address its ROM base register holds, maps the window (its size from the usual probe of the register), copies
  // the images, switches the ROM off, and checks the result. False if the card has no ROM base assigned, the window cannot be mapped (the mapper refuses it) or the image is no good.
  static bool read_from_rom_bar(IPciBus& pci, PciAddress function, IRegisterMapper& mapper, allocators::IAllocator& allocator, ByteBuffer* out);

  // amdgpu_read_bios_from_rom with vi_read_bios_from_rom: through the ROM index and data registers of the SMC. The header is read first and checked (signature, the AMD string), then the
  // image whose length it says.
  static bool read_from_rom_registers(IVcnRegisters& registers, allocators::IAllocator& allocator, ByteBuffer* out);

  struct Sources {
    acpi::AmlInterpreter* aml = nullptr; // the machine's ACPI tables loaded, for the ATRM method
    ConstByteView vfct;                  // the VFCT table, if the machine has one
    IPciBus* pci = nullptr;              // for the expansion ROM
    IRegisterMapper* mapper = nullptr;
    IVcnRegisters* registers = nullptr;  // the card's, for the ROM read through the SMC and for the disabled ROM path
    uint64_t bar0_physical = 0, bar0_bytes = 0;   // the card's memory BAR, for the copy in video memory
    bool display_engine = true;          // the card has display controllers (the disabled ROM path clears their VGA modes)
    allocators::IAllocator* allocator = nullptr;   // for the copies made while reading (the ROM sources need it)
    PciAddress function{};
    uint16_t vendor_id = 0, device_id = 0;
  };
  // amdgpu_get_bios_dgpu, for the sources above: the first that gives an AtomBIOS image, copied into `out`.
  static Source find(const Sources& sources, ByteBuffer* out);
};

}  // namespace hardware
