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
#include "Process/DxgDevice.h"
#include "Process/LinuxFiles.hpp"
#include "Process/Win32k/Dxg/DxgInternal.hpp"
#include "Process/Win32k/Win32k.hpp"

// The Direct3D services of win32u.dll. The kernel calls install_dxg() after dxgk::set_system_kernel(); the handlers look the kernel up on every call, so a call made before
// there is one answers STATUS_NO_SUCH_DEVICE. What is implemented and how is in docs/win32k/Dxg.md.
namespace process::win32k {

namespace dxg {

namespace {

// Which numbers of win32k's range are services of this layer (written while the services are registered, at boot; only read after).
bool g_services[kServiceEnd - kServiceBase] = {};

// The numbers of /dev/dxg's header are the system call numbers (a Linux library is built without the C++ headers).
using N = WindowsSyscallNumber;
static_assert(TCOS_DXG_CLOSE_ADAPTER == uint32_t(N::NtGdiDdDDICloseAdapter) && TCOS_DXG_CREATE_ALLOCATION == uint32_t(N::NtGdiDdDDICreateAllocation) &&
              TCOS_DXG_CREATE_CONTEXT == uint32_t(N::NtGdiDdDDICreateContext) && TCOS_DXG_CREATE_DEVICE == uint32_t(N::NtGdiDdDDICreateDevice) &&
              TCOS_DXG_DESTROY_ALLOCATION == uint32_t(N::NtGdiDdDDIDestroyAllocation) && TCOS_DXG_DESTROY_CONTEXT == uint32_t(N::NtGdiDdDDIDestroyContext) &&
              TCOS_DXG_DESTROY_DEVICE == uint32_t(N::NtGdiDdDDIDestroyDevice) && TCOS_DXG_ENUM_ADAPTERS2 == uint32_t(N::NtGdiDdDDIEnumAdapters2) &&
              TCOS_DXG_ESCAPE == uint32_t(N::NtGdiDdDDIEscape) && TCOS_DXG_GET_DEVICE_STATE == uint32_t(N::NtGdiDdDDIGetDeviceState) &&
              TCOS_DXG_GET_DISPLAY_MODE_LIST == uint32_t(N::NtGdiDdDDIGetDisplayModeList) && TCOS_DXG_LOCK == uint32_t(N::NtGdiDdDDILock) &&
              TCOS_DXG_OPEN_ADAPTER_FROM_LUID == uint32_t(N::NtGdiDdDDIOpenAdapterFromLuid) && TCOS_DXG_PRESENT == uint32_t(N::NtGdiDdDDIPresent) &&
              TCOS_DXG_QUERY_ADAPTER_INFO == uint32_t(N::NtGdiDdDDIQueryAdapterInfo) && TCOS_DXG_RENDER == uint32_t(N::NtGdiDdDDIRender) &&
              TCOS_DXG_UNLOCK == uint32_t(N::NtGdiDdDDIUnlock),
              "DxgDevice.h's service numbers are the NtGdiDdDDI* system call numbers");

// /dev/dxg (LinuxFiles.cpp): a D3DKMT service called by a Linux process. It runs as the system call of a Windows process would: the thunks take their one argument (the
// parameter block) from the first argument register.
bool linux_service(Process& process, uint32_t service, uint64_t params, uint32_t* status) {
  if (!is_service(service)) return false;
  SyscallRequest request;
  request.number = service;
  request.args[0] = params;
  uint64_t result = 0;
  if (!dispatch(process, request, &result)) return false;      // (no object manager yet)
  *status = uint32_t(result);
  return true;
}

// /dev/dxg's GPU images (TCOS_DXG_IOCTL_EXPORT / IMPORT, LinuxFiles.cpp): an export of one of the process's allocations, and a handle of the process for one, recorded
// as the allocations it creates are (so that D3DKMTDestroyAllocation and the process's exit let go of it).
bool linux_export(Process& process, uint32_t allocation, SharedPointer<dxgk::ExportedAllocation>* out, uint64_t* bytes, uint32_t* status) {
  SyscallRequest request;
  request.number = uint32_t(N::NtGdiDdDDICreateAllocation);
  Call call(process, request);
  Session s(call);
  if (!s.ok()) {
    *status = s.failure();
    return true;
  }
  kernel::ScopedMutex g(s.state().lock);
  if (!s.state().find_allocation(allocation)) {
    *status = dxgk::status::kInvalidHandle;
    return true;
  }
  *status = s.kernel().export_allocation(s.pid(), allocation, out);
  if (*status == dxgk::status::kSuccess) *bytes = dxgk::export_info(**out).bytes;
  return true;
}

bool linux_import(Process& process, uint32_t device, const dxgk::ExportedAllocation& exported, linuxabi::DxgImported* out, uint32_t* status) {
  SyscallRequest request;
  request.number = uint32_t(N::NtGdiDdDDIOpenResource);
  Call call(process, request);
  Session s(call);
  if (!s.ok()) {
    *status = s.failure();
    return true;
  }
  kernel::ScopedMutex g(s.state().lock);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) {
    *status = dxgk::status::kInvalidHandle;
    return true;
  }
  dxgk::Handle handle = 0;
  dxgk::AllocationDesc desc;
  *status = s.kernel().import_allocation(s.pid(), device, exported, &handle, &desc);
  if (*status != dxgk::status::kSuccess) return true;
  AllocationRecord rec;
  rec.handle = handle;
  rec.device = device;
  if (!ps.allocations.push_back(rec)) {
    s.kernel().destroy_allocation(s.pid(), handle);
    *status = dxgk::status::kNoMemory;
    return true;
  }
  out->allocation = handle;
  out->width = desc.width;
  out->height = desc.height;
  out->format = static_cast<uint32_t>(desc.format);
  out->pitch = dxgk::export_info(exported).pitch;
  return true;
}

}  // namespace

void register_service(WindowsSyscallNumber number, Handler handler) {
  const uint32_t n = uint32_t(number);
  if (register_handler(number, handler, false) && n >= kServiceBase && n < kServiceEnd) g_services[n - kServiceBase] = true;
}

bool is_service(uint32_t service) { return service >= kServiceBase && service < kServiceEnd && g_services[service - kServiceBase]; }

}  // namespace dxg

void install_dxg() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  dxg::register_adapters();
  dxg::register_allocations();
  dxg::register_sync();
  dxg::register_work();
  dxg::register_unsupported();
  register_process_exit_hook(dxg::process_exited);
  linuxabi::set_dxg_service_handler(&dxg::linux_service);
  linuxabi::set_dxg_buffer_handlers(&dxg::linux_export, &dxg::linux_import);
  set_dxg_buffer_copier(&dxgk::copy_export);      // (mmap of a dmabuf: the CPU's copy, DxgBufferFile.hpp)
}

}  // namespace process::win32k
