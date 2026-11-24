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

/* The virtio-gpu render node: a process's own contexts on QEMU's virtio-gpu (C, shared by the kernel and the user-mode drivers: src/Vulkan's Venus backend,
 * and Mesa's venus driver running unchanged as a Linux program). docs/Venus.md has the design.
 *
 * Linux processes open /dev/dri/renderD128 and use Linux's virtio-gpu DRM interface (the structures and ioctl numbers of <drm/virtgpu_drm.h> and the DRM core
 * calls of <drm/drm.h> a render node answers), on every CPU the same layout. Windows processes reach the same object with NtTcosVirtGpu (system call 514,
 * TCOS_VIRTGPU_OP_*): OPEN gives a handle, IOCTL runs one of the same ioctls on it, MAP maps a buffer object where Linux's mmap() of the node would. The kernel
 * side is one implementation (src/Graphics/VirtGpu/VirtGpuSession.hpp) behind both. */
#ifndef TCOS_VIRTGPU_DEVICE_H
#define TCOS_VIRTGPU_DEVICE_H
#include <stdint.h>

#define TCOS_VIRTGPU_RENDER_NODE_PATH "/dev/dri/renderD128"
#define TCOS_DRM_MAJOR 226u
#define TCOS_VIRTGPU_RENDER_MINOR 128u
/* What DRM_IOCTL_VERSION names the driver (Mesa looks for "virtio_gpu"), and the version Linux 6.12's virtio-gpu reports. */
#define TCOS_VIRTGPU_DRIVER_NAME "virtio_gpu"
#define TCOS_VIRTGPU_DRIVER_DESC "virtio GPU"
#define TCOS_VIRTGPU_DRIVER_DATE "0"
#define TCOS_VIRTGPU_VERSION_MAJOR 0
#define TCOS_VIRTGPU_VERSION_MINOR 1
#define TCOS_VIRTGPU_VERSION_PATCH 0

/* _IOC(dir, type, nr, size): Linux's ioctl number encoding (asm-generic/ioctl.h, which every CPU this system runs Linux programs for uses). */
#define TCOS_IOC(dir, type, nr, size) ((uint32_t)(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr)))
#define TCOS_IOC_NONE 0u
#define TCOS_IOC_WRITE 1u
#define TCOS_IOC_READ 2u
#define TCOS_IOC_NR(request) ((request) & 0xFFu)
#define TCOS_IOC_TYPE(request) (((request) >> 8) & 0xFFu)
#define TCOS_IOC_SIZE(request) (((request) >> 16) & 0x3FFFu)
#define TCOS_IOC_DIR(request) (((request) >> 30) & 3u)
#define TCOS_DRM_IOCTL_BASE 'd'
#define TCOS_DRM_COMMAND_BASE 0x40u

/* ---- the DRM core calls a render node answers (drm.h) ---------------------------------------------------------------------------------------------------- */

typedef struct TcosDrmVersion {
  int32_t version_major, version_minor, version_patchlevel;
  uint32_t pad;
  uint64_t name_len; /* in: the size of the buffer at `name`; out: the length of the name */
  uint64_t name;
  uint64_t date_len;
  uint64_t date;
  uint64_t desc_len;
  uint64_t desc;
} TcosDrmVersion;

typedef struct TcosDrmGemClose {
  uint32_t handle, pad;
} TcosDrmGemClose;

typedef struct TcosDrmGetCap {
  uint64_t capability, value;
} TcosDrmGetCap;

typedef struct TcosDrmSetClientCap {
  uint64_t capability, value;
} TcosDrmSetClientCap;

typedef struct TcosDrmPrimeHandle {
  uint32_t handle;
  uint32_t flags; /* DRM_CLOEXEC, DRM_RDWR */
  int32_t fd;
} TcosDrmPrimeHandle;

typedef struct TcosDrmUnique {
  uint64_t unique_len;
  uint64_t unique;
} TcosDrmUnique;

