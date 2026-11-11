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

// A GPU image passed around as a file descriptor: what /dev/dxg's export makes (DxgDevice.h, TCOS_DXG_IOCTL_EXPORT), the counterpart of a Linux dmabuf. It is an
// ordinary file object (File kind, OpenObject::gpu_buffer says which), so close, dup, fstat, lseek(SEEK_END) for its size and SCM_RIGHTS work on it as on any
// file; reading, writing and mapping it are not supported (the picture is the GPU's). The kernel's Wayland compositor draws the image of one a client passes
// in place (src/Wayland, zwp_linux_dmabuf_v1), and another process can import it into its own device (TCOS_DXG_IOCTL_IMPORT).
//
// It holds the export (Kernel/Dxgkrnl/DxgKernel.hpp, dxgk::ExportedAllocation), which keeps the allocation alive after the process that made it destroyed it
// or exited. The export is only named here, so this layer does not depend on the Direct3D code.
#pragma once
#include <stdint.h>
#include "FileSystem/IFile.hpp"
#include "Utility/SharedPointer.hpp"

namespace dxgk {
class ExportedAllocation;
}

namespace process {

class DxgBufferFile final : public filesystem::IFile {
 public:
  DxgBufferFile(SharedPointer<dxgk::ExportedAllocation> exported, uint64_t bytes) : exported_(static_cast<SharedPointer<dxgk::ExportedAllocation>&&>(exported)), bytes_(bytes) {}
  DxgBufferFile(const DxgBufferFile&) = delete;
  DxgBufferFile& operator=(const DxgBufferFile&) = delete;
  const SharedPointer<dxgk::ExportedAllocation>& exported() const { return exported_; }

  // --- IFile (its size is the image's memory; nothing else)
  uint64_t size() const override { return bytes_; }
  filesystem::Status read(uint64_t, ByteView, uint64_t* transferred) override {
    *transferred = 0;
    return filesystem::Status::Unsupported;
  }
  filesystem::Status write(uint64_t, ConstByteView, uint64_t* transferred) override {
    *transferred = 0;
    return filesystem::Status::Unsupported;
  }
  filesystem::Status resize(uint64_t) override { return filesystem::Status::Unsupported; }
  filesystem::Status flush() override { return filesystem::Status::Ok; }

 private:
  SharedPointer<dxgk::ExportedAllocation> exported_;
  uint64_t bytes_;
};

}  // namespace process
