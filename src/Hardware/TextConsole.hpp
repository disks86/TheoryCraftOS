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
#include "Hardware/IHardware.hpp"
#include "uefi.hpp"

namespace hardware {

// Firmware text output (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL on ConOut).
class TextConsole final : public IHardware {
 public:
  constexpr TextConsole() = default;

  // Must be called before initialize().
  void bind(efi::SystemTable* st) { st_ = st; }

  const char* name() const override { return "UEFI text console"; }
  bool initialize() override { return st_ && st_->con_out; }
  void shutdown() override { st_ = nullptr; }

  void put(char c) const;

 private:
  efi::SystemTable* st_ = nullptr;
};

}  // namespace hardware
