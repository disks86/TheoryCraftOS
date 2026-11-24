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
#include <d3d11_4.h>
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
/* ---- Direct3D 11 fences (ID3D11Device5, ID3D11DeviceContext4) ------------------------------------------------------------------------------------------------------ */

static ID3D11Device5 *device5(ID3D11DeviceContext4 **context) {
  ID3D11Device *dev = NULL;
  ID3D11DeviceContext *ctx = NULL;
  ID3D11Device5 *d5 = NULL;
  *context = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx));
  if (!dev) return NULL;
  CHECK_HR(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5, (void **)&d5));
  CHECK_HR(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4, (void **)context));
  ID3D11DeviceContext_Release(ctx);
  ID3D11Device_Release(dev);
  return d5;
}
typedef struct LateSignal { ID3D11DeviceContext4 *ctx; ID3D11Fence *fence; UINT64 value; volatile LONG done; } LateSignal;
static DWORD WINAPI late_signal(void *p) {
  LateSignal *s = p;
  Sleep(300);
  InterlockedExchange(&s->done, 1);
  ID3D11DeviceContext4_Signal(s->ctx, s->fence, s->value);
  return 0;
}
static void test_d3d11_fences(D12 *a) {
  ID3D11DeviceContext4 *cx = NULL, *cy = NULL;
  ID3D11Device5 *x = device5(&cx), *y = device5(&cy);
  if (!x || !y || !cx || !cy) goto done;
  ID3D11Fence *fence = NULL, *plain = NULL, *none1 = NULL, *bad = NULL, *opened = NULL;
  CHECK_HR(ID3D11Device5_CreateFence(x, 3, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)&fence));
  CHECK_HR(ID3D11Device5_CreateFence(x, 0, (D3D11_FENCE_FLAG)0, &IID_ID3D11Fence, (void **)&plain));
  CHECK_HR(ID3D11Device5_CreateFence(x, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence, (void **)&none1));
  CHECK(ID3D11Device5_CreateFence(x, 0, D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER, &IID_ID3D11Fence, (void **)&bad) == DXGI_ERROR_UNSUPPORTED && !bad);
  CHECK(ID3D11Device5_CreateFence(x, 0, (D3D11_FENCE_FLAG)0x10, &IID_ID3D11Fence, (void **)&bad) == E_INVALIDARG && !bad);
  CHECK(ID3D11Device5_CreateFence(x, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Device, (void **)&bad) == E_NOINTERFACE && !bad);
  if (!fence || !plain) goto fences_done;
  CHECK(ID3D11Fence_GetCompletedValue(fence) == 3);
  ID3D11Device *owner = NULL;
  ID3D11Fence_GetDevice(fence, &owner);
  CHECK(owner == (ID3D11Device *)x);
  if (owner) ID3D11Device_Release(owner);
  ID3D11DeviceChild *as_child = NULL;
  CHECK_HR(ID3D11Fence_QueryInterface(fence, &IID_ID3D11DeviceChild, (void **)&as_child));
  if (as_child) ID3D11DeviceChild_Release(as_child);
  /* handles: GENERIC_ALL only, and only of a shared fence */
  HANDLE handle = NULL, none = NULL;
  CHECK(ID3D11Fence_CreateSharedHandle(plain, NULL, GENERIC_ALL, NULL, &none) == E_INVALIDARG && !none);
  CHECK(ID3D11Fence_CreateSharedHandle(fence, NULL, GENERIC_READ, NULL, &none) == E_INVALIDARG && !none);
  CHECK_HR(ID3D11Fence_CreateSharedHandle(fence, NULL, GENERIC_ALL, L"TcosShareTestFence11", &handle));
  if (!handle) goto fences_done;
  CHECK_HR(ID3D11Device5_OpenSharedFence(y, handle, &IID_ID3D11Fence, (void **)&opened));
  CHECK(ID3D11Device5_OpenSharedFence(y, NULL, &IID_ID3D11Fence, (void **)&bad) == E_INVALIDARG && !bad);
  if (opened) {
    CHECK(ID3D11Fence_GetCompletedValue(opened) == 3);
    /* a signal of one device's context is the other's value */
    CHECK_HR(ID3D11DeviceContext4_Signal(cx, fence, 5));
    CHECK(ID3D11Fence_GetCompletedValue(opened) == 5);
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK_HR(ID3D11Fence_SetEventOnCompletion(fence, 7, event));
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    CHECK_HR(ID3D11DeviceContext4_Signal(cy, opened, 7));
    CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
    CHECK(ID3D11Fence_GetCompletedValue(fence) == 7);
    CHECK(ID3D11Fence_SetEventOnCompletion(fence, 8, NULL) == E_INVALIDARG);
    /* a wait of one context holds back what follows it until the other device signals: x waits for 11, then signals `plain` with 1 */
    LateSignal late = {cy, opened, 11, 0};
    HANDLE thread = CreateThread(NULL, 0, late_signal, &late, 0, NULL);
    CHECK(thread != NULL);
    if (thread) {
      CHECK_HR(ID3D11DeviceContext4_Wait(cx, fence, 11));
      CHECK_HR(ID3D11DeviceContext4_Signal(cx, plain, 1));
      CHECK_HR(ID3D11Fence_SetEventOnCompletion(plain, 1, event));
      CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
      CHECK(late.done == 1);      /* (not before the other device's signal) */
      CHECK(ID3D11Fence_GetCompletedValue(plain) == 1 && ID3D11Fence_GetCompletedValue(fence) == 11);
      WaitForSingleObject(thread, 10000);
      CloseHandle(thread);
    }
    CloseHandle(event);
    CHECK(ID3D11DeviceContext4_Signal(cx, NULL, 1) == E_INVALIDARG);
    CHECK(ID3D11DeviceContext4_Signal(cx, (ID3D11Fence *)x, 1) == E_INVALIDARG);      /* (not a fence) */
    ID3D11Fence_Release(opened);
  }
  /* the same fence is Direct3D 12's: its handle opens there, and a Direct3D 12 fence's opens here */
  ID3D12Fence *f12 = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandle(a->dev, handle, &IID_ID3D12Fence, (void **)&f12));
  if (f12) {
    CHECK(ID3D12Fence_GetCompletedValue(f12) == 11);
    CHECK_HR(ID3D12CommandQueue_Signal(a->queue, f12, 13));
    CHECK(ID3D11Fence_GetCompletedValue(fence) == 13);
    ID3D12Fence_Release(f12);
  }
  ID3D12Fence *from12 = NULL;
  HANDLE h12 = NULL;
  CHECK_HR(ID3D12Device_CreateFence(a->dev, 21, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&from12));
  if (from12) CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)from12, NULL, GENERIC_ALL, NULL, &h12));
  if (h12) {
    ID3D11Fence *in11 = NULL;
    CHECK_HR(ID3D11Device5_OpenSharedFence(x, h12, &IID_ID3D11Fence, (void **)&in11));
    if (in11) {
      CHECK(ID3D11Fence_GetCompletedValue(in11) == 21);
      CHECK_HR(ID3D11DeviceContext4_Signal(cx, in11, 22));
      CHECK(ID3D12Fence_GetCompletedValue(from12) == 22);
      HANDLE again = NULL;      /* (an opened fence is shared too: it gives a handle of its own) */
      CHECK_HR(ID3D11Fence_CreateSharedHandle(in11, NULL, GENERIC_ALL, NULL, &again));
      if (again) CloseHandle(again);
      ID3D11Fence_Release(in11);
    }
    CloseHandle(h12);
  }
  if (from12) ID3D12Fence_Release(from12);
  /* a resource's handle is not a fence's */
  ID3D12Resource *res = buffer(a->dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED, 256, D3D12_RESOURCE_STATE_COMMON);
  HANDLE hres = NULL;
  if (res) CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)res, NULL, GENERIC_ALL, NULL, &hres));
  if (hres) { CHECK(FAILED(ID3D11Device5_OpenSharedFence(x, hres, &IID_ID3D11Fence, (void **)&bad)) && !bad); CloseHandle(hres); }
  if (res) ID3D12Resource_Release(res);
  /* by name */
  HANDLE by_name = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandleByName(a->dev, L"TcosShareTestFence11", GENERIC_ALL, &by_name));
  if (by_name) {
    ID3D11Fence *named = NULL;
    CHECK_HR(ID3D11Device5_OpenSharedFence(y, by_name, &IID_ID3D11Fence, (void **)&named));
    if (named) { CHECK(ID3D11Fence_GetCompletedValue(named) == 13); ID3D11Fence_Release(named); }
    CloseHandle(by_name);
  }
  CloseHandle(handle);
