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
#include "Process/DxgBufferFile.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux file system calls (LinuxSyscalls.cpp maps numbers to these). Each returns the value for the result register: the result, or -errno.
namespace process::linuxabi {

uint64_t sys_memfd_create(Process& p, uint64_t name, uint64_t flags);
uint64_t sys_fallocate(Process& p, uint64_t fd, uint64_t mode, uint64_t offset, uint64_t length);
uint64_t sys_openat(Process& p, UserArch arch, uint64_t dirfd, uint64_t path, uint64_t flags);
uint64_t sys_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t sys_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t sys_pread(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset);
uint64_t sys_pwrite(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset);
uint64_t sys_readv(Process& p, uint64_t fd, uint64_t iov, uint64_t count);
uint64_t sys_writev(Process& p, uint64_t fd, uint64_t iov, uint64_t count);
uint64_t sys_lseek(Process& p, uint64_t fd, uint64_t offset, uint64_t whence);
uint64_t sys_fstat(Process& p, UserArch arch, uint64_t fd, uint64_t buffer);
uint64_t sys_statat(Process& p, UserArch arch, uint64_t dirfd, uint64_t path, uint64_t buffer, uint64_t flags);
uint64_t sys_statx(Process& p, uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t buffer);
uint64_t sys_getdents64(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t sys_mkdirat(Process& p, uint64_t dirfd, uint64_t path);
uint64_t sys_unlinkat(Process& p, uint64_t dirfd, uint64_t path, uint64_t flags);
uint64_t sys_renameat(Process& p, uint64_t old_dirfd, uint64_t old_path, uint64_t new_dirfd, uint64_t new_path);
uint64_t sys_getcwd(Process& p, uint64_t buffer, uint64_t size);
uint64_t sys_chdir(Process& p, uint64_t path);
uint64_t sys_fchdir(Process& p, uint64_t fd);
uint64_t sys_faccessat(Process& p, uint64_t dirfd, uint64_t path, uint64_t mode);
uint64_t sys_truncate(Process& p, uint64_t path, uint64_t length);
uint64_t sys_ftruncate(Process& p, uint64_t fd, uint64_t length);
uint64_t sys_fsync(Process& p, uint64_t fd);
uint64_t sys_dup(Process& p, uint64_t fd);
uint64_t sys_dup3(Process& p, uint64_t old_fd, uint64_t new_fd, uint64_t flags);
uint64_t sys_fcntl(Process& p, uint64_t fd, uint64_t command, uint64_t argument);
uint64_t sys_readlinkat(Process& p, uint64_t dirfd, uint64_t path);
uint64_t sys_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument);

// The D3DKMT services behind /dev/dxg (src/Process/DxgDevice.h): runs service `service` (an NtGdiDdDDI* number) for the process with its parameter block at `params` and
// stores its NTSTATUS. False if `service` is not a D3DKMT service. win32k's Direct3D layer installs it (install_dxg); without one, ioctl on /dev/dxg answers ENODEV.
using DxgServiceHandler = bool (*)(Process& process, uint32_t service, uint64_t params, uint32_t* status);
void set_dxg_service_handler(DxgServiceHandler handler);
// The GPU images of /dev/dxg (TCOS_DXG_IOCTL_EXPORT / IMPORT, DxgBufferFile.hpp), by the same layer: an export of allocation `allocation` of the process (with
// its memory's size), and a handle of the process on `device` for an export. Both store the NTSTATUS; false when there is no Direct3D layer.
struct DxgImported {
  uint32_t allocation = 0, width = 0, height = 0, format = 0, pitch = 0;
};
using DxgExportHandler = bool (*)(Process& process, uint32_t allocation, SharedPointer<dxgk::ExportedAllocation>* out, uint64_t* bytes, uint32_t* status);
using DxgImportHandler = bool (*)(Process& process, uint32_t device, const dxgk::ExportedAllocation& exported, DxgImported* out, uint32_t* status);
void set_dxg_buffer_handlers(DxgExportHandler export_handler, DxgImportHandler import_handler);
// Fences as descriptors (TCOS_DXG_IOCTL_EXPORT_SYNC / IMPORT_SYNC), by the same layer.
using DxgExportSyncHandler = bool (*)(Process& process, uint32_t sync, SharedPointer<dxgk::ExportedSync>* out, uint32_t* status);
using DxgImportSyncHandler = bool (*)(Process& process, uint32_t device, const dxgk::ExportedSync& exported, uint32_t* sync, uint32_t* status);
void set_dxg_sync_handlers(DxgExportSyncHandler export_handler, DxgImportSyncHandler import_handler);
// The kernel's OpenGL behind /dev/dxg (TCOS_DXG_IOCTL_GL): runs the request at `request` (src/Process/WindowsOpenGl.hpp's layout) and answers its NTSTATUS. The kernel's
// OpenGL installs it (opengl::install_linux_gl); without one the ioctl answers ENODEV.
using GlRequestHandler = uint32_t (*)(Process& process, uint64_t request);
void set_gl_request_handler(GlRequestHandler handler);

// Read-write memory in a Linux process for the kernel's own services (a string or a mapped buffer handed to the program), where mmap would put it; 0 if there is none.
// Freed with unmap_user_memory and the same size.
uint64_t map_user_memory(Process& process, uint64_t bytes);
void unmap_user_memory(Process& process, uint64_t address, uint64_t bytes);

// The errno (as a result register value, -errno) for a file system status.
uint64_t errno_of_status(filesystem::Status status);

}  // namespace process::linuxabi
