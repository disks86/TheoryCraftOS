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
#include "Process/Win32k/Dxg/DxgInternal.hpp"

// The services of hardware features the adapters of this kernel do not have: they answer what a display adapter without the feature answers, STATUS_NOT_SUPPORTED (and
// STATUS_NO_SUCH_DEVICE when there is no display driver at all). They are not "fake successes": the caller learns that the feature is absent and takes its fallback.
//
//   multi-plane overlays and the old overlay interface   hardware planes that compose the screen (the display is one plane)
//   output duplication                                   the display driver's copy of the desktop image
//   protected sessions, hardware DRM                     a TPM-backed GPU channel
//   WDDM 2 scheduling: hardware contexts and queues, paging queues, GPU virtual addresses, command submission, doorbells, tracked workloads, native fences
//                                                        the adapters are WDDM 1.3 class (QueryAdapterInfo says 1300): the kernel validates and replays command streams
//                                                        (docs/Direct3DKernelMode.md) instead of running hardware DMA buffers on a GPU address space
//   Miracast, display mux, DisplayPort passthrough, virtualized application isolation (Vail)
namespace process::win32k::dxg {

namespace {

uint64_t not_supported(Call&) {
  return dxgk::system_kernel() ? nt::kNotSupported : nt::kNoSuchDevice;
}

}  // namespace

void register_unsupported() {
  using N = WindowsSyscallNumber;
  static constexpr N kNumbers[] = {
      // multi-plane overlays and overlays
      N::NtGdiDdDDICheckMultiPlaneOverlaySupport, N::NtGdiDdDDICheckMultiPlaneOverlaySupport2, N::NtGdiDdDDICheckMultiPlaneOverlaySupport3, N::NtGdiDdDDIGetMultiPlaneOverlayCaps,
      N::NtGdiDdDDIPresentMultiPlaneOverlay, N::NtGdiDdDDIPresentMultiPlaneOverlay2, N::NtGdiDdDDIPresentMultiPlaneOverlay3, N::NtGdiDdDDICreateOverlay, N::NtGdiDdDDIUpdateOverlay,
      N::NtGdiDdDDIFlipOverlay, N::NtGdiDdDDIDestroyOverlay, N::NtGdiDdDDIGetOverlayState, N::NtDxgkCheckSinglePlaneForMultiPlaneOverlaySupport,
      // output duplication
      N::NtGdiDdDDICreateOutputDupl, N::NtGdiDdDDIDestroyOutputDupl, N::NtGdiDdDDIOutputDuplGetFrameInfo, N::NtGdiDdDDIOutputDuplGetMetaData, N::NtGdiDdDDIOutputDuplGetPointerShapeData,
      N::NtGdiDdDDIOutputDuplPresent, N::NtGdiDdDDIOutputDuplReleaseFrame, N::NtDxgkOutputDuplPresentToHwQueue,
      // protected sessions
      N::NtGdiDdDDICreateProtectedSession, N::NtGdiDdDDIDestroyProtectedSession, N::NtGdiDdDDIOpenProtectedSessionFromNtHandle, N::NtGdiDdDDIQueryProtectedSessionInfoFromNtHandle,
      N::NtGdiDdDDIQueryProtectedSessionStatus, N::NtGdiDdDDISetHwProtectionTeardownRecovery, N::NtGdiDdDDISetVidPnSourceHwProtection,
      // WDDM 2 scheduling
      N::NtGdiDdDDICreateHwContext, N::NtGdiDdDDIDestroyHwContext, N::NtGdiDdDDICreateHwQueue, N::NtGdiDdDDIDestroyHwQueue, N::NtGdiDdDDISubmitCommandToHwQueue,
      N::NtGdiDdDDISubmitSignalSyncObjectsToHwQueue, N::NtGdiDdDDISubmitWaitForSyncObjectsToHwQueue, N::NtGdiDdDDICreatePagingQueue, N::NtGdiDdDDIDestroyPagingQueue,
      N::NtGdiDdDDISubmitCommand, N::NtGdiDdDDIReserveGpuVirtualAddress, N::NtGdiDdDDIMapGpuVirtualAddress, N::NtGdiDdDDIFreeGpuVirtualAddress, N::NtGdiDdDDIUpdateGpuVirtualAddress,
      N::NtDxgkCreateDoorbell, N::NtDxgkConnectDoorbell, N::NtDxgkDestroyDoorbell, N::NtDxgkNotifyWorkSubmission, N::NtDxgkCreateTrackedWorkload, N::NtDxgkDestroyTrackedWorkload,
      N::NtDxgkUpdateTrackedWorkload, N::NtDxgkGetAvailableTrackedWorkloadIndex, N::NtDxgkGetTrackedWorkloadStatistics, N::NtDxgkResetTrackedWorkloadStatistics,
      N::NtDxgkCreateNativeFence, N::NtDxgkOpenNativeFenceFromNtHandle, N::NtDxgkGetNativeFenceLogDetail, N::NtDxgkSubmitPresentToHwQueue, N::NtDxgkSubmitPresentBltToHwQueue,
      N::NtDxgkCancelPresents,
      // Miracast, display mux, DisplayPort, Vail
      N::NtGdiDdDDINetDispGetNextChunkInfo, N::NtGdiDdDDINetDispQueryMiracastDisplayDeviceStatus, N::NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport,
      N::NtGdiDdDDINetDispStartMiracastDisplayDevice, N::NtGdiDdDDINetDispStopMiracastDisplayDevice, N::NtDxgkDisplayMuxSwitchPrepare, N::NtDxgkDisplayMuxSwitchExecute,
      N::NtDxgkDisplayMuxSwitchFinish, N::NtDxgkDisplayPortOperation, N::NtDxgkVailConnect, N::NtDxgkVailDisconnect, N::NtDxgkVailPromoteCompositionSurface,
      N::NtDxgkRegisterVailProcess,
  };
  for (N n : kNumbers) register_handler(n, &not_supported, false);
}

}  // namespace process::win32k::dxg
