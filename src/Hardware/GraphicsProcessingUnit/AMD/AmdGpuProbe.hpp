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

// The probe of a discrete Radeon that has no display driver here (a hybrid laptop's second GPU, see Multi/GpuProbe.hpp): which chip it is, how much video
// memory it has, its temperature, and the part number and name strings of its video BIOS. Everything is read the way the Linux amdgpu driver reads it, from the
// register BAR (BAR 5):
//
// - Video memory, in MiB: CONFIG_MEMSIZE (dword 0x150A, bif_5_0_d.h; gmc_v8_0_mc_init, with its "garbage in the upper 16 bits" rule) on Polaris and Vega M;
//   RCC_CONFIG_MEMSIZE (NBIO segment 2 at 0xD20 + 0xC3 = dword 0xDE3; nbio_v6_1/v7_4/v2_3/v4_3_get_memsize, and amdgpu_discovery.c reads the same dword for every
//   ASIC with an IP discovery table) on Vega, RDNA 1, 2, 3 and 4. The video BIOS writes it while it POSTs the card, so it reads 0 on a card nobody POSTed.
// - Temperature: CG_MULT_THERMAL_STATUS.CTF_TEMP (bits 17:9, whole degrees). On Polaris and Vega M through the SMC's index and data registers (SMC_IND_INDEX_11 /
//   SMC_IND_DATA_11, ixCG_MULT_THERMAL_STATUS 0xC0300014; smu7_thermal_get_temperature); on Vega 10/12 in the THM block at 0x16600 + 0x5A (thm_9_0_offset.h;
//   vega10/vega12_thermal_get_temperature); on Vega 20 at 0x16600 + 0x5F (thm_11_0_2_offset.h; vega20_thermal_get_temperature). RDNA 1 and 2 have the same THM
//   block as Vega 20 (navi10_ppt.c and sienna_cichlid_ppt.c include thm_11_0_2), but the driver takes their temperature from the SMU's metrics table, which
//   needs a firmware message: the register is read here anyway and the reading is only believed between 1 and 150 degrees. RDNA 3 and 4 put the THM block where
//   the IP discovery table says and report temperature only through the SMU: -1.
// - The video BIOS, on Polaris and Vega M from the card's ROM through the SMC (AmdVbios::read_from_rom_registers, vi_read_bios_from_rom), or an image the caller
//   hands in (Inputs::vbios): the part number (atom_get_vbios_pn) and the name string (atom_get_vbios_name).
//
// The only writes are to the SMC's index register (and the ROM index register behind it) of a Polaris or Vega M: index registers that select what the next read
// returns, nothing a power cut could lose that matters. resume() needs nothing set up again; it checks the card answers and reads the memory size again.
// One caller at a time (the GpuPool's): the SMC index and data pair is not locked.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuProbe.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

class AmdGpuProbe final : public IGpuProbe {
 public:
  // Where the registers this probe reads are, by generation.
  enum class Layout : uint8_t {
    None,     // not a discrete GPU this probe knows
    Vi,       // Polaris, Vega M: CONFIG_MEMSIZE, temperature through the SMC
    Vega10,   // Vega 10, Vega 12: RCC_CONFIG_MEMSIZE, THM (thm 9.0)
    Vega20,   // Vega 20 (Radeon VII): RCC_CONFIG_MEMSIZE, THM (thm 11.0.2)
    Navi,     // RDNA 1, RDNA 2: RCC_CONFIG_MEMSIZE, THM (thm 11.0.2, not what the driver reads)
    Discovery // RDNA 3, RDNA 4: RCC_CONFIG_MEMSIZE only
  };
  static Layout layout_of(uint16_t device_id);
  static bool supported(uint16_t device_id) { return layout_of(device_id) != Layout::None; }

