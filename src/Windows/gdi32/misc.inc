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
/* gdi32.dll: printing and escapes, enumeration of stock objects, the Direct3D kernel-mode thunks. Included by gdi32.c. */

/* ---- printing ----------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT int WINAPI StartDocW(HDC hdc, const DOCINFOW *doc) { BOOL banding = FALSE; return NtGdiStartDoc(RD(hdc), doc, &banding, 0); }
EXPORT int WINAPI StartDocA(HDC hdc, const DOCINFOA *doc) {
  DOCINFOW w;
  int r;
  if (!doc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  w.cbSize = sizeof(w); w.fwType = doc->fwType;
  w.lpszDocName = dup_AtoW(doc->lpszDocName, CP_ACP);
  w.lpszOutput = dup_AtoW(doc->lpszOutput, CP_ACP);
  w.lpszDatatype = dup_AtoW(doc->lpszDatatype, CP_ACP);
  r = StartDocW(hdc, &w);
  mem_free((void *)w.lpszDocName); mem_free((void *)w.lpszOutput); mem_free((void *)w.lpszDatatype);
  return r;
}
EXPORT int WINAPI EndDoc(HDC hdc) { return NtGdiEndDoc(hdc); }
EXPORT int WINAPI AbortDoc(HDC hdc) { return NtGdiAbortDoc(hdc); }
EXPORT int WINAPI StartPage(HDC hdc) { return NtGdiStartPage(hdc); }
EXPORT int WINAPI EndPage(HDC hdc) { return NtGdiEndPage(hdc); }
EXPORT int WINAPI SetAbortProc(HDC hdc, ABORTPROC proc) { return 1; }      /* (the kernel prints without calling back) */
EXPORT DWORD WINAPI GdiGetSpoolMessage(void *p1, DWORD d2, void *p3, DWORD d4) { return NtGdiGetSpoolMessage(p1, d2, p3, d4); }
EXPORT DWORD WINAPI GdiInitSpool(void) { return NtGdiInitSpool(); }

EXPORT int WINAPI ExtEscape(HDC hdc, int escape, int in_size, LPCSTR in, int out_size, LPSTR out) { return NtGdiExtEscape(RD(hdc), NULL, 0, escape, in_size, in, out_size, out); }
EXPORT int WINAPI Escape(HDC hdc, int escape, int in_size, LPCSTR in, LPVOID out) {
  POINT *pt = out;
  switch (escape) {
    case ABORTDOC: return AbortDoc(hdc);
    case ENDDOC: return EndDoc(hdc);
    case NEWFRAME: return EndPage(hdc);
    case SETABORTPROC: return SetAbortProc(hdc, (ABORTPROC)in);
    case GETPHYSPAGESIZE: if (!pt) return 0; pt->x = GetDeviceCaps(hdc, PHYSICALWIDTH); pt->y = GetDeviceCaps(hdc, PHYSICALHEIGHT); return 1;
    case GETPRINTINGOFFSET: if (!pt) return 0; pt->x = GetDeviceCaps(hdc, PHYSICALOFFSETX); pt->y = GetDeviceCaps(hdc, PHYSICALOFFSETY); return 1;
    case GETSCALINGFACTOR: if (!pt) return 0; pt->x = GetDeviceCaps(hdc, SCALINGFACTORX); pt->y = GetDeviceCaps(hdc, SCALINGFACTORY); return 1;
    case STARTDOC: {
      DOCINFOA doc;
      int r;
      memset(&doc, 0, sizeof(doc));
      doc.cbSize = sizeof(doc);
      doc.lpszDocName = in;
      if (in) { const char *p = in; int n = 0; while (n < in_size && p[n]) ++n; if (n + 1 < in_size && p[n] == 0 && p[n + 1]) doc.lpszOutput = p + n + 1; }
      r = StartDocA(hdc, &doc);
      if (r > 0) r = StartPage(hdc);
      return r;
    }
    case QUERYESCSUPPORT: {
      int code;
      if (in_size >= (int)sizeof(DWORD)) code = (int)*(const DWORD *)in; else if (in_size >= (int)sizeof(WORD)) code = *(const WORD *)in; else return 0;
      switch (code) {
        case ABORTDOC: case ENDDOC: case GETPHYSPAGESIZE: case GETPRINTINGOFFSET: case GETSCALINGFACTOR: case NEWFRAME: case QUERYESCSUPPORT: case SETABORTPROC: case STARTDOC: return TRUE;
        default: break;
      }
      break;
    }
    default: break;
  }
  return ExtEscape(hdc, escape, in_size, in, 0, out);
}
EXPORT int WINAPI DrawEscape(HDC hdc, int escape, int in_size, LPCSTR in) { return 0; }
EXPORT int WINAPI NamedEscape(HDC hdc, LPCWSTR driver, int escape, int in_size, LPCSTR in, int out_size, LPSTR out) { return 0; }

