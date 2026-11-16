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

/* Direct3D in a 32-bit Windows program, for tests/wow64_qemu.sh (docs/Wow64.md). Two halves:
 *   - the D3DKMT functions of gdi32.dll called as a program calls them, with the structures as a 32-bit compiler lays them out (the ones that find
 *     and ask about an adapter and make a device);
 *   - all of tests/windows/d3d12test.c (dxgi.dll and d3d12.dll: devices, fences waited for on other threads, copies, a triangle, textures, depth,
 *     compute, the extras, a swap chain), whose own main is run from here.
 * Ends with exit code 42 if every check of both passed. dxfixtures.h (the shaders) is made by the script, as tests/emulated_dx.sh makes it.
 *   i686-w64-mingw32-gcc -O1 -I <folder of dxfixtures.h> -o dx32.exe tests/windows/dx32.c -ldxgi -ld3d12 -luuid -lgdi32 */
#define main d3d12test_main
#include "d3d12test.c"
#undef main

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "dx32: ok: %s\n" : "dx32: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

/* (d3dkmthk.h's structures, with the types that give them their layout in the program's own width) */
typedef UINT KMT_HANDLE;
typedef struct { KMT_HANDLE hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; } KMT_ADAPTERINFO;
typedef struct { ULONG NumAdapters; KMT_ADAPTERINFO *pAdapters; } KMT_ENUMADAPTERS2;
typedef struct { LUID AdapterLuid; KMT_HANDLE hAdapter; } KMT_OPENADAPTERFROMLUID;
typedef struct { KMT_HANDLE hAdapter; } KMT_CLOSEADAPTER;
typedef struct { KMT_HANDLE hAdapter; UINT Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; } KMT_QUERYADAPTERINFO;
typedef struct { HANDLE hProcess; KMT_HANDLE hAdapter; UINT MemorySegmentGroup; UINT64 Budget, CurrentUsage, CurrentReservation, AvailableForReservation; UINT PhysicalAdapterIndex; } KMT_QUERYVIDEOMEMORYINFO;
typedef struct { UINT Width, Height, Format, RefreshNumerator, RefreshDenominator, ScanLineOrdering, DisplayOrientation, DisplayFixedOutput; } KMT_DISPLAYMODE;
typedef struct { KMT_HANDLE hAdapter; UINT VidPnSourceId; KMT_DISPLAYMODE *pModeList; UINT ModeCount; } KMT_GETDISPLAYMODELIST;
typedef struct { union { KMT_HANDLE hAdapter; void *pAdapter; }; UINT Flags; KMT_HANDLE hDevice; void *pCommandBuffer; UINT CommandBufferSize; void *pAllocationList; UINT AllocationListSize;
                 void *pPatchLocationList; UINT PatchLocationListSize; } KMT_CREATEDEVICE;
typedef struct { KMT_HANDLE hDevice; } KMT_DESTROYDEVICE;
NTSTATUS WINAPI D3DKMTEnumAdapters2(KMT_ENUMADAPTERS2 *);
NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(KMT_OPENADAPTERFROMLUID *);
NTSTATUS WINAPI D3DKMTCloseAdapter(const KMT_CLOSEADAPTER *);
NTSTATUS WINAPI D3DKMTQueryAdapterInfo(KMT_QUERYADAPTERINFO *);
NTSTATUS WINAPI D3DKMTQueryVideoMemoryInfo(KMT_QUERYVIDEOMEMORYINFO *);
NTSTATUS WINAPI D3DKMTGetDisplayModeList(KMT_GETDISPLAYMODELIST *);
NTSTATUS WINAPI D3DKMTCreateDevice(KMT_CREATEDEVICE *);
NTSTATUS WINAPI D3DKMTDestroyDevice(const KMT_DESTROYDEVICE *);

