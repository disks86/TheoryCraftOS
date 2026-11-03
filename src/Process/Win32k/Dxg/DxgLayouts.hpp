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

// The user-mode structures of the D3DKMT thunks (Windows x64 layout; d3dkmthk.h / d3dukmdt.h of the Windows SDK, as shipped in tests/windows/wine/include), as the sizes the
// handlers read and the offsets they use. The handlers read a structure once into a UserBlock and take the fields out by offset: no C++ structure with bit-fields or
// padding stands for the caller's. The comments name the fields. Handles are 32 bits, pointers 8 bytes.
namespace process::win32k::dxg::layout {

// --- Adapters and devices
constexpr uint64_t sizeof_handle = 4;                       // D3DKMT_CLOSEADAPTER, DESTROYDEVICE, DESTROYCONTEXT, DESTROYSYNCHRONIZATIONOBJECT, WAITFORIDLE: { hX }
constexpr uint64_t sizeof_open_from_luid = 12;              // { LUID AdapterLuid @0 (low, high); hAdapter @8 }
constexpr uint64_t sizeof_open_from_device_name = 24;       // { pDeviceName @0; hAdapter @8; LUID @12 }
constexpr uint64_t sizeof_adapter_info = 20;                // D3DKMT_ADAPTERINFO { hAdapter @0; LUID @4; NumOfSources @12; bPrecisePresentRegionsPreferred @16 }
constexpr uint64_t sizeof_enum_adapters = 324;              // { NumAdapters @0; Adapters[16] @4 }
constexpr uint64_t sizeof_enum_adapters2 = 16;              // { NumAdapters @0; pAdapters @8 }
constexpr uint64_t sizeof_enum_adapters3 = 24;              // { Filter (64 bits) @0; NumAdapters @8; pAdapters @16 }
constexpr uint64_t sizeof_query_adapter_info = 24;          // { hAdapter @0; Type @4; pPrivateDriverData @8; PrivateDriverDataSize @16 }
constexpr uint64_t sizeof_query_video_memory_info = 56;     // { hProcess @0; hAdapter @8; Group @12; Budget @16; CurrentUsage @24; CurrentReservation @32; AvailableForReservation @40; PhysicalAdapterIndex @48 }
constexpr uint64_t sizeof_create_device = 64;               // { hAdapter @0; Flags @8; hDevice @12; pCommandBuffer @16; size @24; pAllocationList @32; size @40; pPatchLocationList @48; size @56 }
constexpr uint64_t sizeof_get_device_state = 12;            // { hDevice @0; StateType @4; union @8 }
constexpr uint64_t sizeof_get_display_mode_list = 24;       // { hAdapter @0; VidPnSourceId @4; pModeList @8; ModeCount @16 }
constexpr uint64_t sizeof_display_mode = 32;                // D3DKMT_DISPLAYMODE: Width, Height, Format, IntegerRefreshRate, ScanLineOrdering, DisplayOrientation, DisplayFixedOutput, Flags
constexpr uint64_t sizeof_set_display_mode = 8;             // { hDevice @0; hPrimaryAllocation @4; ... }
constexpr uint64_t sizeof_wait_vblank = 12;                 // { hAdapter @0; hDevice @4; VidPnSourceId @8 }
constexpr uint64_t sizeof_set_queued_limit = 12;            // { hDevice @0; Type @4; QueuedPresentLimit @8 }
constexpr uint64_t sizeof_set_vidpn_source_owner = 32;     // { hDevice @0; pType @8; pVidPnSourceId @16; VidPnSourceCount @24 }
constexpr uint64_t sizeof_check_vidpn_exclusive_ownership = 8;   // { hAdapter @0; VidPnSourceId @4 }
constexpr uint64_t sizeof_evict = 16;                       // D3DKMT_EVICT { hDevice @0; NumAllocations @4; AllocationList @8; Flags @16; NumBytesToTrim @24 }
constexpr uint64_t sizeof_trim_process_commitment = 8;      // { hProcess @0; Flags @8; DecommitRequested @16 }

// --- Allocations. D3DKMT_CREATEALLOCATION: { hDevice @0; hResource @4; hGlobalShare @8; pPrivateRuntimeData @16; PrivateRuntimeDataSize @24; pPrivateDriverData @32;
// PrivateDriverDataSize @40; NumAllocations @44; pAllocationInfo @48; Flags @56; hPrivateRuntimeResourceHandle @64 }
constexpr uint64_t sizeof_create_allocation = 72;
constexpr uint64_t kAllocationInfoStride = 40;              // D3DDDI_ALLOCATIONINFO { hAllocation @0; pSystemMem @8; pPrivateDriverData @16; size @24; VidPnSourceId @28; Flags @32 }
constexpr uint64_t kAllocationInfo2Stride = 96;             // D3DDDI_ALLOCATIONINFO2 (the same leading fields)
constexpr uint64_t kOpenAllocationInfoStride = 24;          // D3DDDI_OPENALLOCATIONINFO { hAllocation @0; pPrivateDriverData @8; PrivateDriverDataSize @16 }
constexpr uint64_t kOpenAllocationInfo2Stride = 80;
constexpr uint64_t sizeof_destroy_allocation = 20;          // { hDevice @0; hResource @4; phAllocationList @8; AllocationCount @16; [Flags @20] }
constexpr uint64_t sizeof_lock = 48;                        // { hDevice @0; hAllocation @4; PrivateDriverData @8; NumPages @12; pPages @16; pData @24; Flags @32; GpuVirtualAddress @40 }
constexpr uint64_t sizeof_lock2 = 24;                       // { hDevice @0; hAllocation @4; Flags @8; pData @16 }
constexpr uint64_t sizeof_unlock = 16;                      // { hDevice @0; NumAllocations @4; phAllocations @8 }
constexpr uint64_t sizeof_unlock2 = 8;                      // { hDevice @0; hAllocation @4 }
constexpr uint64_t sizeof_query_residency = 32;             // { hDevice @0; hResource @4; phAllocationList @8; AllocationCount @16; pResidencyStatus @24 }
constexpr uint64_t sizeof_query_resource_info = 32;         // { hDevice @0; hGlobalShare @4; pPrivateRuntimeData @8; PrivateRuntimeDataSize @16; TotalPrivateDriverDataSize @20; ResourcePrivateDriverDataSize @24; NumAllocations @28 }
constexpr uint64_t sizeof_open_resource = 72;               // { hDevice @0; hGlobalShare @4; NumAllocations @8; pOpenAllocationInfo @16; pPrivateRuntimeData @24; size @32; pResourcePrivateDriverData @40; size @48;
                                                            //   pTotalPrivateDriverDataBuffer @56; size @64; hResource @68 }
constexpr uint32_t kResourceHandleBase = 0xE0000000u;       // the layer's resource handles: this in the top four bits, the resource number below

// D3DKMT_CREATEALLOCATIONFLAGS bits.
constexpr uint32_t kCaCreateResource = 1u << 0, kCaCreateShared = 1u << 1, kCaCreateProtected = 1u << 3, kCaExistingSysMem = 1u << 5, kCaNtSecuritySharing = 1u << 6,
                   kCaCrossAdapter = 1u << 11, kCaOpenCrossAdapter = 1u << 12, kCaStandardAllocation = 1u << 16, kCaExistingSection = 1u << 17;

// D3DKMT_LOCKFLAGS bits.
constexpr uint32_t kLockReadOnly = 1u << 0, kLockWriteOnly = 1u << 1, kLockDoNotWait = 1u << 2, kLockDiscard = 1u << 7;

// The private driver data of an allocation (the miniport's description: little-endian 32-bit words). In: the first 15 words. Out (CreateAllocation fills them): the last three.
constexpr uint32_t kAllocPrivateVersion = 1;
enum AllocWord : uint32_t {
  kAwVersion = 0, kAwFlags, kAwFormat, kAwWidth, kAwHeight, kAwMips, kAwSizeLo, kAwSizeHi, kAwKind, kAwSamples, kAwLayers, kAwInitialState, kAwHeap, kAwHeapOffsetLo, kAwHeapOffsetHi,
  kAwPitch, kAwBytesLo, kAwBytesHi, kAwWords
};
constexpr uint32_t kAllocPrivateBytes = kAwWords * 4;

// --- Synchronization
constexpr uint64_t sizeof_create_sync = 76;                 // { hDevice @0; Info @4 { Type @4; union @8 }; hSyncObject @72 }
constexpr uint64_t sizeof_create_sync2 = 96;                // { hDevice @0; Info2 @8 { Type @8; Flags @12; union @16; SharedHandle @80 }; hSyncObject @88 }
constexpr uint64_t sizeof_signal_from_cpu = 32;             // { hDevice @0; ObjectCount @4; ObjectHandleArray @8; FenceValueArray @16; Flags @24 }
constexpr uint64_t sizeof_wait_from_cpu = 40;               // { hDevice @0; ObjectCount @4; ObjectHandleArray @8; FenceValueArray @16; hAsyncEvent @24; Flags @32 }
constexpr uint64_t sizeof_signal_sync = 140;                // { hContext @0; ObjectCount @4; ObjectHandleArray[32] @8; Flags @136 }
constexpr uint64_t sizeof_wait_sync = 136;                  // { hContext @0; ObjectCount @4; ObjectHandleArray[32] @8 }
constexpr uint64_t sizeof_signal_from_gpu = 24;             // { hContext @0; ObjectCount @4; ObjectHandleArray @8; MonitoredFenceValueArray @16 }

// --- Contexts and work
constexpr uint64_t sizeof_create_context = 96;              // { hDevice @0; NodeOrdinal @4; EngineAffinity @8; Flags @12; pPrivateDriverData @16; size @24; ClientHint @28; hContext @32;
                                                            //   pCommandBuffer @40; CommandBufferSize @48; pAllocationList @56; size @64; pPatchLocationList @72; size @80; CommandBuffer (GPU VA) @88 }
constexpr uint64_t sizeof_create_context_virtual = 40;      // the same up to hContext @32
constexpr uint64_t sizeof_context_priority = 8;             // { hContext @0; Priority @4 }
constexpr uint64_t sizeof_render_head = 84;                 // { hContext @0; CommandOffset @4; CommandLength @8; AllocationCount @12; PatchLocationCount @16; pNewCommandBuffer @24; NewCommandBufferSize @32;
                                                            //   pNewAllocationList @40; size @48; pNewPatchLocationList @56; size @64; Flags @68; PresentHistoryToken @72; BroadcastContextCount @80 ... }
constexpr uint64_t sizeof_present_head = 92;                // { hContext @0; hWindow @8; VidPnSourceId @16; hSource @20; hDestination @24; Color @28; DstRect @32; SrcRect @48; SubRectCnt @64; pSrcSubRects @72;
                                                            //   PresentCount @80; FlipInterval @84; Flags @88 ... }
constexpr uint64_t sizeof_escape = 32;                      // { hAdapter @0; hDevice @4; Type @8; Flags @12; pPrivateDriverData @16; size @24; hContext @28 }

// The buffers CreateContext maps for a context: DMA (command) buffer, allocation list (8 bytes per entry), patch location list (24 bytes per entry).
constexpr uint64_t kContextRegionBytes = 0x18000;
constexpr uint64_t kAllocationListOffset = 0x10000;
constexpr uint64_t kPatchListOffset = 0x12000;

}  // namespace process::win32k::dxg::layout