/* ---- stock objects as an enumeration ------------------------------------------------------------------------------------------------------------------------ */

static const COLORREF solid_colors[] = {
  RGB(0x00, 0x00, 0x00), RGB(0xff, 0xff, 0xff), RGB(0xff, 0x00, 0x00), RGB(0x00, 0xff, 0x00), RGB(0x00, 0x00, 0xff), RGB(0xff, 0xff, 0x00), RGB(0xff, 0x00, 0xff), RGB(0x00, 0xff, 0xff),
  RGB(0x80, 0x00, 0x00), RGB(0x00, 0x80, 0x00), RGB(0x80, 0x80, 0x00), RGB(0x00, 0x00, 0x80), RGB(0x80, 0x00, 0x80), RGB(0x00, 0x80, 0x80), RGB(0xc0, 0xc0, 0xc0), RGB(0x80, 0x80, 0x80),
};
EXPORT int WINAPI EnumObjects(HDC hdc, int type, GOBJENUMPROC proc, LPARAM param) {
  int ret = 0;
  const unsigned n = sizeof(solid_colors) / sizeof(solid_colors[0]);
  switch (type) {
    case OBJ_PEN:
      for (unsigned i = 0; i < n; ++i) {
        LOGPEN pen;
        pen.lopnStyle = PS_SOLID; pen.lopnWidth.x = 1; pen.lopnWidth.y = 0; pen.lopnColor = solid_colors[i];
        ret = proc(&pen, param);
        if (!ret) break;
      }
      break;
    case OBJ_BRUSH:
      for (unsigned i = 0; i < n; ++i) {
        LOGBRUSH brush;
        brush.lbStyle = BS_SOLID; brush.lbColor = solid_colors[i]; brush.lbHatch = 0;
        ret = proc(&brush, param);
        if (!ret) break;
      }
      if (ret) for (int i = HS_HORIZONTAL; i <= HS_DIAGCROSS; ++i) {
        LOGBRUSH brush;
        brush.lbStyle = BS_HATCHED; brush.lbColor = RGB(0, 0, 0); brush.lbHatch = i;
        ret = proc(&brush, param);
        if (!ret) break;
      }
      break;
    default: SetLastError(ERROR_INVALID_PARAMETER); break;
  }
  return ret;
}

/* ---- the Direct3D kernel-mode thunks: one pointer to a parameter block each ------------------------------------------------------------------------------- */

