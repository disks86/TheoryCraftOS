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

#include "Gui/Controls/ProgressBar.hpp"
#include "Utility/String.hpp"

namespace gui {

void ProgressBar::paint(Painter& painter) {
  const Theme& theme = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, theme.field);
  painter.bevel(all, true);
  const Rect2D inner{2, 2, bounds_.width > 4 ? bounds_.width - 4 : 0, bounds_.height > 4 ? bounds_.height - 4 : 0};
  const uint32_t filled = maximum_ ? static_cast<uint32_t>(static_cast<uint64_t>(inner.width) * value_ / maximum_) : 0;
  if (filled) painter.fill({inner.x, inner.y, filled, inner.height}, theme.selection);
  String text(allocator());
  text.append_uint(percent());
  text.append("%");
  const int32_t tx = inner.x + (static_cast<int32_t>(inner.width) - Painter::text_width(text.view())) / 2;
  const int32_t ty = inner.y + (static_cast<int32_t>(inner.height) - Painter::kGlyphHeight) / 2;
  // The part of the label over the bar is drawn light, the rest dark.
  painter.push_frame({inner.x, inner.y, filled, inner.height});
  painter.text(tx - inner.x, ty - inner.y, text.view(), theme.selection_text);
  painter.pop_frame();
  painter.push_frame({inner.x + static_cast<int32_t>(filled), inner.y, inner.width - filled, inner.height});
  painter.text(tx - inner.x - static_cast<int32_t>(filled), ty - inner.y, text.view(), theme.text);
  painter.pop_frame();
}

}  // namespace gui
