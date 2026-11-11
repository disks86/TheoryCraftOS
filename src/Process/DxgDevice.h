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

/* /dev/dxg: the kernel-mode Direct3D layer for Linux processes (C, shared by the kernel and the Linux user-mode libraries such as src/Linux/libvulkan).
 *
 * A Windows process reaches the kernel's D3DKMT services with the NtGdiDdDDI* system calls (gdi32.dll's D3DKMT* thunks). A Linux process opens /dev/dxg (the name WSL gives
 * its device of the same kind) and makes the same calls with one ioctl: TCOS_DXG_IOCTL_CALL with a TcosDxgCall that names the service and points at its parameter block. The
 * service runs exactly as the system call would (src/Process/Win32k/Dxg): the parameter block has the layout of Windows' d3dkmthk.h for x64 (handles 32 bits, pointers and
 * sizes 64 bits, the offsets of DxgLayouts.hpp), on every CPU, so a driver built over D3DKMT (the Vulkan driver of src/Vulkan) is the same code on both systems.
 *
 * The ioctl answers 0 when the service ran (its NTSTATUS is in TcosDxgCall::status), -EFAULT when the TcosDxgCall cannot be read or written, -EINVAL for a service that is not
 * one of the D3DKMT services, -ENODEV when the system has no Direct3D layer, -ENOTTY for another request. */
#ifndef TCOS_DXG_DEVICE_H
#define TCOS_DXG_DEVICE_H
#include <stdint.h>

#define TCOS_DXG_DEVICE_PATH "/dev/dxg"

typedef struct TcosDxgCall {
  uint32_t service;   /* a TCOS_DXG_* number (the NtGdiDdDDI* system call number of the service) */
  int32_t status;     /* out: the NTSTATUS of the service */
  uint64_t params;    /* the address of the D3DKMT_* parameter block (the one argument of the D3DKMT function) */
} TcosDxgCall;

/* _IOWR('G', 1, TcosDxgCall): read and written, type 'G', number 1, 16 bytes. */
#define TCOS_DXG_IOCTL_CALL 0xC0104701u

/* A GPU image as a file descriptor (the counterpart of a Linux dmabuf: what a Wayland client passes in zwp_linux_buffer_params_v1.add). TCOS_DXG_IOCTL_EXPORT
 * makes one for an allocation of the process: a 2D image of one level and one sample in an 8-bit four-channel colour format, made shareable (the allocation flag
 * 1 << 7, kAllocShared). The descriptor holds the image on its own: it stays valid after the allocation is destroyed or the process exits. It can be closed,
 * duplicated, passed with SCM_RIGHTS, measured with lseek(SEEK_END) and mapped (a CPU copy of the image, moved with DMA_BUF_IOCTL_SYNC as on Linux), not read or
 * written. TCOS_DXG_IOCTL_IMPORT gives the process an allocation
 * handle for the image of such a descriptor, on one of its devices (of the same adapter).
 * Both answer 0 when the service ran (its NTSTATUS in `status`; fd / allocation only on success), -EFAULT for an unreadable structure, -EBADF for a descriptor
 * that is not such an image (import), -EMFILE when the table is full (export), -ENODEV without a Direct3D layer. */
typedef struct TcosDxgExport {
  uint32_t allocation; /* the allocation handle */
  uint32_t flags;      /* TCOS_DXG_EXPORT_CLOEXEC */
  int32_t fd;          /* out: the descriptor */
  int32_t status;      /* out: the NTSTATUS */
} TcosDxgExport;
#define TCOS_DXG_EXPORT_CLOEXEC 1u
/* _IOWR('G', 2, TcosDxgExport), 16 bytes */
#define TCOS_DXG_IOCTL_EXPORT 0xC0104702u

typedef struct TcosDxgImport {
  int32_t fd;          /* the descriptor of the image */
  uint32_t device;     /* the device handle to open it on */
  uint32_t allocation; /* out: the new allocation handle */
  int32_t status;      /* out: the NTSTATUS */
  uint32_t width, height;
  uint32_t format;     /* out: the kernel's format number (the AW_FORMAT word of an allocation's description) */
  uint32_t pitch;      /* out: bytes per row */
} TcosDxgImport;
/* _IOWR('G', 3, TcosDxgImport), 32 bytes */
#define TCOS_DXG_IOCTL_IMPORT 0xC0204703u

/* The services, numbered as the NtGdiDdDDI* system calls (src/Process/WindowsSyscalls.hpp; the kernel checks that the numbers agree). Any D3DKMT service of the kernel may be
 * named by its number; these are the ones the user-mode drivers use. */
enum {
  TCOS_DXG_CLOSE_ADAPTER = 4311,
  TCOS_DXG_CREATE_ALLOCATION = 4314,
  TCOS_DXG_CREATE_CONTEXT = 4316,
  TCOS_DXG_CREATE_DEVICE = 4319,
  TCOS_DXG_DESTROY_ALLOCATION = 4333,
  TCOS_DXG_DESTROY_CONTEXT = 4334,
  TCOS_DXG_DESTROY_DEVICE = 4336,
  TCOS_DXG_ENUM_ADAPTERS2 = 4348,
  TCOS_DXG_ESCAPE = 4350,
  TCOS_DXG_GET_DEVICE_STATE = 4360,
  TCOS_DXG_GET_DISPLAY_MODE_LIST = 4361,
  TCOS_DXG_LOCK = 4384,
  TCOS_DXG_OPEN_ADAPTER_FROM_LUID = 4397,
  TCOS_DXG_PRESENT = 4418,
  TCOS_DXG_QUERY_ADAPTER_INFO = 4423,
  TCOS_DXG_RENDER = 4443,
  TCOS_DXG_UNLOCK = 4477
};

#endif
