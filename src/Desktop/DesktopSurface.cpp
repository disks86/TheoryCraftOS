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

#include "Desktop/DesktopSurface.hpp"

namespace desktop {

void DesktopSurface::paint(gui::Painter& painter) {
  painter.fill({0, 0, bounds().width, bounds().height}, painter.theme().desktop);
  // A quiet watermark in the corner.
  const StringView name = "TheoryCraftOS";
  painter.text(static_cast<int32_t>(bounds().width) - gui::Painter::text_width(name) - 16,
               static_cast<int32_t>(bounds().height) - 32, name, 0xFF40A0A0);
}

}  // namespace desktop
