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

// A table in a dialog or a property sheet. A DataGrid keeps Enter for itself (it activates the selected row); in a dialog Enter belongs to the
// default button (docs/UserInterfaceGuidelines.md, 11.2), so this one lets the key go on to the frame it is in.
#pragma once
#include "Gui/Controls/DataGrid.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

class DialogGrid final : public gui::DataGrid {
 public:
  explicit DialogGrid(allocators::IAllocator& allocator) : DataGrid(allocator) {}
  bool on_key(const gui::KeyArgs& args) override { return args.usage == hardware::key::kEnter ? false : DataGrid::on_key(args); }
};

}  // namespace desktop
