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
#include "Process/RegistryStore.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/UniquePointer.hpp"

namespace process::windowsabi {

namespace {

enum class ReadResult : uint8_t { Ok, Missing, Bad };

// The whole file, if it is there and holds a valid image.
ReadResult read_image(filesystem::IFileSystem& fs, StringView path, ByteBuffer* out) {
  out->clear();
  UniquePointer<filesystem::IFile> file;
  const filesystem::Status opened = fs.open_file(path, filesystem::OpenMode::Read, &file);
  if (opened == filesystem::Status::NotFound) return ReadResult::Missing;
  if (opened != filesystem::Status::Ok || !file) return ReadResult::Bad;
  const uint64_t size = file->size();
  if (size == 0 || size > kRegistryStoreMaxBytes || !out->put_zeros(size)) return ReadResult::Bad;
  uint64_t got = 0;
  if (file->read(0, out->writable_view(), &got) != filesystem::Status::Ok || got != size || !RegistryTree::valid_image(out->view())) {
    out->clear();
    return ReadResult::Bad;
  }
  return ReadResult::Ok;
}

bool make_folder(filesystem::IFileSystem& fs, const char* path) {
  const filesystem::Status s = fs.create_directory(StringView(path));
  return s == filesystem::Status::Ok || s == filesystem::Status::AlreadyExists;
}

}  // namespace

RegistryLoad read_saved_registry(filesystem::IFileSystem& fs, ByteBuffer* image) {
  const ReadResult fresh = read_image(fs, StringView(filesystem::layout::kWindowsRegistryNewFile), image);
  if (fresh == ReadResult::Ok) return RegistryLoad::Loaded;
  const ReadResult saved = read_image(fs, StringView(filesystem::layout::kWindowsRegistryFile), image);
  if (saved == ReadResult::Ok) return RegistryLoad::Loaded;
  if (saved == ReadResult::Missing && fresh == ReadResult::Missing) return RegistryLoad::NoFile;
  // A file that holds no image: kept aside, so that the next save does not write over the only copy of what might still be read by hand.
  if (saved == ReadResult::Bad) {
    (void)fs.remove(StringView(filesystem::layout::kWindowsRegistryBadFile));
    (void)fs.rename(StringView(filesystem::layout::kWindowsRegistryFile), StringView(filesystem::layout::kWindowsRegistryBadFile));
  }
  if (fresh == ReadResult::Bad) (void)fs.remove(StringView(filesystem::layout::kWindowsRegistryNewFile));
  return RegistryLoad::Damaged;
}

bool write_saved_registry(filesystem::IFileSystem& fs, ConstByteView image) {
  static const char* const kFolders[] = {filesystem::layout::kWindows, filesystem::layout::kWindowsDirectory, filesystem::layout::kWindowsSystem32, filesystem::layout::kWindowsRegistryFolder};
  for (const char* folder : kFolders)
    if (!make_folder(fs, folder)) return false;
  const StringView fresh(filesystem::layout::kWindowsRegistryNewFile), saved(filesystem::layout::kWindowsRegistryFile);
  {
    UniquePointer<filesystem::IFile> file;
    if (fs.open_file(fresh, filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok || !file) return false;
    uint64_t put = 0;
    if (file->write(0, image, &put) != filesystem::Status::Ok || put != image.size() || file->flush() != filesystem::Status::Ok) {
      file = UniquePointer<filesystem::IFile>();
      (void)fs.remove(fresh);
      return false;
    }
  }      // (closed before it is renamed: an open file is Busy)
  if (fs.flush() != filesystem::Status::Ok) return false;      // the new image is on the medium before the old one goes
  const filesystem::Status removed = fs.remove(saved);
  if (removed != filesystem::Status::Ok && removed != filesystem::Status::NotFound) return false;      // (the new file stays: the next load takes it)
  if (fs.rename(fresh, saved) != filesystem::Status::Ok) return false;
  return fs.flush() == filesystem::Status::Ok;
}

RegistryLoad load_saved_registry(RegistryTree& tree, filesystem::IFileSystem& fs, uint64_t now) {
  ByteBuffer image(tree.allocator());
  const RegistryLoad read = read_saved_registry(fs, &image);
  if (read != RegistryLoad::Loaded) return read;
  const bool merged = tree.restore(RegistryTree::kRoot, image.view(), false, now) == kRegSuccess;
  tree.make_defaults(now);      // (the system's own keys are this system's, whatever the image said)
  return merged ? RegistryLoad::Loaded : RegistryLoad::NotMerged;
}

}  // namespace process::windowsabi
