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

// The firmware and board files of Qualcomm's ath10k, ath11k and ath12k chips (linux-firmware's ath10k/, ath11k/, ath12k/ folders), parsed after the Linux drivers:
//
//  - "firmware-N.bin" of ath10k (ath10k_core_fetch_firmware_api_n in ath10k/core.c): the magic "QCA-ATH10K" with its NUL, padded to 4 bytes, then elements (IEs) of
//    a little-endian id and length and the data, padded to 4 bytes: the version text, a build time, a feature bit set, the firmware image, the OTP (calibration) image,
//    the WMI and HTT interface versions and the code swap image.
//  - "firmware-2.bin" of ath11k and ath12k (ath11k_fw_request_firmware_api_n in ath11k/fw.c, the same in ath12k/fw.c): "QCOM-ATH11K-FW" / "QCOM-ATH12K-FW", then IEs
//    holding a build time, features, the AMSS (main) image, the M3 image and (ath12k) a dual-MAC AMSS image.
//  - "board-2.bin" (ath10k_core_fetch_board_data_api_n, ath11k_core_fetch_board_data_api_n): "QCA-ATH10K-BOARD" / "QCA-ATH11K-BOARD" / "QCA-ATH12K-BOARD", then
//    top-level IEs (BOARD, BOARD_EXT or REGDB) each holding sub-IEs: one or more NAMEs followed by the DATA they name. The driver builds a name from the bus and the
//    IDs of the chip ("bus=pci,vendor=168c,device=003e,subsystem-vendor=1a56,subsystem-device=1535") and takes the data of the first entry whose name is exactly that,
//    trying a fallback name (without the variant, with the default board id, or only the bus) after it.
//
// The files come from disk and the images go to a chip: every length is checked against what is left, the results are views into the file (which must stay valid).
// Validated against the linux-firmware files of QCA6174 hw3.0, QCA9377 hw1.0, QCA988X hw2.0, WCN6855 hw2.0, QCA6390 hw2.0, WCN7850 hw2.0 and QCN9274 hw2.0.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/StringView.hpp"

namespace hardware::ath {

// ath10k/hw.h enum ath10k_fw_ie_type.
enum class Ath10kFwIe : uint32_t {
  FwVersion = 0, Timestamp = 1, Features = 2, FwImage = 3, OtpImage = 4, WmiOpVersion = 5, HttOpVersion = 6, FwCodeSwapImage = 7,
};
// ath10k/core.h enum ath10k_fw_features (the ones this driver looks at).
constexpr uint32_t kAth10kFeatureWmi10x = 1, kAth10kFeatureWowlan = 6, kAth10kFeatureIgnoreOtpResult = 7, kAth10kFeatureSkipClockInit = 9, kAth10kFeatureNonBmi = 19;
// enum ath10k_fw_wmi_op_version and ath10k_fw_htt_op_version.
enum class Ath10kWmiOp : uint32_t { Unset = 0, Main = 1, V10_1 = 2, V10_2 = 3, Tlv = 4, V10_2_4 = 5, V10_4 = 6 };
enum class Ath10kHttOp : uint32_t { Unset = 0, Main = 1, V10_1 = 2, Tlv = 3, V10_4 = 4 };

struct Ath10kFirmwareFile {
  FixedString<48> version;          // "WLAN.RM.4.4.1-00309-QCARMSWPZ-1", "10.2.4-1.0-00047"
  uint32_t timestamp = 0;
  uint64_t features = 0;            // feature bits 0..63 (ath10k knows about 25)
  ConstByteView firmware;           // the image BMI downloads to the patch address (LZ compressed by Qualcomm; the target unpacks it)
  ConstByteView otp;                // the OTP program the target runs once to apply its calibration (optional)
  ConstByteView code_swap;          // the code swap image (optional; QCA6174 firmware-4/5 used it)
  Ath10kWmiOp wmi_op = Ath10kWmiOp::Unset;
  Ath10kHttOp htt_op = Ath10kHttOp::Unset;
  uint32_t unknown_ies = 0;         // IEs this parser skipped by their length

