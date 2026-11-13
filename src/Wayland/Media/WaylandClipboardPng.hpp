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

// Pictures on the desktop's clipboard as PNG files, for the Wayland selection (Server::set_clipboard_images, given by main.cpp): the kernel's image codecs
// (src/Media). Kept apart from the rest of src/Wayland so that the compositor builds without them.
#pragma once
#include "Kernel/Clipboard.hpp"
#include "Utility/ByteBuffer.hpp"

namespace wayland {

// The clipboard's picture encoded as a PNG file (appended to `png`); false when it holds none or it cannot be encoded.
bool clipboard_to_png(allocators::IAllocator& allocator, kernel::Clipboard& clipboard, ByteBuffer* png);
// A PNG file decoded and put on the clipboard as its picture (an animation's first frame); false when it is not a picture the codec reads, or the
// clipboard refuses it (too big). The clipboard is not changed then.
bool png_to_clipboard(allocators::IAllocator& allocator, kernel::Clipboard& clipboard, ConstByteView png);

}  // namespace wayland
