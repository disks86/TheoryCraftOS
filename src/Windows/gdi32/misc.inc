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
