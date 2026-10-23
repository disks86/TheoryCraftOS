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

// Layout of multi-column layout (CSS Multi-column Level 1). Not written yet: the children are laid out as blocks.
#include "Layout/LayoutInternal.hpp"

namespace layout {

ChildrenResult LayoutContext::layout_columns(BoxId id, LayoutUnit content_width, bool height_known, LayoutUnit height) {
  (void)height_known;
  (void)height;
  return layout_block_children(id, content_width, false, false);
}

}  // namespace layout
