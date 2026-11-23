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

/* gdi32.dll for i386: the GDI services whose data a 32-bit program lays out differently (docs/Wow64.md). Included by gdi32.c when it is built for i386,
 * after the service stubs. The kernel reads and writes the 64-bit layouts; the rest of gdi32.dll (and the program) has the 32-bit ones. Each
 * function here stands in for the stub of the same service, by the #define after it.
 *   - GetObject of a bitmap (BITMAP: the bits' address; DIBSECTION: the section's handle), of a brush (LOGBRUSH: the hatch is pointer-sized) and of
 *     an extended pen (EXTLOGPEN: the same);
 *   - CreateDIBSection: the kernel writes the address of the bits as 8 bytes;
 *   - opening a device context by the device's name: two UNICODE_STRINGs. */
static UINT gdi_wow_u32(const BYTE *at) { UINT v; memcpy(&v, at, 4); return v; }

static INT gdi_wow_get_object(HGDIOBJ handle, INT count, void *buffer) {
  const DWORD type = (DWORD)(ULONG_PTR)handle & NTGDI_HANDLE_TYPE_MASK;
  BYTE wide[128], narrow[128];
  INT size;
  if (type != NTGDI_OBJ_BITMAP && type != NTGDI_OBJ_BRUSH && type != NTGDI_OBJ_EXTPEN) return NtGdiExtGetObjectW(handle, count, buffer);
  memset(wide, 0, sizeof(wide));
  const INT got = NtGdiExtGetObjectW(handle, sizeof(wide), wide);
  if (got <= 0) return got;
  if (type == NTGDI_OBJ_BITMAP) {      /* 32 bytes (BITMAP) or 104 (DIBSECTION) there; 24 or 84 here */
    memcpy(narrow, wide, 20);                       /* bmType ... bmBitsPixel */
    memcpy(narrow + 20, wide + 24, 4);              /* bmBits */
    size = 24;
    if (got >= 104 && (!buffer || count >= 84)) {
      memcpy(narrow + 24, wide + 32, 40);           /* dsBmih */
      memcpy(narrow + 64, wide + 72, 12);           /* dsBitfields */
      memcpy(narrow + 76, wide + 88, 4);            /* dshSection */
      memcpy(narrow + 80, wide + 96, 4);            /* dsOffset */
      size = 84;
    }
  } else if (type == NTGDI_OBJ_BRUSH) {      /* LOGBRUSH: 16 bytes there, 12 here */
    memcpy(narrow, wide, 12);
    size = 12;
  } else {                                    /* EXTLOGPEN: the hatch is 8 bytes there (at 16), so everything after it is 4 bytes further on */
    if (got < 32) return 0;
    memcpy(narrow, wide, 20);
    memcpy(narrow + 20, wide + 24, (size_t)got - 24);
    size = got - 4;
  }
  (void)gdi_wow_u32;
  if (!buffer) return size;
  if (count < size) size = count < 0 ? 0 : count;
  memcpy(buffer, narrow, (size_t)size);
  return size;
}
#define NtGdiExtGetObjectW gdi_wow_get_object

static HBITMAP gdi_wow_create_dib_section(HDC hdc, HANDLE section, DWORD offset, const BITMAPINFO *bmi, UINT usage, UINT header_size, ULONG flags, ULONG_PTR color_space, void **bits) {
  ULONGLONG wide = 0;
  const HBITMAP bitmap = NtGdiCreateDIBSection(hdc, section, offset, bmi, usage, header_size, flags, color_space, bits ? (void **)&wide : NULL);
  if (bitmap && bits) *bits = (void *)(ULONG_PTR)wide;
  return bitmap;
}
#define NtGdiCreateDIBSection gdi_wow_create_dib_section

typedef struct { USHORT length, maximum; ULONG pad; ULONGLONG buffer; } GdiWowString;
static UNICODE_STRING *gdi_wow_string(GdiWowString *out, const UNICODE_STRING *in) {
  if (!in) return NULL;
  out->length = in->Length; out->maximum = in->MaximumLength; out->pad = 0; out->buffer = (ULONG_PTR)in->Buffer;
  return (UNICODE_STRING *)out;
}
static HDC gdi_wow_open_dc(UNICODE_STRING *device, const DEVMODEW *devmode, UNICODE_STRING *output, ULONG type, BOOL is_display, HANDLE hspool, DRIVER_INFO_2W *driver_info, void *pdev) {
  GdiWowString device64, output64;
  return NtGdiOpenDCW(gdi_wow_string(&device64, device), devmode, gdi_wow_string(&output64, output), type, is_display, hspool, driver_info, pdev);
}
#define NtGdiOpenDCW gdi_wow_open_dc

