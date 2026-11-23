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

/* Shared handles of Direct3D 12 and Direct3D 11 (NT handles: D3DKMTShareObjects and the *FromNtHandle calls of win32k): resources, heaps and fences shared between
 * two devices, by handle, by a duplicated handle and by name; what one device writes the other reads; lifetimes and refusals. With "child <handle> <handle>" as its
 * arguments the program is the second process of tests/d3d_share_qemu.sh: it opens the handles it inherited, checks what the parent wrote, writes its own answer and
 * signals the shared fence. tests/emulated_dx.sh runs the one-process part on the emulator. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS 1
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d12.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <winternl.h>

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)
#define CHECK_HR(x) do { HRESULT hr_ = (x); ++checks; if (FAILED(hr_)) { ++failed; printf("FAILED line %d: %s = %08lx\n", __LINE__, #x, (unsigned long)hr_); } } while (0)

/* ---- a Direct3D 12 device with a queue, a list and a fence to wait with ---------------------------------------------------------------------------------------- */

typedef struct D12 {
  ID3D12Device *dev;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12Fence *fence;
  UINT64 value;
} D12;

static int d12_make(D12 *d) {
  memset(d, 0, sizeof *d);
  if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d->dev))) return 0;
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  CHECK_HR(ID3D12Device_CreateCommandQueue(d->dev, &qd, &IID_ID3D12CommandQueue, (void **)&d->queue));
  CHECK_HR(ID3D12Device_CreateCommandAllocator(d->dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&d->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(d->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d->list));
  CHECK_HR(ID3D12Device_CreateFence(d->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d->fence));
  return d->queue && d->allocator && d->list && d->fence;
}
/* Runs what the list recorded and waits for it; the list records again afterwards. */
static void d12_run(D12 *d) {
  CHECK_HR(ID3D12GraphicsCommandList_Close(d->list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)d->list};
  ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
  CHECK_HR(ID3D12CommandQueue_Signal(d->queue, d->fence, ++d->value));
  HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
  CHECK_HR(ID3D12Fence_SetEventOnCompletion(d->fence, d->value, event));
  CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
  CloseHandle(event);
  CHECK_HR(ID3D12CommandAllocator_Reset(d->allocator));
  CHECK_HR(ID3D12GraphicsCommandList_Reset(d->list, d->allocator, NULL));
}
static void d12_free(D12 *d) {
  if (d->fence) ID3D12Fence_Release(d->fence);
  if (d->list) ID3D12GraphicsCommandList_Release(d->list);
  if (d->allocator) ID3D12CommandAllocator_Release(d->allocator);
  if (d->queue) ID3D12CommandQueue_Release(d->queue);
  if (d->dev) ID3D12Device_Release(d->dev);
}
static ID3D12Resource *buffer(ID3D12Device *dev, D3D12_HEAP_TYPE type, D3D12_HEAP_FLAGS heap_flags, UINT64 size, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *r = NULL;
  const HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, heap_flags, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
  if (FAILED(hr)) printf("buffer: %08lx\n", (unsigned long)hr);
  return r;
}
/* `bytes` into a GPU buffer (through an upload buffer of the device's). */
static void d12_write(D12 *d, ID3D12Resource *dst, UINT64 offset, const void *bytes, UINT size) {
  ID3D12Resource *up = buffer(d->dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE, size, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!up) { CHECK(0); return; }
  void *p = NULL;
  CHECK_HR(ID3D12Resource_Map(up, 0, NULL, &p));
  if (p) memcpy(p, bytes, size);
  ID3D12Resource_Unmap(up, 0, NULL);
  ID3D12GraphicsCommandList_CopyBufferRegion(d->list, dst, offset, up, 0, size);
  d12_run(d);
  ID3D12Resource_Release(up);
}
/* What a GPU buffer holds (through a readback buffer): 1 if it is `expect`. */
static int d12_holds(D12 *d, ID3D12Resource *src, UINT64 offset, const void *expect, UINT size) {
  ID3D12Resource *down = buffer(d->dev, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE, size, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!down) return 0;
  ID3D12GraphicsCommandList_CopyBufferRegion(d->list, down, 0, src, offset, size);
  d12_run(d);
  void *p = NULL;
  int same = 0;
  if (SUCCEEDED(ID3D12Resource_Map(down, 0, NULL, &p)) && p) {
    same = memcmp(p, expect, size) == 0;
    if (!same) printf("  holds %08x, expected %08x\n", *(const unsigned *)p, *(const unsigned *)expect);
    ID3D12Resource_Unmap(down, 0, NULL);
  }
  ID3D12Resource_Release(down);
  return same;
}
static void aliasing(D12 *d, ID3D12Resource *before, ID3D12Resource *after) {
  D3D12_RESOURCE_BARRIER b;
  memset(&b, 0, sizeof b);
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  b.Aliasing.pResourceBefore = before;
  b.Aliasing.pResourceAfter = after;
  ID3D12GraphicsCommandList_ResourceBarrier(d->list, 1, &b);
}
static void pattern(UINT *words, UINT count, UINT seed) {
  for (UINT i = 0; i < count; ++i) words[i] = seed * 0x01000193u + i * 0x9E3779B9u;
}