fences_done:
  if (fence) ID3D11Fence_Release(fence);
  if (plain) ID3D11Fence_Release(plain);
  if (none1) ID3D11Fence_Release(none1);
done:
  if (cx) ID3D11DeviceContext4_Release(cx);
  if (cy) ID3D11DeviceContext4_Release(cy);
  if (x) ID3D11Device5_Release(x);
  if (y) ID3D11Device5_Release(y);
}

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

/* ---- keyed mutexes -------------------------------------------------------------------------------------------------------------------------------------------------- */

/* A texture of `color` made SHARED_KEYEDMUTEX | SHARED_NTHANDLE on `dev` (a render target, cleared), its mutex held by nobody with key 0. */
static ID3D11Texture2D *km_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx, const float color[4]) {
  D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                             D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ID3D11Texture2D *tex = NULL;
  CHECK_HR(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex));
  if (!tex) return NULL;
  ID3D11RenderTargetView *rtv = NULL;
  CHECK_HR(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv));
  if (rtv) {
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
    ID3D11RenderTargetView_Release(rtv);
  }
  ID3D11DeviceContext_Flush(ctx);
  return tex;
}
/* The first texel of `tex`, read on `dev` through a staging texture. */
static UINT texel_of(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex) {
  D3D11_TEXTURE2D_DESC sd = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  ID3D11Texture2D *staging = NULL;
  UINT texel = 0xDEADDEAD;
  if (FAILED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging)) || !staging) return texel;
  ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
    texel = *(const UINT *)m.pData;
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
  }
  ID3D11Texture2D_Release(staging);
  return texel;
}
static IDXGIKeyedMutex *mutex_of(void *texture) {
  IDXGIKeyedMutex *m = NULL;
  CHECK_HR(IUnknown_QueryInterface((IUnknown *)texture, &IID_IDXGIKeyedMutex, (void **)&m));
  return m;
}

typedef struct { IDXGIKeyedMutex *mutex; UINT64 key; HRESULT result; } Waiter;
static DWORD WINAPI wait_for_key(void *p) {
  Waiter *w = p;
  w->result = IDXGIKeyedMutex_AcquireSync(w->mutex, w->key, INFINITE);
  return 0;
}

