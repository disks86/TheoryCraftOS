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

// Wire structures and constants of the virtio-gpu device (virtio spec 5.7), plus
// the few virgl constants the 3D path needs. All fields are little-endian, which
// is what this machine is, so they are plain integers.
#pragma once
#include <stdint.h>

namespace hardware::virtio_gpu {

constexpr uint16_t kPciDevice = 0x1050;  // 0x1040 + device type 16
constexpr uint32_t kFeatureVirgl = 1u << 0;
constexpr uint32_t kFeatureEdid = 1u << 1;
constexpr uint32_t kFeatureResourceUuid = 1u << 2;
constexpr uint32_t kFeatureResourceBlob = 1u << 3;   // RESOURCE_CREATE_BLOB, MAP_BLOB, UNMAP_BLOB
constexpr uint32_t kFeatureContextInit = 1u << 4;    // CTX_CREATE takes a context type (capset id), commands may name a ring (kFlagInfoRingIndex)

// The shared memory region (virtio PCI capability type 8) a host-visible blob is mapped into (VIRTIO_GPU_SHM_ID_HOST_VISIBLE).
constexpr uint8_t kShmHostVisible = 1;

enum CommandType : uint32_t {
  kCmdGetDisplayInfo = 0x0100,
  kCmdResourceCreate2d,
  kCmdResourceUnref,
  kCmdSetScanout,
  kCmdResourceFlush,
  kCmdTransferToHost2d,
  kCmdResourceAttachBacking,
  kCmdResourceDetachBacking,
  kCmdGetCapsetInfo,
  kCmdGetCapset,
  kCmdGetEdid,
  kCmdResourceAssignUuid,
  kCmdResourceCreateBlob,
  kCmdSetScanoutBlob,
  kCmdCtxCreate = 0x0200,
  kCmdCtxDestroy,
  kCmdCtxAttachResource,
  kCmdCtxDetachResource,
  kCmdResourceCreate3d,
  kCmdTransferToHost3d,
  kCmdTransferFromHost3d,
  kCmdSubmit3d,
  kCmdResourceMapBlob,
  kCmdResourceUnmapBlob,
  kRespOkNoData = 0x1100,
  kRespOkDisplayInfo,
  kRespOkCapsetInfo,
  kRespOkCapset,
  kRespOkEdid,
  kRespOkResourceUuid,
  kRespOkMapInfo,
  kRespErrUnspec = 0x1200,   // (the errors start here)
  kRespErrOutOfMemory,
  kRespErrInvalidScanoutId,
  kRespErrInvalidResourceId,
  kRespErrInvalidContextId,
  kRespErrInvalidParameter,
};

constexpr uint32_t kFlagFence = 1u << 0;
constexpr uint32_t kFlagInfoRingIndex = 1u << 1;   // the fence belongs to ring `ring_index` of the context (with kFeatureContextInit)
constexpr uint32_t kResourceFlagY0Top = 1u << 0;

constexpr uint32_t kFormatB8G8R8A8Unorm = 1;
constexpr uint32_t kFormatB8G8R8X8Unorm = 2;
constexpr uint32_t kFormatR8G8B8A8Unorm = 67;

constexpr uint32_t kCapsetVirgl = 1;
constexpr uint32_t kCapsetVirgl2 = 2;
constexpr uint32_t kCapsetGfxstreamVulkan = 3;
constexpr uint32_t kCapsetVenus = 4;               // Vulkan, the host's driver behind the venus protocol (virglrenderer's vkr)
constexpr uint32_t kCapsetCrossDomain = 5;
constexpr uint32_t kCapsetDrm = 6;
constexpr uint32_t kContextInitCapsetMask = 0xFF;   // CtxCreate::context_init: the capset id in the low byte

// RESOURCE_CREATE_BLOB: where the memory is and what may be done with it.
constexpr uint32_t kBlobMemGuest = 1;          // guest pages (the entries that follow)
constexpr uint32_t kBlobMemHost3d = 2;         // memory of the host's 3D context (blob_id names it)
constexpr uint32_t kBlobMemHost3dGuest = 3;    // a host 3D object over guest pages
constexpr uint32_t kBlobFlagMappable = 1u << 0;
constexpr uint32_t kBlobFlagShareable = 1u << 1;
constexpr uint32_t kBlobFlagCrossDevice = 1u << 2;

// RespMapInfo::map_info: how the guest must map the memory.
constexpr uint32_t kMapCacheMask = 0x0F;
constexpr uint32_t kMapCacheNone = 0, kMapCacheCached = 1, kMapCacheUncached = 2, kMapCacheWriteCombine = 3;

#pragma pack(push, 1)
struct ControlHeader {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t context_id;
  uint8_t ring_index;
  uint8_t padding[3];
};
static_assert(sizeof(ControlHeader) == 24, "wire size");

struct Rect {
  uint32_t x, y, width, height;
};

struct ResourceCreate2d {
  ControlHeader header;
  uint32_t resource_id, format, width, height;
};

struct ResourceUnref {
  ControlHeader header;
  uint32_t resource_id, padding;
};

struct SetScanout {
  ControlHeader header;
  Rect rect;
  uint32_t scanout_id, resource_id;
};

struct ResourceFlush {
  ControlHeader header;
  Rect rect;
  uint32_t resource_id, padding;
};

struct TransferToHost2d {
  ControlHeader header;
  Rect rect;
  uint64_t offset;
  uint32_t resource_id, padding;
};

struct MemEntry {
  uint64_t address;
  uint32_t length, padding;
};

struct ResourceAttachBacking {
  ControlHeader header;
  uint32_t resource_id, entry_count;
  MemEntry entry;  // one contiguous run
};

struct GetDisplayInfo {
  ControlHeader header;
};

struct DisplayOne {
  Rect rect;
  uint32_t enabled, flags;
};

struct RespDisplayInfo {
  ControlHeader header;
  DisplayOne modes[16];
};

struct GetEdid {
  ControlHeader header;
  uint32_t scanout, padding;
};

struct RespEdid {
  ControlHeader header;
  uint32_t size, padding;
  uint8_t edid[1024];
};

struct GetCapsetInfo {
  ControlHeader header;
  uint32_t index, padding;
};

struct GetCapset {
  ControlHeader header;
  uint32_t capset_id, capset_version;
};

// The answer: the header and the capability set (the start of virgl_caps_v1 is what the driver reads).
struct RespCapset {
  ControlHeader header;
  uint8_t data[512];
};

struct RespCapsetInfo {
  ControlHeader header;
  uint32_t capset_id, max_version, max_size, padding;
};

struct CtxCreate {
  ControlHeader header;
  uint32_t name_length, context_init;
  char debug_name[64];
};

struct CtxResource {
  ControlHeader header;
  uint32_t resource_id, padding;
};

struct ResourceCreate3d {
  ControlHeader header;
  uint32_t resource_id, target, format, bind, width, height, depth, array_size, last_level, sample_count, flags,
      padding;
};

struct Box {
  uint32_t x, y, z, w, h, d;
};

struct TransferHost3d {
  ControlHeader header;
  Box box;
  uint64_t offset;
  uint32_t resource_id, level, stride, layer_stride;
};

struct ResourceCreateBlob {
  ControlHeader header;
  uint32_t resource_id, blob_mem, blob_flags, entry_count;
  uint64_t blob_id, size;
  // `entry_count` MemEntry follow (guest blobs).
};
static_assert(sizeof(ResourceCreateBlob) == 56, "wire size");

struct ResourceMapBlob {
  ControlHeader header;
  uint32_t resource_id, padding;
  uint64_t offset;   // where in the host-visible shared memory region
  uint64_t padding2;
};
static_assert(sizeof(ResourceMapBlob) == 48, "wire size");

struct RespMapInfo {
  ControlHeader header;
  uint32_t map_info, padding;
};

struct ResourceUnmapBlob {
  ControlHeader header;
  uint32_t resource_id, padding;
};

struct CmdSubmit {
  ControlHeader header;
  uint32_t size, padding;
  // `size` bytes of virgl commands follow.
};
#pragma pack(pop)

}  // namespace hardware::virtio_gpu