static void thunks(void) {
  check(sizeof(KMT_ENUMADAPTERS2) == 2 * sizeof(void *) && sizeof(KMT_QUERYADAPTERINFO) == 8 + 2 * sizeof(void *) && sizeof(KMT_CREATEDEVICE) == (sizeof(void *) == 4 ? 36 : 64),
        "the structures have the sizes of d3dkmthk.h for this width");
  KMT_ENUMADAPTERS2 all = {0, NULL};
  check(D3DKMTEnumAdapters2(&all) >= 0 && all.NumAdapters >= 1 && all.NumAdapters <= 16, "D3DKMTEnumAdapters2 counts the adapters");
  KMT_ADAPTERINFO adapters[16];
  memset(adapters, 0, sizeof(adapters));
  all.NumAdapters = 16;
  all.pAdapters = adapters;
  const NTSTATUS listed = D3DKMTEnumAdapters2(&all);
  check(listed >= 0 && all.NumAdapters >= 1 && all.pAdapters == adapters && adapters[0].hAdapter != 0 && adapters[0].NumOfSources >= 1,
        "D3DKMTEnumAdapters2 lists them where the program's pointer says");
  if (listed < 0 || all.NumAdapters == 0) return;
  KMT_OPENADAPTERFROMLUID opened = {adapters[0].AdapterLuid, 0};
  check(D3DKMTOpenAdapterFromLuid(&opened) >= 0 && opened.hAdapter != 0, "D3DKMTOpenAdapterFromLuid");
  const KMT_HANDLE adapter = opened.hAdapter;

  UINT64 segments[3] = {0, 0, 0};
  KMT_QUERYADAPTERINFO query = {adapter, 3 /* KMTQAITYPE_GETSEGMENTSIZE */, segments, sizeof(segments)};
  check(D3DKMTQueryAdapterInfo(&query) >= 0 && (segments[0] | segments[1] | segments[2]) != 0 && query.pPrivateDriverData == segments && query.PrivateDriverDataSize == sizeof(segments),
        "D3DKMTQueryAdapterInfo fills the program's buffer (the segment sizes)");

  KMT_QUERYVIDEOMEMORYINFO memory;
  memset(&memory, 0, sizeof(memory));
  memory.hAdapter = adapter;
  memory.PhysicalAdapterIndex = 0;
  check(D3DKMTQueryVideoMemoryInfo(&memory) >= 0 && memory.Budget != 0 && memory.hAdapter == adapter && memory.hProcess == NULL, "D3DKMTQueryVideoMemoryInfo: a budget");

  static KMT_DISPLAYMODE modes[64];
  KMT_GETDISPLAYMODELIST list = {adapter, 0, modes, 64};
  check(D3DKMTGetDisplayModeList(&list) >= 0 && list.ModeCount >= 1 && list.ModeCount <= 64 && list.pModeList == modes && modes[0].Width >= 320 && modes[0].Height >= 200,
        "D3DKMTGetDisplayModeList: the modes, in the program's array");

  KMT_CREATEDEVICE device;
  memset(&device, 0, sizeof(device));
  device.hAdapter = adapter;
  check(D3DKMTCreateDevice(&device) >= 0 && device.hDevice != 0 && device.hAdapter == adapter, "D3DKMTCreateDevice");
  const KMT_DESTROYDEVICE destroy = {device.hDevice};
  check(device.hDevice != 0 && D3DKMTDestroyDevice(&destroy) >= 0, "D3DKMTDestroyDevice");

  for (ULONG i = 0; i < all.NumAdapters && i < 16; ++i) {
    const KMT_CLOSEADAPTER close = {adapters[i].hAdapter};
    D3DKMTCloseAdapter(&close);
  }
  const KMT_CLOSEADAPTER close = {adapter};
  check(D3DKMTCloseAdapter(&close) >= 0, "D3DKMTCloseAdapter");
}

int main(void) {
  thunks();
  const int result = d3d12test_main();      /* (prints its own failures, and its count) */
  check(result == 0 && checks > 300 && failed == 0, "tests/windows/d3d12test.c: every check of dxgi.dll and d3d12.dll");
  if (g_failed == 0) printf("dx32: all %d checks passed (and the %d of d3d12test)\n", g_passed, checks);
  else printf("dx32: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