static void test_keyed_mutex(void) {
  ID3D11Device *x = NULL, *y = NULL;
  ID3D11DeviceContext *cx = NULL, *cy = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &x, NULL, &cx));
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &y, NULL, &cy));
  if (!x || !y) return;
  const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
  ID3D11Texture2D *tex = km_texture(x, cx, red);
  if (!tex) return;
  /* (a texture made without SHARED_KEYEDMUTEX has no mutex) */
  D3D11_TEXTURE2D_DESC pd = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ID3D11Texture2D *plain = NULL;
  IDXGIKeyedMutex *none = NULL;
  CHECK_HR(ID3D11Device_CreateTexture2D(x, &pd, NULL, &plain));
  if (plain) { CHECK(ID3D11Texture2D_QueryInterface(plain, &IID_IDXGIKeyedMutex, (void **)&none) == E_NOINTERFACE && !none); ID3D11Texture2D_Release(plain); }

  IDXGIKeyedMutex *mx = mutex_of(tex), *my = NULL;
  IDXGIResource1 *dres = NULL;
  HANDLE handle = NULL;
  CHECK_HR(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dres));
  if (dres) { CHECK_HR(IDXGIResource1_CreateSharedHandle(dres, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle)); IDXGIResource1_Release(dres); }
  ID3D11Device1 *y1 = NULL;
  ID3D11Texture2D *opened = NULL;
  CHECK_HR(ID3D11Device_QueryInterface(y, &IID_ID3D11Device1, (void **)&y1));
  if (y1 && handle) CHECK_HR(ID3D11Device1_OpenSharedResource1(y1, handle, &IID_ID3D11Texture2D, (void **)&opened));
  if (opened) my = mutex_of(opened);
  if (!mx || !my) return;

  /* The key says who has it: 0 first, then whatever each release names. */
  CHECK(IDXGIKeyedMutex_AcquireSync(mx, 0, 0) == S_OK);
  CHECK(IDXGIKeyedMutex_AcquireSync(my, 0, 0) == WAIT_TIMEOUT);
  CHECK(IDXGIKeyedMutex_AcquireSync(mx, 0, 0) == DXGI_ERROR_INVALID_CALL);      /* (held by it already) */
  CHECK(IDXGIKeyedMutex_ReleaseSync(my, 1) == DXGI_ERROR_INVALID_CALL);         /* (not its holder) */
  CHECK(IDXGIKeyedMutex_ReleaseSync(mx, 1) == S_OK);
  CHECK(IDXGIKeyedMutex_AcquireSync(mx, 0, 20) == WAIT_TIMEOUT);                /* (free, but for key 1) */
  CHECK(IDXGIKeyedMutex_AcquireSync(my, 1, 1000) == S_OK);
  CHECK(texel_of(y, cy, opened) == 0xFF0000FFu);      /* (what the first device drew before its release: red) */
  /* the second device draws, and gives it back with key 2 */
  ID3D11RenderTargetView *rtv = NULL;
  CHECK_HR(ID3D11Device_CreateRenderTargetView(y, (ID3D11Resource *)opened, NULL, &rtv));
  if (rtv) { ID3D11DeviceContext_ClearRenderTargetView(cy, rtv, blue); ID3D11RenderTargetView_Release(rtv); }
  CHECK(IDXGIKeyedMutex_ReleaseSync(my, 2) == S_OK);
  CHECK(IDXGIKeyedMutex_AcquireSync(mx, 2, INFINITE) == S_OK);
  CHECK(texel_of(x, cx, tex) == 0xFFFF0000u);         /* (blue, which the release sent before it let go) */

  /* A waiter on another thread goes on when the holder releases with its key. */
  Waiter w = {my, 5, E_FAIL};
  HANDLE thread = CreateThread(NULL, 0, wait_for_key, &w, 0, NULL);
  CHECK(thread != NULL);
  if (thread) {
    CHECK(WaitForSingleObject(thread, 100) == WAIT_TIMEOUT);      /* (still waiting) */
    CHECK(IDXGIKeyedMutex_ReleaseSync(mx, 5) == S_OK);
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CHECK(w.result == S_OK);
    CloseHandle(thread);
  }
  /* The holder lets its texture go without releasing: the mutex is abandoned. */
  IDXGIKeyedMutex_Release(my);
  ID3D11Texture2D_Release(opened);
  CHECK(IDXGIKeyedMutex_AcquireSync(mx, 5, 0) == WAIT_ABANDONED);
  IDXGIKeyedMutex_Release(mx);
  if (handle) CloseHandle(handle);
  if (y1) ID3D11Device1_Release(y1);
  ID3D11Texture2D_Release(tex);
  ID3D11DeviceContext_Release(cx);
  ID3D11DeviceContext_Release(cy);
  ID3D11Device_Release(x);
  ID3D11Device_Release(y);
}

/* ---- legacy shared handles (IDXGIResource::GetSharedHandle, ID3D11Device::OpenSharedResource) ------------------------------------------------------------------------ */

/* A texture of `color` made with `misc` (SHARED or SHARED_KEYEDMUTEX, without NTHANDLE), cleared on `dev`. */
static ID3D11Texture2D *legacy_texture(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT misc, const float color[4]) {
  D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, misc};
  ID3D11Texture2D *tex = NULL;
  CHECK_HR(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex));
  if (!tex) return NULL;
  ID3D11RenderTargetView *rtv = NULL;
  CHECK_HR(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv));
  if (rtv) { ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color); ID3D11RenderTargetView_Release(rtv); }
  ID3D11DeviceContext_Flush(ctx);
  return tex;
}
static HANDLE legacy_handle(void *texture) {
  IDXGIResource *r = NULL;
  HANDLE h = NULL;
  CHECK_HR(IUnknown_QueryInterface((IUnknown *)texture, &IID_IDXGIResource, (void **)&r));
  if (r) { CHECK_HR(IDXGIResource_GetSharedHandle(r, &h)); IDXGIResource_Release(r); }
  return h;
}

