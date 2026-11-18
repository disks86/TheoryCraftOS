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

#include "Hardware/Platform/Framework/FrameworkPlatform.hpp"

namespace hardware {
namespace framework {

namespace {
constexpr uint32_t kCommon13 = kExpansionCards | kPrivacySwitches | kFingerprint;
constexpr ModelInfo kModels[] = {
    {Model::Unknown, "Framework computer", EcChip::Unknown, 0, 0},
    {Model::Laptop13Intel11, "Framework Laptop 13 (11th Gen Intel Core)", EcChip::Mec1521, kCommon13, 4},
    {Model::Laptop13Intel12, "Framework Laptop 13 (12th Gen Intel Core)", EcChip::Mec1521, kCommon13, 4},
    {Model::Laptop13Intel13, "Framework Laptop 13 (13th Gen Intel Core)", EcChip::Mec1521, kCommon13, 4},
    {Model::Laptop13Ultra1, "Framework Laptop 13 (Intel Core Ultra Series 1)", EcChip::Mec172x, kCommon13, 4},
    {Model::Laptop13Amd7040, "Framework Laptop 13 (AMD Ryzen 7040 Series)", EcChip::Npcx, kCommon13 | kAmdSensorHub, 4},
    {Model::Laptop13AmdAi300, "Framework Laptop 13 (AMD Ryzen AI 300 Series)", EcChip::Npcx, kCommon13 | kAmdSensorHub, 4},
    {Model::Laptop16Amd7040, "Framework Laptop 16 (AMD Ryzen 7040 Series)", EcChip::Npcx, kCommon13 | kInputModules | kExpansionBay | kAmdSensorHub, 6},
    {Model::Laptop16AmdAi300, "Framework Laptop 16 (AMD Ryzen AI 300 Series)", EcChip::Npcx, kCommon13 | kInputModules | kExpansionBay | kAmdSensorHub, 6},
    {Model::Laptop12Intel13, "Framework Laptop 12 (13th Gen Intel Core)", EcChip::Mec172x, kExpansionCards | kTouchscreen | kPrivacySwitches, 4},
    {Model::Desktop, "Framework Desktop", EcChip::Unknown, 0, 0},
};

// Case-insensitive "does `text` contain `word`".
bool has(const char* text, const char* word) {
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* t = text; *t; ++t) {
    const char* a = t;
    const char* b = word;
    while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
    if (!*b) return true;
  }
  return false;
}

constexpr Module kModules[] = {
    // Expansion cards that are Framework's own USB devices (USB-A, USB-C, the microSD/SD readers and the audio card are plain USB parts).
    {kVendorFramework, 0x0002, ModuleKind::ExpansionCard, "HDMI Expansion Card"},
    {kVendorFramework, 0x0003, ModuleKind::ExpansionCard, "DisplayPort Expansion Card"},
    {kVendorFramework, 0x0005, ModuleKind::ExpansionCard, "Storage Expansion Card (250 GB)"},       // (unverified)
    {kVendorFramework, 0x0006, ModuleKind::ExpansionCard, "Storage Expansion Card (1 TB)"},         // (unverified)
    {kVendorFramework, 0x0010, ModuleKind::ExpansionCard, "Audio Expansion Card"},                  // (unverified)
    {0x0BDA, 0x8156, ModuleKind::ExpansionCard, "Ethernet Expansion Card (Realtek RTL8156, 2.5 Gbit/s)"},
    // The Laptop 16's input modules: RP2040 microcontrollers running QMK (keyboards: USB HID) or Framework's own firmware (the LED matrix:
    // a USB serial port, Usb/LedMatrix).
    {kVendorFramework, 0x0012, ModuleKind::InputModule, "Laptop 16 Keyboard Module (ANSI)"},
    {kVendorFramework, 0x0013, ModuleKind::InputModule, "Laptop 16 RGB Macropad Module"},
    {kVendorFramework, 0x0014, ModuleKind::InputModule, "Laptop 16 Numpad Module"},
    {kVendorFramework, 0x0018, ModuleKind::InputModule, "Laptop 16 Keyboard Module (ISO)"},         // (unverified)
    {kVendorFramework, 0x0019, ModuleKind::InputModule, "Laptop 16 Keyboard Module (JIS)"},         // (unverified)
    {kVendorFramework, kLedMatrixProduct, ModuleKind::InputModule, "Laptop 16 LED Matrix Input Module"},
    // The fingerprint reader in the power button.
    {0x27C6, 0x609C, ModuleKind::Other, "Goodix fingerprint reader"},
    {0x27C6, 0x6098, ModuleKind::Other, "Goodix fingerprint reader"},                               // (unverified)
};
}  // namespace

const ModelInfo& model_info(Model model) {
  for (const ModelInfo& m : kModels)
    if (m.model == model) return m;
  return kModels[0];
}

const ModelInfo* identify(const SmbiosInfo& s) {
  if (!has(s.manufacturer, "Framework")) return nullptr;
  const char* p = s.product;
  if (has(p, "Desktop")) return &model_info(Model::Desktop);
  const bool amd = has(p, "AMD") || has(p, "Ryzen");
  const bool ai300 = has(p, "AI 300") || has(p, "AI300");
  if (has(p, "Laptop 16")) return &model_info(ai300 ? Model::Laptop16AmdAi300 : Model::Laptop16Amd7040);
  if (has(p, "Laptop 12")) return &model_info(Model::Laptop12Intel13);
  if (amd) return &model_info(ai300 ? Model::Laptop13AmdAi300 : Model::Laptop13Amd7040);
  if (has(p, "Core Ultra")) return &model_info(Model::Laptop13Ultra1);
  if (has(p, "11th Gen")) return &model_info(Model::Laptop13Intel11);
  if (has(p, "12th Gen")) return &model_info(Model::Laptop13Intel12);
  if (has(p, "13th Gen")) return &model_info(Model::Laptop13Intel13);
  return &model_info(Model::Unknown);
}

const Module* find_module(uint16_t vendor_id, uint16_t product_id) {
  for (const Module& m : kModules)
    if (m.vendor_id == vendor_id && m.product_id == product_id) return &m;
  return nullptr;
}

uint32_t module_count() { return sizeof(kModules) / sizeof(kModules[0]); }
const Module& module_at(uint32_t index) { return kModules[index < module_count() ? index : 0]; }

}  // namespace framework
}  // namespace hardware