  static constexpr uint32_t kConfigMemsize = 0x150A;        // mmCONFIG_MEMSIZE (bif_5_0_d.h)
  static constexpr uint32_t kRccConfigMemsize = 0xDE3;      // NBIO segment 2 (0xD20) + mmRCC_CONFIG_MEMSIZE (0xC3)
  static constexpr uint32_t kSmcIndex = 0x1AC, kSmcData = 0x1AD;   // mmSMC_IND_INDEX_11, mmSMC_IND_DATA_11 (smu_7_1_3_d.h)
  static constexpr uint32_t kSmcCgMultThermalStatus = 0xC0300014;  // ixCG_MULT_THERMAL_STATUS (smu_7_1_3_d.h)
  static constexpr uint32_t kThmBase = 0x16600;             // THM_BASE segment 0 (vega10/vega20/navi10/sienna_cichlid_ip_offset.h)
  static constexpr uint32_t kThm9CgMultThermalStatus = kThmBase + 0x5A;    // thm_9_0_offset.h
  static constexpr uint32_t kThm11CgMultThermalStatus = kThmBase + 0x5F;   // thm_11_0_2_offset.h
  static constexpr uint32_t kCtfTempMask = 0x3FE00, kCtfTempShift = 9;     // CG_MULT_THERMAL_STATUS__CTF_TEMP
  static constexpr uint64_t kViRegisterBytes = 256 * 1024;    // the register BAR of a Polaris
  static constexpr uint64_t kSoc15RegisterBytes = 512 * 1024; // of a Vega or later (THM is at 0x59800)

  // The conversions, in thousandths of a degree (-1: no reading).
  static int32_t smu7_temperature(uint32_t cg_mult_thermal_status);   // smu7_thermal_get_temperature
  static int32_t thm_temperature(uint32_t cg_mult_thermal_status);    // vega10/vega20_thermal_get_temperature
  // CONFIG_MEMSIZE / RCC_CONFIG_MEMSIZE as bytes (0: none, or a value no card has).
  static uint64_t memsize_bytes(uint32_t raw, bool vi);

  // The strings of an AtomBIOS image, as amdgpu's atom.c finds them. Empty strings when the image has none; at most kPartNumberChars / kNameChars - 1 characters.
  static constexpr uint32_t kPartNumberChars = 44, kNameChars = 64;   // BIOS_STRING_LENGTH + 1, STRLEN_LONG
  static void vbios_part_number(ConstByteView image, ArrayView<char> out);   // atom_get_vbios_pn
  static void vbios_name(ConstByteView image, ArrayView<char> out);          // atom_get_vbios_name

  // On the machine: start() maps BAR 5 of `context.device` through `context.registers`.
  AmdGpuProbe(const GpuContext& context, allocators::IAllocator& allocator);
  // With the registers given (a test's model, or a window the caller mapped); `vbios`, if not empty, is copied and used instead of the ROM read.
  AmdGpuProbe(IVcnRegisters& registers, uint16_t device_id, allocators::IAllocator& allocator, ConstByteView vbios = ConstByteView());
  ~AmdGpuProbe() = default;
  AmdGpuProbe(const AmdGpuProbe&) = delete;
  AmdGpuProbe& operator=(const AmdGpuProbe&) = delete;

  const char* name() const override { return info_.name; }
  const char* status() const override { return status_; }
  [[nodiscard]] bool start() override;
  [[nodiscard]] bool resume() override;
  uint64_t vram_bytes() const override { return vram_bytes_; }
  int32_t temperature_millicelsius() override;

  Layout layout() const { return layout_; }
  const AmdGpuInfo& info() const { return info_; }
  const char* vbios_part_number() const { return part_number_; }
  const char* vbios_name() const { return vbios_name_; }
  bool has_vbios() const { return !vbios_.view().empty(); }

 private:
  bool fail(const char* why) {
    status_ = why;
    return false;
  }
  bool map_registers();
  // The memory size register of the layout; all ones when the card does not answer.
  uint32_t read_memsize();
  uint32_t smc_read(uint32_t address);
  void read_vbios();

  GpuContext context_{};   // (a copy: the pool's is gone after the factory); its mapper is null when the registers were given
  allocators::IAllocator& allocator_;
  uint16_t device_id_ = 0;
  AmdGpuInfo info_;
  Layout layout_ = Layout::None;
  MmioVcnRegisters mmio_{MmioRegion()};
  IVcnRegisters* registers_ = nullptr;
  ByteBuffer vbios_;
  bool vbios_given_ = false;
  bool started_ = false;   // start() succeeded (resume() and the sensors need it)
  uint64_t vram_bytes_ = 0;
  const char* status_ = "not started";
  char part_number_[kPartNumberChars] = {};
  char vbios_name_[kNameChars] = {};
};

}  // namespace hardware
