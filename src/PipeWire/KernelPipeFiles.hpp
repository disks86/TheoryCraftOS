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


// The kernel's IPipeFiles (Pipe.hpp): Linux FIFOs (src/Process/Fifo.hpp), named as a Linux program names them (/tmp/music.output is /Linux/tmp/music.output
// of the OS's file system, /mnt/c/... the Windows tree), so that `cat /tmp/music.output` in a Linux program reads what module-pipe-sink writes. Header only:
// the PipeWire code itself never links src/Process (main.cpp and the tests that do include this).
#pragma once
#include "FileSystem/SystemLayout.hpp"
#include "PipeWire/Pipe.hpp"
#include "Process/Fifo.hpp"
#include "Process/ProcessFiles.hpp"

namespace pipewire {

class KernelPipeFiles final : public IPipeFiles {
 public:
  KernelPipeFiles(allocators::IAllocator& allocator, filesystem::IFileSystem& fs) : allocator_(allocator), fs_(fs), names_(process::PathStyle::Posix) {
    names_.attach(allocator, fs, filesystem::layout::kLinux);      // (only for its names: no descriptor is ever opened in it)
  }

  UniquePointer<IPipeFile> open(StringView path) override {
    String virtual_path(allocator_), host(allocator_);
    if (names_.resolve(path, &virtual_path) != process::PathResult::Ok || !names_.host_path_of(virtual_path.view(), &host)) return UniquePointer<IPipeFile>();
    UniquePointer<process::KernelFifo> fifo;
    if (process::KernelFifo::open(allocator_, fs_, host.view(), true, &fifo) != filesystem::Status::Ok) return UniquePointer<IPipeFile>();
    return UniquePointer<IPipeFile>::make_as<File>(allocator_, static_cast<UniquePointer<process::KernelFifo>&&>(fifo));
  }

 private:
  struct File final : IPipeFile {
    explicit File(UniquePointer<process::KernelFifo> f) : fifo(static_cast<UniquePointer<process::KernelFifo>&&>(f)) {}
    uint32_t write(ConstByteView data) override { return fifo->write(data); }
    uint32_t read(ByteView out) override { return fifo->read(out); }
    UniquePointer<process::KernelFifo> fifo;
  };

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& fs_;
  process::ProcessFiles names_;
};

}  // namespace pipewire
