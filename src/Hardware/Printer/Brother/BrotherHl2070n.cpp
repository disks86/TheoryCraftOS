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

#include "Hardware/Printer/Brother/BrotherHl2070n.hpp"

namespace hardware {

const PrinterInfo& BrotherHl2070n::model_info() {
  static const PrinterInfo kInfo = [] {
    PrinterInfo i;
    i.manufacturer = "Brother";
    i.model = "Brother HL-2070N";
    i.languages = static_cast<uint8_t>((1u << static_cast<uint8_t>(PrinterLanguage::Pcl)) | (1u << static_cast<uint8_t>(PrinterLanguage::PostScript)));
    i.native_dpi = 600;
    i.max_dpi = 1200;
    i.pages_per_minute = 23;
    i.color = false;
    i.duplex = false;
    i.paper_sizes = static_cast<uint16_t>((1u << static_cast<uint8_t>(PaperSize::A4)) | (1u << static_cast<uint8_t>(PaperSize::Letter)) |
                                         (1u << static_cast<uint8_t>(PaperSize::Legal)) | (1u << static_cast<uint8_t>(PaperSize::Executive)) |
                                         (1u << static_cast<uint8_t>(PaperSize::A5)) | (1u << static_cast<uint8_t>(PaperSize::B5)) |
                                         (1u << static_cast<uint8_t>(PaperSize::A6)));
    i.unprintable_mm10 = 42;
    return i;
  }();
  return kInfo;
}

}  // namespace hardware