/* ---- one process, two Direct3D 12 devices ---------------------------------------------------------------------------------------------------------------------- */

static void test_resources(D12 *a, D12 *b) {
  UINT first[64], second[64];
  pattern(first, 64, 1);
  pattern(second, 64, 2);
  ID3D12Resource *shared = buffer(a->dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED, sizeof first, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *plain = buffer(a->dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, sizeof first, D3D12_RESOURCE_STATE_COMMON);
  CHECK(shared && plain);
  if (!shared || !plain) return;
  /* (upload memory is not shared: Direct3D 12 shares GPU memory) */
  CHECK(buffer(a->dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_SHARED, 256, D3D12_RESOURCE_STATE_GENERIC_READ) == NULL);
  d12_write(a, shared, 0, first, sizeof first);

  HANDLE handle = NULL, again = NULL;
  CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)shared, NULL, GENERIC_ALL, L"TcosShareTestBuffer", &handle));
  CHECK(handle != NULL);
  CHECK(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)shared, NULL, GENERIC_ALL, L"TcosShareTestBuffer", &again) == DXGI_ERROR_NAME_ALREADY_EXISTS && !again);
  CHECK(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)shared, NULL, GENERIC_READ, NULL, &again) == E_INVALIDARG && !again);
  CHECK(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)plain, NULL, GENERIC_ALL, NULL, &again) == E_INVALIDARG && !again);
  CHECK(ID3D12Device_CreateSharedHandle(b->dev, (ID3D12DeviceChild *)shared, NULL, GENERIC_ALL, NULL, &again) == E_INVALIDARG && !again);      /* (of another device) */
  CHECK(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)shared, NULL, GENERIC_ALL, NULL, &again) == S_OK && again);      /* (a second handle, without a name) */
  if (again) CloseHandle(again);

  /* The other device opens it: the same buffer, the same contents; what it writes, the first reads. */
  ID3D12Resource *opened = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, handle, &IID_ID3D12Resource, (void **)&opened));
  if (opened) {
    const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(opened);
    D3D12_HEAP_PROPERTIES hp;
    D3D12_HEAP_FLAGS hf = 0;
    CHECK_HR(ID3D12Resource_GetHeapProperties(opened, &hp, &hf));
    CHECK(rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && rd.Width == sizeof first && hp.Type == D3D12_HEAP_TYPE_DEFAULT && (hf & D3D12_HEAP_FLAG_SHARED));
    CHECK(d12_holds(b, opened, 0, first, sizeof first));
    d12_write(b, opened, 0, second, sizeof second);
    CHECK(d12_holds(a, shared, 0, second, sizeof second));
    /* (shared again from the device that opened it) */
    CHECK_HR(ID3D12Device_CreateSharedHandle(b->dev, (ID3D12DeviceChild *)opened, NULL, GENERIC_ALL, NULL, &again));
    if (again) CloseHandle(again);
  }
  void *wrong = (void *)1;
  CHECK(ID3D12Device_OpenSharedHandle(b->dev, handle, &IID_ID3D12Fence, &wrong) == E_NOINTERFACE && wrong == NULL);
  CHECK(ID3D12Device_OpenSharedHandle(b->dev, NULL, &IID_ID3D12Resource, &wrong) == E_INVALIDARG);
  CHECK(ID3D12Device_OpenSharedHandle(b->dev, (HANDLE)(UINT_PTR)0x7FF4, &IID_ID3D12Resource, &wrong) == E_INVALIDARG);

  /* By name, and through a duplicated handle. */
  HANDLE by_name = NULL, copy = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandleByName(b->dev, L"TcosShareTestBuffer", GENERIC_ALL, &by_name));
  CHECK(ID3D12Device_OpenSharedHandleByName(b->dev, L"TcosNoSuchShare", GENERIC_ALL, &again) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !again);
  CHECK(ID3D12Device_OpenSharedHandleByName(b->dev, L"TcosShareTestBuffer", GENERIC_READ, &again) == E_INVALIDARG);
  CHECK(DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS));
  ID3D12Resource *third = NULL, *fourth = NULL;
  if (by_name) CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, by_name, &IID_ID3D12Resource, (void **)&third));
  if (copy) CHECK_HR(ID3D12Device_OpenSharedHandle(a->dev, copy, &IID_ID3D12Resource, (void **)&fourth));
  if (third) CHECK(d12_holds(b, third, 0, second, sizeof second));
  if (fourth) CHECK(d12_holds(a, fourth, 0, second, sizeof second));

  /* The creator lets its resource and handles go: what was opened still has the memory; the name goes with the last handle. */
  ID3D12Resource_Release(shared);
  CloseHandle(handle);
  if (copy) CloseHandle(copy);
  if (opened) CHECK(d12_holds(b, opened, 0, second, sizeof second));
  if (by_name) CloseHandle(by_name);
  CHECK(ID3D12Device_OpenSharedHandleByName(b->dev, L"TcosShareTestBuffer", GENERIC_ALL, &again) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  if (third) ID3D12Resource_Release(third);
  if (fourth) ID3D12Resource_Release(fourth);
  if (opened) ID3D12Resource_Release(opened);
  ID3D12Resource_Release(plain);
}