#define TCOS_DRM_IOCTL_VERSION TCOS_IOC(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x00, sizeof(TcosDrmVersion))
#define TCOS_DRM_IOCTL_GET_UNIQUE TCOS_IOC(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x01, sizeof(TcosDrmUnique))
#define TCOS_DRM_IOCTL_GEM_CLOSE TCOS_IOC(TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x09, sizeof(TcosDrmGemClose))
#define TCOS_DRM_IOCTL_GET_CAP TCOS_IOC(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x0c, sizeof(TcosDrmGetCap))
#define TCOS_DRM_IOCTL_SET_CLIENT_CAP TCOS_IOC(TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x0d, sizeof(TcosDrmSetClientCap))
#define TCOS_DRM_IOCTL_PRIME_HANDLE_TO_FD TCOS_IOC(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x2d, sizeof(TcosDrmPrimeHandle))
#define TCOS_DRM_IOCTL_PRIME_FD_TO_HANDLE TCOS_IOC(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_DRM_IOCTL_BASE, 0x2e, sizeof(TcosDrmPrimeHandle))

#define TCOS_DRM_CAP_DUMB_BUFFER 0x1
#define TCOS_DRM_CAP_PRIME 0x5
#define TCOS_DRM_PRIME_CAP_IMPORT 0x1
#define TCOS_DRM_PRIME_CAP_EXPORT 0x2
#define TCOS_DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define TCOS_DRM_CAP_SYNCOBJ 0x13
#define TCOS_DRM_CAP_SYNCOBJ_TIMELINE 0x14
#define TCOS_DRM_CLOEXEC 02000000u /* O_CLOEXEC */
#define TCOS_DRM_RDWR 02u          /* O_RDWR */

/* ---- virtio-gpu (virtgpu_drm.h) -------------------------------------------------------------------------------------------------------------------------- */

#define TCOS_VIRTGPU_MAP 0x01
#define TCOS_VIRTGPU_EXECBUFFER 0x02
#define TCOS_VIRTGPU_GETPARAM 0x03
#define TCOS_VIRTGPU_RESOURCE_CREATE 0x04
#define TCOS_VIRTGPU_RESOURCE_INFO 0x05
#define TCOS_VIRTGPU_TRANSFER_FROM_HOST 0x06
#define TCOS_VIRTGPU_TRANSFER_TO_HOST 0x07
#define TCOS_VIRTGPU_WAIT 0x08
#define TCOS_VIRTGPU_GET_CAPS 0x09
#define TCOS_VIRTGPU_RESOURCE_CREATE_BLOB 0x0a
#define TCOS_VIRTGPU_CONTEXT_INIT 0x0b

#define TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN 0x01u
#define TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02u
#define TCOS_VIRTGPU_EXECBUF_RING_IDX 0x04u
#define TCOS_VIRTGPU_EXECBUF_FLAGS (TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN | TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT | TCOS_VIRTGPU_EXECBUF_RING_IDX)

typedef struct TcosVirtgpuMap {
  uint64_t offset; /* out: what to give mmap() (Linux) or TCOS_VIRTGPU_OP_MAP (Windows) */
  uint32_t handle;
  uint32_t pad;
} TcosVirtgpuMap;

typedef struct TcosVirtgpuExecbufferSyncobj {
  uint32_t handle, flags;
  uint64_t point;
} TcosVirtgpuExecbufferSyncobj;

typedef struct TcosVirtgpuExecbuffer {
  uint32_t flags;
  uint32_t size;
  uint64_t command;      /* the commands (`size` bytes) */
  uint64_t bo_handles;   /* uint32_t handles */
  uint32_t num_bo_handles;
  int32_t fence_fd;      /* in/out fence (TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN / _OUT) */
  uint32_t ring_idx;     /* with TCOS_VIRTGPU_EXECBUF_RING_IDX */
  uint32_t syncobj_stride;
  uint32_t num_in_syncobjs;
  uint32_t num_out_syncobjs;
  uint64_t in_syncobjs;
  uint64_t out_syncobjs;
} TcosVirtgpuExecbuffer;

#define TCOS_VIRTGPU_PARAM_3D_FEATURES 1
#define TCOS_VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define TCOS_VIRTGPU_PARAM_RESOURCE_BLOB 3
#define TCOS_VIRTGPU_PARAM_HOST_VISIBLE 4
#define TCOS_VIRTGPU_PARAM_CROSS_DEVICE 5
#define TCOS_VIRTGPU_PARAM_CONTEXT_INIT 6
#define TCOS_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS 7
#define TCOS_VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME 8

