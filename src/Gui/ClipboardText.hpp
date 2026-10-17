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

// Text through the clipboard for the text controls: the system clipboard of their window when it has one, else a private
// String of the control (so copy and paste still work inside one control when no clipboard is installed).
#pragma once
#include "Kernel/Clipboard.hpp"
#include "Utility/String.hpp"

namespace gui {

// Puts `text` on the clipboard (or in `fallback`). False if that failed (nothing to put, too big, no memory).
inline bool clipboard_put_text(kernel::Clipboard* clipboard, String* fallback, StringView text) {
  if (text.empty()) return false;
  return clipboard ? clipboard->set_text(text) : fallback->assign(text);
}

// Copies the clipboard's text (or the fallback) into `out`. False if there is none.
inline bool clipboard_get_text(kernel::Clipboard* clipboard, const String& fallback, String* out) {
  if (clipboard) return clipboard->get_text(out);
  return !fallback.empty() && out->assign(fallback.view());
}

}  // namespace gui
