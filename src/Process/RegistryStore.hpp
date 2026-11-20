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
#include "FileSystem/IFileSystem.hpp"
#include "Process/RegistryTree.hpp"
#include "Utility/ByteBuffer.hpp"

// The registry of Windows programs on disk: the image of RegistryTree::save_persistent() in one file (filesystem::layout::kWindowsRegistryFile).
//
// A save writes the whole image to kWindowsRegistryNewFile, flushes it, removes the old file and renames the new one in its place. So at any moment
// one of the two files holds a complete image: a power cut while the new file is written leaves the old one; one after the old file went leaves
// the new one. Only a complete new file is ever taken, so a load tries the new file first (it is the newer when both are whole) and then the old.
namespace process::windowsabi {

// Largest image read back (a registry with every program of a big install is a few megabytes).
inline constexpr uint64_t kRegistryStoreMaxBytes = 256ull << 20;

enum class RegistryLoad : uint8_t {
  Loaded,     // *image holds a valid image
  NoFile,     // nothing saved yet
  Damaged,    // a file is there but holds no valid image: it was renamed to kWindowsRegistryBadFile (for a look later) and *image is empty
  NotMerged,  // (load_saved_registry, registry_attach_storage) a valid image that could not be merged (out of memory): the file is left as it is
};

// Reads the saved image. The image is checked with RegistryTree::valid_image().
[[nodiscard]] RegistryLoad read_saved_registry(filesystem::IFileSystem& fs, ByteBuffer* image);
// Writes `image` as the saved registry (the folders are made if missing). False if the file system refused; the previous image is then still there.
[[nodiscard]] bool write_saved_registry(filesystem::IFileSystem& fs, ConstByteView image);
// Reads the saved image and merges it into `tree` (what the image holds wins over what the tree holds), then makes the system's own values again
// (RegistryTree::make_defaults: those are this system's). Loaded only if it was merged; NotMerged leaves the tree with part of the image.
[[nodiscard]] RegistryLoad load_saved_registry(RegistryTree& tree, filesystem::IFileSystem& fs, uint64_t now);

}  // namespace process::windowsabi
