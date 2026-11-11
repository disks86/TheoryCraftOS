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
// file; read() and write() do not (the picture is the GPU's). The kernel's Wayland compositor draws the image of one a client passes in place (src/Wayland,
// zwp_linux_dmabuf_v1), and another process can import it into its own device (TCOS_DXG_IOCTL_IMPORT).
//
// The CPU's view (mmap). The image lives in the video memory manager's memory, which a process cannot map, so mmap maps a copy of it: page-aligned memory of the
// descriptor's own (a MemoryBlock, as a memfd's), filled from the image at the first mmap. DMA_BUF_IOCTL_SYNC moves the pictures as on Linux: START with READ
// brings the image's current pixels into the copy (after the GPU's queued work on it), END with WRITE sends the copy to the image. A program that maps the
// copy writable and never calls the ioctl (several GBM-style clients do not) still has its writes reach the image: at each commit of a buffer made from the
// descriptor the compositor sends the copy to the image (flush_cpu_writes) - only while such a mapping is still there and only when the copy changed since it
// was last moved (a checksum says), so that the frames a client renders on the GPU are not overwritten by an old copy. Every descriptor passed or duplicated
// from one export shares one copy.
//
// It holds the export (Kernel/Dxgkrnl/DxgKernel.hpp, dxgk::ExportedAllocation), which keeps the allocation alive after the process that made it destroyed it
// or exited. The export is only named here, so that this layer does not depend on the Direct3D code: the copying is the Direct3D layer's, given at boot
// (set_dxg_buffer_copier; without one, mapping fails with ENODEV).
//
// Locking: a mutex of the descriptor's own guards the copy; the copier is called with it held (descriptor -> the Direct3D layer's lock).
#pragma once
#include <stdint.h>
#include "FileSystem/IFile.hpp"
#include "Kernel/Sync.hpp"
#include "Process/MemoryFile.hpp"
#include "Utility/SharedPointer.hpp"

namespace dxgk {
class ExportedAllocation;
class ExportedSync;
}

namespace process {

// Copies between an export's image and `bytes` (to the image when `to_image`): false when the GPU still works on it or the copy failed. The first bytes of the
// image's memory (its rows, pitch apart) and of `bytes` are what is copied: the shorter of the two.
using DxgBufferCopier = bool (*)(dxgk::ExportedAllocation& exported, ByteView bytes, bool to_image);
void set_dxg_buffer_copier(DxgBufferCopier copier);

class DxgBufferFile final : public filesystem::IFile {
 public:
  // DMA_BUF_IOCTL_SYNC's flags
  static constexpr uint64_t kSyncRead = 1, kSyncWrite = 2, kSyncEnd = 4;
  enum class SyncResult : uint8_t { Ok, Invalid, Busy, NoCopier };

  DxgBufferFile(allocators::IAllocator& allocator, SharedPointer<dxgk::ExportedAllocation> exported, uint64_t bytes);
  ~DxgBufferFile();
  DxgBufferFile(const DxgBufferFile&) = delete;
  DxgBufferFile& operator=(const DxgBufferFile&) = delete;
  const SharedPointer<dxgk::ExportedAllocation>& exported() const { return exported_; }

  // mmap: the copy (made and filled from the image the first time) with a use for the caller; empty when there is no copier or no memory. `writable_shared`:
  // the mapping is MAP_SHARED and writable (its writes are sent to the image at each commit from then on).
  BlockRef map_block(bool writable_shared);
  // DMA_BUF_IOCTL_SYNC.
  SyncResult sync(uint64_t flags);
  // Sends the copy to the image if a writable shared mapping of it is still there and the copy changed since it was last moved (the compositor, at a commit).
  // False when that did not work (the GPU still works on it).
  bool flush_cpu_writes();

  // --- IFile (its size is the image's memory; reading, writing and resizing are not supported)
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
  bool copy_locked(bool to_image);

  allocators::IAllocator& allocator_;
  SharedPointer<dxgk::ExportedAllocation> exported_;
  uint64_t bytes_;
  kernel::Mutex lock_;
  BlockRef copy_;                 // the CPU's copy (empty until the first mmap)
  bool written_shared_ = false;   // a writable shared mapping of the copy was made
  uint64_t moved_sum_ = 0;        // the checksum of the copy when it was last moved (either way)
};

// A fence passed around as a file descriptor: what /dev/dxg's TCOS_DXG_IOCTL_EXPORT_SYNC makes, the counterpart of a Linux DRM sync object's timeline (a Wayland
// client hands the compositor one in wp_linux_drm_syncobj_manager_v1.import_timeline). Closed, duplicated, passed with SCM_RIGHTS like any file; it has no bytes.
// It holds the export (dxgk::ExportedSync), which keeps the fence alive after its process destroyed it or exited.
class DxgSyncFile final : public filesystem::IFile {
 public:
  explicit DxgSyncFile(SharedPointer<dxgk::ExportedSync> exported);
  ~DxgSyncFile();
  DxgSyncFile(const DxgSyncFile&) = delete;
  DxgSyncFile& operator=(const DxgSyncFile&) = delete;
  const SharedPointer<dxgk::ExportedSync>& exported() const { return exported_; }

  uint64_t size() const override { return 0; }
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
  SharedPointer<dxgk::ExportedSync> exported_;
};

}  // namespace process
