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

// The first steps of the Modem Host Interface (MHI) that ath11k and ath12k chips boot through, after Linux's drivers/bus/mhi (common.h, host/main.c, host/boot.c,
// host/pm.c) and ath11k/pci.c, ath11k/mhi.c, ath12k/pci.c:
//
//  - the MHI registers at the start of BAR 0: MHICTRL (the host's state request and RESET), MHISTATUS (the device's state, READY, SYSERR), BHIOFF / BHIEOFF (where the
//    boot interfaces are);
//  - BHI, the boot loader interface of the device's primary boot loader (PBL): the execution environment (BHI_EXECENV), and one image download: the host writes the
//    image's bus address and size and rings BHI_IMGTXDB with a session id, the PBL fetches it and reports success or an error in BHI_STATUS (mhi_fw_load_bhi). With
//    "fbc" download (every ath11k/ath12k PCIe chip) that image is the first 512 KiB of amss.bin, the secondary boot loader (SBL);
//  - the register window of ath11k/ath12k (WINDOW_REG_ADDRESS selects which 512 KiB of the chip appear at BAR + 0x80000), used to read TCSR_SOC_HW_VERSION.
//
// What follows on a real chip is not written: once the SBL runs, the device goes to MHI READY and the host must set up the MHI context (channel, event and command
// rings in host memory, MHICTRLBASE/MHIDATABASE, then M0); the SBL then asks for the rest of amss.bin over BHIe and the firmware talks QMI over MHI channels.
// The register layout is from the Linux source; nothing here was run on hardware.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware::ath {

namespace mhi_reg {
constexpr uint32_t kMhiRegLen = 0x00, kMhiVer = 0x08, kMhiCfg = 0x10, kChDbOff = 0x18, kErDbOff = 0x20, kBhiOff = 0x28, kBhieOff = 0x2C, kDebugOff = 0x30,
                   kMhiCtrl = 0x38, kMhiStatus = 0x48;
constexpr uint32_t kMhiCtrlStateMask = 0x0000FF00, kMhiCtrlStateShift = 8, kMhiCtrlReset = 1u << 1;
constexpr uint32_t kMhiStatusStateMask = 0x0000FF00, kMhiStatusStateShift = 8, kMhiStatusSysErr = 1u << 2, kMhiStatusReady = 1u << 0;
// BHI registers, relative to BHIOFF.
constexpr uint32_t kBhiVersionMinor = 0x00, kBhiVersionMajor = 0x04, kBhiImgAddrLow = 0x08, kBhiImgAddrHigh = 0x0C, kBhiImgSize = 0x10, kBhiImgTxDb = 0x18,
                   kBhiIntVec = 0x20, kBhiExecEnv = 0x28, kBhiStatus = 0x2C, kBhiErrCode = 0x30, kBhiErrDbg1 = 0x34, kBhiErrDbg2 = 0x38, kBhiErrDbg3 = 0x3C,
                   kBhiSerialNumber = 0x40;
constexpr uint32_t kBhiStatusMask = 0xC0000000u, kBhiStatusShift = 30, kBhiStatusReset = 0, kBhiStatusSuccess = 2, kBhiStatusError = 3;
constexpr uint32_t kBhiTxDbSeqMask = 0x3FFFFFFF;
}  // namespace mhi_reg

enum class MhiState : uint8_t { Reset = 0, Ready = 1, M0 = 2, M1 = 3, M2 = 4, M3 = 5, M3Fast = 6, Bhi = 7, SysErr = 0xFF };
// enum mhi_ee_type.
enum class MhiExecEnv : uint32_t { Pbl = 0, Sbl = 1, Amss = 2, Rddm = 3, Wfw = 4, Pthru = 5, Edl = 6, Fp = 7 };
const char* mhi_state_name(uint32_t state);
const char* mhi_exec_env_name(uint32_t ee);

enum class MhiResult : uint8_t { Ok, Gone, BadOffset, Timeout, ImageError, NoMemory };

// ath11k/ath12k's MHI controller settings (ath11k_mhi_register: fbc_download, sbl_size = seg_len = 512 KiB; the configs' timeout_ms = 2000 ... 10000).
constexpr uint32_t kAthSblSize = 512 * 1024;
constexpr uint32_t kMhiTimeoutMs = 2000;