static void test_textures(D12 *a, D12 *b) {
  /* A texture: its description (format, levels, flags) comes along */
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource *tex = NULL, *opened = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(a->dev, &hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&tex));
  if (!tex) return;
  HANDLE handle = NULL;
  CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)tex, NULL, GENERIC_ALL, NULL, &handle));
  /* a clear on the first device, read on the second */
  ID3D12DescriptorHeap *rtv_heap = NULL;
  D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(a->dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  if (rtv_heap) {
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(a->dev, tex, NULL, rtv);
    const float teal[4] = {0.0f, 0.5f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList_ClearRenderTargetView(a->list, rtv, teal, 0, NULL);
    d12_run(a);
    ID3D12DescriptorHeap_Release(rtv_heap);
  }
  if (handle) CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, handle, &IID_ID3D12Resource, (void **)&opened));
  if (opened) {
    const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(opened);
    CHECK(rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width == 8 && rd.Height == 4 && rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM && rd.MipLevels == 1 &&
          (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    ID3D12Resource *down = buffer(b->dev, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE, 256 * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    to.pResource = down;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    to.PlacedFootprint.Footprint.Width = 8;
    to.PlacedFootprint.Footprint.Height = 4;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = 256;
    from.pResource = opened;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(b->list, &to, 0, 0, 0, &from, NULL);
    d12_run(b);
    const UINT *px = NULL;
    if (down && SUCCEEDED(ID3D12Resource_Map(down, 0, NULL, (void **)&px)) && px) {
      printf("shared texture: %08x\n", px[0]);
      CHECK(px[0] == 0xFFFF8000u && px[3 * 64 + 7] == 0xFFFF8000u);      /* (R 0, G 128, B 255, A 255) */
      ID3D12Resource_Unmap(down, 0, NULL);
    }
    if (down) ID3D12Resource_Release(down);
    ID3D12Resource_Release(opened);
  }
  if (handle) CloseHandle(handle);
  ID3D12Resource_Release(tex);
}

static void test_heaps(D12 *a, D12 *b) {
  /* A shared heap: a placed buffer in it on one device, another at the same offset in the opened heap on the other, the bytes handed over with aliasing barriers */
  D3D12_HEAP_DESC hd = {65536, {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1}, 0, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ID3D12Heap *heap = NULL, *opened = NULL;
  CHECK_HR(ID3D12Device_CreateHeap(a->dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  if (!heap) return;
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1024, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *placed = NULL, *placed_b = NULL;
  CHECK_HR(ID3D12Device_CreatePlacedResource(a->dev, heap, 4096, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&placed));
  UINT words[256];
  pattern(words, 256, 7);
  if (placed) {
    d12_write(a, placed, 0, words, sizeof words);
    aliasing(a, placed, NULL);      /* (its contents go to the heap's memory) */
    d12_run(a);
  }
  HANDLE handle = NULL;
  CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)heap, NULL, GENERIC_ALL, NULL, &handle));
  if (handle) CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, handle, &IID_ID3D12Heap, (void **)&opened));
  if (opened) {
    const D3D12_HEAP_DESC got = ID3D12Heap_GetDesc(opened);
    CHECK(got.SizeInBytes == 65536 && (got.Flags & D3D12_HEAP_FLAG_SHARED) && got.Properties.Type == D3D12_HEAP_TYPE_DEFAULT);
    CHECK_HR(ID3D12Device_CreatePlacedResource(b->dev, opened, 4096, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&placed_b));
    if (placed_b) {
      aliasing(b, NULL, placed_b);
      CHECK(d12_holds(b, placed_b, 0, words, sizeof words));
      ID3D12Resource_Release(placed_b);
    }
    ID3D12Heap_Release(opened);
  }
  if (handle) CloseHandle(handle);
  if (placed) ID3D12Resource_Release(placed);
  ID3D12Heap_Release(heap);
}