/* ---- the Direct3D thunks (D3DKMT*) ------------------------------------------------------------------------------------------------------------------
 * The structures a program hands D3DKMT functions itself: the ones that find and ask about an adapter, make a device, and the few others a program
 * (not a display driver's user-mode half) has a use for. Each is copied field by field into the kernel's layout and back. A field is given as where
 * it is here, where it is there, and what it is: 4 or 8 bytes copied as they are, or a pointer or handle (KMT_POINTER: 4 bytes here, 8 there,
 * widened with its sign as every handle is). What a pointer in them points at holds no pointers itself (adapter records, display modes, a
 * driver's private bytes). The offsets are those of d3dkmthk.h for the two widths.
 *
 * The other D3DKMT functions pass their structure on as it is. Where it has pointers in it that is the kernel's layout, not a 32-bit program's:
 * they are the interface of a display driver's user-mode half (allocations, locks, command buffers, synchronization objects), and the libraries
 * that are that half here - dxgi.dll, d3d12.dll - call win32u.dll with the kernel's layout themselves (src/Windows/dxcommon.h). */
#define KMT_POINTER 0
typedef struct { unsigned char here, there, kind; } KmtField;
#define KMT_WIDE_BYTES 128
static NTSTATUS gdi_wow_kmt(NTSTATUS (__cdecl *service)(void *), void *params, const KmtField *fields, unsigned count) {
  BYTE wide[KMT_WIDE_BYTES] __attribute__((aligned(8)));
  BYTE *narrow = params;
  if (!params) return service(NULL);
  memset(wide, 0, sizeof(wide));
  for (unsigned i = 0; i < count; ++i) {
    const KmtField f = fields[i];
    if (f.kind == KMT_POINTER) { const LONG64 value = *(const LONG *)(narrow + f.here); memcpy(wide + f.there, &value, 8); }
    else memcpy(wide + f.there, narrow + f.here, f.kind);
  }
  const NTSTATUS status = service(wide);
  for (unsigned i = 0; i < count; ++i) {
    const KmtField f = fields[i];
    memcpy(narrow + f.here, wide + f.there, f.kind == KMT_POINTER ? 4 : f.kind);
  }
  return status;
}
#define KMT_CONVERTED(name, ...)                                                                    \
  static NTSTATUS __cdecl gdi_wow_kmt_##name(void *params) {                                        \
    static const KmtField fields[] = {__VA_ARGS__};                                                 \
    return gdi_wow_kmt(NtGdiDdDDI##name, params, fields, sizeof(fields) / sizeof(fields[0]));       \
  }

/* D3DKMT_OPENADAPTERFROMHDC { hDc; hAdapter; AdapterLuid; VidPnSourceId }: 20 bytes here, 24 there */
KMT_CONVERTED(OpenAdapterFromHdc, {0, 0, KMT_POINTER}, {4, 8, 4}, {8, 12, 8}, {16, 20, 4})
#define NtGdiDdDDIOpenAdapterFromHdc gdi_wow_kmt_OpenAdapterFromHdc
/* D3DKMT_OPENADAPTERFROMDEVICENAME { pDeviceName; hAdapter; AdapterLuid }: 16 and 24 */
KMT_CONVERTED(OpenAdapterFromDeviceName, {0, 0, KMT_POINTER}, {4, 8, 4}, {8, 12, 8})
#define NtGdiDdDDIOpenAdapterFromDeviceName gdi_wow_kmt_OpenAdapterFromDeviceName
/* D3DKMT_ENUMADAPTERS2 { NumAdapters; pAdapters }: 8 and 16 (a D3DKMT_ADAPTERINFO is 20 bytes in both) */
KMT_CONVERTED(EnumAdapters2, {0, 0, 4}, {4, 8, KMT_POINTER})
#define NtGdiDdDDIEnumAdapters2 gdi_wow_kmt_EnumAdapters2
/* D3DKMT_QUERYADAPTERINFO { hAdapter; Type; pPrivateDriverData; PrivateDriverDataSize }: 16 and 24 */
KMT_CONVERTED(QueryAdapterInfo, {0, 0, 4}, {4, 4, 4}, {8, 8, KMT_POINTER}, {12, 16, 4})
#define NtGdiDdDDIQueryAdapterInfo gdi_wow_kmt_QueryAdapterInfo
/* D3DKMT_QUERYVIDEOMEMORYINFO { hProcess; hAdapter; MemorySegmentGroup; Budget, CurrentUsage, CurrentReservation, AvailableForReservation;
 * PhysicalAdapterIndex }: 56 bytes in both, the first three fields in other places */