class AthMhi {
 public:
  // `register_length` bounds the offsets the device reports (mhi_cntrl->reg_len).
  AthMhi(INicRegisters& bar, uint32_t register_length) : bar_(bar), register_length_(register_length) {}

  uint32_t mhi_state() { return (bar_.read32(mhi_reg::kMhiStatus) & mhi_reg::kMhiStatusStateMask) >> mhi_reg::kMhiStatusStateShift; }
  bool ready() { return bar_.read32(mhi_reg::kMhiStatus) & mhi_reg::kMhiStatusReady; }
  bool sys_error() { return bar_.read32(mhi_reg::kMhiStatus) & mhi_reg::kMhiStatusSysErr; }
  // Finds the BHI block (mhi_prepare_for_power_up). BadOffset if BHIOFF points outside the registers.
  [[nodiscard]] MhiResult locate_bhi();
  uint32_t bhi_offset() const { return bhi_; }
  uint32_t exec_env() { return bar_.read32(bhi_ + mhi_reg::kBhiExecEnv); }
  uint32_t bhi_status() { return (bar_.read32(bhi_ + mhi_reg::kBhiStatus) & mhi_reg::kBhiStatusMask) >> mhi_reg::kBhiStatusShift; }
  uint32_t bhi_error_code() { return bar_.read32(bhi_ + mhi_reg::kBhiErrCode); }
  uint32_t serial_number() { return bar_.read32(bhi_ + mhi_reg::kBhiSerialNumber); }

  // ath11k_mhi_set_mhictrl_reset: MHICTRL.RESET (clears a SYSERR left after the SoC reset), then 10 ms.
  void request_reset();
  // mhi_set_mhi_state(RESET) and the wait until the device clears the bit again (mhi_async_power_up for a device in SYS_ERR); also clears BHI_INTVEC.
  [[nodiscard]] MhiResult reset_and_wait(uint32_t timeout_ms = kMhiTimeoutMs);
  // Polls MHISTATUS until READY (and not SYSERR).
  [[nodiscard]] MhiResult wait_ready(uint32_t timeout_ms = kMhiTimeoutMs);
  // mhi_fw_load_bhi: `image` (DMA memory below 4 GiB is not required: BHI takes 64-bit addresses) is fetched by the PBL. Waits for BHI_STATUS success or error.
  [[nodiscard]] MhiResult load_bhi(ConstByteView image, uint32_t session_id, uint32_t timeout_ms = kMhiTimeoutMs);

 private:
  INicRegisters& bar_;
  uint32_t register_length_;
  uint32_t bhi_ = 0;
};

// The ath11k/ath12k register window (ath11k_pci_select_window / ath12k_pci_select_window without the static windows ath12k QCN9274 sets later).
constexpr uint32_t kAthWindowRegAddress = 0x310C, kAthWindowEnable = 0x40000000, kAthWindowValueMask = 0x01F80000, kAthWindowValueShift = 19, kAthWindowStart = 0x80000,
                   kAthWindowRangeMask = 0x0007FFFF;
// Reads a chip address: directly below WINDOW_START, through the window above it.
uint32_t ath_window_read32(INicRegisters& bar, uint32_t offset);
// TCSR_SOC_HW_VERSION: ath11k at 0x0224 (direct), ath12k at 0x1B00000 (windowed). Major in bits 8..11, minor in 0..7.
constexpr uint32_t kAth11kTcsrSocHwVersion = 0x0224, kAth12kTcsrSocHwVersion = 0x01B00000;
constexpr uint32_t kTcsrSocHwVersionMajorMask = 0x00000F00, kTcsrSocHwVersionMajorShift = 8, kTcsrSocHwVersionMinorMask = 0x000000FF;
// ath11k's PCIE_SOC_GLOBAL_RESET (ath11k_pci_soc_global_reset; offset from ath11k/pci.c, not verified on hardware).
constexpr uint32_t kAth11kPcieSocGlobalReset = 0x3008, kAth11kPcieSocGlobalResetV = 1;

}  // namespace hardware::ath
