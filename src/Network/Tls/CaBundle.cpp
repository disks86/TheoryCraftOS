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

#include "Network/Tls/CaBundle.hpp"
#include "Utility/AllocatedView.hpp"

namespace network {

uint32_t load_ca_bundle(crypto::TrustStore& store, filesystem::IFileSystem* fs, allocators::IAllocator& allocator) {
  using namespace filesystem;
  if (fs) {
    UniquePointer<IFile> file;
    if (fs->open_file("/certs/ca-bundle.pem", OpenMode::Read, &file) == Status::Ok) {
      const uint64_t size = file->size();
      if (size > 0 && size <= (4u << 20)) {
        ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator, size);
        uint64_t got = 0;
        uint32_t added = 0;
        if (!bytes.empty() && file->read(0, bytes.first(size), &got) == Status::Ok) added = store.add_pem(reinterpret_cast<const char*>(bytes.data()), got);
        release_view(allocator, bytes);
        if (added > 0) return added;
      }
    }
  }
  return store.add_pem(tcos_ca_bundle_start, static_cast<uint64_t>(tcos_ca_bundle_end - tcos_ca_bundle_start));
}

}  // namespace network