KMT_CONVERTED(QueryVideoMemoryInfo, {0, 0, KMT_POINTER}, {4, 8, 4}, {8, 12, 4}, {16, 16, 8}, {24, 24, 8}, {32, 32, 8}, {40, 40, 8}, {48, 48, 4})
#define NtGdiDdDDIQueryVideoMemoryInfo gdi_wow_kmt_QueryVideoMemoryInfo
/* D3DKMT_CREATEDEVICE { hAdapter (or pAdapter); Flags; hDevice; pCommandBuffer; CommandBufferSize; pAllocationList; AllocationListSize;
 * pPatchLocationList; PatchLocationListSize }: 36 and 64 */
KMT_CONVERTED(CreateDevice, {0, 0, KMT_POINTER}, {4, 8, 4}, {8, 12, 4}, {12, 16, KMT_POINTER}, {16, 24, 4}, {20, 32, KMT_POINTER}, {24, 40, 4}, {28, 48, KMT_POINTER},
              {32, 56, 4})
#define NtGdiDdDDICreateDevice gdi_wow_kmt_CreateDevice
/* D3DKMT_GETDISPLAYMODELIST { hAdapter; VidPnSourceId; pModeList; ModeCount }: 16 and 24 (a D3DKMT_DISPLAYMODE holds no pointer) */
KMT_CONVERTED(GetDisplayModeList, {0, 0, 4}, {4, 4, 4}, {8, 8, KMT_POINTER}, {12, 16, 4})
#define NtGdiDdDDIGetDisplayModeList gdi_wow_kmt_GetDisplayModeList
/* D3DKMT_CREATEDCFROMMEMORY { pMemory; Format; Width; Height; Pitch; hDeviceDc; pColorTable; hDc; hBitmap }: 36 and 56 */
KMT_CONVERTED(CreateDCFromMemory, {0, 0, KMT_POINTER}, {4, 8, 4}, {8, 12, 4}, {12, 16, 4}, {16, 20, 4}, {20, 24, KMT_POINTER}, {24, 32, KMT_POINTER}, {28, 40, KMT_POINTER},
              {32, 48, KMT_POINTER})
#define NtGdiDdDDICreateDCFromMemory gdi_wow_kmt_CreateDCFromMemory
/* D3DKMT_DESTROYDCFROMMEMORY { hDc; hBitmap }: 8 and 16 */
KMT_CONVERTED(DestroyDCFromMemory, {0, 0, KMT_POINTER}, {4, 8, KMT_POINTER})
#define NtGdiDdDDIDestroyDCFromMemory gdi_wow_kmt_DestroyDCFromMemory
/* D3DKMT_SETVIDPNSOURCEOWNER { hDevice; pType; pVidPnSourceId; VidPnSourceCount }: 16 and 32 */
KMT_CONVERTED(SetVidPnSourceOwner, {0, 0, 4}, {4, 8, KMT_POINTER}, {8, 16, KMT_POINTER}, {12, 24, 4})
#define NtGdiDdDDISetVidPnSourceOwner gdi_wow_kmt_SetVidPnSourceOwner
/* D3DKMT_ESCAPE { hAdapter; hDevice; Type; Flags; pPrivateDriverData; PrivateDriverDataSize; hContext }: 28 and 32 */
KMT_CONVERTED(Escape, {0, 0, 4}, {4, 4, 4}, {8, 8, 4}, {12, 12, 4}, {16, 16, KMT_POINTER}, {20, 24, 4}, {24, 28, 4})
#define NtGdiDdDDIEscape gdi_wow_kmt_Escape

