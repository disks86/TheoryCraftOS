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

// Framework laptops: which one this is (from SMBIOS) and the modules that plug into them (expansion cards, the Laptop 16's input modules),
// so the rest of the system can say "Framework Laptop 13 (AMD Ryzen 7040)" and name an "HDMI Expansion Card" instead of a bare USB id.
//
// Framework puts "Framework" in the system manufacturer and the model in the product name ("Laptop 13 (AMD Ryzen 7040Series)", "Laptop 16
// (AMD Ryzen 7040 Series)", "Laptop (12th Gen Intel Core)" ...); the match is on words of the product name, so firmware updates that
// respell it slightly still match. The parts lists come from Framework's public documentation as remembered; the USB product ids marked
// "(unverified)" in the .cpp are the least certain.
#pragma once
#include <stdint.h>
#include "Hardware/Firmware/Smbios.hpp"

namespace hardware {
namespace framework {

enum class Model : uint8_t {
  Unknown,          // a Framework machine this table does not know (a newer one)
  Laptop13Intel11,  // 11th Gen Intel Core (Tiger Lake)
  Laptop13Intel12,  // 12th Gen (Alder Lake)
  Laptop13Intel13,  // 13th Gen (Raptor Lake)
  Laptop13Ultra1,   // Intel Core Ultra Series 1 (Meteor Lake)
  Laptop13Amd7040,  // AMD Ryzen 7040 (Phoenix)
  Laptop13AmdAi300, // AMD Ryzen AI 300 (Strix Point)
  Laptop16Amd7040,  // Laptop 16, AMD Ryzen 7040
  Laptop16AmdAi300, // Laptop 16, AMD Ryzen AI 300
  Laptop12Intel13,  // Laptop 12, 13th Gen Intel Core (Raptor Lake U)
  Desktop,          // Framework Desktop (AMD Ryzen AI Max)
};

enum class EcChip : uint8_t { Unknown, Mec1521, Mec172x, Npcx };   // Microchip on the Intel boards, Nuvoton NPCX on the AMD ones
enum Feature : uint32_t {
  kExpansionCards = 1,     // the four (13, 12) or six (16) USB-C expansion card bays
  kInputModules = 2,       // the Laptop 16's swappable keyboard, numpad, macropad, LED matrix and spacers
  kExpansionBay = 4,       // the Laptop 16's rear bay: a graphics module or a dual SSD module
  kPrivacySwitches = 8,    // hardware switches for the camera and the microphones
  kFingerprint = 16,       // the power button's fingerprint reader
  kTouchscreen = 32,       // (the Laptop 12)
  kAmdSensorHub = 64,      // the ambient light sensor is behind AMD's Sensor Fusion Hub (else the Intel ISH or the EC)
};

struct ModelInfo {
  Model model;
  const char* name;
  EcChip ec;
  uint32_t features;
  uint8_t expansion_card_bays;
};

// The model of a machine whose SMBIOS says it is a Framework; null when it is not one. An unknown Framework product gives the Unknown entry.
const ModelInfo* identify(const SmbiosInfo& smbios);
const ModelInfo& model_info(Model model);

// The modules Framework makes that connect over USB.
enum class ModuleKind : uint8_t { ExpansionCard, InputModule, Other };
struct Module {
  uint16_t vendor_id, product_id;
  ModuleKind kind;
  const char* name;
};
const Module* find_module(uint16_t vendor_id, uint16_t product_id);
uint32_t module_count();
const Module& module_at(uint32_t index);

constexpr uint16_t kVendorFramework = 0x32AC;
constexpr uint16_t kLedMatrixProduct = 0x0020;

}  // namespace framework
}  // namespace hardware