EXPORT NTSTATUS WINAPI D3DKMTAcquireKeyedMutex(void *params) { return NtGdiDdDDIAcquireKeyedMutex(params); }
EXPORT NTSTATUS WINAPI D3DKMTAcquireKeyedMutex2(void *params) { return NtGdiDdDDIAcquireKeyedMutex2(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckOcclusion(void *params) { return NtGdiDdDDICheckOcclusion(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckVidPnExclusiveOwnership(void *params) { return NtGdiDdDDICheckVidPnExclusiveOwnership(params); }
EXPORT NTSTATUS WINAPI D3DKMTCloseAdapter(void *params) { return NtGdiDdDDICloseAdapter(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateAllocation(void *params) { return NtGdiDdDDICreateAllocation(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateAllocation2(void *params) { return NtGdiDdDDICreateAllocation2(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateDCFromMemory(void *params) { return NtGdiDdDDICreateDCFromMemory(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateDevice(void *params) { return NtGdiDdDDICreateDevice(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateKeyedMutex(void *params) { return NtGdiDdDDICreateKeyedMutex(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateKeyedMutex2(void *params) { return NtGdiDdDDICreateKeyedMutex2(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateSynchronizationObject(void *params) { return NtGdiDdDDICreateSynchronizationObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateSynchronizationObject2(void *params) { return NtGdiDdDDICreateSynchronizationObject2(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyAllocation(void *params) { return NtGdiDdDDIDestroyAllocation(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyAllocation2(void *params) { return NtGdiDdDDIDestroyAllocation2(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyDCFromMemory(void *params) { return NtGdiDdDDIDestroyDCFromMemory(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyDevice(void *params) { return NtGdiDdDDIDestroyDevice(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyKeyedMutex(void *params) { return NtGdiDdDDIDestroyKeyedMutex(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroySynchronizationObject(void *params) { return NtGdiDdDDIDestroySynchronizationObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTEnumAdapters(void *params) { return NtGdiDdDDIEnumAdapters(params); }
EXPORT NTSTATUS WINAPI D3DKMTEnumAdapters2(void *params) { return NtGdiDdDDIEnumAdapters2(params); }
EXPORT NTSTATUS WINAPI D3DKMTEscape(void *params) { return NtGdiDdDDIEscape(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenAdapterFromDeviceName(void *params) { return NtGdiDdDDIOpenAdapterFromDeviceName(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenAdapterFromHdc(void *params) { return NtGdiDdDDIOpenAdapterFromHdc(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(void *params) { return NtGdiDdDDIOpenAdapterFromLuid(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenKeyedMutex(void *params) { return NtGdiDdDDIOpenKeyedMutex(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenKeyedMutex2(void *params) { return NtGdiDdDDIOpenKeyedMutex2(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenKeyedMutexFromNtHandle(void *params) { return NtGdiDdDDIOpenKeyedMutexFromNtHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenNtHandleFromName(void *params) { return NtGdiDdDDIOpenNtHandleFromName(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenResource(void *params) { return NtGdiDdDDIOpenResource(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenResource2(void *params) { return NtGdiDdDDIOpenResource2(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenResourceFromNtHandle(void *params) { return NtGdiDdDDIOpenResourceFromNtHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenSyncObjectFromNtHandle(void *params) { return NtGdiDdDDIOpenSyncObjectFromNtHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenSyncObjectFromNtHandle2(void *params) { return NtGdiDdDDIOpenSyncObjectFromNtHandle2(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenSyncObjectNtHandleFromName(void *params) { return NtGdiDdDDIOpenSyncObjectNtHandleFromName(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenSynchronizationObject(void *params) { return NtGdiDdDDIOpenSynchronizationObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryAdapterInfo(void *params) { return NtGdiDdDDIQueryAdapterInfo(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryResourceInfo(void *params) { return NtGdiDdDDIQueryResourceInfo(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryResourceInfoFromNtHandle(void *params) { return NtGdiDdDDIQueryResourceInfoFromNtHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryStatistics(void *params) { return NtGdiDdDDIQueryStatistics(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryVideoMemoryInfo(void *params) { return NtGdiDdDDIQueryVideoMemoryInfo(params); }
EXPORT NTSTATUS WINAPI D3DKMTReleaseKeyedMutex(void *params) { return NtGdiDdDDIReleaseKeyedMutex(params); }
EXPORT NTSTATUS WINAPI D3DKMTReleaseKeyedMutex2(void *params) { return NtGdiDdDDIReleaseKeyedMutex2(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetQueuedLimit(void *params) { return NtGdiDdDDISetQueuedLimit(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetVidPnSourceOwner(void *params) { return NtGdiDdDDISetVidPnSourceOwner(params); }
EXPORT NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromCpu(void *params) { return NtGdiDdDDISignalSynchronizationObjectFromCpu(params); }
EXPORT NTSTATUS WINAPI D3DKMTWaitForSynchronizationObjectFromCpu(void *params) { return NtGdiDdDDIWaitForSynchronizationObjectFromCpu(params); }
EXPORT NTSTATUS WINAPI D3DKMTAbandonSwapChain(void *params) { return NtGdiDdDDIAbandonSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTAcquireSwapChain(void *params) { return NtGdiDdDDIAcquireSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTAddSurfaceToSwapChain(void *params) { return NtGdiDdDDIAddSurfaceToSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTAdjustFullscreenGamma(void *params) { return NtGdiDdDDIAdjustFullscreenGamma(params); }
EXPORT NTSTATUS WINAPI D3DKMTCacheHybridQueryValue(void *params) { return NtGdiDdDDICacheHybridQueryValue(params); }
EXPORT NTSTATUS WINAPI D3DKMTChangeVideoMemoryReservation(void *params) { return NtGdiDdDDIChangeVideoMemoryReservation(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckExclusiveOwnership(void *params) { return NtGdiDdDDICheckExclusiveOwnership(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckMonitorPowerState(void *params) { return NtGdiDdDDICheckMonitorPowerState(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckMultiPlaneOverlaySupport(void *params) { return NtGdiDdDDICheckMultiPlaneOverlaySupport(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckMultiPlaneOverlaySupport2(void *params) { return NtGdiDdDDICheckMultiPlaneOverlaySupport2(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckMultiPlaneOverlaySupport3(void *params) { return NtGdiDdDDICheckMultiPlaneOverlaySupport3(params); }
EXPORT NTSTATUS WINAPI D3DKMTCheckSharedResourceAccess(void *params) { return NtGdiDdDDICheckSharedResourceAccess(params); }
EXPORT NTSTATUS WINAPI D3DKMTConfigureSharedResource(void *params) { return NtGdiDdDDIConfigureSharedResource(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateBundleObject(void *params) { return NtGdiDdDDICreateBundleObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateContext(void *params) { return NtGdiDdDDICreateContext(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateContextVirtual(void *params) { return NtGdiDdDDICreateContextVirtual(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateHwContext(void *params) { return NtGdiDdDDICreateHwContext(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateHwQueue(void *params) { return NtGdiDdDDICreateHwQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateOutputDupl(void *params) { return NtGdiDdDDICreateOutputDupl(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateOverlay(void *params) { return NtGdiDdDDICreateOverlay(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreatePagingQueue(void *params) { return NtGdiDdDDICreatePagingQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateProtectedSession(void *params) { return NtGdiDdDDICreateProtectedSession(params); }
EXPORT NTSTATUS WINAPI D3DKMTCreateSwapChain(void *params) { return NtGdiDdDDICreateSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTDDisplayEnum(void *params) { return NtGdiDdDDIDDisplayEnum(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyContext(void *params) { return NtGdiDdDDIDestroyContext(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyHwContext(void *params) { return NtGdiDdDDIDestroyHwContext(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyHwQueue(void *params) { return NtGdiDdDDIDestroyHwQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyOutputDupl(void *params) { return NtGdiDdDDIDestroyOutputDupl(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyOverlay(void *params) { return NtGdiDdDDIDestroyOverlay(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyPagingQueue(void *params) { return NtGdiDdDDIDestroyPagingQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTDestroyProtectedSession(void *params) { return NtGdiDdDDIDestroyProtectedSession(params); }
EXPORT NTSTATUS WINAPI D3DKMTDispMgrCreate(void *params) { return NtGdiDdDDIDispMgrCreate(params); }
EXPORT NTSTATUS WINAPI D3DKMTDispMgrSourceOperation(void *params) { return NtGdiDdDDIDispMgrSourceOperation(params); }
EXPORT NTSTATUS WINAPI D3DKMTDispMgrTargetOperation(void *params) { return NtGdiDdDDIDispMgrTargetOperation(params); }
EXPORT NTSTATUS WINAPI D3DKMTEvict(void *params) { return NtGdiDdDDIEvict(params); }
EXPORT NTSTATUS WINAPI D3DKMTExtractBundleObject(void *params) { return NtGdiDdDDIExtractBundleObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTFlipOverlay(void *params) { return NtGdiDdDDIFlipOverlay(params); }
EXPORT NTSTATUS WINAPI D3DKMTFlushHeapTransitions(void *params) { return NtGdiDdDDIFlushHeapTransitions(params); }
EXPORT NTSTATUS WINAPI D3DKMTFreeGpuVirtualAddress(void *params) { return NtGdiDdDDIFreeGpuVirtualAddress(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetAllocationPriority(void *params) { return NtGdiDdDDIGetAllocationPriority(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetCachedHybridQueryValue(void *params) { return NtGdiDdDDIGetCachedHybridQueryValue(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetContextInProcessSchedulingPriority(void *params) { return NtGdiDdDDIGetContextInProcessSchedulingPriority(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetContextSchedulingPriority(void *params) { return NtGdiDdDDIGetContextSchedulingPriority(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetDWMVerticalBlankEvent(void *params) { return NtGdiDdDDIGetDWMVerticalBlankEvent(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetDeviceState(void *params) { return NtGdiDdDDIGetDeviceState(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetDisplayModeList(void *params) { return NtGdiDdDDIGetDisplayModeList(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetMemoryBudgetTarget(void *params) { return NtGdiDdDDIGetMemoryBudgetTarget(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetMultiPlaneOverlayCaps(void *params) { return NtGdiDdDDIGetMultiPlaneOverlayCaps(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetMultisampleMethodList(void *params) { return NtGdiDdDDIGetMultisampleMethodList(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetOverlayState(void *params) { return NtGdiDdDDIGetOverlayState(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetPostCompositionCaps(void *params) { return NtGdiDdDDIGetPostCompositionCaps(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetPresentHistory(void *params) { return NtGdiDdDDIGetPresentHistory(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetPresentQueueEvent(void *params) { return NtGdiDdDDIGetPresentQueueEvent(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetProcessDeviceRemovalSupport(void *params) { return NtGdiDdDDIGetProcessDeviceRemovalSupport(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetProcessSchedulingPriorityBand(void *params) { return NtGdiDdDDIGetProcessSchedulingPriorityBand(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetProcessSchedulingPriorityClass(void *params) { return NtGdiDdDDIGetProcessSchedulingPriorityClass(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetResourcePresentPrivateDriverData(void *params) { return NtGdiDdDDIGetResourcePresentPrivateDriverData(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetRuntimeData(void *params) { return NtGdiDdDDIGetRuntimeData(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetScanLine(void *params) { return NtGdiDdDDIGetScanLine(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetSetSwapChainMetadata(void *params) { return NtGdiDdDDIGetSetSwapChainMetadata(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetSharedPrimaryHandle(void *params) { return NtGdiDdDDIGetSharedPrimaryHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetSharedResourceAdapterLuid(void *params) { return NtGdiDdDDIGetSharedResourceAdapterLuid(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetSharedResourceAdapterLuidFlipManager(void *params) { return NtGdiDdDDIGetSharedResourceAdapterLuidFlipManager(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetSwapChainSurfacePhysicalAddress(void *params) { return NtGdiDdDDIGetSwapChainSurfacePhysicalAddress(params); }
EXPORT NTSTATUS WINAPI D3DKMTGetYieldPercentage(void *params) { return NtGdiDdDDIGetYieldPercentage(params); }
EXPORT NTSTATUS WINAPI D3DKMTInvalidateActiveVidPn(void *params) { return NtGdiDdDDIInvalidateActiveVidPn(params); }
EXPORT NTSTATUS WINAPI D3DKMTInvalidateCache(void *params) { return NtGdiDdDDIInvalidateCache(params); }
EXPORT NTSTATUS WINAPI D3DKMTLock(void *params) { return NtGdiDdDDILock(params); }
EXPORT NTSTATUS WINAPI D3DKMTLock2(void *params) { return NtGdiDdDDILock2(params); }
EXPORT NTSTATUS WINAPI D3DKMTMakeResident(void *params) { return NtGdiDdDDIMakeResident(params); }
EXPORT NTSTATUS WINAPI D3DKMTMapGpuVirtualAddress(void *params) { return NtGdiDdDDIMapGpuVirtualAddress(params); }
EXPORT NTSTATUS WINAPI D3DKMTMarkDeviceAsError(void *params) { return NtGdiDdDDIMarkDeviceAsError(params); }
EXPORT NTSTATUS WINAPI D3DKMTNetDispGetNextChunkInfo(void *params) { return NtGdiDdDDINetDispGetNextChunkInfo(params); }
EXPORT NTSTATUS WINAPI D3DKMTNetDispQueryMiracastDisplayDeviceStatus(void *params) { return NtGdiDdDDINetDispQueryMiracastDisplayDeviceStatus(params); }
EXPORT NTSTATUS WINAPI D3DKMTNetDispQueryMiracastDisplayDeviceSupport(void *params) { return NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport(params); }
EXPORT NTSTATUS WINAPI D3DKMTNetDispStartMiracastDisplayDevice(void *params) { return NtGdiDdDDINetDispStartMiracastDisplayDevice(params); }
EXPORT NTSTATUS WINAPI D3DKMTNetDispStopMiracastDisplayDevice(void *params) { return NtGdiDdDDINetDispStopMiracastDisplayDevice(params); }
EXPORT NTSTATUS WINAPI D3DKMTOfferAllocations(void *params) { return NtGdiDdDDIOfferAllocations(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenBundleObjectNtHandleFromName(void *params) { return NtGdiDdDDIOpenBundleObjectNtHandleFromName(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenProtectedSessionFromNtHandle(void *params) { return NtGdiDdDDIOpenProtectedSessionFromNtHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTOpenSwapChain(void *params) { return NtGdiDdDDIOpenSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTOutputDuplGetFrameInfo(void *params) { return NtGdiDdDDIOutputDuplGetFrameInfo(params); }
EXPORT NTSTATUS WINAPI D3DKMTOutputDuplGetMetaData(void *params) { return NtGdiDdDDIOutputDuplGetMetaData(params); }
EXPORT NTSTATUS WINAPI D3DKMTOutputDuplGetPointerShapeData(void *params) { return NtGdiDdDDIOutputDuplGetPointerShapeData(params); }
EXPORT NTSTATUS WINAPI D3DKMTOutputDuplPresent(void *params) { return NtGdiDdDDIOutputDuplPresent(params); }
EXPORT NTSTATUS WINAPI D3DKMTOutputDuplReleaseFrame(void *params) { return NtGdiDdDDIOutputDuplReleaseFrame(params); }
EXPORT NTSTATUS WINAPI D3DKMTPollDisplayChildren(void *params) { return NtGdiDdDDIPollDisplayChildren(params); }
EXPORT NTSTATUS WINAPI D3DKMTPresent(void *params) { return NtGdiDdDDIPresent(params); }
EXPORT NTSTATUS WINAPI D3DKMTPresentMultiPlaneOverlay(void *params) { return NtGdiDdDDIPresentMultiPlaneOverlay(params); }
EXPORT NTSTATUS WINAPI D3DKMTPresentMultiPlaneOverlay2(void *params) { return NtGdiDdDDIPresentMultiPlaneOverlay2(params); }
EXPORT NTSTATUS WINAPI D3DKMTPresentMultiPlaneOverlay3(void *params) { return NtGdiDdDDIPresentMultiPlaneOverlay3(params); }
EXPORT NTSTATUS WINAPI D3DKMTPresentRedirected(void *params) { return NtGdiDdDDIPresentRedirected(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryAllocationResidency(void *params) { return NtGdiDdDDIQueryAllocationResidency(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryClockCalibration(void *params) { return NtGdiDdDDIQueryClockCalibration(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryFSEBlock(void *params) { return NtGdiDdDDIQueryFSEBlock(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryProcessOfferInfo(void *params) { return NtGdiDdDDIQueryProcessOfferInfo(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryProtectedSessionInfoFromNtHandle(void *params) { return NtGdiDdDDIQueryProtectedSessionInfoFromNtHandle(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryProtectedSessionStatus(void *params) { return NtGdiDdDDIQueryProtectedSessionStatus(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryRemoteVidPnSourceFromGdiDisplayName(void *params) { return NtGdiDdDDIQueryRemoteVidPnSourceFromGdiDisplayName(params); }
EXPORT NTSTATUS WINAPI D3DKMTQueryVidPnExclusiveOwnership(void *params) { return NtGdiDdDDIQueryVidPnExclusiveOwnership(params); }
EXPORT NTSTATUS WINAPI D3DKMTReclaimAllocations(void *params) { return NtGdiDdDDIReclaimAllocations(params); }
EXPORT NTSTATUS WINAPI D3DKMTReclaimAllocations2(void *params) { return NtGdiDdDDIReclaimAllocations2(params); }
EXPORT NTSTATUS WINAPI D3DKMTReleaseProcessVidPnSourceOwners(void *params) { return NtGdiDdDDIReleaseProcessVidPnSourceOwners(params); }
EXPORT NTSTATUS WINAPI D3DKMTReleaseSwapChain(void *params) { return NtGdiDdDDIReleaseSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTRemoveSurfaceFromSwapChain(void *params) { return NtGdiDdDDIRemoveSurfaceFromSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTRender(void *params) { return NtGdiDdDDIRender(params); }
EXPORT NTSTATUS WINAPI D3DKMTReserveGpuVirtualAddress(void *params) { return NtGdiDdDDIReserveGpuVirtualAddress(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetAllocationPriority(void *params) { return NtGdiDdDDISetAllocationPriority(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetContextInProcessSchedulingPriority(void *params) { return NtGdiDdDDISetContextInProcessSchedulingPriority(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetContextSchedulingPriority(void *params) { return NtGdiDdDDISetContextSchedulingPriority(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetDisplayMode(void *params) { return NtGdiDdDDISetDisplayMode(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetDodIndirectSwapchain(void *params) { return NtGdiDdDDISetDodIndirectSwapchain(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetFSEBlock(void *params) { return NtGdiDdDDISetFSEBlock(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetGammaRamp(void *params) { return NtGdiDdDDISetGammaRamp(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetHwProtectionTeardownRecovery(void *params) { return NtGdiDdDDISetHwProtectionTeardownRecovery(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetMemoryBudgetTarget(void *params) { return NtGdiDdDDISetMemoryBudgetTarget(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetMonitorColorSpaceTransform(void *params) { return NtGdiDdDDISetMonitorColorSpaceTransform(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetProcessDeviceRemovalSupport(void *params) { return NtGdiDdDDISetProcessDeviceRemovalSupport(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetProcessSchedulingPriorityBand(void *params) { return NtGdiDdDDISetProcessSchedulingPriorityBand(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetProcessSchedulingPriorityClass(void *params) { return NtGdiDdDDISetProcessSchedulingPriorityClass(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetStablePowerState(void *params) { return NtGdiDdDDISetStablePowerState(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetStereoEnabled(void *params) { return NtGdiDdDDISetStereoEnabled(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetSyncRefreshCountWaitTarget(void *params) { return NtGdiDdDDISetSyncRefreshCountWaitTarget(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetVidPnSourceHwProtection(void *params) { return NtGdiDdDDISetVidPnSourceHwProtection(params); }
EXPORT NTSTATUS WINAPI D3DKMTSetYieldPercentage(void *params) { return NtGdiDdDDISetYieldPercentage(params); }
EXPORT NTSTATUS WINAPI D3DKMTShareObjects(void *params) { return NtGdiDdDDIShareObjects(params); }
EXPORT NTSTATUS WINAPI D3DKMTSharedPrimaryLockNotification(void *params) { return NtGdiDdDDISharedPrimaryLockNotification(params); }
EXPORT NTSTATUS WINAPI D3DKMTSharedPrimaryUnLockNotification(void *params) { return NtGdiDdDDISharedPrimaryUnLockNotification(params); }
EXPORT NTSTATUS WINAPI D3DKMTSignalSynchronizationObject(void *params) { return NtGdiDdDDISignalSynchronizationObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromGpu(void *params) { return NtGdiDdDDISignalSynchronizationObjectFromGpu(params); }
EXPORT NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromGpu2(void *params) { return NtGdiDdDDISignalSynchronizationObjectFromGpu2(params); }
EXPORT NTSTATUS WINAPI D3DKMTSubmitCommand(void *params) { return NtGdiDdDDISubmitCommand(params); }
EXPORT NTSTATUS WINAPI D3DKMTSubmitCommandToHwQueue(void *params) { return NtGdiDdDDISubmitCommandToHwQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTSubmitSignalSyncObjectsToHwQueue(void *params) { return NtGdiDdDDISubmitSignalSyncObjectsToHwQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTSubmitWaitForSyncObjectsToHwQueue(void *params) { return NtGdiDdDDISubmitWaitForSyncObjectsToHwQueue(params); }
EXPORT NTSTATUS WINAPI D3DKMTTrimProcessCommitment(void *params) { return NtGdiDdDDITrimProcessCommitment(params); }
EXPORT NTSTATUS WINAPI D3DKMTUnOrderedPresentSwapChain(void *params) { return NtGdiDdDDIUnOrderedPresentSwapChain(params); }
EXPORT NTSTATUS WINAPI D3DKMTUnlock(void *params) { return NtGdiDdDDIUnlock(params); }
EXPORT NTSTATUS WINAPI D3DKMTUnlock2(void *params) { return NtGdiDdDDIUnlock2(params); }
EXPORT NTSTATUS WINAPI D3DKMTUpdateAllocationProperty(void *params) { return NtGdiDdDDIUpdateAllocationProperty(params); }
EXPORT NTSTATUS WINAPI D3DKMTUpdateGpuVirtualAddress(void *params) { return NtGdiDdDDIUpdateGpuVirtualAddress(params); }
EXPORT NTSTATUS WINAPI D3DKMTUpdateOverlay(void *params) { return NtGdiDdDDIUpdateOverlay(params); }
EXPORT NTSTATUS WINAPI D3DKMTWaitForIdle(void *params) { return NtGdiDdDDIWaitForIdle(params); }
EXPORT NTSTATUS WINAPI D3DKMTWaitForSynchronizationObject(void *params) { return NtGdiDdDDIWaitForSynchronizationObject(params); }
EXPORT NTSTATUS WINAPI D3DKMTWaitForSynchronizationObjectFromGpu(void *params) { return NtGdiDdDDIWaitForSynchronizationObjectFromGpu(params); }
EXPORT NTSTATUS WINAPI D3DKMTWaitForVerticalBlankEvent(void *params) { return NtGdiDdDDIWaitForVerticalBlankEvent(params); }
EXPORT NTSTATUS WINAPI D3DKMTWaitForVerticalBlankEvent2(void *params) { return NtGdiDdDDIWaitForVerticalBlankEvent2(params); }

/* ---- small aliases and undocumented exports ----------------------------------------------------------------------------------------------------------------- */

#undef GetKerningPairs
#undef GetGlyphOutline
EXPORT DWORD WINAPI GetKerningPairs(HDC hdc, DWORD count, KERNINGPAIR *pairs) { return GetKerningPairsA(hdc, count, pairs); }
EXPORT DWORD WINAPI GetGlyphOutline(HDC hdc, UINT ch, UINT format, LPGLYPHMETRICS gm, DWORD size, LPVOID buffer, const MAT2 *mat) { return GetGlyphOutlineA(hdc, ch, format, gm, size, buffer, mat); }
EXPORT BOOL WINAPI GetCharABCWidthsFloatI(HDC hdc, UINT first, UINT count, WORD *glyphs, ABCFLOAT *abc) { return NtGdiGetCharABCWidthsW(RD(hdc), first, count, (WCHAR *)glyphs, NTGDI_GETCHARABCWIDTHS_INDICES, abc); }
EXPORT BOOL WINAPI SetMagicColors(HDC hdc, DWORD magic, ULONG index) { return NtGdiSetMagicColors(RD(hdc), magic, index); }
EXPORT BOOL WINAPI GdiSwapBuffers(HDC hdc) { return NtGdiSwapBuffers(hdc); }
EXPORT BOOL WINAPI GdiSetPixelFormat(HDC hdc, int format) { return NtGdiSetPixelFormat(RD(hdc), format); }
EXPORT int WINAPI GdiDescribePixelFormat(HDC hdc, int format, UINT size, PIXELFORMATDESCRIPTOR *descr) { return NtGdiDescribePixelFormat(RD(hdc), format, size, descr); }
EXPORT BOOL WINAPI InternalDeleteDC(HDC hdc) { return DeleteDC(hdc); }
EXPORT int WINAPI StartFormPage(HDC hdc) { return NtGdiStartPage(hdc); }
EXPORT int WINAPI EndFormPage(HDC hdc) { return NtGdiEndPage(hdc); }
EXPORT int WINAPI GdiAddFontResourceW(LPCWSTR name, DWORD flags, DWORD reserved) { return AddFontResourceExW(name, flags, NULL); }
EXPORT BOOL WINAPI GdiIsMetaFileDC(HDC hdc) { return is_meta_dc(hdc); }
EXPORT BOOL WINAPI GdiIsMetaPrintDC(HDC hdc) { return FALSE; }
EXPORT BOOL WINAPI GdiIsPlayMetafileDC(HDC hdc) { return FALSE; }
EXPORT BOOL WINAPI GdiIsScreenDC(HDC hdc) { DWORD r; return NtGdiGetDCDword(RD(hdc), NtGdiGetBkMode, &r); }
EXPORT BOOL WINAPI GdiDeleteSpoolFileHandle(HANDLE h) { return FALSE; }
EXPORT BOOL WINAPI DeleteColorSpace(HCOLORSPACE cs) { return TRUE; }
EXPORT HCOLORSPACE WINAPI GetColorSpace(HDC hdc) { return NULL; }
EXPORT HCOLORSPACE WINAPI SetColorSpace(HDC hdc, HCOLORSPACE cs) { return NULL; }
EXPORT UINT WINAPI GetBitmapDpiScaleValue(HBITMAP bm) { return 100; }
EXPORT UINT WINAPI GetDCDpiScaleValue(HDC hdc) { return 100; }