  bool has_feature(uint32_t bit) const { return bit < 64 && (features >> bit) & 1; }
};

enum class AthParse : uint8_t { Ok, BadMagic, Truncated, BadLength, NoImage };
const char* ath_parse_name(AthParse result);

// Parses an ath10k firmware-N.bin. Ok only with a firmware image; when the WMI op version is not in the file it is derived from the features as
// ath10k_core_init_firmware_features does (10.2 / 10.1 / main).
[[nodiscard]] AthParse ath10k_parse_firmware(ConstByteView file, Ath10kFirmwareFile* out);

// ath11k/fw.h and ath12k/fw.h enum ath1Xk_fw_ie_type.
enum class AthQcomFwIe : uint32_t { Timestamp = 0, Features = 1, AmssImage = 2, M3Image = 3, AmssDualMacImage = 4 };

struct AthQcomFirmwareFile {
  uint32_t timestamp = 0;
  uint64_t features = 0;
  ConstByteView amss, m3, amss_dual_mac;
  uint32_t unknown_ies = 0;
};

// The magics of the containers (with their NUL, as the files have them).
constexpr const char* kAth10kFirmwareMagic = "QCA-ATH10K";
constexpr const char* kAth11kFirmwareMagic = "QCOM-ATH11K-FW";
constexpr const char* kAth12kFirmwareMagic = "QCOM-ATH12K-FW";
constexpr const char* kAth10kBoardMagic = "QCA-ATH10K-BOARD";
constexpr const char* kAth11kBoardMagic = "QCA-ATH11K-BOARD";
constexpr const char* kAth12kBoardMagic = "QCA-ATH12K-BOARD";

// Parses an ath11k/ath12k firmware-2.bin with that magic. Ok only with an AMSS image.
[[nodiscard]] AthParse ath_parse_qcom_firmware(ConstByteView file, StringView magic, AthQcomFirmwareFile* out);

// True if `image` begins like the 32-bit little-endian ELF files amss.bin and m3.bin are (the MHI boot loader takes them as they are).
[[nodiscard]] bool ath_is_elf32(ConstByteView image);

// board-2.bin top-level IEs: ath10k BOARD = 0, BOARD_EXT = 1; ath11k/ath12k BOARD = 0, REGDB = 1 (ath12k BOARD_EXT = 2, not used here). Sub-IEs: NAME = 0, DATA = 1
// for every kind.
constexpr uint32_t kAthBoardIeBoard = 0, kAth10kBoardIeBoardExt = 1, kAthBoardIeRegdb = 1;
constexpr uint32_t kAthBoardSubIeName = 0, kAthBoardSubIeData = 1;

enum class AthBoardFind : uint8_t { Found, NotFound, Invalid };

// The data of the first `ie_type` entry named exactly `name`. Invalid if the file is not well-formed up to that entry (the driver stops there too).
[[nodiscard]] AthBoardFind ath_board_find(ConstByteView file, StringView magic, uint32_t ie_type, StringView name, ConstByteView* data);

// Every (type, name, data) entry of a board file in file order, for listing and checking. False if the file is not well-formed (entries before the damage were
// visited); `visit` returning false stops early (then true). Of the names in front of one DATA, the first 8 are reported (the real files have one or two).
using AthBoardVisitor = bool (*)(void* context, uint32_t ie_type, StringView name, ConstByteView data);
[[nodiscard]] bool ath_board_for_each(ConstByteView file, StringView magic, AthBoardVisitor visit, void* context);

// What a board name is made of.
struct AthBoardIds {
  const char* bus = "pci";              // ath10k_bus_str / ath11k_bus_str: "pci", "sdio", "usb", "snoc", "ahb"
  uint16_t vendor = 0, device = 0, subsystem_vendor = 0, subsystem_device = 0;
  bool bmi_ids_valid = false;           // ath10k: board and chip id read from the OTP (BMI_EXECUTE with BMI_PARAM_GET_EEPROM_BOARD_ID)
  uint32_t bmi_chip_id = 0, bmi_board_id = 0;
  bool qmi_ids_valid = false;           // ath10k on SNOC; ath11k/ath12k always (from QMI's target capability answer)
  uint32_t qmi_chip_id = 0, qmi_board_id = 0;
  StringView variant;                   // the board data extension from SMBIOS or the device tree ("" for none)
};

using AthBoardName = FixedString<200>;
constexpr uint32_t kAthBoardIdDefault = 0xFF;    // ATH11K_BOARD_ID_DEFAULT / ATH12K_BOARD_ID_DEFAULT

// ath10k_core_create_board_name: "bus=pci,bmi-chip-id=%d,bmi-board-id=%d[,variant=]" with the OTP ids, "bus=snoc,qmi-board-id=%x[,qmi-chip-id=%x][,variant=]" with QMI ids,
// else "bus=pci,vendor=%04x,device=%04x,subsystem-vendor=%04x,subsystem-device=%04x[,variant=]".
[[nodiscard]] bool ath10k_board_name(const AthBoardIds& ids, bool with_variant, bool with_chip_id, AthBoardName* out);

// How ath11k/ath12k search the board file (ab->id.bdf_search): by the QMI ids only, or (WCN6855, WCN7850) by the PCI ids and the QMI ids.
enum class AthBdfSearch : uint8_t { Default, BusAndBoard };
// __ath11k_core_create_board_name (and the identical __ath12k_core_create_board_name):
//   BusAndBoard: "bus=pci,vendor=%04x,device=%04x,subsystem-vendor=%04x,subsystem-device=%04x,qmi-chip-id=%d,qmi-board-id=%d[,variant=]", or "bus=pci" in bus-type mode;
//   Default: "bus=pci,qmi-chip-id=%d,qmi-board-id=%d[,variant=]", with board id 255 when `with_default`.
[[nodiscard]] bool ath11k_board_name(const AthBoardIds& ids, AthBdfSearch search, bool with_variant, bool bus_type_mode, bool with_default, AthBoardName* out);

// The board data as the drivers pick it: ath10k (ath10k_core_fetch_board_file, BOARD type) tries the name with the variant, then without. ath11k/ath12k
// (ath11k_core_fetch_bdf) try ath11k_core_create_board_name (with variant), then the fallback name (no variant, default board id). For REGDB entries
// (ath11k_core_fetch_regdb) the second try is the bus-type name ("bus=pci" in the bus-and-board search, the default board's name otherwise). `tried` (optional) receives the name that matched, or the last one tried.
enum class AthFamily : uint8_t { Ath10k, Ath11k, Ath12k };
[[nodiscard]] AthBoardFind ath_select_board(ConstByteView file, AthFamily family, AthBdfSearch search, uint32_t ie_type, const AthBoardIds& ids, ConstByteView* data,
                                            AthBoardName* tried);

}  // namespace hardware::ath