/* ---- NT-handle sharing: the structures, and the OBJECT_ATTRIBUTES and handles they point at --------------------------------------------------------- */
/* An OBJECT_ATTRIBUTES of the kernel's layout { Length; RootDirectory; ObjectName; Attributes; SecurityDescriptor; SecurityQualityOfService }: 48 bytes (24 here),
 * its UNICODE_STRING widened too. */
typedef struct { ULONG length, pad0; ULONGLONG root, name; ULONG attributes, pad1; ULONGLONG descriptor, quality; } GdiWowAttributes;
static void *gdi_wow_attributes(GdiWowAttributes *out, GdiWowString *name, const OBJECT_ATTRIBUTES *in) {
  if (!in) return NULL;
  memset(out, 0, sizeof(*out));
  out->length = in->Length == sizeof(OBJECT_ATTRIBUTES) ? sizeof(GdiWowAttributes) : in->Length;      /* (a wrong length stays wrong) */
  out->root = (ULONGLONG)(LONG64)(LONG)(ULONG_PTR)in->RootDirectory;
  out->name = (ULONG_PTR)gdi_wow_string(name, in->ObjectName);
  out->attributes = in->Attributes;
  out->descriptor = (ULONG_PTR)in->SecurityDescriptor;
  out->quality = (ULONG_PTR)in->SecurityQualityOfService;
  return out;
}
typedef NTSTATUS(__cdecl *gdi_wow_share_fn)(UINT, const UINT *, void *, DWORD, ULONGLONG *);
static NTSTATUS gdi_wow_share_objects(UINT count, const UINT *objects, void *attributes, DWORD access, HANDLE *handle) {
  GdiWowAttributes wide;
  GdiWowString name;
  ULONGLONG made = 0;
  const NTSTATUS status = ((gdi_wow_share_fn)(void *)NtGdiDdDDIShareObjects)(count, objects, gdi_wow_attributes(&wide, &name, attributes), access, handle ? &made : NULL);
  if (handle) *handle = (HANDLE)(ULONG_PTR)made;
  return status;
}
/* D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE { hDevice; hNtHandle; pPrivateRuntimeData; PrivateRuntimeDataSize; TotalPrivateDriverDataSize;
 * ResourcePrivateDriverDataSize; NumAllocations }: 28 and 40 */
KMT_CONVERTED(QueryResourceInfoFromNtHandle, {0, 0, 4}, {4, 8, KMT_POINTER}, {8, 16, KMT_POINTER}, {12, 24, 4}, {16, 28, 4}, {20, 32, 4}, {24, 36, 4})
#define NtGdiDdDDIQueryResourceInfoFromNtHandle gdi_wow_kmt_QueryResourceInfoFromNtHandle
/* D3DKMT_OPENSYNCOBJECTFROMNTHANDLE { hNtHandle; hSyncObject }: 8 and 16 */
KMT_CONVERTED(OpenSyncObjectFromNtHandle, {0, 0, KMT_POINTER}, {4, 8, 4})
#define NtGdiDdDDIOpenSyncObjectFromNtHandle gdi_wow_kmt_OpenSyncObjectFromNtHandle
/* D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 { hNtHandle; hDevice; Flags; hSyncObject; MonitoredFence { FenceValueCPUVirtualAddress; FenceValueGPUVirtualAddress;
 * EngineAffinity } }: 80 and 88 */
