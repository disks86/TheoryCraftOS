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
#include <stdint.h>
#include "FileSystem/IFile.hpp"
#include "Utility/ArrayView.hpp"

namespace exe {

// An AppImage is an ELF program (the "runtime") with a filesystem image appended to it. The runtime marks itself with "AI" and the type in
// bytes 8..10 of the ELF identification (the padding the ELF standard leaves unused):
//   type 1: the image is ISO 9660 (the old format, rare today);
//   type 2: the image is SquashFS, starting right after the runtime ELF (after its section header table or last section, whichever ends last).
// The runtime would mount the image and run the AppRun at its root; this OS mounts it itself (Program/AppImageLaunch.hpp) and never runs the runtime.
enum class AppImageType : uint8_t { None, Type1, Type2 };

// Looks at the ELF identification in `head` (the first bytes of the file) only.
AppImageType identify_appimage(ConstByteView head);

// Where the filesystem image of a type 2 AppImage starts in `file`: the end of the runtime ELF (32- or 64-bit, either byte order), checked to hold a
// SquashFS superblock. False if the file is not a type 2 AppImage or the image is not where the runtime says.
[[nodiscard]] bool appimage_payload_offset(filesystem::IFile& file, uint64_t* offset);

}  // namespace exe