typedef struct TcosVirtgpuGetparam {
  uint64_t param;
  uint64_t value; /* the address the answer is written to: an int (4 bytes), as Linux writes it */
} TcosVirtgpuGetparam;

typedef struct TcosVirtgpuResourceCreate {
  uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
  uint32_t bo_handle;  /* out */
  uint32_t res_handle; /* out */
  uint32_t size;
  uint32_t stride;
} TcosVirtgpuResourceCreate;

typedef struct TcosVirtgpuResourceInfo {
  uint32_t bo_handle;
  uint32_t res_handle; /* out */
  uint32_t size;       /* out */
  uint32_t blob_mem;   /* out */
} TcosVirtgpuResourceInfo;

typedef struct TcosVirtgpuBox {
  uint32_t x, y, z, w, h, d;
} TcosVirtgpuBox;

typedef struct TcosVirtgpuTransfer {
  uint32_t bo_handle;
  TcosVirtgpuBox box;
  uint32_t level, offset, stride, layer_stride;
} TcosVirtgpuTransfer;

#define TCOS_VIRTGPU_CAPSET_VIRGL 1
#define TCOS_VIRTGPU_CAPSET_VIRGL2 2
#define TCOS_VIRTGPU_CAPSET_GFXSTREAM_VULKAN 3
#define TCOS_VIRTGPU_CAPSET_VENUS 4
#define TCOS_VIRTGPU_CAPSET_CROSS_DOMAIN 5
#define TCOS_VIRTGPU_CAPSET_DRM 6

#define TCOS_VIRTGPU_WAIT_NOWAIT 1u
typedef struct TcosVirtgpuWait {
  uint32_t handle, flags;
} TcosVirtgpuWait;

typedef struct TcosVirtgpuGetCaps {
  uint32_t cap_set_id, cap_set_ver;
  uint64_t addr;
  uint32_t size, pad;
} TcosVirtgpuGetCaps;

#define TCOS_VIRTGPU_BLOB_MEM_GUEST 0x0001u
#define TCOS_VIRTGPU_BLOB_MEM_HOST3D 0x0002u
#define TCOS_VIRTGPU_BLOB_MEM_HOST3D_GUEST 0x0003u
#define TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001u
#define TCOS_VIRTGPU_BLOB_FLAG_USE_SHAREABLE 0x0002u
#define TCOS_VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004u

/* (programs built against headers before Linux 6.14 send the first 48 bytes, without blob_hints: both sizes are taken) */
typedef struct TcosVirtgpuResourceCreateBlob {
  uint32_t blob_mem;
  uint32_t blob_flags;
  uint32_t bo_handle;  /* out */
  uint32_t res_handle; /* out */
  uint64_t size;
  uint32_t pad;
  uint32_t cmd_size;   /* commands sent to the context before the resource is made */
  uint64_t cmd;
  uint64_t blob_id;
  uint32_t blob_hints;
  uint32_t pad2;
} TcosVirtgpuResourceCreateBlob;
#define TCOS_VIRTGPU_RESOURCE_CREATE_BLOB_SIZE_V1 48u

#define TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID 0x0001
#define TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS 0x0002
#define TCOS_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define TCOS_VIRTGPU_CONTEXT_PARAM_DEBUG_NAME 0x0004
typedef struct TcosVirtgpuContextSetParam {
  uint64_t param, value;
} TcosVirtgpuContextSetParam;

typedef struct TcosVirtgpuContextInit {
  uint32_t num_params, pad;
  uint64_t ctx_set_params; /* TcosVirtgpuContextSetParam[num_params] */
} TcosVirtgpuContextInit;

#define TCOS_VIRTGPU_EVENT_FENCE_SIGNALED 0x90000000u