KMT_CONVERTED(OpenSyncObjectFromNtHandle2, {0, 0, KMT_POINTER}, {4, 8, 4}, {8, 12, 4}, {12, 16, 4}, {16, 24, KMT_POINTER}, {24, 32, 8}, {32, 40, 4})
#define NtGdiDdDDIOpenSyncObjectFromNtHandle2 gdi_wow_kmt_OpenSyncObjectFromNtHandle2
/* D3DKMT_OPENNTHANDLEFROMNAME / D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME { dwDesiredAccess; pObjAttrib; hNtHandle }: 12 and 24, the OBJECT_ATTRIBUTES widened */
static NTSTATUS gdi_wow_open_by_name(NTSTATUS (__cdecl *service)(void *), void *params) {
  BYTE wide[24] __attribute__((aligned(8)));
  BYTE *narrow = params;
  GdiWowAttributes attributes;
  GdiWowString name;
  if (!params) return service(NULL);
  memset(wide, 0, sizeof(wide));
  memcpy(wide, narrow, 4);
  const ULONGLONG at = (ULONG_PTR)gdi_wow_attributes(&attributes, &name, *(OBJECT_ATTRIBUTES **)(narrow + 4));
  memcpy(wide + 8, &at, 8);
  const NTSTATUS status = service(wide);
  memcpy(narrow + 8, wide + 16, 4);      /* (a handle: its low half) */
  return status;
}
static NTSTATUS __cdecl gdi_wow_kmt_OpenNtHandleFromName(void *params) { return gdi_wow_open_by_name(NtGdiDdDDIOpenNtHandleFromName, params); }
#define NtGdiDdDDIOpenNtHandleFromName gdi_wow_kmt_OpenNtHandleFromName
static NTSTATUS __cdecl gdi_wow_kmt_OpenSyncObjectNtHandleFromName(void *params) { return gdi_wow_open_by_name(NtGdiDdDDIOpenSyncObjectNtHandleFromName, params); }
#define NtGdiDdDDIOpenSyncObjectNtHandleFromName gdi_wow_kmt_OpenSyncObjectNtHandleFromName
/* D3DKMT_OPENRESOURCEFROMNTHANDLE: 60 and 104, and its D3DDDI_OPENALLOCATIONINFO2 array { hAllocation; pPrivateDriverData; PrivateDriverDataSize;
 * GpuVirtualAddress; Reserved[6] }: 48 and 80 each, filled in by the kernel and narrowed back */
static NTSTATUS __cdecl gdi_wow_kmt_OpenResourceFromNtHandle(void *params) {
  static const KmtField fields[] = {{0, 0, 4},   {4, 8, KMT_POINTER},  {8, 16, 4},  {16, 32, 4},  {20, 40, KMT_POINTER}, {24, 48, 4}, {28, 56, KMT_POINTER}, {32, 64, 4},
                                    {36, 72, KMT_POINTER}, {40, 80, 4}, {44, 84, 4}, {48, 88, KMT_POINTER}, {52, 96, 4}, {56, 100, 4}};
  BYTE wide[104] __attribute__((aligned(8)));
  BYTE *narrow = params;
  if (!params) return NtGdiDdDDIOpenResourceFromNtHandle(NULL);
  memset(wide, 0, sizeof(wide));
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const KmtField f = fields[i];
    if (f.kind == KMT_POINTER) { const LONG64 value = *(const LONG *)(narrow + f.here); memcpy(wide + f.there, &value, 8); }
    else memcpy(wide + f.there, narrow + f.here, f.kind);
  }
  UINT count = 0;
  BYTE *infos = *(BYTE **)(narrow + 12);
  memcpy(&count, narrow + 8, 4);
  BYTE *wide_infos = NULL;
  if (infos && count && count <= 256) {
    wide_infos = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)count * 80);
    if (!wide_infos) return 0xC0000017; /* STATUS_NO_MEMORY */
    const ULONGLONG at = (ULONG_PTR)wide_infos;
    memcpy(wide + 24, &at, 8);
  } else {
    const LONG64 value = (LONG)(ULONG_PTR)infos;
    memcpy(wide + 24, &value, 8);      /* (as it is: the kernel refuses it) */
  }
  const NTSTATUS status = NtGdiDdDDIOpenResourceFromNtHandle(wide);
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const KmtField f = fields[i];
    memcpy(narrow + f.here, wide + f.there, f.kind == KMT_POINTER ? 4 : f.kind);
  }
  memcpy(narrow + 12, &infos, 4);
  if (wide_infos) {
    if (status == 0)
      for (UINT i = 0; i < count; ++i) {
        BYTE *n = infos + (SIZE_T)i * 48, *w = wide_infos + (SIZE_T)i * 80;
        memcpy(n, w, 4);            /* hAllocation */
        memcpy(n + 4, w + 8, 4);    /* pPrivateDriverData (in the program's own buffer) */
        memcpy(n + 8, w + 16, 4);   /* PrivateDriverDataSize */
        memcpy(n + 16, w + 24, 8);  /* GpuVirtualAddress */
      }
    HeapFree(GetProcessHeap(), 0, wide_infos);
  }
  return status;
}
#define NtGdiDdDDIOpenResourceFromNtHandle gdi_wow_kmt_OpenResourceFromNtHandle