static void test_legacy(D12 *a) {
  ID3D11Device *x = NULL, *y = NULL;
  ID3D11DeviceContext *cx = NULL, *cy = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &x, NULL, &cx));
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &y, NULL, &cy));
  if (!x || !y) return;
  const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
  ID3D11Texture2D *tex = legacy_texture(x, cx, D3D11_RESOURCE_MISC_SHARED, red);
  if (!tex) return;
  const HANDLE handle = legacy_handle(tex);
  CHECK(handle != NULL);
  /* (a shared texture must be in GPU memory; one made for NT handles has no legacy handle, and the other way round) */
  D3D11_TEXTURE2D_DESC staging = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, D3D11_RESOURCE_MISC_SHARED};
  ID3D11Texture2D *bad = NULL;
  CHECK(ID3D11Device_CreateTexture2D(x, &staging, NULL, &bad) == E_INVALIDARG && !bad);
  IDXGIResource1 *r1 = NULL;
  HANDLE none = NULL;
  CHECK_HR(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1));
  if (r1) { CHECK(IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &none) == E_INVALIDARG && !none); IDXGIResource1_Release(r1); }
  D3D11_TEXTURE2D_DESC nt = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ID3D11Texture2D *nt_tex = NULL;
  HANDLE nt_handle = NULL;
  CHECK_HR(ID3D11Device_CreateTexture2D(x, &nt, NULL, &nt_tex));
  if (nt_tex) {
    IDXGIResource1 *nr = NULL;
    CHECK_HR(ID3D11Texture2D_QueryInterface(nt_tex, &IID_IDXGIResource1, (void **)&nr));
    if (nr) {
      CHECK(IDXGIResource1_GetSharedHandle(nr, &none) == DXGI_ERROR_INVALID_CALL && !none);
      CHECK_HR(IDXGIResource1_CreateSharedHandle(nr, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &nt_handle));
      IDXGIResource1_Release(nr);
    }
  }

  /* The other device opens it by that handle: the same texture, which knows the same handle. */
  ID3D11Texture2D *opened = NULL, *wrong = NULL;
  CHECK_HR(ID3D11Device_OpenSharedResource(y, handle, &IID_ID3D11Texture2D, (void **)&opened));
  if (opened) {
    D3D11_TEXTURE2D_DESC got;
    ID3D11Texture2D_GetDesc(opened, &got);
    CHECK(got.Width == 4 && got.MiscFlags == D3D11_RESOURCE_MISC_SHARED && got.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET));
    CHECK(texel_of(y, cy, opened) == 0xFF0000FFu);
    CHECK(legacy_handle(opened) == handle);
    /* (what the second device draws, the first sees) */
    ID3D11RenderTargetView *rtv = NULL;
    CHECK_HR(ID3D11Device_CreateRenderTargetView(y, (ID3D11Resource *)opened, NULL, &rtv));
    if (rtv) { ID3D11DeviceContext_ClearRenderTargetView(cy, rtv, blue); ID3D11RenderTargetView_Release(rtv); }
    ID3D11DeviceContext_Flush(cy);
    CHECK(texel_of(x, cx, tex) == 0xFFFF0000u);
  }
  ID3D11Device1 *y1 = NULL;
  CHECK_HR(ID3D11Device_QueryInterface(y, &IID_ID3D11Device1, (void **)&y1));
  if (y1) { CHECK(ID3D11Device1_OpenSharedResource1(y1, handle, &IID_ID3D11Texture2D, (void **)&wrong) == E_INVALIDARG && !wrong); ID3D11Device1_Release(y1); }      /* (NT handles only) */
  if (nt_handle) CHECK(ID3D11Device_OpenSharedResource(y, nt_handle, &IID_ID3D11Texture2D, (void **)&wrong) == E_INVALIDARG && !wrong);      /* (legacy handles only) */
  CHECK(ID3D11Device_OpenSharedResource(y, (HANDLE)(UINT_PTR)0x7FFFFFF0u, &IID_ID3D11Texture2D, (void **)&wrong) == E_INVALIDARG && !wrong);
  /* Direct3D 12 opens the legacy handle too */
  ID3D12Resource *as12 = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandle(a->dev, handle, &IID_ID3D12Resource, (void **)&as12));
  if (as12) {
    const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(as12);
    CHECK(rd.Width == 4 && rd.Height == 4);
    ID3D12Resource_Release(as12);
  }

  /* A legacy share with a keyed mutex: the mutex comes with the texture. */
  ID3D11Texture2D *ktex = legacy_texture(x, cx, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, red), *kopened = NULL;
  HANDLE kh = ktex ? legacy_handle(ktex) : NULL;
  if (kh) CHECK_HR(ID3D11Device_OpenSharedResource(y, kh, &IID_ID3D11Texture2D, (void **)&kopened));
  IDXGIKeyedMutex *kx = ktex ? mutex_of(ktex) : NULL, *ky = kopened ? mutex_of(kopened) : NULL;
  if (kx && ky) {
    CHECK(IDXGIKeyedMutex_AcquireSync(kx, 0, 0) == S_OK);
    CHECK(IDXGIKeyedMutex_AcquireSync(ky, 0, 0) == WAIT_TIMEOUT);
    CHECK(IDXGIKeyedMutex_ReleaseSync(kx, 3) == S_OK);
    CHECK(IDXGIKeyedMutex_AcquireSync(ky, 3, 1000) == S_OK);
    CHECK(texel_of(y, cy, kopened) == 0xFF0000FFu);
    CHECK(IDXGIKeyedMutex_ReleaseSync(ky, 0) == S_OK);
  }
  if (kx) IDXGIKeyedMutex_Release(kx);
  if (ky) IDXGIKeyedMutex_Release(ky);
  if (kopened) ID3D11Texture2D_Release(kopened);
  if (ktex) ID3D11Texture2D_Release(ktex);
  if (nt_handle) CloseHandle(nt_handle);
  if (nt_tex) ID3D11Texture2D_Release(nt_tex);
  if (opened) ID3D11Texture2D_Release(opened);
  ID3D11Texture2D_Release(tex);
  ID3D11DeviceContext_Release(cx);
  ID3D11DeviceContext_Release(cy);
  ID3D11Device_Release(x);
  ID3D11Device_Release(y);
}

/* ---- two processes ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* ---- NV12 textures: every plane of every slice goes with the share ------------------------------------------------------------------------------------------- */