static void test_fences(D12 *a, D12 *b) {
  ID3D12Fence *fence = NULL, *plain = NULL, *opened = NULL;
  CHECK_HR(ID3D12Device_CreateFence(a->dev, 3, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&fence));
  CHECK_HR(ID3D12Device_CreateFence(a->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&plain));
  CHECK(ID3D12Device_CreateFence(a->dev, 0, D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER | D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&opened) == DXGI_ERROR_UNSUPPORTED);
  if (!fence || !plain) return;
  HANDLE handle = NULL, none = NULL;
  CHECK(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)plain, NULL, GENERIC_ALL, NULL, &none) == E_INVALIDARG && !none);
  CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)fence, NULL, GENERIC_ALL, L"TcosShareTestFence", &handle));
  void *wrong = (void *)1;
  if (handle) CHECK(ID3D12Device_OpenSharedHandle(b->dev, handle, &IID_ID3D12Resource, &wrong) == E_NOINTERFACE && !wrong);
  if (handle) CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, handle, &IID_ID3D12Fence, (void **)&opened));
  if (opened) {
    CHECK(ID3D12Fence_GetCompletedValue(opened) == 3);
    CHECK_HR(ID3D12Fence_Signal(fence, 5));
    CHECK(ID3D12Fence_GetCompletedValue(opened) == 5);
    /* the other device's queue signals it; the first device's waiters see it */
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK_HR(ID3D12Fence_SetEventOnCompletion(fence, 9, event));
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    CHECK_HR(ID3D12CommandQueue_Signal(b->queue, opened, 9));
    CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
    CHECK(ID3D12Fence_GetCompletedValue(fence) == 9);
    CloseHandle(event);
    ID3D12Fence1 *f1 = NULL;
    CHECK_HR(ID3D12Fence_QueryInterface(opened, &IID_ID3D12Fence1, (void **)&f1));
    if (f1) { CHECK(ID3D12Fence1_GetCreationFlags(f1) & D3D12_FENCE_FLAG_SHARED); ID3D12Fence1_Release(f1); }
    ID3D12Fence_Release(opened);
  }
  HANDLE by_name = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandleByName(b->dev, L"TcosShareTestFence", GENERIC_ALL, &by_name));
  if (by_name) CloseHandle(by_name);
  if (handle) CloseHandle(handle);
  ID3D12Fence_Release(fence);
  ID3D12Fence_Release(plain);
}

/* ---- Direct3D 11 ------------------------------------------------------------------------------------------------------------------------------------------------ */

