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

#include "Font/FontFiles.hpp"
#include "Font/BundledFonts.hpp"
#include "Font/WoffDecode.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace font {

namespace {

constexpr uint64_t kMaxFont = uint64_t{8} << 20;      // (the largest decoded face is a few hundred KiB)

// Makes `path` and the folders above it.
bool make_folders(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView path) {
  String prefix(allocator);
  for (uint64_t i = 1; i <= path.size(); ++i) {
    if (i < path.size() && path[i] != '/') continue;
    if (!prefix.assign(path.first(i))) return false;
    filesystem::DirectoryEntry entry;
    if (fs.stat(prefix.view(), &entry) == filesystem::Status::Ok) {
      if (entry.type != filesystem::NodeType::Directory) return false;
      continue;
    }
    const filesystem::Status made = fs.create_directory(prefix.view());
    if (made != filesystem::Status::Ok && made != filesystem::Status::AlreadyExists) return false;
  }
  return true;
}

// "TCOS-Sans-BoldItalic.ttf" and the like.
bool file_name(const BundledFontInfo& face, String* out) {
  out->clear();
  for (char c : StringView(face.family))
    if (!out->push_back(c == ' ' ? '-' : c)) return false;
  const char* style = face.weight >= 700 ? (face.italic ? "-BoldItalic" : "-Bold") : (face.italic ? "-Italic" : "-Regular");
  return out->append(StringView(style)) && out->append(StringView(".ttf"));
}

}  // namespace

bool write_bundled_fonts(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView folder) {
  if (!make_folders(allocator, fs, folder)) return false;
  bool ok = true;
  const ArrayView<const BundledFontInfo> families[] = {bundled_sans_faces(), bundled_serif_faces(), bundled_mono_faces(), bundled_fallback_faces()};
  for (const ArrayView<const BundledFontInfo>& faces : families) {
    for (const BundledFontInfo& face : faces) {
      String name(allocator), path(allocator);
      if (!file_name(face, &name) || !path.assign(folder) || !path.push_back('/') || !path.append(name.view())) return false;
      filesystem::DirectoryEntry entry;
      if (fs.stat(path.view(), &entry) == filesystem::Status::Ok && entry.size > 0) continue;      // (there already)
      ByteBuffer sfnt(allocator);
      if (!decode_woff(face.woff, &sfnt, kMaxFont)) {
        ok = false;
        continue;
      }
      UniquePointer<filesystem::IFile> file;
      uint64_t written = 0;
      if (fs.open_file(path.view(), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok ||
          file->write(0, sfnt.view(), &written) != filesystem::Status::Ok || written != sfnt.size())
        ok = false;
    }
  }
  return ok;
}

}  // namespace font
