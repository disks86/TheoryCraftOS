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
#include "Graphics/SystemGraphics.hpp"

// Builds a small window with every control of the GUI toolkit (menu bar, tool bar, side panel with
// a text box and drop-down, an evenly split content area, status bar), drives it with synthetic
// mouse and keyboard input - typing, choosing from the drop-down, running a menu item, clicking
// buttons - checks the results and leaves the window on screen for a moment. Reports on the console;
// false if a check failed. Re-attach the console afterwards.
bool run_gui_demo(graphics::SystemGraphics& graphics, allocators::IAllocator& allocator);
