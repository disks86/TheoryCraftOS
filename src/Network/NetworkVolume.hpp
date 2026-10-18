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

// A network filesystem (SFTP, FTPS, ...) as a volume of the mount table: one folder of the file explorer's tree. The filesystem must outlive it.
#pragma once
#include "FileSystem/Mount/MountTable.hpp"

namespace network {

template <class FileSystem>
class NetworkVolume final : public filesystem::IVolume {
 public:
  explicit NetworkVolume(FileSystem& fs) : fs_(fs) {}
  filesystem::IFileSystem& file_system() override { return fs_; }

 private:
  FileSystem& fs_;
};

}  // namespace network
