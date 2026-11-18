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

// The files a Broadcom FullMAC (brcmfmac) chip needs and how they are named and prepared, after Linux's brcmfmac firmware.c (brcmf_fw_alloc_request,
// brcm_alt_fw_path, brcmf_fw_nvram_strip and its helpers), pcie.c and sdio.c (brcmf_pcie_fwnames, brcmf_sdio_fwnames) and usb.c (brcmf_usb_fwnames):
//
//   - The firmware base name comes from a table by chip ID and a mask of chip revisions (bit n set: revision n uses this file). The tables here are the ones of
//     the Linux 7.2 brcmfmac module, read out of its .rodata (chip id, revision mask, name), so they are not retyped from memory.
//   - Every file of a chip shares that base: "<base>.bin" (the ARM image), "<base>.txt" (NVRAM: board calibration as text), "<base>.clm_blob" (the regulatory
//     database, downloaded once the firmware runs), "<base>.txcap_blob" (PCIe: transmit power caps). Each is first looked for with the board type in its name
//     ("brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt"), then without.
//   - The NVRAM text is turned into the zero-separated "key=value" list the firmware reads from the end of its RAM, exactly as brcmf_fw_nvram_strip does, with
//     its quirks (a last line without a newline is dropped, values keep trailing blanks, multi-device files are reduced to this device's entries).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

enum class BrcmfBusType : uint8_t { Pcie, Sdio, Usb };

// brcmf_firmware_mapping
struct BrcmfFirmwareMapping {
  uint32_t chip;        // the ChipID register's chip number (0x4356, 0xaa52 = 43602, ...)
  uint32_t revmask;     // bit n: chip revision n
  const char* base;     // "brcm/brcmfmac4356-pcie"
};

ArrayView<const BrcmfFirmwareMapping> brcmf_firmware_table(BrcmfBusType bus);
// The first entry for this chip whose mask has the revision's bit (brcmf_fw_alloc_request); nullptr for an unknown chip or a revision past 31.
const BrcmfFirmwareMapping* brcmf_find_firmware(BrcmfBusType bus, uint32_t chip, uint32_t revision);

// Firmware file names are short; 255 characters is more than any in linux-firmware (the longest board-specific ones are about 90).
using BrcmfFileName = FixedString<256>;

// The file extensions of a firmware request (brcmf_pcie_prepare_fw_request / brcmf_sdio_prepare_fw_request).
constexpr const char* kBrcmfExtensionCode = ".bin";
constexpr const char* kBrcmfExtensionNvram = ".txt";
constexpr const char* kBrcmfExtensionClm = ".clm_blob";
constexpr const char* kBrcmfExtensionTxcap = ".txcap_blob";

// "<base><extension>". False if it does not fit.
[[nodiscard]] bool brcmf_file_name(StringView base, StringView extension, BrcmfFileName& out);
// brcm_alt_fw_path: "<path without its extension>.<board_type><extension>". False when the path has no extension (or only a leading dot), the board type is empty,
// or the result does not fit.
[[nodiscard]] bool brcmf_board_file_name(StringView path, StringView board_type, BrcmfFileName& out);

// The board type Linux puts into the board-specific names: the devicetree root's first "compatible" string (of.c), or "<sys_vendor>-<product_name>" from DMI
// (dmi.c), with every '/' turned into '-' so that it stays one path component. False if empty or too long.
[[nodiscard]] bool brcmf_board_type_from_compatible(StringView compatible, BrcmfFileName& out);
[[nodiscard]] bool brcmf_board_type_from_dmi(StringView sys_vendor, StringView product_name, BrcmfFileName& out);

// brcmf_chip_name: "BCM4356/2", or decimal for the IDs that are decimal numbers ("BCM43602/1").
using BrcmfChipName = FixedString<24>;
void brcmf_chip_name(uint32_t chip, uint32_t revision, BrcmfChipName& out);

// ---- NVRAM ------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace brcmf_nvram {
constexpr uint32_t kMaxBytes = 64000;                  // BRCMF_FW_MAX_NVRAM_SIZE
constexpr uint32_t kDevpathLength = 19;                // BRCMF_FW_NVRAM_DEVPATH_LEN: "devpath0=pcie/1/4/"
constexpr const char* kDefaultBoardrev = "boardrev=0xff";   // BRCMF_FW_DEFAULT_BOARDREV
}  // namespace brcmf_nvram

struct BrcmfNvramOptions {
  // Which device of a multi-device file this is: PCIe uses the PCI domain + 1 (domain 0 is the SDK's fake bus) and the bus number, as brcmf_pcie_prepare_fw_request does;
  // SDIO leaves both 0.
  uint16_t domain_nr = 0;
  uint16_t bus_nr = 0;
  // A platform-provided address (devicetree "local-mac-address"): the file's "macaddr" entries are dropped and this one is added (brcmf_fw_add_macaddr).
  bool has_platform_mac = false;
  uint8_t platform_mac[6] = {};
};

enum class BrcmfNvramResult : uint8_t {
  Ok,
  Empty,        // nothing left (no entries, or a multi-device file without this device): Linux returns NULL and goes on without NVRAM where it is optional
  NoMemory,
};

// brcmf_fw_nvram_strip: `out` becomes the zero-separated entries, padded with zeros to a multiple of 4 bytes (at least one zero), followed by the little-endian
// token ((~words) << 16) | words, where words is the padded length / 4. Input is untrusted text; anything not printable ends or skips an entry.
[[nodiscard]] BrcmfNvramResult brcmf_nvram_strip(ConstByteView text, const BrcmfNvramOptions& options, ByteBuffer& out);

// The value of `key` in a stripped NVRAM image (zero-separated "key=value" entries), or false.
[[nodiscard]] bool brcmf_nvram_find(ConstByteView image, StringView key, StringView* value);

// ---- firmware image ---------------------------------------------------------------------------------------------------------------------------------------------------------

// The version in the image's build string ("43455c0-roml/... Version: 7.45.286 (be70ab3 CY) CRC: ..."), as `brcmfmac` prints it from the "ver" iovar. False if the
// image has none. `out` gets a NUL-terminated copy, cut to fit.
[[nodiscard]] bool brcmf_image_version(ConstByteView image, ArrayView<char> out);
// The version from the firmware's console log: the token after the "RTE (<bus>-<protocol>) " banner the firmware prints at start ("RTE (PCIE-MSG) 7.35.180.119 on BCM4356
// r2 @ ..."). False if the log has no banner.
[[nodiscard]] bool brcmf_console_version(ConstByteView log, ArrayView<char> out);

}  // namespace hardware