#define NV12_W 16
#define NV12_H 8
/* The byte of plane `plane` (0 the luma, 1 the interleaved chroma) of a slice at (x, y) - x counts bytes - of pattern `seed`. */
static BYTE nv12_byte(UINT seed, UINT slice, UINT plane, UINT x, UINT y) { return (BYTE)(seed * 37u + slice * 101u + plane * 53u + x * 7u + y * 13u); }
/* Pattern `seed` into every plane of every slice of an NV12 texture of Direct3D 12, through an upload buffer laid out as GetCopyableFootprints says. */
static void d12_write_nv12(D12 *d, ID3D12Resource *tex, UINT layers, UINT seed) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[8];
  UINT rows[8];
  UINT64 row_bytes[8], total = 0;
  ID3D12Device_GetCopyableFootprints(d->dev, &rd, 0, 2 * layers, 0, fp, rows, row_bytes, &total);
  ID3D12Resource *up = buffer(d->dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  BYTE *p = NULL;
  if (!up || FAILED(ID3D12Resource_Map(up, 0, NULL, (void **)&p)) || !p) { CHECK(0); if (up) ID3D12Resource_Release(up); return; }
  for (UINT sub = 0; sub < 2 * layers; ++sub)
    for (UINT y = 0; y < rows[sub]; ++y)
      for (UINT x = 0; x < row_bytes[sub]; ++x) p[fp[sub].Offset + (UINT64)y * fp[sub].Footprint.RowPitch + x] = nv12_byte(seed, sub % layers, sub / layers, x, y);
  ID3D12Resource_Unmap(up, 0, NULL);
  for (UINT sub = 0; sub < 2 * layers; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION to = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}}, from = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}};
    to.SubresourceIndex = sub;
    from.PlacedFootprint = fp[sub];
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  }
  d12_run(d);
  ID3D12Resource_Release(up);
}
/* Whether every plane of every slice of an NV12 texture of Direct3D 12 holds pattern `seed` (slice `except` - when below `layers` - pattern `except_seed`). */
static int d12_nv12_is(D12 *d, ID3D12Resource *tex, UINT layers, UINT seed, UINT except, UINT except_seed) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[8];
  UINT rows[8];
  UINT64 row_bytes[8], total = 0;
  ID3D12Device_GetCopyableFootprints(d->dev, &rd, 0, 2 * layers, 0, fp, rows, row_bytes, &total);
  ID3D12Resource *down = buffer(d->dev, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE, total, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!down) return 0;
  for (UINT sub = 0; sub < 2 * layers; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION to = {down, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {0}}, from = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    to.PlacedFootprint = fp[sub];
    from.SubresourceIndex = sub;
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  }
  d12_run(d);
  const BYTE *p = NULL;
  int same = 1;
  if (FAILED(ID3D12Resource_Map(down, 0, NULL, (void **)&p)) || !p) same = 0;
  for (UINT sub = 0; same && sub < 2 * layers; ++sub)
    for (UINT y = 0; same && y < rows[sub]; ++y)
      for (UINT x = 0; same && x < row_bytes[sub]; ++x) {
        const UINT slice = sub % layers, plane = sub / layers;
        const BYTE want = nv12_byte(slice == except ? except_seed : seed, slice, plane, x, y), got = p[fp[sub].Offset + (UINT64)y * fp[sub].Footprint.RowPitch + x];
        if (got != want) { printf("  NV12 slice %u plane %u (%u, %u): %u, want %u\n", slice, plane, x, y, got, want); same = 0; }
      }
  if (p) ID3D12Resource_Unmap(down, 0, NULL);
  ID3D12Resource_Release(down);
  return same;
}
/* The same through Direct3D 11: each slice copied into a staging NV12 texture and mapped (the luma rows, then the chroma rows). */
static int d11_nv12_is(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, UINT layers, UINT seed, UINT except, UINT except_seed) {
  D3D11_TEXTURE2D_DESC sd = {NV12_W, NV12_H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  ID3D11Texture2D *staging = NULL;
  if (FAILED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging)) || !staging) return 0;
  int same = 1;
  for (UINT slice = 0; same && slice < layers; ++slice) {
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)tex, slice, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) { same = 0; break; }
    for (UINT y = 0; same && y < NV12_H * 3 / 2; ++y)
      for (UINT x = 0; same && x < NV12_W; ++x) {
        const UINT plane = y >= NV12_H, row = plane ? y - NV12_H : y;
        const BYTE want = nv12_byte(slice == except ? except_seed : seed, slice, plane, x, row), got = ((const BYTE *)m.pData)[(SIZE_T)y * m.RowPitch + x];
        if (got != want) { printf("  NV12 (Direct3D 11) slice %u plane %u (%u, %u): %u, want %u\n", slice, plane, x, row, got, want); same = 0; }
      }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
  }
  ID3D11Texture2D_Release(staging);
  return same;
}
/* Pattern `seed` into slice `slice` of a Direct3D 11 NV12 texture, by UpdateSubresource of the slice (its luma rows, then its chroma rows). */
static void d11_write_nv12_slice(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, UINT slice, UINT seed) {
  BYTE data[NV12_W * NV12_H * 3 / 2];
  for (UINT y = 0; y < NV12_H * 3 / 2; ++y)
    for (UINT x = 0; x < NV12_W; ++x) data[y * NV12_W + x] = nv12_byte(seed, slice, y >= NV12_H, x, y >= NV12_H ? y - NV12_H : y);
  ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, slice, NULL, data, NV12_W, 0);
  ID3D11DeviceContext_Flush(ctx);
}
static ID3D11Texture2D *nv12_texture(ID3D11Device *dev, UINT layers, UINT misc) {
  D3D11_TEXTURE2D_DESC td = {NV12_W, NV12_H, 1, layers, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, misc};
  ID3D11Texture2D *tex = NULL;
  CHECK_HR(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex));
  return tex;
}