#define TCOS_DRM_IOCTL_VIRTGPU(dir, nr, type) TCOS_IOC((dir), TCOS_DRM_IOCTL_BASE, TCOS_DRM_COMMAND_BASE + (nr), sizeof(type))
#define TCOS_DRM_IOCTL_VIRTGPU_MAP TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_MAP, TcosVirtgpuMap)
#define TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_EXECBUFFER, TcosVirtgpuExecbuffer)
#define TCOS_DRM_IOCTL_VIRTGPU_GETPARAM TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_GETPARAM, TcosVirtgpuGetparam)
#define TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_RESOURCE_CREATE, TcosVirtgpuResourceCreate)
#define TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_RESOURCE_INFO, TcosVirtgpuResourceInfo)
#define TCOS_DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_WRITE, TCOS_VIRTGPU_TRANSFER_FROM_HOST, TcosVirtgpuTransfer)
#define TCOS_DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_WRITE, TCOS_VIRTGPU_TRANSFER_TO_HOST, TcosVirtgpuTransfer)
#define TCOS_DRM_IOCTL_VIRTGPU_WAIT TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_WRITE, TCOS_VIRTGPU_WAIT, TcosVirtgpuWait)
#define TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_GET_CAPS, TcosVirtgpuGetCaps)
#define TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_RESOURCE_CREATE_BLOB, TcosVirtgpuResourceCreateBlob)
#define TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT TCOS_DRM_IOCTL_VIRTGPU(TCOS_IOC_READ | TCOS_IOC_WRITE, TCOS_VIRTGPU_CONTEXT_INIT, TcosVirtgpuContextInit)

/* ---- NtTcosVirtGpu (Windows) ------------------------------------------------------------------------------------------------------------------------------
 * NtTcosVirtGpu(PULONG64 arguments): 8 ULONG64 in the program's memory, one layout for 32- and 64-bit programs:
 *   [0] operation  [1] the handle (all but OPEN)  [2..3] parameters  [4..7] results (written back)
 *
 *   1 OPEN    [4] = a handle for a new session (a render node opened). STATUS_NO_SUCH_DEVICE without a virtio-gpu that has context types.
 *   2 CLOSE   the session goes (its context, buffer handles and mappings with it once nothing else holds them).
 *   3 IOCTL   [2] the request (a TCOS_DRM_IOCTL_* number), [3] the address of its structure; [4] = 0 or the Linux error number the render node would give
 *             (an errno, positive). The fence descriptors of EXECBUFFER and the PRIME calls have no Windows meaning: they are EOPNOTSUPP there.
 *   4 MAP     [2] an offset TCOS_DRM_IOCTL_VIRTGPU_MAP gave, [3] the size; [4] = where the buffer is mapped in the process (read and write).
 *   5 UNMAP   [2] such an address, [3] the size.
 * The status: STATUS_SUCCESS, STATUS_INVALID_HANDLE for a handle that is not a session, STATUS_INVALID_PARAMETER for an unknown operation, STATUS_ACCESS_VIOLATION for
 * memory the program does not have, STATUS_NO_MEMORY / STATUS_CONFLICTING_ADDRESSES when a mapping does not fit. */
#define TCOS_VIRTGPU_SERVICE 514
#define TCOS_VIRTGPU_OP_OPEN 1
#define TCOS_VIRTGPU_OP_CLOSE 2
#define TCOS_VIRTGPU_OP_IOCTL 3
#define TCOS_VIRTGPU_OP_MAP 4
#define TCOS_VIRTGPU_OP_UNMAP 5

#ifdef __cplusplus
static_assert(sizeof(TcosDrmVersion) == 64, "drm_version");
static_assert(sizeof(TcosDrmPrimeHandle) == 12, "drm_prime_handle");
static_assert(sizeof(TcosVirtgpuExecbuffer) == 64, "drm_virtgpu_execbuffer");
static_assert(sizeof(TcosVirtgpuResourceCreate) == 56, "drm_virtgpu_resource_create");
static_assert(sizeof(TcosVirtgpuTransfer) == 44, "drm_virtgpu_3d_transfer");
static_assert(sizeof(TcosVirtgpuGetCaps) == 24, "drm_virtgpu_get_caps");
static_assert(sizeof(TcosVirtgpuResourceCreateBlob) == 56, "drm_virtgpu_resource_create_blob");
static_assert(sizeof(TcosVirtgpuContextInit) == 16, "drm_virtgpu_context_init");
#endif

#endif