static void test_d3d11(D12 *a) {
  ID3D11Device *x = NULL, *y = NULL;
  ID3D11DeviceContext *cx = NULL, *cy = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &x, NULL, &cx));
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &y, NULL, &cy));
  if (!x || !y) return;
  UINT texels[16];
  pattern(texels, 16, 11);
  D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                             D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  D3D11_SUBRESOURCE_DATA init = {texels, 16, 0};
  ID3D11Texture2D *tex = NULL, *opened = NULL, *bad = NULL, *plain = NULL;
  CHECK_HR(ID3D11Device_CreateTexture2D(x, &td, &init, &tex));
  D3D11_TEXTURE2D_DESC no_shared = td;
  no_shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;      /* (an NT handle needs SHARED or SHARED_KEYEDMUTEX) */
  CHECK(ID3D11Device_CreateTexture2D(x, &no_shared, NULL, &bad) == E_INVALIDARG && !bad);
  no_shared.MiscFlags = 0;
  CHECK_HR(ID3D11Device_CreateTexture2D(x, &no_shared, NULL, &plain));
  if (!tex) return;
  ID3D11DeviceContext_Flush(cx);
  IDXGIResource1 *dres = NULL, *dplain = NULL;
  HANDLE handle = NULL, none = NULL;
  CHECK_HR(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dres));
  if (plain) CHECK_HR(ID3D11Texture2D_QueryInterface(plain, &IID_IDXGIResource1, (void **)&dplain));
  if (dplain) { CHECK(IDXGIResource1_CreateSharedHandle(dplain, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &none) == E_INVALIDARG && !none); IDXGIResource1_Release(dplain); }
  if (dres) {
    CHECK(IDXGIResource1_CreateSharedHandle(dres, NULL, 0, NULL, &none) == E_INVALIDARG);
    CHECK_HR(IDXGIResource1_CreateSharedHandle(dres, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, L"TcosShareTestTexture", &handle));
    IDXGIResource1_Release(dres);
  }
  ID3D11Device1 *y1 = NULL;
  CHECK_HR(ID3D11Device_QueryInterface(y, &IID_ID3D11Device1, (void **)&y1));
  if (y1 && handle) {
    CHECK_HR(ID3D11Device1_OpenSharedResource1(y1, handle, &IID_ID3D11Texture2D, (void **)&opened));
    if (opened) {
      D3D11_TEXTURE2D_DESC got;
      ID3D11Texture2D_GetDesc(opened, &got);
      CHECK(got.Width == 4 && got.Height == 4 && got.Format == DXGI_FORMAT_R8G8B8A8_UNORM && got.BindFlags == td.BindFlags && got.MiscFlags == td.MiscFlags);
      /* its texels, read on the other device through a staging texture */
      D3D11_TEXTURE2D_DESC sd = td;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      ID3D11Texture2D *staging = NULL;
      CHECK_HR(ID3D11Device_CreateTexture2D(y, &sd, NULL, &staging));
      if (staging) {
        ID3D11DeviceContext_CopyResource(cy, (ID3D11Resource *)staging, (ID3D11Resource *)opened);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(cy, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
          int same = 1;
          for (int r = 0; r < 4; ++r) same &= memcmp((BYTE *)m.pData + r * m.RowPitch, texels + 4 * r, 16) == 0;
          CHECK(same);
          ID3D11DeviceContext_Unmap(cy, (ID3D11Resource *)staging, 0);
        } else CHECK(0);
        ID3D11Texture2D_Release(staging);
      }
    }
    ID3D11Texture2D *by_name = NULL;
    CHECK_HR(ID3D11Device1_OpenSharedResourceByName(y1, L"TcosShareTestTexture", DXGI_SHARED_RESOURCE_READ, &IID_ID3D11Texture2D, (void **)&by_name));
    if (by_name) ID3D11Texture2D_Release(by_name);
    ID3D11Texture2D *wrong = NULL;
    CHECK(ID3D11Device1_OpenSharedResource(y1, handle, &IID_ID3D11Texture2D, (void **)&wrong) == E_INVALIDARG);      /* (an NT handle: OpenSharedResource1's) */
    /* Direct3D 12 opens Direct3D 11's share */
    ID3D12Resource *as12 = NULL;
    CHECK_HR(ID3D12Device_OpenSharedHandle(a->dev, handle, &IID_ID3D12Resource, (void **)&as12));
    if (as12) {
      const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(as12);
      CHECK(rd.Width == 4 && rd.Height == 4 && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
      ID3D12Resource_Release(as12);
    }
  }
  /* A one-dimensional texture is opened as one */
  D3D11_TEXTURE1D_DESC t1 = {8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ID3D11Texture1D *line = NULL, *line_b = NULL;
  CHECK_HR(ID3D11Device_CreateTexture1D(x, &t1, NULL, &line));
  if (line && y1) {
    IDXGIResource1 *r1 = NULL;
    HANDLE h1 = NULL;
    CHECK_HR(ID3D11Texture1D_QueryInterface(line, &IID_IDXGIResource1, (void **)&r1));
    if (r1) { CHECK_HR(IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &h1)); IDXGIResource1_Release(r1); }
    if (h1) {
      CHECK_HR(ID3D11Device1_OpenSharedResource1(y1, h1, &IID_ID3D11Texture1D, (void **)&line_b));
      if (line_b) {
        D3D11_TEXTURE1D_DESC got;
        ID3D11Texture1D_GetDesc(line_b, &got);
        CHECK(got.Width == 8 && got.ArraySize == 1);
        ID3D11Texture1D_Release(line_b);
      }
      CloseHandle(h1);
    }
  }
  if (line) ID3D11Texture1D_Release(line);
  if (y1) ID3D11Device1_Release(y1);
  if (opened) ID3D11Texture2D_Release(opened);
  if (handle) CloseHandle(handle);
  if (plain) ID3D11Texture2D_Release(plain);
  ID3D11Texture2D_Release(tex);
  ID3D11DeviceContext_Release(cx);
  ID3D11DeviceContext_Release(cy);
  ID3D11Device_Release(x);
  ID3D11Device_Release(y);
}

