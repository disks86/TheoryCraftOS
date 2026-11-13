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

// The PipeWire server's GPU images (SocketServer::set_gpu_images): images of the kernel's Direct3D layer, made in a process of the server's own on an
// adapter, exported as /dev/dxg exports are (so a client imports one as any DMA-BUF of the system) and passed as an open file whose gpu_buffer is the
// export. The allocation's handle is let go at once: the export holds it. Header-only, for the kernel (main.cpp) and the tests that link the Direct3D
// layer; the PipeWire server itself only sees IGpuImages.
#pragma once
#include <stdint.h>
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "PipeWire/SocketServer.hpp"
#include "Process/DxgBufferFile.hpp"
#include "Process/ProcessFiles.hpp"

namespace pipewire {

class DxgGpuImages final : public IGpuImages {
 public:
  static constexpr dxgk::ProcessId kProcess = 0x50573030;      // (the process the server is to the Direct3D layer)
  DxgGpuImages(dxgk::Kernel& kernel, allocators::IAllocator& allocator, uint32_t adapter = 0) : kernel_(kernel), allocator_(allocator), adapter_index_(adapter) {}
  ~DxgGpuImages() {
    if (opened_) kernel_.close_process(kProcess);
  }
  DxgGpuImages(const DxgGpuImages&) = delete;
  DxgGpuImages& operator=(const DxgGpuImages&) = delete;

  bool make(uint32_t width, uint32_t height, process::UnixInFlight* out, uint32_t* stride) override {
    *stride = 0;
    if (!open()) return false;
    dxgk::AllocationDesc d;
    d.flags = dxgk::kAllocTexture | dxgk::kAllocRenderTarget | dxgk::kAllocShared;
    d.format = dxgk::Format::A8R8G8B8;      // (B, G, R, A in memory: SPA's BGRA / BGRx)
    d.width = width;
    d.height = height;
    dxgk::Handle image = 0;
    if (!dxgk::status::ok(kernel_.create_allocation(kProcess, device_, d, &image))) return false;
    SharedPointer<dxgk::ExportedAllocation> exported;
    const bool ok = dxgk::status::ok(kernel_.export_allocation(kProcess, image, &exported)) && exported;
    kernel_.destroy_allocation(kProcess, image);      // (the export holds the image)
    if (!ok) return false;
    const dxgk::ExportInfo& info = dxgk::export_info(*exported);
    SharedPointer<process::OpenObject> object = SharedPointer<process::OpenObject>::make(allocator_, allocator_);
    UniquePointer<process::DxgBufferFile> file = UniquePointer<process::DxgBufferFile>::make(allocator_, allocator_, exported, info.bytes);
    if (!object || !file) return false;
    object->gpu_buffer = file.get();
    object->file = util::move(file);
    object->readable = true;
    object->writable = true;
    out->kind = process::DescriptorKind::File;
    out->object = util::move(object);
    *stride = info.pitch;
    return true;
  }

 private:
  bool open() {
    if (device_) return true;
    if (!opened_) {
      if (!dxgk::status::ok(kernel_.open_process(kProcess))) return false;
      opened_ = true;
    }
    dxgk::Handle adapter = 0;
    if (!dxgk::status::ok(kernel_.open_adapter_from_index(kProcess, adapter_index_, &adapter))) return false;
    return dxgk::status::ok(kernel_.create_device(kProcess, adapter, &device_));
  }

  dxgk::Kernel& kernel_;
  allocators::IAllocator& allocator_;
  uint32_t adapter_index_;
  bool opened_ = false;
  dxgk::Handle device_ = 0;
};

}  // namespace pipewire