static void test_nv12(D12 *a, D12 *b) {
  ID3D11Device *x = NULL, *y = NULL;
  ID3D11DeviceContext *cx = NULL, *cy = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &x, NULL, &cx));
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &y, NULL, &cy));
  if (!x || !y) return;
  ID3D11Device1 *y1 = NULL;
  CHECK_HR(ID3D11Device_QueryInterface(y, &IID_ID3D11Device1, (void **)&y1));
  /* Direct3D 11's NV12 array (NT handle) opened by Direct3D 12: the description, then what each writes the other reads, plane by plane and slice by slice */
  ID3D11Texture2D *tex = nv12_texture(x, 2, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
  HANDLE handle = NULL;
  if (tex) {
    d11_write_nv12_slice(cx, tex, 0, 3);
    d11_write_nv12_slice(cx, tex, 1, 3);
    IDXGIResource1 *dres = NULL;
    CHECK_HR(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dres));
    if (dres) { CHECK_HR(IDXGIResource1_CreateSharedHandle(dres, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, L"TcosShareTestNV12", &handle)); IDXGIResource1_Release(dres); }
  }
  ID3D12Resource *as12 = NULL;
  if (handle) CHECK_HR(ID3D12Device_OpenSharedHandle(a->dev, handle, &IID_ID3D12Resource, (void **)&as12));
  if (as12) {
    const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(as12);
    CHECK(rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width == NV12_W && rd.Height == NV12_H && rd.DepthOrArraySize == 2 && rd.Format == DXGI_FORMAT_NV12 &&
          rd.MipLevels == 1);
    CHECK(d12_nv12_is(a, as12, 2, 3, 2, 0));                  /* (Direct3D 11's bytes in both planes of both slices) */
    d12_write_nv12(a, as12, 2, 4);
    CHECK(d11_nv12_is(x, cx, tex, 2, 4, 2, 0));               /* (and Direct3D 12's in Direct3D 11's texture) */
  }
  /* ... by another Direct3D 11 device (by handle and by name), which writes one slice */
  ID3D11Texture2D *opened = NULL, *by_name = NULL;
  if (y1 && handle) CHECK_HR(ID3D11Device1_OpenSharedResource1(y1, handle, &IID_ID3D11Texture2D, (void **)&opened));
  if (y1) CHECK_HR(ID3D11Device1_OpenSharedResourceByName(y1, L"TcosShareTestNV12", DXGI_SHARED_RESOURCE_READ, &IID_ID3D11Texture2D, (void **)&by_name));
  if (opened) {
    D3D11_TEXTURE2D_DESC got;
    ID3D11Texture2D_GetDesc(opened, &got);
    CHECK(got.Format == DXGI_FORMAT_NV12 && got.ArraySize == 2 && got.Width == NV12_W && got.MiscFlags == (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE));
    CHECK(d11_nv12_is(y, cy, opened, 2, 4, 2, 0));
    d11_write_nv12_slice(cy, opened, 1, 5);
    if (as12) CHECK(d12_nv12_is(a, as12, 2, 4, 1, 5));      /* (slice 1 is the second device's, slice 0 still Direct3D 12's) */
    CHECK(d11_nv12_is(x, cx, tex, 2, 4, 1, 5));
  }
  if (by_name) { CHECK(d11_nv12_is(y, cy, by_name, 2, 4, 1, 5)); ID3D11Texture2D_Release(by_name); }
  /* Direct3D 12's NV12 texture (HEAP_FLAG_SHARED) opened by Direct3D 11 and by a second Direct3D 12 device */
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, NV12_W, NV12_H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *made12 = NULL, *made12_b = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(a->dev, &hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&made12));
  HANDLE h12 = NULL;
  if (made12) {
    d12_write_nv12(a, made12, 1, 6);
    CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)made12, NULL, GENERIC_ALL, NULL, &h12));
  }
  if (h12) {
    CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, h12, &IID_ID3D12Resource, (void **)&made12_b));
    if (made12_b) { CHECK(d12_nv12_is(b, made12_b, 1, 6, 1, 0)); ID3D12Resource_Release(made12_b); }
    ID3D11Texture2D *as11 = NULL;
    if (y1) CHECK_HR(ID3D11Device1_OpenSharedResource1(y1, h12, &IID_ID3D11Texture2D, (void **)&as11));
    if (as11) {
      D3D11_TEXTURE2D_DESC got;
      ID3D11Texture2D_GetDesc(as11, &got);
      CHECK(got.Format == DXGI_FORMAT_NV12 && got.ArraySize == 1 && got.Width == NV12_W && got.Height == NV12_H);
      CHECK(d11_nv12_is(y, cy, as11, 1, 6, 1, 0));
      ID3D11Texture2D_Release(as11);
    }
    CloseHandle(h12);
  }
  /* A legacy (global) share of an NV12 texture, opened by Direct3D 11 and by Direct3D 12 */
  ID3D11Texture2D *legacy = nv12_texture(x, 1, D3D11_RESOURCE_MISC_SHARED);
  if (legacy) {
    d11_write_nv12_slice(cx, legacy, 0, 7);
    HANDLE hl = legacy_handle(legacy);
    ID3D11Texture2D *legacy_y = NULL;
    ID3D12Resource *legacy12 = NULL;
    if (hl) CHECK_HR(ID3D11Device_OpenSharedResource(y, hl, &IID_ID3D11Texture2D, (void **)&legacy_y));
    if (legacy_y) { CHECK(d11_nv12_is(y, cy, legacy_y, 1, 7, 1, 0)); ID3D11Texture2D_Release(legacy_y); }
    if (hl) CHECK_HR(ID3D12Device_OpenSharedHandle(b->dev, hl, &IID_ID3D12Resource, (void **)&legacy12));
    if (legacy12) { CHECK(d12_nv12_is(b, legacy12, 1, 7, 1, 0)); ID3D12Resource_Release(legacy12); }
    ID3D11Texture2D_Release(legacy);
  }
  /* ... and one with a keyed mutex (SHARED_KEYEDMUTEX, a legacy handle): the opener's mutex is the creator's */
  ID3D11Texture2D *keyed = nv12_texture(x, 1, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
  if (keyed) {
    IDXGIKeyedMutex *km = mutex_of(keyed), *km_y = NULL;
    HANDLE hk = legacy_handle(keyed);
    ID3D11Texture2D *keyed_y = NULL;
    if (km) { CHECK(IDXGIKeyedMutex_AcquireSync(km, 0, 0) == S_OK); }
    d11_write_nv12_slice(cx, keyed, 0, 8);
    if (km) CHECK(IDXGIKeyedMutex_ReleaseSync(km, 1) == S_OK);
    if (hk) CHECK_HR(ID3D11Device_OpenSharedResource(y, hk, &IID_ID3D11Texture2D, (void **)&keyed_y));
    if (keyed_y) km_y = mutex_of(keyed_y);
    if (km_y) {
      CHECK(IDXGIKeyedMutex_AcquireSync(km_y, 1, 10000) == S_OK);
      CHECK(d11_nv12_is(y, cy, keyed_y, 1, 8, 1, 0));
      CHECK(IDXGIKeyedMutex_ReleaseSync(km_y, 0) == S_OK);
      IDXGIKeyedMutex_Release(km_y);
    }
    if (keyed_y) ID3D11Texture2D_Release(keyed_y);
    if (km) IDXGIKeyedMutex_Release(km);
    ID3D11Texture2D_Release(keyed);
  }
  if (made12) ID3D12Resource_Release(made12);
  if (opened) ID3D11Texture2D_Release(opened);
  if (as12) ID3D12Resource_Release(as12);
  if (handle) CloseHandle(handle);
  if (tex) ID3D11Texture2D_Release(tex);
  if (y1) ID3D11Device1_Release(y1);
  ID3D11DeviceContext_Release(cx);
  ID3D11DeviceContext_Release(cy);
  ID3D11Device_Release(x);
  ID3D11Device_Release(y);
}

