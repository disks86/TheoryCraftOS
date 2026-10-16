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

// Colors (0xAARRGGBB) of the classic "gray window" look.
#pragma once
#include <stdint.h>

namespace gui {

struct Theme {
  uint32_t desktop = 0xFF008080;
  uint32_t face = 0xFFC0C0C0;       // window and button surface
  uint32_t highlight = 0xFFFFFFFF;  // bevel, lit edge
  uint32_t light = 0xFFDFDFDF;      // bevel, inner lit edge
  uint32_t shadow = 0xFF808080;     // bevel, inner dark edge
  uint32_t dark = 0xFF404040;       // bevel, outer dark edge
  uint32_t text = 0xFF000000;
  uint32_t disabled_text = 0xFF808080;
  uint32_t field = 0xFFFFFFFF;      // text box and list background
  uint32_t selection = 0xFF000080;
  uint32_t selection_text = 0xFFFFFFFF;
  uint32_t title = 0xFF000080;
  uint32_t title_text = 0xFFFFFFFF;
};

}  // namespace gui