/* ---- gdi32's D3DKMT functions, with the program's own structures (a 32-bit program's are converted by gdi32: wow64.inc) ---------------------------------------- */

typedef struct { DWORD access; OBJECT_ATTRIBUTES *attributes; HANDLE nt; } KmtOpenByName;      /* D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME */
typedef struct { HANDLE nt; UINT sync; } KmtOpenSync;                                            /* D3DKMT_OPENSYNCOBJECTFROMNTHANDLE */
typedef NTSTATUS(WINAPI *KmtByNameFn)(KmtOpenByName *);
typedef NTSTATUS(WINAPI *KmtOpenSyncFn)(KmtOpenSync *);
typedef NTSTATUS(WINAPI *KmtShareFn)(UINT, const UINT *, OBJECT_ATTRIBUTES *, DWORD, HANDLE *);
typedef NTSTATUS(WINAPI *KmtDestroySyncFn)(UINT *);
static void name_attributes(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us, const WCHAR *text) {
  us->Buffer = (WCHAR *)text;
  us->Length = (USHORT)(wcslen(text) * 2);
  us->MaximumLength = (USHORT)(us->Length + 2);
  memset(oa, 0, sizeof *oa);
  oa->Length = sizeof *oa;
  oa->ObjectName = us;
}
static void test_kmt_direct(D12 *a) {
  HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
  KmtByNameFn by_name = (KmtByNameFn)(void *)GetProcAddress(gdi, "D3DKMTOpenSyncObjectNtHandleFromName");
  KmtOpenSyncFn open_sync = (KmtOpenSyncFn)(void *)GetProcAddress(gdi, "D3DKMTOpenSyncObjectFromNtHandle");
  KmtShareFn share = (KmtShareFn)(void *)GetProcAddress(gdi, "D3DKMTShareObjects");
  KmtDestroySyncFn destroy = (KmtDestroySyncFn)(void *)GetProcAddress(gdi, "D3DKMTDestroySynchronizationObject");
  CHECK(by_name && open_sync && share && destroy);
  if (!by_name || !open_sync || !share || !destroy) return;
  ID3D12Fence *fence = NULL;
  HANDLE handle = NULL;
  CHECK_HR(ID3D12Device_CreateFence(a->dev, 4, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&fence));
  if (fence) CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)fence, NULL, GENERIC_ALL, L"TcosKmtFence", &handle));
  if (!handle) { if (fence) ID3D12Fence_Release(fence); return; }
  OBJECT_ATTRIBUTES oa;
  UNICODE_STRING us;
  name_attributes(&oa, &us, L"\\Sessions\\1\\BaseNamedObjects\\TcosKmtFence");
  KmtOpenByName n = {GENERIC_ALL, &oa, NULL};
  CHECK(by_name(&n) == 0 && n.nt != NULL);
  KmtOpenSync o = {n.nt, 0};
  CHECK(open_sync(&o) == 0 && o.sync != 0);
  /* the opened fence shared again under a name of its own, and found by it */
  HANDLE again = NULL;
  OBJECT_ATTRIBUTES oa2;
  UNICODE_STRING us2;
  name_attributes(&oa2, &us2, L"\\Sessions\\1\\BaseNamedObjects\\TcosKmtFence2");
  CHECK(share(1, &o.sync, &oa2, GENERIC_ALL, &again) == 0 && again != NULL);
  KmtOpenByName n2 = {GENERIC_ALL, &oa2, NULL};
  CHECK(by_name(&n2) == 0 && n2.nt != NULL);
  ID3D12Fence *via_kmt = NULL;
  if (n2.nt) CHECK_HR(ID3D12Device_OpenSharedHandle(a->dev, n2.nt, &IID_ID3D12Fence, (void **)&via_kmt));
  if (via_kmt) { CHECK(ID3D12Fence_GetCompletedValue(via_kmt) == 4); ID3D12Fence_Release(via_kmt); }
  if (o.sync) destroy(&o.sync);
  if (again) CloseHandle(again);
  if (n2.nt) CloseHandle(n2.nt);
  if (n.nt) CloseHandle(n.nt);
  CloseHandle(handle);
  ID3D12Fence_Release(fence);
}