/* The parent shares a buffer and a fence, starts this program again with the two handles inherited, and waits for the fence: the child checks the buffer holds the
 * parent's words, writes its own after them and signals the fence with 2; the parent then reads the child's words. A texture with a keyed mutex goes along: the
 * parent draws it red and releases it with key 1, the child acquires it with 1, sees red, draws it green and releases it with 2, which the parent then acquires. */
static void child_keyed(HANDLE texture_handle) {
  ID3D11Device *dev = NULL;
  ID3D11DeviceContext *ctx = NULL;
  ID3D11Device1 *dev1 = NULL;
  ID3D11Texture2D *tex = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx));
  if (!dev) return;
  CHECK_HR(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1));
  if (dev1) CHECK_HR(ID3D11Device1_OpenSharedResource1(dev1, texture_handle, &IID_ID3D11Texture2D, (void **)&tex));
  IDXGIKeyedMutex *m = tex ? mutex_of(tex) : NULL;
  if (m) {
    CHECK(IDXGIKeyedMutex_AcquireSync(m, 1, 60000) == S_OK);
    CHECK(texel_of(dev, ctx, tex) == 0xFF0000FFu);
    const float green[4] = {0, 1, 0, 1};
    ID3D11RenderTargetView *rtv = NULL;
    CHECK_HR(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv));
    if (rtv) { ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green); ID3D11RenderTargetView_Release(rtv); }
    CHECK(IDXGIKeyedMutex_ReleaseSync(m, 2) == S_OK);
    IDXGIKeyedMutex_Release(m);
  }
  if (tex) ID3D11Texture2D_Release(tex);
  if (dev1) ID3D11Device1_Release(dev1);
  ID3D11DeviceContext_Release(ctx);
  ID3D11Device_Release(dev);
}
/* The parent's legacy shared texture (a global share handle: the same number in every process), drawn yellow. */
static void child_legacy(HANDLE legacy) {
  ID3D11Device *dev = NULL;
  ID3D11DeviceContext *ctx = NULL;
  ID3D11Texture2D *tex = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx));
  if (!dev) return;
  CHECK_HR(ID3D11Device_OpenSharedResource(dev, legacy, &IID_ID3D11Texture2D, (void **)&tex));
  if (tex) { CHECK(texel_of(dev, ctx, tex) == 0xFF00FFFFu); ID3D11Texture2D_Release(tex); }
  ID3D11DeviceContext_Release(ctx);
  ID3D11Device_Release(dev);
}
/* The parent's Direct3D 12 NV12 array of two slices (pattern 9): the child sees it through Direct3D 12, then writes slice 1 (pattern 10) through Direct3D 11. */
static void child_nv12(D12 *c, HANDLE nv12) {
  ID3D12Resource *tex = NULL;
  CHECK_HR(ID3D12Device_OpenSharedHandle(c->dev, nv12, &IID_ID3D12Resource, (void **)&tex));
  if (tex) { CHECK(d12_nv12_is(c, tex, 2, 9, 2, 0)); ID3D12Resource_Release(tex); }
  ID3D11Device *dev = NULL;
  ID3D11DeviceContext *ctx = NULL;
  ID3D11Device1 *dev1 = NULL;
  ID3D11Texture2D *t11 = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx));
  if (!dev) return;
  CHECK_HR(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1));
  if (dev1) CHECK_HR(ID3D11Device1_OpenSharedResource1(dev1, nv12, &IID_ID3D11Texture2D, (void **)&t11));
  if (t11) {
    CHECK(d11_nv12_is(dev, ctx, t11, 2, 9, 2, 0));
    d11_write_nv12_slice(ctx, t11, 1, 10);
    ID3D11DeviceContext_Flush(ctx);
    CHECK(d11_nv12_is(dev, ctx, t11, 2, 9, 1, 10));
    ID3D11Texture2D_Release(t11);
  }
  if (dev1) ID3D11Device1_Release(dev1);
  ID3D11DeviceContext_Release(ctx);
  ID3D11Device_Release(dev);
}
/* The parent's Direct3D 11 fence: the child's context waits for 1 (the parent's signal) and then signals 2, which the parent waits for. */
static void child_fence11(HANDLE fence_handle) {
  ID3D11DeviceContext4 *ctx = NULL;
  ID3D11Device5 *dev = device5(&ctx);
  ID3D11Fence *fence = NULL;
  if (dev) CHECK_HR(ID3D11Device5_OpenSharedFence(dev, fence_handle, &IID_ID3D11Fence, (void **)&fence));
  if (fence && ctx) {
    CHECK_HR(ID3D11DeviceContext4_Wait(ctx, fence, 1));
    CHECK_HR(ID3D11DeviceContext4_Signal(ctx, fence, 2));
    /* (the wait is the GPU's: the CPU goes on, and the signal behind it is done once the parent signals 1 - the child stays until then) */
    HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK_HR(ID3D11Fence_SetEventOnCompletion(fence, 2, done));
    CHECK(WaitForSingleObject(done, 60000) == WAIT_OBJECT_0);
    CHECK(ID3D11Fence_GetCompletedValue(fence) == 2);
    CloseHandle(done);
  }
  if (fence) ID3D11Fence_Release(fence);
  if (ctx) ID3D11DeviceContext4_Release(ctx);
  if (dev) ID3D11Device5_Release(dev);
}
static int child(HANDLE buffer_handle, HANDLE fence_handle, HANDLE texture_handle, HANDLE legacy, HANDLE fence11, HANDLE nv12) {
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
  if (nv12) child_nv12(&c, nv12);
  d12_free(&c);
  if (texture_handle) child_keyed(texture_handle);
  if (legacy) child_legacy(legacy);
  if (fence11) child_fence11(fence11);
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
  /* (the texture with a keyed mutex: drawn red, released with key 1 for the child) */
  ID3D11Device *x = NULL;
  ID3D11DeviceContext *cx = NULL;
  ID3D11Texture2D *tex = NULL;
  IDXGIKeyedMutex *km = NULL;
  HANDLE ht = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &x, NULL, &cx));
  if (x) {
    const float red[4] = {1, 0, 0, 1};
    tex = km_texture(x, cx, red);
    if (tex) km = mutex_of(tex);
    IDXGIResource1 *dres = NULL;
    if (tex) CHECK_HR(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dres));
    if (dres) { CHECK_HR(IDXGIResource1_CreateSharedHandle(dres, &inherit, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &ht)); IDXGIResource1_Release(dres); }
    if (km) { CHECK(IDXGIKeyedMutex_AcquireSync(km, 0, 0) == S_OK); CHECK(IDXGIKeyedMutex_ReleaseSync(km, 1) == S_OK); }
  }
  /* (and a legacy shared texture, yellow: its handle is a number every process opens) */
  ID3D11Texture2D *legacy_tex = NULL;
  HANDLE hl = NULL;
  if (x) {
    const float yellow[4] = {1, 1, 0, 1};
    legacy_tex = legacy_texture(x, cx, D3D11_RESOURCE_MISC_SHARED, yellow);
    if (legacy_tex) hl = legacy_handle(legacy_tex);
  }
  /* (and a Direct3D 11 fence: the child waits on it for 1, then signals 2) */
  ID3D11Device5 *x5 = NULL;
  ID3D11DeviceContext4 *cx4 = NULL;
  ID3D11Fence *f11 = NULL;
  HANDLE hf11 = NULL;
  if (x) { CHECK_HR(ID3D11Device_QueryInterface(x, &IID_ID3D11Device5, (void **)&x5)); CHECK_HR(ID3D11DeviceContext_QueryInterface(cx, &IID_ID3D11DeviceContext4, (void **)&cx4)); }
  if (x5) CHECK_HR(ID3D11Device5_CreateFence(x5, 0, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)&f11));
  if (f11) CHECK_HR(ID3D11Fence_CreateSharedHandle(f11, &inherit, GENERIC_ALL, NULL, &hf11));
  /* (and an NV12 array of Direct3D 12's, every plane of both slices: the child reads it through both APIs and writes slice 1 through Direct3D 11) */
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC nd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, NV12_W, NV12_H, 2, 1, DXGI_FORMAT_NV12, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource *nv12 = NULL;
  HANDLE hn = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(a->dev, &hp, D3D12_HEAP_FLAG_SHARED, &nd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&nv12));
  if (nv12) {
    d12_write_nv12(a, nv12, 2, 9);
    CHECK_HR(ID3D12Device_CreateSharedHandle(a->dev, (ID3D12DeviceChild *)nv12, &inherit, GENERIC_ALL, NULL, &hn));
  }
  WCHAR self[MAX_PATH], line[MAX_PATH + 160];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  swprintf(line, sizeof line / sizeof line[0], L"\"%ls\" child %llu %llu %llu %llu %llu %llu", self, (unsigned long long)(UINT_PTR)hb, (unsigned long long)(UINT_PTR)hf,
           (unsigned long long)(UINT_PTR)ht, (unsigned long long)(UINT_PTR)hl, (unsigned long long)(UINT_PTR)hf11, (unsigned long long)(UINT_PTR)hn);
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
    if (f11 && cx4) {
      Sleep(200);      /* (the child is waiting on the fence by now, most likely: the signal releases it) */
      CHECK_HR(ID3D11DeviceContext4_Signal(cx4, f11, 1));
      CHECK_HR(ID3D11Fence_SetEventOnCompletion(f11, 2, event));
      CHECK(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0);      /* (the child's signal, after its wait) */
      CHECK(ID3D11Fence_GetCompletedValue(f11) == 2);
    }
    CloseHandle(event);
    CHECK(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("child exit code %lu\n", (unsigned long)code);
    CHECK(code == 0);
    CHECK(d12_holds(a, shared, sizeof mine, theirs, sizeof theirs));      /* (what the child wrote) */
    if (nv12) CHECK(d12_nv12_is(a, nv12, 2, 9, 1, 10));                  /* (slice 1 the child's, through Direct3D 11) */
    if (km) {
      CHECK(IDXGIKeyedMutex_AcquireSync(km, 2, 60000) == S_OK);      /* (the child's release) */
      CHECK(texel_of(x, cx, tex) == 0xFF00FF00u);                    /* (green: what it drew) */
      CHECK(IDXGIKeyedMutex_ReleaseSync(km, 0) == S_OK);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  CloseHandle(hb);
  CloseHandle(hf);
  if (ht) CloseHandle(ht);
  if (hf11) CloseHandle(hf11);
  if (hn) CloseHandle(hn);
  if (nv12) ID3D12Resource_Release(nv12);
  if (f11) ID3D11Fence_Release(f11);
  if (cx4) ID3D11DeviceContext4_Release(cx4);
  if (x5) ID3D11Device5_Release(x5);
  if (km) IDXGIKeyedMutex_Release(km);
  if (legacy_tex) ID3D11Texture2D_Release(legacy_tex);
  if (tex) ID3D11Texture2D_Release(tex);
  if (cx) ID3D11DeviceContext_Release(cx);
  if (x) ID3D11Device_Release(x);
  ID3D12Resource_Release(shared);
  ID3D12Fence_Release(fence);
}

int main(int argc, char **argv) {
  if (argc == 8 && strcmp(argv[1], "child") == 0)
    return child((HANDLE)(UINT_PTR)_strtoui64(argv[2], NULL, 10), (HANDLE)(UINT_PTR)_strtoui64(argv[3], NULL, 10), (HANDLE)(UINT_PTR)_strtoui64(argv[4], NULL, 10),
                 (HANDLE)(UINT_PTR)_strtoui64(argv[5], NULL, 10), (HANDLE)(UINT_PTR)_strtoui64(argv[6], NULL, 10), (HANDLE)(UINT_PTR)_strtoui64(argv[7], NULL, 10));
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
  test_keyed_mutex();
  test_legacy(&a);
  test_d3d11_fences(&a);
  test_nv12(&a, &b);
  if (processes) test_processes(&a);
  d12_free(&a);
  d12_free(&b);
  printf("d3dsharetest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