/* ---- two processes ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* The parent shares a buffer and a fence, starts this program again with the two handles inherited, and waits for the fence: the child checks the buffer holds the
 * parent's words, writes its own after them and signals the fence with 2; the parent then reads the child's words. */
static int child(HANDLE buffer_handle, HANDLE fence_handle) {
  D12 c;
  if (!d12_make(&c)) { printf("child: no device\n"); return 1; }
  ID3D12Resource *shared = NULL;
  ID3D12Fence *fence = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandle(c.dev, buffer_handle, &IID_ID3D12Resource, (void **)&shared));
  CHECK_HR(ID3D12Device_OpenSharedHandle(c.dev, fence_handle, &IID_ID3D12Fence, (void **)&fence));
  UINT mine[64], theirs[64];
  pattern(theirs, 64, 21);
  pattern(mine, 64, 22);
  if (shared && fence) {
    CHECK(ID3D12Fence_GetCompletedValue(fence) == 1);
    CHECK(d12_holds(&c, shared, 0, theirs, sizeof theirs));
    d12_write(&c, shared, sizeof theirs, mine, sizeof mine);
    CHECK_HR(ID3D12CommandQueue_Signal(c.queue, fence, 2));
  }
  if (shared) ID3D12Resource_Release(shared);
  if (fence) ID3D12Fence_Release(fence);
  d12_free(&c);
  printf("d3dsharetest child: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

static void test_processes(D12 *a) {
  UINT theirs[64], mine[64];
  pattern(mine, 64, 21);
  pattern(theirs, 64, 22);
  ID3D12Resource *shared = buffer(a->dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED, 2 * sizeof mine, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Fence *fence = NULL;
  CHECK_HR(ID3D12Device_CreateFence(a->dev, 1, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&fence));
  if (!shared || !fence) return;
  d12_write(a, shared, 0, mine, sizeof mine);
  SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
  HANDLE hb = NULL, hf = NULL;
  CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)shared, &inherit, GENERIC_ALL, NULL, &hb));
  CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)fence, &inherit, GENERIC_ALL, NULL, &hf));
  WCHAR self[MAX_PATH], line[MAX_PATH + 64];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  swprintf(line, sizeof line / sizeof line[0], L"\"%ls\" child %llu %llu", self, (unsigned long long)(UINT_PTR)hb, (unsigned long long)(UINT_PTR)hf);
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  memset(&pi, 0, sizeof pi);
  const BOOL started = CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  CHECK(started);
  if (started) {
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK_HR(ID3D12Fence_SetEventOnCompletion(fence, 2, event));
    CHECK(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0);
    CloseHandle(event);
    CHECK(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("child exit code %lu\n", (unsigned long)code);
    CHECK(code == 0);
    CHECK(d12_holds(a, shared, sizeof mine, theirs, sizeof theirs));      /* (what the child wrote) */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  CloseHandle(hb);
  CloseHandle(hf);
  ID3D12Resource_Release(shared);
  ID3D12Fence_Release(fence);
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "child") == 0) return child((HANDLE)(UINT_PTR)_strtoui64(argv[2], NULL, 10), (HANDLE)(UINT_PTR)_strtoui64(argv[3], NULL, 10));
  const int processes = argc > 1 && strcmp(argv[1], "processes") == 0;
  D12 a, b;
  if (!d12_make(&a) || !d12_make(&b)) {
    printf("d3dsharetest: no device\nd3dsharetest: %d checks, %d failed\n", checks, failed + 1);
    return 1;
  }
  test_resources(&a, &b);
  test_textures(&a, &b);
  test_heaps(&a, &b);
  test_fences(&a, &b);
  test_d3d11(&a);
  test_kmt_direct(&a);
  if (processes) test_processes(&a);
  d12_free(&a);
  d12_free(&b);
  printf("d3dsharetest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
