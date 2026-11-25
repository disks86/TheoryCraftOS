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

/* dxgi.dll and d3d12.dll end to end: the program uses the two DLLs as any Direct3D 12 program does, on the kernel's Direct3D (tests/emulated_dx.sh runs it on the CPU emulator with
 * the software adapter): adapters and outputs, the device, a triangle drawn with a root signature and pipeline state and read back, a compute shader writing buffers, fences and
 * events, copies, the root signature serializer, a swap chain. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS 1      /* (the methods that return structures: the wrappers hide the pointer to the result) */
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "../../src/Windows/d3d12/d3d12_types.h"      /* (what the later Agility SDKs add: root signature 1.2, Device11 .. 14, List8 .. 10) */
#include "dxfixtures.h"

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)
#define CHECK_HR(x) do { HRESULT hr_ = (x); ++checks; if (FAILED(hr_)) { ++failed; printf("FAILED line %d: %s = %08lx\n", __LINE__, #x, (unsigned long)hr_); } } while (0)

static int g_hardware;      /* the adapter is a GPU (not the CPU renderer) */
static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static ID3D12Resource *make_buffer(ID3D12Device *dev, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
  ID3D12Resource *r = NULL;
  HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
  if (FAILED(hr)) printf("buffer %08lx\n", (unsigned long)hr);
  return r;
}

static void wait_fence(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *value) {
  ++*value;
  CHECK_HR(ID3D12CommandQueue_Signal(queue, fence, *value));
  CHECK(ID3D12Fence_GetCompletedValue(fence) >= *value);
}

static void test_dxgi(IDXGIAdapter1 **adapter_out) {
  IDXGIFactory4 *factory = NULL;
  CHECK_HR(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory));
  if (!factory) return;
  IDXGIAdapter1 *adapter = NULL;
  CHECK_HR(IDXGIFactory4_EnumAdapters1(factory, 0, &adapter));
  CHECK(IDXGIFactory4_EnumAdapters1(factory, 15, &adapter) == DXGI_ERROR_NOT_FOUND || adapter != NULL);
  CHECK_HR(IDXGIFactory4_EnumAdapters1(factory, 0, &adapter));
  if (adapter) {
    DXGI_ADAPTER_DESC1 d;
    CHECK_HR(IDXGIAdapter1_GetDesc1(adapter, &d));
    CHECK(d.Description[0] != 0);
    CHECK(d.DedicatedVideoMemory > 0 || d.SharedSystemMemory > 0);
    printf("adapter: %ls (flags %x)\n", d.Description, (unsigned)d.Flags);
    printf("device: %s\n", (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "software (the CPU renderer)" : "hardware");
    g_hardware = !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
    IDXGIOutput *output = NULL;
    CHECK_HR(IDXGIAdapter1_EnumOutputs(adapter, 0, &output));
    if (output) {
      DXGI_OUTPUT_DESC od;
      CHECK_HR(IDXGIOutput_GetDesc(output, &od));
      CHECK(od.DesktopCoordinates.right > 0 && od.DesktopCoordinates.bottom > 0);
      UINT modes = 0;
      CHECK_HR(IDXGIOutput_GetDisplayModeList(output, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &modes, NULL));
      CHECK(modes > 0);
      IDXGIOutput_Release(output);
    }
    CHECK(IDXGIAdapter1_EnumOutputs(adapter, 5, &output) == DXGI_ERROR_NOT_FOUND);
    IDXGIAdapter3 *a3 = NULL;
    CHECK_HR(IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter3, (void **)&a3));
    if (a3) {
      DXGI_QUERY_VIDEO_MEMORY_INFO info;
      CHECK_HR(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
      IDXGIAdapter3_Release(a3);
    }
    *adapter_out = adapter;
  }
  IDXGIFactory6 *f6 = NULL;
  CHECK_HR(IDXGIFactory4_QueryInterface(factory, &IID_IDXGIFactory6, (void **)&f6));
  if (f6) {
    IDXGIAdapter1 *pref = NULL;
    CHECK_HR(IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&pref));
    if (pref) IDXGIAdapter1_Release(pref);
    IDXGIFactory6_Release(f6);
  }
  IDXGIFactory4_Release(factory);
}

static void test_triangle(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  /* the render target, a readback buffer and the vertices */
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource *target = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&target));
  ID3D12Resource *readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 64 * 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  CHECK(target && readback && vb);
  if (!target || !readback || !vb) return;
  const float verts[3][7] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  CHECK(mapped != NULL);
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(target);
  CHECK(rd.Width == 64 && rd.Height == 64 && rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
  CHECK(ID3D12Resource_GetGPUVirtualAddress(vb) != 0);

  /* the render target view */
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  CHECK(rtv.ptr != 0);
  ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);

  /* the root signature: 16 root constants for the vertex shader's matrix */
  D3D12_ROOT_PARAMETER param;
  memset(&param, 0, sizeof param);
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  param.Constants.Num32BitValues = 16;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));

  /* the pipeline state */
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

  /* the commands */
  ID3D12CommandAllocator *allocator = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list || !pso || !rs) return;
  const float clear[4] = {0, 0, 0, 1};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, 28};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER barrier;
  memset(&barrier, 0, sizeof barrier);
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = target;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION dst, src;
  memset(&dst, 0, sizeof dst);
  memset(&src, 0, sizeof src);
  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = 64;
  dst.PlacedFootprint.Footprint.Height = 64;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = 256;
  src.pResource = target;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);

  const UINT32 *pixels = NULL;
  CHECK_HR(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
  if (pixels) {
    int coloured = 0, black = 0;
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) {
        const UINT32 p = pixels[y * 64 + x];
        if ((p & 0xFFFFFF) == 0) ++black;
        else ++coloured;
      }
    printf("triangle: %d coloured, %d black pixels, corner %08x, centre %08x\n", coloured, black, (unsigned)pixels[0], (unsigned)pixels[32 * 64 + 32]);
    CHECK(pixels[0] == 0xFF000000u);         /* the clear colour */
    CHECK(coloured > 600 && coloured < 2000);
    CHECK((pixels[32 * 64 + 32] & 0xFFFFFF) != 0);
    /* (the apex is at the top: a row near it has fewer coloured pixels than one near the base) */
    int top_row = 0, base_row = 0;
    for (int x = 0; x < 64; ++x) {
      top_row += (pixels[10 * 64 + x] & 0xFFFFFF) != 0;
      base_row += (pixels[54 * 64 + x] & 0xFFFFFF) != 0;
    }
    printf("triangle: row 10 has %d coloured pixels, row 54 has %d\n", top_row, base_row);
    CHECK(top_row > 0 && top_row < base_row);
    ID3D12Resource_Unmap(readback, 0, NULL);
  }
  /* the same list again with a different clear colour: lists can be reset and recorded again */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, allocator, pso));
  const float green[4] = {0, 1, 0, 1};
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, green, 0, NULL);
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  CHECK_HR(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
  if (pixels) {
    CHECK(pixels[0] == 0xFF00FF00u);
    CHECK(pixels[63 * 64 + 63] == 0xFF00FF00u);
    ID3D12Resource_Unmap(readback, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D10Blob_Release(blob);
  ID3D12DescriptorHeap_Release(rtv_heap);
  ID3D12Resource_Release(vb);
  ID3D12Resource_Release(readback);
  ID3D12Resource_Release(target);
}


/* The textured quad of test_texture with texture coordinates from `lo` to `hi` and a static sampler of the given address mode; `row` gets the 32 pixels of row 18 of the
 * target (v about 0.73 for -1 .. 2: the texture's second row, blue then white) when it is not NULL. With `mips` the texture is 4 x 4 with three levels, each one shade of
 * blue (255, 128, 64), and the sampler picks the nearest level. */
static void texture_scene(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value, D3D12_TEXTURE_ADDRESS_MODE address, D3D12_STATIC_BORDER_COLOR border,
                          float lo, float hi, int mips, UINT32 *row) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rt_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_RESOURCE_DESC tex_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, mips ? 4 : 2, mips ? 4 : 2, 1, mips ? 3 : 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *target = NULL, *texture = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&target));
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&texture));
  ID3D12Resource *upload = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 32 * 128, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!target || !texture || !upload || !readback || !vb) { CHECK(0); return; }
  /* the texels: red, green / blue, white; the vertices of two triangles, colour (u/2, v/2, 1, 1) so that the vertex shader's texture coordinate is (u, v) */
  UINT8 *p = NULL;
  CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void **)&p));
  const UINT32 texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};      /* (R8G8B8A8: 0xAABBGGRR) */
  if (mips) {
    /* (level n at byte 1024 * n of the buffer, rows 256 bytes apart) */
    static const UINT32 shades[3] = {0xFFFF0000u, 0xFF800000u, 0xFF400000u};
    for (int level = 0; level < 3; ++level)
      for (int y = 0; y < (4 >> level); ++y)
        for (int x = 0; x < (4 >> level); ++x) memcpy(p + 1024 * level + 256 * y + 4 * x, &shades[level], 4);
  } else {
    memcpy(p, &texels[0], 8);
    memcpy(p + 256, &texels[2], 8);
  }
  ID3D12Resource_Unmap(upload, 0, NULL);
  const float uv[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
  float verts[6][7];
  for (int i = 0; i < 6; ++i) {
    verts[i][0] = uv[i][0] * 2 - 1; verts[i][1] = 1 - uv[i][1] * 2; verts[i][2] = 0.5f;
    verts[i][3] = (lo + uv[i][0] * (hi - lo)) / 2; verts[i][4] = (lo + uv[i][1] * (hi - lo)) / 2; verts[i][5] = 1; verts[i][6] = 1;
  }
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, (void **)&p));
  memcpy(p, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtv_heap = NULL, *srv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 4;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap);
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap);
  CHECK(srv_gpu.ptr != 0);
  const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.ptr += inc;      /* (descriptor 1: the table starts in the middle of the heap) */
  srv_gpu.ptr += inc;
  ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv;
  memset(&sv, 0, sizeof sv);
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = mips ? 3 : 1;
  ID3D12Device_CreateShaderResourceView(dev, texture, &sv, srv);

  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 16;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_STATIC_SAMPLER_DESC ss;
  memset(&ss, 0, sizeof ss);
  ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  ss.AddressU = ss.AddressV = ss.AddressW = address;
  ss.BorderColor = border;
  ss.MaxLOD = 1000.0f;
  ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_tex_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_tex_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list || !pso || !rs) return;
  D3D12_TEXTURE_COPY_LOCATION dst, src;
  memset(&dst, 0, sizeof dst);
  memset(&src, 0, sizeof src);
  dst.pResource = texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  src.PlacedFootprint.Footprint.Width = 2;
  src.PlacedFootprint.Footprint.Height = 2;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = 256;
  for (int level = 0; level < (mips ? 3 : 1); ++level) {
    dst.SubresourceIndex = (UINT)level;
    src.PlacedFootprint.Offset = 1024 * (UINT64)level;
    src.PlacedFootprint.Footprint.Width = src.PlacedFootprint.Footprint.Height = mips ? 4u >> level : 2u;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  }
  const float clear[4] = {0, 0, 0, 1};
  D3D12_VIEWPORT vp = {0, 0, 32, 32, 0, 1};
  D3D12_RECT sc = {0, 0, 32, 32};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 512, 28};
  ID3D12DescriptorHeap *heaps[1] = {srv_heap};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 6, 1, 0, 0);
  D3D12_TEXTURE_COPY_LOCATION back_dst, back_src;
  memset(&back_dst, 0, sizeof back_dst);
  memset(&back_src, 0, sizeof back_src);
  back_dst.pResource = readback;
  back_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  back_dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  back_dst.PlacedFootprint.Footprint.Width = 32;
  back_dst.PlacedFootprint.Footprint.Height = 32;
  back_dst.PlacedFootprint.Footprint.Depth = 1;
  back_dst.PlacedFootprint.Footprint.RowPitch = 128;
  back_src.pResource = target;
  back_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &back_dst, 0, 0, 0, &back_src, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  const UINT32 *pixels = NULL;
  CHECK_HR(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
  if (pixels && row) memcpy(row, pixels + 18 * 32, 32 * sizeof(UINT32));
  if (pixels && !row) {
    const UINT32 q00 = pixels[8 * 32 + 8], q10 = pixels[8 * 32 + 24], q01 = pixels[24 * 32 + 8], q11 = pixels[24 * 32 + 24];
    printf("texture: quadrants %08x %08x %08x %08x\n", (unsigned)q00, (unsigned)q10, (unsigned)q01, (unsigned)q11);
    /* (R8G8B8A8: 0xAABBGGRR) red (scaled by the colour), green, blue, white */
    CHECK((q00 & 0xFF) > 0 && ((q00 >> 8) & 0xFF) == 0 && ((q00 >> 16) & 0xFF) == 0);
    CHECK((q10 & 0xFF) == 0 && ((q10 >> 8) & 0xFF) > 0 && ((q10 >> 16) & 0xFF) == 0);
    CHECK((q01 & 0xFF) == 0 && ((q01 >> 8) & 0xFF) == 0 && ((q01 >> 16) & 0xFF) > 200);
    CHECK((q11 & 0xFF) > 0 && ((q11 >> 8) & 0xFF) > 0 && ((q11 >> 16) & 0xFF) > 200);
  }
  if (pixels) ID3D12Resource_Unmap(readback, 0, NULL);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D10Blob_Release(blob);
  ID3D12DescriptorHeap_Release(srv_heap);
  ID3D12DescriptorHeap_Release(rtv_heap);
  ID3D12Resource_Release(vb);
  ID3D12Resource_Release(readback);
  ID3D12Resource_Release(upload);
  ID3D12Resource_Release(texture);
  ID3D12Resource_Release(target);
}

static void test_texture(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  texture_scene(dev, queue, fence, fence_value, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, 1, 0, NULL);
}

/* The address modes of a sampler: the quad's texture coordinates run from -1 to 2, so the texture is seen three times across. Six pixels of the row through the texture's second
 * row (blue, white) are looked at, at u = -0.77, -0.20, 0.27, 0.73, 1.30, 1.77: each is the first texel (0), the second (1) or a black border (b). */
static void test_address_modes(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  static const struct { D3D12_TEXTURE_ADDRESS_MODE mode; D3D12_STATIC_BORDER_COLOR border; const char *name, *expect; UINT alpha; } cases[] = {
      {D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, "wrap", "010101", 255},
      {D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, "mirror", "100110", 255},
      {D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, "clamp", "000111", 255},
      {D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, "border (opaque black)", "bb01bb", 255},
      {D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, "border (transparent)", "bb01bb", 0},
      {D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, "border (white)", "110111", 255},
      {D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, "mirror once", "100111", 255},
  };
  static const int columns[6] = {2, 8, 13, 18, 24, 29};
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    UINT32 row[32];
    char seen[7] = "??????";
    memset(row, 0, sizeof row);
    texture_scene(dev, queue, fence, fence_value, cases[i].mode, cases[i].border, -1, 2, 0, row);
    for (int k = 0; k < 6; ++k) {
      const UINT32 px = row[columns[k]];      /* (R8G8B8A8: 0xAABBGGRR; the pixel shader multiplies by the vertex colour, whose blue and alpha are 1 and green v / 2) */
      const UINT g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
      seen[k] = b == 0 ? 'b' : b > 200 && g == 0 ? '0' : b > 200 && g > 50 ? '1' : '?';
    }
    printf("address %s: %s (expected %s), outer alpha %u\n", cases[i].name, seen, cases[i].expect, (unsigned)(row[2] >> 24));
    CHECK(strcmp(seen, cases[i].expect) == 0);
    CHECK((row[2] >> 24) == cases[i].alpha);
  }
}

/* Mipmaps: the 4 x 4 texture with three levels drawn so that a pixel covers a quarter of a texel (level 0), three texels each way (between levels 1 and 2, nearer 2) and one
 * and a half (between 0 and 1, nearer 1). */
static void test_mipmaps(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  static const struct { float repeats; UINT blue; } cases[3] = {{2, 255}, {24, 64}, {12, 128}};
  for (int i = 0; i < 3; ++i) {
    UINT32 row[32];
    memset(row, 0, sizeof row);
    texture_scene(dev, queue, fence, fence_value, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0, cases[i].repeats, 1, row);
    const UINT blue = (row[16] >> 16) & 0xFF;
    printf("mipmaps: the texture %g times across 32 pixels: blue %u (level's %u)\n", cases[i].repeats, (unsigned)blue, (unsigned)cases[i].blue);
    CHECK(blue + 1 >= cases[i].blue && blue <= cases[i].blue + 1);
  }
}

/* Depth testing, an index buffer and a root constant buffer view: three triangles drawn in the order big (far), small (near), medium (between); the small one must stay on top. */
static void test_depth(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rt_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_RESOURCE_DESC ds_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  ID3D12Resource *target = NULL, *depth = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&target));
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &ds_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
  ID3D12Resource *readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 64 * 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 1024, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *ib = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *cb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!target || !depth || !readback || !vb || !ib || !cb) { CHECK(0); return; }
  float verts[9][7];
  const float scale[3] = {0.9f, 0.4f, 0.65f}, z[3] = {0.8f, 0.2f, 0.5f};
  const float colour[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int t = 0; t < 3; ++t) {
    const float xs[3] = {-1, 1, 0}, ys[3] = {-1, -1, 1};
    for (int v = 0; v < 3; ++v) {
      float *f = verts[t * 3 + v];
      f[0] = xs[v] * scale[t]; f[1] = ys[v] * scale[t]; f[2] = z[t];
      f[3] = colour[t][0]; f[4] = colour[t][1]; f[5] = colour[t][2]; f[6] = 1;
    }
  }
  UINT8 *p = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, (void **)&p));
  memcpy(p, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  CHECK_HR(ID3D12Resource_Map(ib, 0, NULL, (void **)&p));
  const UINT16 indices[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  memcpy(p, indices, sizeof indices);
  ID3D12Resource_Unmap(ib, 0, NULL);
  CHECK_HR(ID3D12Resource_Map(cb, 0, NULL, (void **)&p));
  memcpy(p, kIdentity, sizeof kIdentity);
  ID3D12Resource_Unmap(cb, 0, NULL);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtv_heap = NULL, *dsv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
  ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);
  ID3D12Device_CreateDepthStencilView(dev, depth, NULL, dsv);

  D3D12_ROOT_PARAMETER param;
  memset(&param, 0, sizeof param);
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  param.Descriptor.ShaderRegister = 0;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.DepthStencilState.DepthEnable = TRUE;
  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list || !pso || !rs) return;
  const float clear[4] = {0, 0, 0, 1};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 1024, 28};
  D3D12_INDEX_BUFFER_VIEW ibv = {ID3D12Resource_GetGPUVirtualAddress(ib), 256, DXGI_FORMAT_R16_UINT};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(cb));
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
  ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
  for (int t = 0; t < 3; ++t) ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, (UINT)t * 3, 0, 0);
  D3D12_TEXTURE_COPY_LOCATION dst, src;
  memset(&dst, 0, sizeof dst);
  memset(&src, 0, sizeof src);
  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = 64;
  dst.PlacedFootprint.Footprint.Height = 64;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = 256;
  src.pResource = target;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  const UINT32 *pixels = NULL;
  CHECK_HR(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
  if (pixels) {
    /* (the centre of the small triangle is a third of the way up its height; the medium one reaches higher; the big one lower and wider) */
    const UINT32 inner = pixels[36 * 64 + 32], ring = pixels[16 * 64 + 32], outer = pixels[58 * 64 + 8];
    printf("depth: inner %08x, ring %08x, outer %08x\n", (unsigned)inner, (unsigned)ring, (unsigned)outer);
    CHECK((inner & 0xFFFFFF) == 0x00FF00);      /* near: green stays on top of the later blue one */
    CHECK((ring & 0xFFFFFF) == 0x0000FF);       /* blue is nearer than the red one */
    CHECK((outer & 0xFFFFFF) == 0xFF0000);
    ID3D12Resource_Unmap(readback, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D10Blob_Release(blob);
  ID3D12DescriptorHeap_Release(dsv_heap);
  ID3D12DescriptorHeap_Release(rtv_heap);
  ID3D12Resource_Release(cb);
  ID3D12Resource_Release(ib);
  ID3D12Resource_Release(vb);
  ID3D12Resource_Release(readback);
  ID3D12Resource_Release(depth);
  ID3D12Resource_Release(target);
}

static void test_compute(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  ID3D12Resource *picture = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 32 * 32 * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource *structured = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 32 * 32 * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource *counters = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource *back = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 32 * 32 * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *back_counters = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(picture && structured && counters && back && back_counters);
  if (!picture || !structured || !counters || !back || !back_counters) return;
  D3D12_ROOT_PARAMETER params[4];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 4;
  for (int i = 1; i < 4; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[i].Descriptor.ShaderRegister = (UINT)(i - 1);
  }
  D3D12_ROOT_SIGNATURE_DESC rsd = {4, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
  memset(&cd, 0, sizeof cd);
  cd.pRootSignature = rs;
  cd.CS.pShaderBytecode = dx_cs_fill_dxbc;
  cd.CS.BytecodeLength = sizeof dx_cs_fill_dxbc;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list || !pso || !rs) return;
  const UINT constants[4] = {0x00102030u, 0, 0, 0};
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 4, constants, 0);
  ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 1, ID3D12Resource_GetGPUVirtualAddress(picture));
  ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 2, ID3D12Resource_GetGPUVirtualAddress(structured));
  ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 3, ID3D12Resource_GetGPUVirtualAddress(counters));
  ID3D12GraphicsCommandList_Dispatch(list, 4, 8, 1);
  ID3D12GraphicsCommandList_CopyBufferRegion(list, back, 0, picture, 0, 32 * 32 * 4);
  ID3D12GraphicsCommandList_CopyBufferRegion(list, back_counters, 0, counters, 0, 16);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  const UINT32 *counts = NULL, *pixels = NULL;
  CHECK_HR(ID3D12Resource_Map(back_counters, 0, NULL, (void **)&counts));
  if (counts) {
    printf("compute: counters %u %u\n", (unsigned)counts[0], (unsigned)counts[1]);
    CHECK(counts[0] == 1024 && counts[1] == 1024);
    ID3D12Resource_Unmap(back_counters, 0, NULL);
  }
  CHECK_HR(ID3D12Resource_Map(back, 0, NULL, (void **)&pixels));
  if (pixels) {
    CHECK((pixels[0] >> 24) == 0xFF && (pixels[1023] >> 24) == 0xFF);
    CHECK(pixels[0] != pixels[1]);
    ID3D12Resource_Unmap(back, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D10Blob_Release(blob);
}

static void test_copies(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  /* buffer to buffer through the GPU, texture upload and download with the footprints the device reports */
  ID3D12Resource *up = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *gpu = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(up && gpu && down);
  if (!up || !gpu || !down) return;
  UINT8 *p = NULL;
  CHECK_HR(ID3D12Resource_Map(up, 0, NULL, (void **)&p));
  for (int i = 0; i < 4096; ++i) p[i] = (UINT8)(i * 7 + 3);
  /* (no Unmap: the data goes to the GPU with the next execute, as with a persistently mapped buffer) */
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  ID3D12GraphicsCommandList_CopyBufferRegion(list, gpu, 0, up, 0, 4096);
  ID3D12GraphicsCommandList_CopyBufferRegion(list, down, 0, gpu, 0, 4096);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  const UINT8 *q = NULL;
  CHECK_HR(ID3D12Resource_Map(down, 0, NULL, (void **)&q));
  int same = q != NULL;
  for (int i = 0; q && i < 4096; ++i) same &= q[i] == (UINT8)(i * 7 + 3);
  CHECK(same);
  if (q) ID3D12Resource_Unmap(down, 0, NULL);

  /* a texture: the footprint of a 5 x 3 image (rows 256 bytes apart), uploaded, then read back from the texture */
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 5, 3, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *texture = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&texture));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows = 0;
  UINT64 row_size = 0, total = 0;
  ID3D12Device_GetCopyableFootprints(dev, &td, 0, 1, 0, &fp, &rows, &row_size, &total);
  CHECK(fp.Footprint.RowPitch == 256 && rows == 3 && row_size == 20 && total == 256 * 2 + 20);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 20; ++x) p[fp.Offset + y * fp.Footprint.RowPitch + x] = (UINT8)(y * 50 + x + 1);
  CHECK_HR(ID3D12CommandAllocator_Reset(allocator));
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, allocator, NULL));
  D3D12_TEXTURE_COPY_LOCATION dst, src;
  memset(&dst, 0, sizeof dst);
  memset(&src, 0, sizeof src);
  dst.pResource = texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = up;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  D3D12_TEXTURE_COPY_LOCATION back_dst, back_src;
  memset(&back_dst, 0, sizeof back_dst);
  memset(&back_src, 0, sizeof back_src);
  back_dst.pResource = down;
  back_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  back_dst.PlacedFootprint = fp;
  back_src.pResource = texture;
  back_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &back_dst, 0, 0, 0, &back_src, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  CHECK_HR(ID3D12Resource_Map(down, 0, NULL, (void **)&q));
  same = q != NULL;
  for (int y = 0; q && y < 3; ++y)
    for (int x = 0; x < 20; ++x) same &= q[fp.Offset + y * fp.Footprint.RowPitch + x] == (UINT8)(y * 50 + x + 1);
  CHECK(same);
  if (q) ID3D12Resource_Unmap(down, 0, NULL);
  ID3D12Resource_Unmap(up, 0, NULL);
  ID3D12Resource_Release(texture);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12Resource_Release(up);
  ID3D12Resource_Release(gpu);
  ID3D12Resource_Release(down);
}

static void test_fences(ID3D12Device *dev, ID3D12CommandQueue *queue) {
  ID3D12Fence *fence = NULL;
  CHECK_HR(ID3D12Device_CreateFence(dev, 5, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
  if (!fence) return;
  CHECK(ID3D12Fence_GetCompletedValue(fence) == 5);
  HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
  CHECK_HR(ID3D12Fence_SetEventOnCompletion(fence, 3, event));      /* (already there) */
  CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
  CHECK_HR(ID3D12Fence_SetEventOnCompletion(fence, 9, event));
  CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
  CHECK_HR(ID3D12Fence_Signal(fence, 9));
  CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
  CHECK(ID3D12Fence_GetCompletedValue(fence) == 9);
  /* a queue waits for a value another signals */
  CHECK_HR(ID3D12CommandQueue_Wait(queue, fence, 9));
  CHECK_HR(ID3D12CommandQueue_Signal(queue, fence, 12));
  CHECK(ID3D12Fence_GetCompletedValue(fence) == 12);
  CloseHandle(event);
  ID3D12Fence_Release(fence);
}

static void test_swap_chain(IDXGIAdapter1 *adapter, ID3D12Device *dev, ID3D12CommandQueue *queue) {
  IDXGIFactory4 *factory = NULL;
  CHECK_HR(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
  if (!factory) return;
  DXGI_SWAP_CHAIN_DESC1 sd;
  memset(&sd, 0, sizeof sd);
  sd.Width = 32;
  sd.Height = 16;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1 *chain = NULL;
  CHECK_HR(IDXGIFactory4_CreateSwapChainForComposition(factory, (IUnknown *)queue, &sd, NULL, &chain));
  if (chain) {
    IDXGISwapChain3 *chain3 = NULL;
    CHECK_HR(IDXGISwapChain1_QueryInterface(chain, &IID_IDXGISwapChain3, (void **)&chain3));
    if (chain3) {
      CHECK(IDXGISwapChain3_GetCurrentBackBufferIndex(chain3) == 0);
      ID3D12Resource *buffer = NULL;
      CHECK_HR(IDXGISwapChain3_GetBuffer(chain3, 0, &IID_ID3D12Resource, (void **)&buffer));
      if (buffer) {
        D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(buffer);
        CHECK(rd.Width == 32 && rd.Height == 16);
        ID3D12Resource_Release(buffer);
      }
      CHECK_HR(IDXGISwapChain3_Present(chain3, 1, 0));
      CHECK(IDXGISwapChain3_GetCurrentBackBufferIndex(chain3) == 1);
      CHECK_HR(IDXGISwapChain3_Present(chain3, 0, 0));
      CHECK(IDXGISwapChain3_GetCurrentBackBufferIndex(chain3) == 0);
      DXGI_SWAP_CHAIN_DESC1 got;
      CHECK_HR(IDXGISwapChain3_GetDesc1(chain3, &got));
      CHECK(got.Width == 32 && got.BufferCount == 2);
      CHECK(IDXGISwapChain3_SetFullscreenState(chain3, TRUE, NULL) == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
      IDXGISwapChain3_Release(chain3);
    }
    IDXGISwapChain1_Release(chain);
  }
  IDXGIFactory4_Release(factory);
}

static void test_device_queries(ID3D12Device *dev) {
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels;
  D3D_FEATURE_LEVEL asked[2] = {D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_0};
  levels.NumFeatureLevels = 2;
  levels.pFeatureLevelsRequested = asked;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof levels));
  CHECK(levels.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_11_0);
  D3D12_FEATURE_DATA_D3D12_OPTIONS opts;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof opts));
  CHECK(ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) > 0);
  LUID luid = ID3D12Device_GetAdapterLuid(dev);
  CHECK(luid.LowPart != 0 || luid.HighPart != 0);
  CHECK(ID3D12Device_GetNodeCount(dev) == 1);
  CHECK(ID3D12Device_GetDeviceRemovedReason(dev) == S_OK);
  D3D12_ROOT_SIGNATURE_DESC1 d11;
  memset(&d11, 0, sizeof d11);
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned;
  memset(&versioned, 0, sizeof versioned);
  versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  versioned.Desc_1_1 = d11;
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeVersionedRootSignature(&versioned, &blob, &error));
  if (blob) {
    ID3D12RootSignature *rs = NULL;
    CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    if (rs) ID3D12RootSignature_Release(rs);
    ID3D10Blob_Release(blob);
  }
  ID3D12Debug *debug = NULL;
  CHECK(FAILED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)) && debug == NULL);
}

/* Queries, indirect execution, bundles and tiled resources on the triangle of test_triangle. */
typedef struct Scene {
  ID3D12Device *dev;
  ID3D12CommandQueue *queue;
  ID3D12Fence *fence;
  UINT64 *fence_value;
  ID3D12Resource *target, *readback, *vb;
  ID3D12DescriptorHeap *rtv_heap;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  ID3D12RootSignature *rs;
  ID3D12PipelineState *pso;
  ID3D12CommandAllocator *allocator;
} Scene;

static int scene_make(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&c->target));
  c->readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 64 * 256 + 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  c->vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!c->target || !c->readback || !c->vb) return 0;
  const float verts[3][7] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(c->vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(c->vb, 0, NULL);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&c->rtv_heap));
  c->rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(c->rtv_heap);
  ID3D12Device_CreateRenderTargetView(dev, c->target, NULL, c->rtv);
  D3D12_ROOT_PARAMETER param;
  memset(&param, 0, sizeof param);
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  param.Constants.Num32BitValues = 16;
  D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&c->rs));
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&c->pso));
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&c->allocator));
  if (blob) ID3D10Blob_Release(blob);
  return c->rs && c->pso && c->allocator;
}

static void scene_begin(Scene *c, ID3D12GraphicsCommandList *list, int clear) {
  const float black[4] = {0, 0, 0, 1};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &c->rtv, FALSE, NULL);
  if (clear) ID3D12GraphicsCommandList_ClearRenderTargetView(list, c->rtv, black, 0, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}

static void scene_state(Scene *c, ID3D12GraphicsCommandList *list, D3D12_GPU_VIRTUAL_ADDRESS vb) {
  D3D12_VERTEX_BUFFER_VIEW vbv = {vb, 256, 28};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetPipelineState(list, c->pso);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
}

static void scene_read_target(Scene *c, ID3D12GraphicsCommandList *list) {
  D3D12_TEXTURE_COPY_LOCATION dst, src;
  memset(&dst, 0, sizeof dst);
  memset(&src, 0, sizeof src);
  dst.pResource = c->readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = 64;
  dst.PlacedFootprint.Footprint.Height = 64;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = 256;
  src.pResource = c->target;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
}

static void scene_run(Scene *c, ID3D12GraphicsCommandList *list) {
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(c->queue, 1, lists);
  wait_fence(c->queue, c->fence, c->fence_value);
}

static int scene_coloured(Scene *c, int x0, int y0, int x1, int y1) {
  const UINT32 *pixels = NULL;
  int n = 0;
  if (FAILED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) || !pixels) return -1;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) n += (pixels[y * 64 + x] & 0xFFFFFF) != 0;
  ID3D12Resource_Unmap(c->readback, 0, NULL);
  return n;
}

static void test_queries(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, 4, 0};
  ID3D12QueryHeap *occlusion = NULL, *timestamps = NULL;
  CHECK_HR(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&occlusion));
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  CHECK_HR(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&timestamps));
  qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
  ID3D12QueryHeap *stats = NULL;
  CHECK(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&stats) == DXGI_ERROR_UNSUPPORTED && stats == NULL);
  ID3D12Resource *results = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, c->pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!occlusion || !timestamps || !results || !list) { CHECK(0); return; }
  scene_begin(c, list, 1);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_EndQuery(list, timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  ID3D12GraphicsCommandList_BeginQuery(list, occlusion, D3D12_QUERY_TYPE_OCCLUSION, 0);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_EndQuery(list, occlusion, D3D12_QUERY_TYPE_OCCLUSION, 0);
  ID3D12GraphicsCommandList_BeginQuery(list, occlusion, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_EndQuery(list, occlusion, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
  ID3D12GraphicsCommandList_BeginQuery(list, occlusion, D3D12_QUERY_TYPE_OCCLUSION, 2);
  ID3D12GraphicsCommandList_EndQuery(list, occlusion, D3D12_QUERY_TYPE_OCCLUSION, 2);
  ID3D12GraphicsCommandList_EndQuery(list, timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
  ID3D12GraphicsCommandList_ResolveQueryData(list, occlusion, D3D12_QUERY_TYPE_OCCLUSION, 0, 3, results, 0);
  ID3D12GraphicsCommandList_ResolveQueryData(list, timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, results, 64);
  scene_run(c, list);
  const UINT64 *r = NULL;
  CHECK_HR(ID3D12Resource_Map(results, 0, NULL, (void **)&r));
  if (r) {
    printf("queries: occlusion %llu, binary %llu, empty %llu, timestamps %llu %llu\n", (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[8], (unsigned long long)r[9]);
    CHECK(r[0] > 600 && r[0] < 2000);
    CHECK(r[1] == 1);
    CHECK(r[2] == 0);
    CHECK(r[9] >= r[8]);
    ID3D12Resource_Unmap(results, 0, NULL);
  }
  UINT64 frequency = 0;
  CHECK_HR(ID3D12CommandQueue_GetTimestampFrequency(c->queue, &frequency));
  CHECK(frequency != 0);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12Resource_Release(results);
  ID3D12QueryHeap_Release(occlusion);
  ID3D12QueryHeap_Release(timestamps);
}

static void test_indirect(Scene *c) {
  ID3D12Device *dev = c->dev;
  /* each command: the matrix (16 root constants) and a draw: two triangles, one in each half of the target */
  D3D12_INDIRECT_ARGUMENT_DESC args[2];
  memset(args, 0, sizeof args);
  args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  args[0].Constant.RootParameterIndex = 0;
  args[0].Constant.DestOffsetIn32BitValues = 0;
  args[0].Constant.Num32BitValuesToSet = 16;
  args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  D3D12_COMMAND_SIGNATURE_DESC sd = {16 * 4 + 16, 2, args, 0};
  ID3D12CommandSignature *sig = NULL;
  CHECK_HR(ID3D12Device_CreateCommandSignature(dev, &sd, c->rs, &IID_ID3D12CommandSignature, (void **)&sig));
  ID3D12Resource *arguments = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *count = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!sig || !arguments || !count) { CHECK(0); return; }
  BYTE *p = NULL;
  CHECK_HR(ID3D12Resource_Map(arguments, 0, NULL, (void **)&p));
  for (int i = 0; i < 2; ++i) {
    float m[16];
    memcpy(m, kIdentity, sizeof m);
    m[0] = m[5] = 0.5f;
    m[3] = i ? 0.5f : -0.5f;      /* (the constant buffer holds the matrix by columns: the translation of mul(position, m) is the fourth value) */
    memcpy(p + i * sd.ByteStride, m, sizeof m);
    const UINT draw[4] = {3, 1, 0, 0};
    memcpy(p + i * sd.ByteStride + sizeof m, draw, sizeof draw);
  }
  ID3D12Resource_Unmap(arguments, 0, NULL);
  CHECK_HR(ID3D12Resource_Map(count, 0, NULL, (void **)&p));
  *(UINT *)p = 2;
  ID3D12Resource_Unmap(count, 0, NULL);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, c->pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  scene_begin(c, list, 1);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_ExecuteIndirect(list, sig, 4, arguments, 0, count, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  const int left = scene_coloured(c, 0, 0, 32, 64), right = scene_coloured(c, 32, 0, 64, 64), middle = scene_coloured(c, 31, 40, 33, 64);
  printf("indirect: %d pixels left, %d right, %d in the middle\n", left, right, middle);

  CHECK(left > 150 && right > 150 && middle == 0);
  /* a count of 1 draws one */
  CHECK_HR(ID3D12Resource_Map(count, 0, NULL, (void **)&p));
  *(UINT *)p = 1;
  ID3D12Resource_Unmap(count, 0, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, c->pso));
  scene_begin(c, list, 1);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_ExecuteIndirect(list, sig, 4, arguments, 0, count, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  CHECK(scene_coloured(c, 0, 0, 32, 64) > 150 && scene_coloured(c, 32, 0, 64, 64) == 0);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12Resource_Release(count);
  ID3D12Resource_Release(arguments);
  ID3D12CommandSignature_Release(sig);
}

static void test_bundle(Scene *c) {
  ID3D12Device *dev = c->dev;
  ID3D12CommandAllocator *balloc = NULL;
  ID3D12GraphicsCommandList *bundle = NULL, *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_BUNDLE, &IID_ID3D12CommandAllocator, (void **)&balloc));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, balloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&bundle));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!bundle || !list) { CHECK(0); return; }
  scene_state(c, bundle, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
  CHECK_HR(ID3D12GraphicsCommandList_Close(bundle));
  /* the bundle twice in one list, with a clear between: it takes the targets from the list */
  scene_begin(c, list, 1);
  ID3D12GraphicsCommandList_ExecuteBundle(list, bundle);
  scene_read_target(c, list);
  scene_run(c, list);
  const int n = scene_coloured(c, 0, 0, 64, 64);
  printf("bundle: %d coloured pixels\n", n);
  CHECK(n > 600 && n < 2000);
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, NULL));
  scene_begin(c, list, 1);
  scene_read_target(c, list);      /* (cleared, nothing drawn) */
  scene_run(c, list);
  CHECK(scene_coloured(c, 0, 0, 64, 64) == 0);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12GraphicsCommandList_Release(bundle);
  ID3D12CommandAllocator_Release(balloc);
}

static void test_tiled(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_FEATURE_DATA_D3D12_OPTIONS opts;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof opts));
  CHECK(opts.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_1);
  D3D12_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.SizeInBytes = 2 * 65536;
  hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  hd.Properties.CreationNodeMask = hd.Properties.VisibleNodeMask = 1;
  hd.Alignment = 65536;
  hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ID3D12Heap *heap = NULL;
  CHECK_HR(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 65536, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *placed = NULL, *reserved = NULL;
  if (heap) CHECK_HR(ID3D12Device_CreatePlacedResource(dev, heap, 0, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&placed));
  bd.Width = 2 * 65536;
  CHECK_HR(ID3D12Device_CreateReservedResource(dev, &bd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&reserved));
  if (!heap || !placed || !reserved) { CHECK(0); return; }
  UINT tiles = 0;
  ID3D12Device_GetResourceTiling(dev, reserved, &tiles, NULL, NULL, NULL, 0, NULL);
  CHECK(tiles == 2);
  /* the triangle goes to the heap through the placed buffer; the reserved buffer's second tile is that tile of the heap */
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, c->pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_CopyBufferRegion(list, placed, 0, c->vb, 0, 256);
  scene_run(c, list);
  const D3D12_GPU_VIRTUAL_ADDRESS base = ID3D12Resource_GetGPUVirtualAddress(reserved);
  /* before the mapping: both tiles read as zero, so a draw from the second one makes nothing */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, c->pso));
  scene_begin(c, list, 1);
  scene_state(c, list, base + 65536);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  CHECK(scene_coloured(c, 0, 0, 64, 64) == 0);
  D3D12_TILED_RESOURCE_COORDINATE start = {1, 0, 0, 0};
  D3D12_TILE_REGION_SIZE size = {1, FALSE, 0, 0, 0};
  D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_start = 0, count = 1;
  ID3D12CommandQueue_UpdateTileMappings(c->queue, reserved, 1, &start, &size, heap, 1, &flags, &heap_start, &count, D3D12_TILE_MAPPING_FLAG_NONE);
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, c->pso));
  scene_begin(c, list, 1);
  scene_state(c, list, base + 65536);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  const int mapped = scene_coloured(c, 0, 0, 64, 64);
  printf("tiled: %d coloured pixels with the tile mapped\n", mapped);
  CHECK(mapped > 600 && mapped < 2000);
  /* unmapped again (a NULL range) */
  flags = D3D12_TILE_RANGE_FLAG_NULL;
  ID3D12CommandQueue_UpdateTileMappings(c->queue, reserved, 1, &start, &size, NULL, 1, &flags, NULL, &count, D3D12_TILE_MAPPING_FLAG_NONE);
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, c->pso));
  scene_begin(c, list, 1);
  scene_state(c, list, base + 65536);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  CHECK(scene_coloured(c, 0, 0, 64, 64) == 0);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12Resource_Release(reserved);
  ID3D12Resource_Release(placed);
  ID3D12Heap_Release(heap);
}

static void scene_translated(Scene *c, ID3D12GraphicsCommandList *list, float tx) {
  float m[16];
  memcpy(m, kIdentity, sizeof m);
  m[0] = m[5] = 0.5f;
  m[3] = tx;
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, m, 0);
}

static void test_predication(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2, 0};
  ID3D12QueryHeap *heap = NULL;
  CHECK_HR(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&heap));
  ID3D12Resource *predicate = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, c->pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!heap || !predicate || !list) { CHECK(0); return; }
  /* predicate[0] = 1 (something was visible), predicate[1] = 0 (nothing was) */
  scene_begin(c, list, 1);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  scene_translated(c, list, 0.5f);
  ID3D12GraphicsCommandList_BeginQuery(list, heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_EndQuery(list, heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0);
  ID3D12GraphicsCommandList_BeginQuery(list, heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
  ID3D12GraphicsCommandList_EndQuery(list, heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
  ID3D12GraphicsCommandList_ResolveQueryData(list, heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 2, predicate, 0);
  scene_run(c, list);
  /* skipped (zero, EQUAL_ZERO) on the left, executed (non-zero) on the right: only the right triangle is drawn */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, c->pso));
  scene_begin(c, list, 1);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_SetPredication(list, predicate, 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
  scene_translated(c, list, -0.5f);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_SetPredication(list, predicate, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  scene_translated(c, list, 0.5f);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  scene_read_target(c, list);
  scene_run(c, list);
  const int left = scene_coloured(c, 0, 0, 32, 64), right = scene_coloured(c, 32, 0, 64, 64);
  printf("predication: %d pixels left, %d right\n", left, right);
  CHECK(left == 0 && right > 150);
  /* NOT_EQUAL_ZERO runs the commands when the value is zero: the left triangle appears, the unpredicated clear in between is not affected */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, c->pso));
  scene_begin(c, list, 0);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_SetPredication(list, predicate, 8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  scene_translated(c, list, -0.5f);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_SetPredication(list, predicate, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);      /* (skipped: the value is not zero) */
  ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  scene_read_target(c, list);
  scene_run(c, list);
  const int left2 = scene_coloured(c, 0, 0, 32, 64), right2 = scene_coloured(c, 32, 0, 64, 64);
  printf("predication: %d pixels left, %d right\n", left2, right2);
  CHECK(left2 > 150 && right2 == right);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12Resource_Release(predicate);
  ID3D12QueryHeap_Release(heap);
}

static void test_stream_output(Scene *c) {
  ID3D12Device *dev = c->dev;
  ID3D12Resource *target = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
  ID3D12Resource *filled = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *back = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!target || !filled || !back) { CHECK(0); return; }
  /* a pipeline that captures the vertex shader's COLOR output (four floats a vertex) and rasterizes nothing */
  D3D12_SO_DECLARATION_ENTRY entry = {0, "COLOR", 0, 0, 4, 0};
  UINT stride = 16;
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.StreamOutput.pSODeclaration = &entry;
  pd.StreamOutput.NumEntries = 1;
  pd.StreamOutput.pBufferStrides = &stride;
  pd.StreamOutput.NumStrides = 1;
  pd.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!pso || !list) { CHECK(0); return; }
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {ID3D12Resource_GetGPUVirtualAddress(target) + 16, 128, ID3D12Resource_GetGPUVirtualAddress(filled)};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(c->vb), 256, 28};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_SOSetTargets(list, 0, 1, &view);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_CopyBufferRegion(list, back, 0, target, 0, 128);
  scene_run(c, list);
  const float *f = NULL;
  CHECK_HR(ID3D12Resource_Map(back, 0, NULL, (void **)&f));
  if (f) {
    printf("stream output: %g %g %g %g | %g %g %g %g | %g %g %g %g\n", f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    CHECK(f[0] == 0 && f[4] == 1 && f[5] == 0 && f[6] == 0 && f[7] == 1);      /* (the view starts 16 bytes in) */
    CHECK(f[8] == 0 && f[9] == 1 && f[10] == 0 && f[11] == 1);
    CHECK(f[12] == 0 && f[13] == 0 && f[14] == 1 && f[15] == 1);
    CHECK(f[16] == 0);
    ID3D12Resource_Unmap(back, 0, NULL);
  }
  /* the bytes written went to the filled-size location */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, pso));
  ID3D12GraphicsCommandList_CopyBufferRegion(list, back, 0, filled, 0, 16);
  scene_run(c, list);
  const UINT64 *bytes = NULL;
  CHECK_HR(ID3D12Resource_Map(back, 0, NULL, (void **)&bytes));
  if (bytes) {
    printf("stream output: %llu bytes written\n", (unsigned long long)bytes[0]);
    CHECK(bytes[0] == 48);
    ID3D12Resource_Unmap(back, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(pso);
  ID3D12Resource_Release(back);
  ID3D12Resource_Release(filled);
  ID3D12Resource_Release(target);
}

/* A tiled texture with a mip chain: 256 x 256 has two standard levels (4 and 1 tiles) and a tail for the other seven. */
static void test_mip_tails(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 9, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *texture = NULL, *clone = NULL;
  CHECK_HR(ID3D12Device_CreateReservedResource(dev, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&texture));
  CHECK_HR(ID3D12Device_CreateReservedResource(dev, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&clone));
  if (!texture || !clone) { CHECK(0); return; }
  UINT total = 0, nsub = 9;
  D3D12_PACKED_MIP_INFO packed;
  D3D12_TILE_SHAPE shape;
  D3D12_SUBRESOURCE_TILING tilings[9];
  ID3D12Device_GetResourceTiling(dev, texture, &total, &packed, &shape, &nsub, 0, tilings);
  printf("mip tail: %u tiles, %u standard + %u packed mips, the tail at tile %u\n", total, packed.NumStandardMips, packed.NumPackedMips, packed.StartTileIndexInOverallResource);
  CHECK(total == 6 && packed.NumStandardMips == 2 && packed.NumPackedMips == 7 && packed.NumTilesForPackedMips == 1 && packed.StartTileIndexInOverallResource == 5);
  CHECK(shape.WidthInTexels == 128 && shape.HeightInTexels == 128 && nsub == 9);
  CHECK(tilings[0].WidthInTiles == 2 && tilings[0].HeightInTiles == 2 && tilings[0].StartTileIndexInOverallResource == 0);
  CHECK(tilings[1].WidthInTiles == 1 && tilings[1].StartTileIndexInOverallResource == 4 && tilings[2].StartTileIndexInOverallResource == 0xFFFFFFFFu);

  D3D12_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.SizeInBytes = 6 * 65536;
  hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  hd.Properties.CreationNodeMask = hd.Properties.VisibleNodeMask = 1;
  hd.Alignment = 65536;
  ID3D12Heap *heap = NULL;
  CHECK_HR(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  ID3D12Resource *upload = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 6 * 65536, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 6 * 65536, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!heap || !upload || !down) { CHECK(0); return; }
  /* all six tiles to the heap in order; the pattern of every tile differs */
  D3D12_TILED_RESOURCE_COORDINATE start = {0, 0, 0, 0};
  D3D12_TILE_REGION_SIZE size = {6, FALSE, 0, 0, 0};
  D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_start = 0, count = 6;
  ID3D12CommandQueue_UpdateTileMappings(c->queue, texture, 1, &start, &size, heap, 1, &flags, &heap_start, &count, D3D12_TILE_MAPPING_FLAG_NONE);
  UINT8 *p = NULL;
  CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void **)&p));
  for (UINT t = 0; t < 6; ++t)
    for (UINT i = 0; i < 65536; ++i) p[t * 65536 + i] = (UINT8)(t * 40 + i * 7);
  ID3D12Resource_Unmap(upload, 0, NULL);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_CopyTiles(list, texture, &start, &size, upload, 0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  scene_run(c, list);
  /* the same through a second texture whose mapping was copied from the first, read back out of the tiles */
  D3D12_TILE_REGION_SIZE all = {6, FALSE, 0, 0, 0};
  ID3D12CommandQueue_CopyTileMappings(c->queue, clone, &start, texture, &start, &all, D3D12_TILE_MAPPING_FLAG_NONE);
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, NULL));
  ID3D12GraphicsCommandList_CopyTiles(list, clone, &start, &size, down, 0, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  scene_run(c, list);
  const UINT8 *q = NULL;
  CHECK_HR(ID3D12Resource_Map(down, 0, NULL, (void **)&q));
  int same[6] = {1, 1, 1, 1, 1, 1};
  if (q)
    for (UINT t = 0; t < 6; ++t) {
      /* (the tail tile holds only the 21844 bytes of the packed levels) */
      const UINT n = t == 5 ? 21844 : 65536;
      for (UINT i = 0; i < n; ++i) same[t] &= q[t * 65536 + i] == (UINT8)(t * 40 + i * 7);
    }
  printf("mip tail: tiles round-trip %d %d %d %d %d %d\n", same[0], same[1], same[2], same[3], same[4], same[5]);
  for (UINT t = 0; t < 6; ++t) CHECK(q && same[t]);
  if (q) ID3D12Resource_Unmap(down, 0, NULL);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12Resource_Release(down);
  ID3D12Resource_Release(upload);
  ID3D12Heap_Release(heap);
  ID3D12Resource_Release(clone);
  ID3D12Resource_Release(texture);
}

/* The limits the adapter reports are what the pipelines can use: the software adapter takes sixteen vertex inputs, three vertex buffers here, four textures and eight render targets. */
static ID3D12Resource *upload_floats(ID3D12Device *dev, const float *data, UINT count) {
  ID3D12Resource *r = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256 + count * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *p = NULL;
  if (r && SUCCEEDED(ID3D12Resource_Map(r, 0, NULL, &p)) && p) memcpy(p, data, count * 4);
  if (r) ID3D12Resource_Unmap(r, 0, NULL);
  return r;
}

static void test_attribute_slots(Scene *c) {
  ID3D12Device *dev = c->dev;
  /* buffer 0: position and colour, buffer 1: uv and normal, buffer 2: an offset and a tint for each of two instances */
  const float b0[3][7] = {{-0.3f, -0.3f, 0.5f, 1, 0, 0, 1}, {0.3f, -0.3f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.3f, 0.5f, 0, 0, 1, 1}};
  const float b1[3][5] = {{0, 0, 1, 1, 1}, {1, 0, 1, 1, 1}, {0.5f, 1, 1, 1, 1}};
  const float b2[2][8] = {{-0.5f, 0, 0, 0, 0, 0, 0, 0}, {0.5f, 0, 0, 0, 0, 0, 0.5f, 0}};
  ID3D12Resource *v0 = upload_floats(dev, &b0[0][0], 21), *v1 = upload_floats(dev, &b1[0][0], 15), *v2 = upload_floats(dev, &b2[0][0], 16);
  if (!v0 || !v1 || !v2) { CHECK(0); return; }
  D3D12_INPUT_ELEMENT_DESC layout[6] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"OFFSET", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"TINT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_attrs6_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_attrs6_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 6;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!pso || !list) { CHECK(0); return; }
  D3D12_VERTEX_BUFFER_VIEW views[3] = {{ID3D12Resource_GetGPUVirtualAddress(v0), 21 * 4, 28}, {ID3D12Resource_GetGPUVirtualAddress(v1), 15 * 4, 20}, {ID3D12Resource_GetGPUVirtualAddress(v2), 16 * 4, 32}};
  scene_begin(c, list, 1);
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 3, views);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 2, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  const int left = scene_coloured(c, 0, 0, 32, 64), right = scene_coloured(c, 32, 0, 64, 64);
  const UINT32 *pixels = NULL;
  CHECK_HR(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels));
  printf("slots: %d pixels left, %d right\n", left, right);
  CHECK(left > 100 && right > 100 && left == right);
  if (pixels) {
    /* (a pixel inside each triangle: the tint of the second instance is blue, and the normal of buffer 1 (1,1,1) leaves the colours as they are) */
    const UINT32 a = pixels[40 * 64 + 16], b = pixels[40 * 64 + 48];
    printf("slots: %08x %08x\n", (unsigned)a, (unsigned)b);
    CHECK((a & 0xFFFFFF) != 0 && (b & 0xFFFFFF) != 0 && (b & 0xFF) > (a & 0xFF) + 60);      /* (BGRA: the low byte is blue) */
    ID3D12Resource_Unmap(c->readback, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(pso);
  ID3D12Resource_Release(v0);
  ID3D12Resource_Release(v1);
  ID3D12Resource_Release(v2);
}

static void test_four_textures(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *textures[4] = {0};
  ID3D12Resource *upload = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4 * 512, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  const UINT32 texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};      /* red, green, blue, white (R8G8B8A8: 0xAABBGGRR) */
  UINT8 *p = NULL;
  if (!upload || FAILED(ID3D12Resource_Map(upload, 0, NULL, (void **)&p))) { CHECK(0); return; }
  for (int i = 0; i < 4; ++i) memcpy(p + i * 512, &texels[i], 4);
  ID3D12Resource_Unmap(upload, 0, NULL);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ID3D12DescriptorHeap *srv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
  D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap);
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap);
  const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!srv_heap || !list) { CHECK(0); return; }
  for (int i = 0; i < 4; ++i) {
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&textures[i]));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE at = {srv.ptr + (SIZE_T)i * inc};
    ID3D12Device_CreateShaderResourceView(dev, textures[i], &sv, at);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof dst);
    memset(&src, 0, sizeof src);
    dst.pResource = textures[i];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = (UINT64)i * 512;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = src.PlacedFootprint.Footprint.Height = src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  }
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 16;
  D3D12_STATIC_SAMPLER_DESC ss;
  memset(&ss, 0, sizeof ss);
  ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  ss.MaxLOD = 1000.0f;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_tex4_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_tex4_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  if (!rs || !pso) { CHECK(0); return; }
  ID3D12DescriptorHeap *heaps[1] = {srv_heap};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(c->vb), 256, 28};
  scene_begin(c, list, 1);
  ID3D12GraphicsCommandList_SetPipelineState(list, pso);
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  const UINT32 *pixels = NULL;
  CHECK_HR(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels));
  if (pixels) {
    const UINT32 centre = pixels[40 * 64 + 32];
    printf("textures: %08x\n", (unsigned)centre);
    /* (the average of red, green, blue and white: a mid grey, 0.5 each; the vertex colour does not enter the pixel shader's result) */
    const int r = (centre >> 16) & 0xFF, g = (centre >> 8) & 0xFF, b = centre & 0xFF;
    CHECK(r >= 120 && r <= 135 && g >= 120 && g <= 135 && b >= 120 && b <= 135);
    ID3D12Resource_Unmap(c->readback, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D10Blob_Release(blob);
  ID3D12DescriptorHeap_Release(srv_heap);
  ID3D12Resource_Release(upload);
  for (int i = 0; i < 4; ++i) if (textures[i]) ID3D12Resource_Release(textures[i]);
}

static void test_eight_targets(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource *targets[8] = {0};
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  ID3D12Resource *back = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 8 * 16384, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!rtv_heap || !back) { CHECK(0); return; }
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (int i = 0; i < 8; ++i) {
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&targets[i]));
    D3D12_CPU_DESCRIPTOR_HANDLE at = {rtvs.ptr + (SIZE_T)i * inc};
    ID3D12Device_CreateRenderTargetView(dev, targets[i], NULL, at);
  }
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_mrt8_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_mrt8_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.IndependentBlendEnable = TRUE;
  for (int i = 0; i < 8; ++i) pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 8;
  for (int i = 0; i < 8; ++i) pd.RTVFormats[i] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!pso || !list) { CHECK(0); return; }
  const float black[4] = {0, 0, 0, 1};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(c->vb), 256, 28};
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 8, &rtvs, TRUE, NULL);
  for (int i = 0; i < 8; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = {rtvs.ptr + (SIZE_T)i * inc};
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, at, black, 0, NULL);
  }
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  for (int i = 0; i < 8; ++i) {
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof dst);
    memset(&src, 0, sizeof src);
    dst.pResource = back;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = (UINT64)i * 16384;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = dst.PlacedFootprint.Footprint.Height = 64;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    src.pResource = targets[i];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
  }
  scene_run(c, list);
  const UINT8 *p = NULL;
  CHECK_HR(ID3D12Resource_Map(back, 0, NULL, (void **)&p));
  if (p) {
    static const UINT32 want[8] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu, 0xFFFFFFFFu, 0xFF808080u};
    int right = 0;
    for (int i = 0; i < 8; ++i) {
      const UINT32 pixel = *(const UINT32 *)(p + i * 16384 + 40 * 256 + 32 * 4);
      const int dr = (int)((pixel >> 16) & 0xFF) - (int)((want[i] >> 16) & 0xFF), dg = (int)((pixel >> 8) & 0xFF) - (int)((want[i] >> 8) & 0xFF), db = (int)(pixel & 0xFF) - (int)(want[i] & 0xFF);
      right += dr > -3 && dr < 3 && dg > -3 && dg < 3 && db > -3 && db < 3;
    }
    printf("targets: %d of 8 hold their colour\n", right);
    CHECK(right == 8);
    ID3D12Resource_Unmap(back, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(pso);
  ID3D12Resource_Release(back);
  ID3D12DescriptorHeap_Release(rtv_heap);
  for (int i = 0; i < 8; ++i) if (targets[i]) ID3D12Resource_Release(targets[i]);
}

/* A pipeline of the scene's shaders with another input layout (and, for the depth tests, a depth buffer and maybe no render target). */
static ID3D12PipelineState *scene_pipeline(Scene *c, const D3D12_INPUT_ELEMENT_DESC *layout, UINT elements, int depth, int targets) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (depth) {
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  }
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = elements;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = (UINT)targets;
  if (targets) pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(c->dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  return pso;
}

/* Draws `count` vertices of `stride` bytes from `data` with `pso` on a cleared target and returns the pixel at (x, y) (0xAARRGGBB). */
static UINT32 scene_draw_with(Scene *c, ID3D12PipelineState *pso, const void *data, UINT bytes, UINT stride, UINT count, int x, int y) {
  ID3D12Resource *vb = make_buffer(c->dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12GraphicsCommandList *list = NULL;
  UINT32 pixel = 0;
  if (!vb || !pso) { CHECK(0); if (vb) ID3D12Resource_Release(vb); return 0; }
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, data, bytes);
  ID3D12Resource_Unmap(vb, 0, NULL);
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (list) {
    D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, stride};
    scene_begin(c, list, 1);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_DrawInstanced(list, count, 1, 0, 0);
    scene_read_target(c, list);
    scene_run(c, list);
    const UINT32 *pixels = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
      pixel = pixels[y * 64 + x];
      ID3D12Resource_Unmap(c->readback, 0, NULL);
    }
    ID3D12GraphicsCommandList_Release(list);
  }
  ID3D12Resource_Release(vb);
  return pixel;
}

static int near_colour(UINT32 pixel, UINT r, UINT g, UINT b) {
  const int dr = (int)((pixel >> 16) & 0xFF) - (int)r, dg = (int)((pixel >> 8) & 0xFF) - (int)g, db = (int)(pixel & 0xFF) - (int)b;
  return dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 && db <= 2;
}

/* Vertex elements that are not 32-bit floats: the device reads bytes, 16-bit numbers and half floats and hands the shader floats. One colour for the whole triangle. */
static void test_vertex_formats(Scene *c) {
  /* colours as four normalised bytes, in both byte orders */
  struct { float x, y, z; UINT8 colour[4]; } bytes[3] = {{-0.8f, -0.8f, 0.5f, {51, 102, 204, 255}}, {0.8f, -0.8f, 0.5f, {51, 102, 204, 255}}, {0.0f, 0.8f, 0.5f, {51, 102, 204, 255}}};
  D3D12_INPUT_ELEMENT_DESC rgba[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  ID3D12PipelineState *pso = scene_pipeline(c, rgba, 2, 0, 1);
  UINT32 pixel = scene_draw_with(c, pso, bytes, sizeof bytes, sizeof bytes[0], 3, 32, 40);
  printf("vertex formats: R8G8B8A8_UNORM %08x", (unsigned)pixel);
  CHECK(near_colour(pixel, 51, 102, 204));
  if (pso) ID3D12PipelineState_Release(pso);
  rgba[1].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  pso = scene_pipeline(c, rgba, 2, 0, 1);
  pixel = scene_draw_with(c, pso, bytes, sizeof bytes, sizeof bytes[0], 3, 32, 40);
  printf(", B8G8R8A8_UNORM %08x", (unsigned)pixel);
  CHECK(near_colour(pixel, 204, 102, 51));
  if (pso) ID3D12PipelineState_Release(pso);
  /* positions as two half floats (0.8 is 0x3A66), colours as four normalised 16-bit numbers */
  struct { UINT16 xy[2]; UINT16 colour[4]; } halves[3] = {{{0xBA66, 0xBA66}, {65535, 32768, 0, 65535}}, {{0x3A66, 0xBA66}, {65535, 32768, 0, 65535}}, {{0, 0x3A66}, {65535, 32768, 0, 65535}}};
  D3D12_INPUT_ELEMENT_DESC half[2] = {{"POSITION", 0, DXGI_FORMAT_R16G16_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                      {"COLOR", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  pso = scene_pipeline(c, half, 2, 0, 1);
  pixel = scene_draw_with(c, pso, halves, sizeof halves, sizeof halves[0], 3, 32, 40);
  printf(", halves and R16G16B16A16_UNORM %08x", (unsigned)pixel);
  CHECK(near_colour(pixel, 255, 128, 0));
  if (pso) ID3D12PipelineState_Release(pso);
  /* positions as four signed normalised 16-bit numbers (w = 32767 is 1), colours as signed normalised bytes */
  struct { INT16 pos[4]; INT8 colour[4]; } snorm[3] = {{{-26214, -26214, 16384, 32767}, {127, 0, 64, 127}}, {{26214, -26214, 16384, 32767}, {127, 0, 64, 127}}, {{0, 26214, 16384, 32767}, {127, 0, 64, 127}}};
  D3D12_INPUT_ELEMENT_DESC sn[2] = {{"POSITION", 0, DXGI_FORMAT_R16G16B16A16_SNORM, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                    {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_SNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  pso = scene_pipeline(c, sn, 2, 0, 1);
  pixel = scene_draw_with(c, pso, snorm, sizeof snorm, sizeof snorm[0], 3, 32, 40);
  printf(", signed normalised %08x\n", (unsigned)pixel);
  CHECK(near_colour(pixel, 255, 0, 129));
  if (pso) ID3D12PipelineState_Release(pso);
  /* (a format no vertex can have) */
  rgba[1].Format = DXGI_FORMAT_D32_FLOAT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.InputLayout.pInputElementDescs = rgba;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  pso = NULL;
  CHECK(FAILED(ID3D12Device_CreateGraphicsPipelineState(c->dev, &pd, &IID_ID3D12PipelineState, (void **)&pso)) && pso == NULL);
}

/* A pass with a depth buffer and no render target (the way a shadow map is drawn): a small near triangle goes into the depth buffer alone; a big farther one drawn afterwards
 * with a render target has a hole where the small one is. Then a depth clear that no pass of its list takes: the next list's draw sees it. */
static void test_depth_only(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC ds_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  ID3D12Resource *depth = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &ds_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *dsv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  ID3D12PipelineState *depth_pso = scene_pipeline(c, layout, 2, 1, 0), *colour_pso = scene_pipeline(c, layout, 2, 1, 1);
  if (!depth || !dsv_heap || !vb || !depth_pso || !colour_pso) { CHECK(0); return; }
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
  ID3D12Device_CreateDepthStencilView(dev, depth, NULL, dsv);
  const float verts[6][7] = {{-0.4f, -0.4f, 0.3f, 1, 1, 1, 1}, {0.4f, -0.4f, 0.3f, 1, 1, 1, 1}, {0.0f, 0.4f, 0.3f, 1, 1, 1, 1},      /* small, near */
                             {-1.0f, -1.0f, 0.7f, 0, 1, 0, 1}, {1.0f, -1.0f, 0.7f, 0, 1, 0, 1}, {0.0f, 1.0f, 0.7f, 0, 1, 0, 1}};      /* big, far, green */
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, 28};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  const float black[4] = {0, 0, 0, 1};
  for (int round = 0; round < 2; ++round) {
    ID3D12GraphicsCommandList *list = NULL;
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, depth_pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (!list) { CHECK(0); break; }
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    if (round == 0) {
      /* the depth buffer alone, then the render target with it */
      ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &dsv);
      ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
      ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &c->rtv, FALSE, &dsv);
      ID3D12GraphicsCommandList_ClearRenderTargetView(list, c->rtv, black, 0, NULL);
      ID3D12GraphicsCommandList_SetPipelineState(list, colour_pso);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 3, 0);
      scene_read_target(c, list);
      scene_run(c, list);
      const UINT32 *pixels = NULL;
      if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
        const UINT32 hole = pixels[36 * 64 + 32], around = pixels[58 * 64 + 32];
        printf("depth only: %08x where the depth pass drew, %08x beside it\n", (unsigned)hole, (unsigned)around);
        CHECK((hole & 0xFFFFFF) == 0);
        CHECK((around & 0xFFFFFF) == 0x00FF00);
        ID3D12Resource_Unmap(c->readback, 0, NULL);
      }
    } else {
      /* a list that only clears the depth buffer to "nearest", and then one that draws: nothing passes the depth test */
      ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, NULL);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
      list = NULL;
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, colour_pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
      if (!list) { CHECK(0); break; }
      ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
      ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
      ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
      ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
      ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
      ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &c->rtv, FALSE, &dsv);
      ID3D12GraphicsCommandList_ClearRenderTargetView(list, c->rtv, black, 0, NULL);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 3, 0);
      scene_read_target(c, list);
      scene_run(c, list);
      const int coloured = scene_coloured(c, 0, 0, 64, 64);
      printf("depth only: %d pixels after a clear to 0 in a list of its own\n", coloured);
      CHECK(coloured == 0);
    }
    if (list) ID3D12GraphicsCommandList_Release(list);
  }
  ID3D12PipelineState_Release(colour_pso);
  ID3D12PipelineState_Release(depth_pso);
  ID3D12Resource_Release(vb);
  ID3D12DescriptorHeap_Release(dsv_heap);
  ID3D12Resource_Release(depth);
}

/* Multisampling: a white triangle drawn into a render target of four samples and resolved into the scene's target. Inside it is white, outside black, and the pixels its
 * edges cross are greys (the average of their samples) - a target of one sample has none of those. */
static void test_multisample(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {DXGI_FORMAT_B8G8R8A8_UNORM, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof levels));
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {4, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource *target = NULL;
  HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&target);
  if (!levels.NumQualityLevels) {
    printf("multisample: the adapter has no images of four samples\n");
    CHECK(FAILED(hr) && target == NULL);
    return;
  }
  CHECK_HR(hr);
  td.SampleDesc.Count = 3;      /* (a count no adapter has) */
  ID3D12Resource *odd = NULL;
  CHECK(FAILED(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&odd)) && odd == NULL);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtv_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.MultisampleEnable = TRUE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 4;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  if (pso) CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!target || !rtv_heap || !vb || !pso || !list) { CHECK(0); return; }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);
  const float verts[3][7] = {{-0.8f, -0.8f, 0.5f, 1, 1, 1, 1}, {0.8f, -0.8f, 0.5f, 1, 1, 1, 1}, {0.0f, 0.8f, 0.5f, 1, 1, 1, 1}};
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, 28};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  const float black[4] = {0, 0, 0, 1};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_ResolveSubresource(list, c->target, 0, target, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  scene_read_target(c, list);
  scene_run(c, list);
  const UINT32 *pixels = NULL;
  if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
    int white = 0, grey = 0, dark = 0;
    for (int i = 0; i < 64 * 64; ++i) {
      const UINT v = pixels[i] & 0xFF;
      if (v == 255) ++white; else if (v == 0) ++dark; else ++grey;
    }
    printf("multisample: %d white, %d grey and %d black pixels, the centre %08x\n", white, grey, dark, (unsigned)pixels[40 * 64 + 32]);
    CHECK(white > 1000 && white < 1352);
    CHECK(grey > 40 && grey < 400);
    CHECK((pixels[40 * 64 + 32] & 0xFFFFFF) == 0xFFFFFF && (pixels[0] & 0xFFFFFF) == 0);
    /* (what the region resolved below covers: averaged, its edge pixels are grey) */
    int covered = 0, full = 0;
    static UINT32 averaged[32][32];
    for (int y = 24; y < 56; ++y)
      for (int x = 16; x < 48; ++x) {
        covered += (pixels[y * 64 + x] & 0xFF) != 0;
        full += (pixels[y * 64 + x] & 0xFF) == 0xFF;
        averaged[y - 24][x - 16] = pixels[y * 64 + x];
      }
    ID3D12Resource_Unmap(c->readback, 0, NULL);
    /* ResolveSubresourceRegion: a part of the image into the corner of a target cleared red, averaged, then its greatest samples (the CPU renderer's resolve
     * modes; on virgl a pass of texel fetches) */
    for (int pass = 0; pass < 3; ++pass) {      /* (average, greatest, least) */
    ID3D12GraphicsCommandList1 *list1 = NULL;
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, NULL));
    CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
    if (list1) {
      const float red[4] = {1, 0, 0, 1};
      ID3D12GraphicsCommandList_ClearRenderTargetView(list, c->rtv, red, 0, NULL);
      D3D12_RECT region = {16, 24, 48, 56};
      ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, c->target, 0, 0, 0, target, 0, &region, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                            pass == 2 ? D3D12_RESOLVE_MODE_MIN : pass ? D3D12_RESOLVE_MODE_MAX : D3D12_RESOLVE_MODE_AVERAGE);
      ID3D12GraphicsCommandList1_Release(list1);
      scene_read_target(c, list);
      scene_run(c, list);
      if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
        int max_white = 0, max_grey = 0, outside_red = 1, same = 1;
        for (int y = 0; y < 32; ++y)
          for (int x = 0; x < 32; ++x) {
            const UINT v = pixels[y * 64 + x] & 0xFF;
            if (v == 255) ++max_white; else if (v != 0) ++max_grey;
            same &= pixels[y * 64 + x] == averaged[y][x];
          }
        for (int y = 0; y < 64; ++y)
          for (int x = 32; x < 64; ++x) outside_red &= pixels[y * 64 + x] == 0xFFFF0000u;
        printf("resolve region (%s): %d of %d covered pixels white, %d grey\n", pass == 2 ? "least" : pass ? "greatest" : "average", max_white, covered, max_grey);
        if (pass == 2) CHECK(max_grey == 0 && max_white == full && full > 100);      /* (an edge pixel's least sample is black) */
        else if (pass) CHECK(max_grey == 0 && max_white == covered && covered > 100);
        else CHECK(same);      /* (the region as the whole image's average resolve has it) */
        CHECK(outside_red && pixels[40 * 64 + 8] == 0xFFFF0000u);
        ID3D12Resource_Unmap(c->readback, 0, NULL);
      }
    }
    }
    /* ResolveSubresourceRegion into a subresource other than 0: the same region into level 1 of a texture of two levels, and into layer 1 of an array of two,
     * each read back and compared with the average of the whole image; then a region that does not fit the level, which is left out */
    for (int kind = 0; kind < 2; ++kind) {
      D3D12_RESOURCE_DESC dd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kind ? 32 : 64, kind ? 32 : 64, kind ? 2 : 1, kind ? 1 : 2, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
      ID3D12Resource *sub = NULL;
      CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&sub));
      ID3D12GraphicsCommandList1 *list1 = NULL;
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, NULL));
      CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
      if (!sub || !list1) { if (sub) ID3D12Resource_Release(sub); if (list1) ID3D12GraphicsCommandList1_Release(list1); continue; }
      /* (the readback cleared first: a resolve left out must show) */
      UINT32 *clear = NULL;
      if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&clear)) && clear) { memset(clear, 0, 64 * 64 * 4); ID3D12Resource_Unmap(c->readback, 0, NULL); }
      D3D12_RECT region = {16, 24, 48, 56}, too_big = {0, 0, 40, 40};
      ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, sub, 1, 0, 0, target, 0, &region, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
      ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, sub, 1, 0, 0, target, 0, &too_big, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
      ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, sub, 2, 0, 0, target, 0, &region, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
      ID3D12GraphicsCommandList1_Release(list1);
      D3D12_TEXTURE_COPY_LOCATION to, from;
      memset(&to, 0, sizeof to);
      memset(&from, 0, sizeof from);
      to.pResource = c->readback;
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      to.PlacedFootprint.Footprint.Width = 32;
      to.PlacedFootprint.Footprint.Height = 32;
      to.PlacedFootprint.Footprint.Depth = 1;
      to.PlacedFootprint.Footprint.RowPitch = 256;
      from.pResource = sub;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      from.SubresourceIndex = 1;
      ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
      scene_run(c, list);
      if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
        int same = 1;
        for (int y = 0; y < 32; ++y)
          for (int x = 0; x < 32; ++x) same &= pixels[y * 64 + x] == averaged[y][x];
        printf("resolve region into %s 1: %s\n", kind ? "layer" : "level", same ? "the average" : "not the average");
        CHECK(same);
        ID3D12Resource_Unmap(c->readback, 0, NULL);
      }
      ID3D12Resource_Release(sub);
    }
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(pso);
  ID3D12Resource_Release(vb);
  ID3D12DescriptorHeap_Release(rtv_heap);
  ID3D12Resource_Release(target);
}

static void test_limits(Scene *c) {
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels;
  D3D_FEATURE_LEVEL asked[3] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  levels.NumFeatureLevels = 3;
  levels.pFeatureLevelsRequested = asked;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(c->dev, D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof levels));
  CHECK(levels.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_11_0);      /* (the software adapter takes what 11_0 needs) */
  test_attribute_slots(c);
  test_four_textures(c);
  test_eight_targets(c);
}

static void test_blend_factor(Scene *c) {
  /* OMSetBlendFactor: white over red with the blend factor (left half) and with its alpha (right half), two pipelines and two factors in one list */
  ID3D12Device *dev = c->dev;
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!vb) { CHECK(0); return; }
  const float verts[3][7] = {{-1, -1, 0.5f, 1, 1, 1, 1}, {3, -1, 0.5f, 1, 1, 1, 1}, {-1, 3, 0.5f, 1, 1, 1, 1}};
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  D3D12_RENDER_TARGET_BLEND_DESC *b = &pd.BlendState.RenderTarget[0];
  b->BlendEnable = TRUE;
  b->SrcBlend = D3D12_BLEND_BLEND_FACTOR;
  b->DestBlend = D3D12_BLEND_INV_BLEND_FACTOR;
  b->BlendOp = D3D12_BLEND_OP_ADD;
  b->SrcBlendAlpha = D3D12_BLEND_ONE;
  b->DestBlendAlpha = D3D12_BLEND_ZERO;
  b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
  b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *colour = NULL, *alpha = NULL, *bad = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&colour));
  b->SrcBlend = D3D12_BLEND_ALPHA_FACTOR;
  b->DestBlend = D3D12_BLEND_INV_ALPHA_FACTOR;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&alpha));
  b->SrcBlend = (D3D12_BLEND)12;      /* (no blend factor has that number) */
  CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&bad) == E_INVALIDARG && !bad);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!colour || !alpha || !list) { CHECK(0); return; }
  const float red[4] = {1, 0, 0, 1};
  scene_begin(c, list, 0);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, c->rtv, red, 0, NULL);
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, 28};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  const D3D12_RECT left = {0, 0, 32, 64}, right = {32, 0, 64, 64};
  const float f1[4] = {0.25f, 0.5f, 0.75f, 1.0f}, f2[4] = {0.0f, 0.0f, 0.0f, 0.5f};
  ID3D12GraphicsCommandList_SetPipelineState(list, colour);
  /* (more factors than the PSO keeps pipelines for, in rows the checks do not read: the early ones are pushed out while this list still binds them) */
  const D3D12_RECT churn = {0, 0, 64, 8};
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &churn);
  for (int k = 0; k < 20; ++k) {
    const float fk[4] = {k / 20.0f, 0.5f, 1.0f - k / 20.0f, 1.0f};
    ID3D12GraphicsCommandList_OMSetBlendFactor(list, fk);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  }
  ID3D12GraphicsCommandList_OMSetBlendFactor(list, f1);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &left);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  ID3D12GraphicsCommandList_SetPipelineState(list, alpha);
  ID3D12GraphicsCommandList_OMSetBlendFactor(list, f2);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &right);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  const UINT32 *px = NULL;
  if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&px)) && px) {
    const UINT32 l = px[32 * 64 + 8], r = px[32 * 64 + 56];
    printf("blend factor: %08x %08x\n", (unsigned)l, (unsigned)r);
    /* (left: white * factor + red * (1 - factor) = 1, 0.5, 0.75; right: half of each) */
    CHECK(((l >> 16) & 0xFF) == 255 && ((l >> 8) & 0xFF) >= 127 && ((l >> 8) & 0xFF) <= 128 && (l & 0xFF) >= 191 && (l & 0xFF) <= 192);
    CHECK(((r >> 16) & 0xFF) == 255 && ((r >> 8) & 0xFF) >= 127 && ((r >> 8) & 0xFF) <= 128 && (r & 0xFF) >= 127 && (r & 0xFF) <= 128);
    /* (twenty draws over red, each with its own factor - the list's early pipelines were still there when it ran: green, blended at one half twenty times,
     * and blue, which the first factor made 1, end near 1) */
    const UINT32 first = px[4 * 64 + 8], last_one = px[4 * 64 + 40];
    printf("blend factor churn: %08x\n", (unsigned)first);
    CHECK(((first >> 16) & 0xFF) == 255 && ((first >> 8) & 0xFF) >= 254 && (first & 0xFF) >= 254);
    CHECK(first == last_one);      /* (each draw blends over the one before: all the row's pixels are the same) */
    ID3D12Resource_Unmap(c->readback, 0, NULL);
  }
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(colour);
  ID3D12PipelineState_Release(alpha);
  ID3D12Resource_Release(vb);
}

/* ---- planar textures: placed ones, views of several slices of a plane, integer views of a plane ---------------------------------------------------------------------- */

#define PL_W 16
#define PL_H 8
/* The value of plane `plane` of slice `slice` at (x, y) (x counts channels: the chroma's U and V are 2x and 2x + 1) of pattern `seed`: constant over each slice and plane
 * unless `ramp`. 16-bit planes take it as it is. */
static UINT planar_value(UINT seed, UINT slice, UINT plane, UINT x, UINT y, int ramp) {
  if (!ramp) return plane == 0 ? 40 + 60 * slice + seed : (x & 1) ? 200 - 30 * slice + seed : 10 + 30 * slice + seed;
  return (seed * 31 + slice * 57 + plane * 101 + x * 7 + y * 11) & 0xFF;
}
/* Every plane of every slice of a planar texture from pattern `seed` (8 or 16-bit pixels), through an upload buffer laid out as GetCopyableFootprints says. */
static void planar_fill(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value, ID3D12CommandAllocator *allocator, ID3D12Resource *tex, UINT seed,
                        int ramp) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  const UINT layers = rd.DepthOrArraySize, bytes = rd.Format == DXGI_FORMAT_NV12 ? 1 : 2;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16];
  UINT rows[16];
  UINT64 row_bytes[16], total = 0;
  ID3D12Device_GetCopyableFootprints(dev, &rd, 0, 2 * layers, 0, fp, rows, row_bytes, &total);
  ID3D12Resource *up = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  BYTE *p = NULL;
  if (!up || FAILED(ID3D12Resource_Map(up, 0, NULL, (void **)&p)) || !p) { CHECK(0); if (up) ID3D12Resource_Release(up); return; }
  for (UINT sub = 0; sub < 2 * layers; ++sub)
    for (UINT y = 0; y < rows[sub]; ++y)
      for (UINT x = 0; x < row_bytes[sub] / bytes; ++x) {
        const UINT v = planar_value(seed, sub % layers, sub / layers, x, y, ramp);
        BYTE *at = p + fp[sub].Offset + (UINT64)y * fp[sub].Footprint.RowPitch + (UINT64)x * bytes;
        if (bytes == 1) at[0] = (BYTE)v;
        else { at[0] = (BYTE)(v << 8); at[1] = (BYTE)v; }      /* (16-bit: the value in the high byte, as P010's ten bits are) */
      }
  ID3D12Resource_Unmap(up, 0, NULL);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  for (UINT sub = 0; list && sub < 2 * layers; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    to.pResource = tex;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = up;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.SubresourceIndex = sub;
    from.PlacedFootprint = fp[sub];
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
  }
  if (list) {
    CHECK_HR(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_fence(queue, fence, fence_value);
    ID3D12GraphicsCommandList_Release(list);
  }
  ID3D12Resource_Release(up);
}
/* Subresource `sub` of a planar texture read back: its values (the high byte of a 16-bit one) into out[y * row + x], `row` the channels of a row. */
static int planar_read(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value, ID3D12CommandAllocator *allocator, ID3D12Resource *tex, UINT sub,
                       BYTE *out, UINT *row) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  const UINT bytes = rd.Format == DXGI_FORMAT_NV12 ? 1 : 2;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  ID3D12Device_GetCopyableFootprints(dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!down || !list) { if (down) ID3D12Resource_Release(down); if (list) ID3D12GraphicsCommandList_Release(list); return 0; }
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = down;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.pResource = tex;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.PlacedFootprint = fp;
  from.SubresourceIndex = sub;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
  ID3D12GraphicsCommandList_Release(list);
  const BYTE *p = NULL;
  int ok = SUCCEEDED(ID3D12Resource_Map(down, 0, NULL, (void **)&p)) && p;
  *row = (UINT)(row_bytes / bytes);
  for (UINT y = 0; ok && y < rows; ++y)
    for (UINT x = 0; x < *row; ++x) out[y * *row + x] = p[fp.Offset + (UINT64)y * fp.Footprint.RowPitch + (UINT64)x * bytes + bytes - 1];
  if (p) ID3D12Resource_Unmap(down, 0, NULL);
  ID3D12Resource_Release(down);
  return ok;
}
/* Whether a planar texture holds pattern `seed` in every plane of every slice (`ramp` as planar_fill). */
static int planar_holds(Scene *c, ID3D12Resource *tex, UINT seed, int ramp) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  BYTE values[PL_W * PL_H];
  for (UINT sub = 0; sub < 2u * rd.DepthOrArraySize; ++sub) {
    UINT row = 0;
    if (!planar_read(c->dev, c->queue, c->fence, c->fence_value, c->allocator, tex, sub, values, &row)) return 0;
    const UINT h = sub >= rd.DepthOrArraySize ? PL_H / 2 : PL_H;
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < row; ++x)
        if (values[y * row + x] != (BYTE)planar_value(seed, sub % rd.DepthOrArraySize, sub / rd.DepthOrArraySize, x, y, ramp)) {
          printf("  planar sub %u (%u, %u): %u, want %u\n", sub, x, y, values[y * row + x], planar_value(seed, sub % rd.DepthOrArraySize, sub / rd.DepthOrArraySize, x, y, ramp) & 0xFF);
          return 0;
        }
  }
  return 1;
}
static ID3D12Resource *planar_texture(ID3D12Device *dev, DXGI_FORMAT format, UINT layers, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, PL_W, PL_H, (UINT16)layers, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  ID3D12Resource *t = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t));
  return t;
}

/* A pipeline of `ps` over a root signature of a table of one shader resource view (t0), a point sampler and the vertex shader's matrix; `target` its render target's format. */
typedef struct PlanarDraw { ID3D12RootSignature *rs; ID3D12PipelineState *pso; } PlanarDraw;
static int planar_draw_make(Scene *c, PlanarDraw *p, const void *vs, SIZE_T vs_size, const void *ps, SIZE_T ps_size, DXGI_FORMAT target, int with_table) {
  memset(p, 0, sizeof *p);
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 16;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  D3D12_STATIC_SAMPLER_DESC ss;
  memset(&ss, 0, sizeof ss);
  ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  ss.MaxLOD = 1000.0f;
  D3D12_ROOT_SIGNATURE_DESC rsd = {with_table ? 2u : 1u, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) {
    CHECK_HR(ID3D12Device_CreateRootSignature(c->dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&p->rs));
    ID3D10Blob_Release(blob);
  }
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = p->rs;
  pd.VS.pShaderBytecode = vs;
  pd.VS.BytecodeLength = vs_size;
  pd.PS.pShaderBytecode = ps;
  pd.PS.BytecodeLength = ps_size;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = target;
  pd.SampleDesc.Count = 1;
  if (p->rs) CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(c->dev, &pd, &IID_ID3D12PipelineState, (void **)&p->pso));
  return p->rs && p->pso;
}
static void planar_draw_free(PlanarDraw *p) {
  if (p->pso) ID3D12PipelineState_Release(p->pso);
  if (p->rs) ID3D12RootSignature_Release(p->rs);
}
/* One triangle over all of the target (`rtv`, w x h; c->target when NULL) whose vertices have the colour `colour`, drawn with `p` and the view at `srv` of `heap` (none:
 * NULL); the target's pixel at (x, y) when it is the scene's. */
static UINT32 planar_draw(Scene *c, PlanarDraw *p, ID3D12DescriptorHeap *heap, D3D12_GPU_DESCRIPTOR_HANDLE srv, const float colour[4], const D3D12_CPU_DESCRIPTOR_HANDLE *rtv,
                          UINT w, UINT h, int x, int y) {
  const float v[3][7] = {{-1, -1, 0.5f, colour[0], colour[1], colour[2], colour[3]}, {3, -1, 0.5f, colour[0], colour[1], colour[2], colour[3]},
                         {-1, 3, 0.5f, colour[0], colour[1], colour[2], colour[3]}};
  ID3D12Resource *vb = make_buffer(c->dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12GraphicsCommandList *list = NULL;
  UINT32 pixel = 0;
  void *mapped = NULL;
  if (!vb || FAILED(ID3D12Resource_Map(vb, 0, NULL, &mapped)) || !mapped) { CHECK(0); if (vb) ID3D12Resource_Release(vb); return 0; }
  memcpy(mapped, v, sizeof v);
  ID3D12Resource_Unmap(vb, 0, NULL);
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, p->pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (list) {
    const D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, 28};
    const D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    const D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
    if (rtv) ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, rtv, FALSE, NULL);
    else scene_begin(c, list, 1);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, p->rs);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
    if (heap) {
      ID3D12DescriptorHeap *heaps[1] = {heap};
      ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
      ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, srv);
    }
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    if (!rtv) scene_read_target(c, list);
    scene_run(c, list);
    const UINT32 *pixels = NULL;
    if (!rtv && SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
      pixel = pixels[y * 64 + x];
      ID3D12Resource_Unmap(c->readback, 0, NULL);
    }
    ID3D12GraphicsCommandList_Release(list);
  }
  ID3D12Resource_Release(vb);
  return pixel;
}
static D3D12_SHADER_RESOURCE_VIEW_DESC plane_srv_desc(DXGI_FORMAT format, UINT plane, int array, UINT first, UINT size) {
  D3D12_SHADER_RESOURCE_VIEW_DESC sv;
  memset(&sv, 0, sizeof sv);
  sv.Format = format;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (array) {
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.FirstArraySlice = first;
    sv.Texture2DArray.ArraySize = size;
    sv.Texture2DArray.PlaneSlice = plane;
  } else {
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    sv.Texture2D.PlaneSlice = plane;
  }
  return sv;
}
static int px_red(UINT32 p) { return (int)((p >> 16) & 0xFF); }
static int px_green(UINT32 p) { return (int)((p >> 8) & 0xFF); }
static int px_blue(UINT32 p) { return (int)(p & 0xFF); }

static void test_planar(Scene *c) {
  ID3D12Device *dev = c->dev;
  /* Placed planar textures: the allocation info covers every plane, two textures side by side in one heap keep their own planes, a reserved one is refused */
  D3D12_RESOURCE_DESC nd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, PL_W, PL_H, 2, 1, DXGI_FORMAT_NV12, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  const D3D12_RESOURCE_ALLOCATION_INFO info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &nd);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[4];
  UINT64 footprint_total = 0;
  ID3D12Device_GetCopyableFootprints(dev, &nd, 0, 4, 0, fp, NULL, NULL, &footprint_total);
  printf("NV12 %ux%u x2: allocation %llu bytes (alignment %llu), footprints %llu\n", PL_W, PL_H, (unsigned long long)info.SizeInBytes, (unsigned long long)info.Alignment,
         (unsigned long long)footprint_total);
  CHECK(info.SizeInBytes >= footprint_total && info.Alignment == 65536 && info.SizeInBytes % info.Alignment == 0);
  D3D12_HEAP_DESC hd = {2 * info.SizeInBytes, {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1}, 0, D3D12_HEAP_FLAG_NONE};
  ID3D12Heap *heap = NULL;
  ID3D12Resource *placed[2] = {NULL, NULL};
  CHECK_HR(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  for (int i = 0; heap && i < 2; ++i)
    CHECK_HR(ID3D12Device_CreatePlacedResource(dev, heap, (UINT64)i * info.SizeInBytes, &nd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&placed[i]));
  ID3D12Resource *beyond = NULL;
  if (heap) CHECK(FAILED(ID3D12Device_CreatePlacedResource(dev, heap, info.SizeInBytes + 65536, &nd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&beyond)) && !beyond);
  if (placed[0] && placed[1]) {
    planar_fill(dev, c->queue, c->fence, c->fence_value, c->allocator, placed[0], 1, 1);
    planar_fill(dev, c->queue, c->fence, c->fence_value, c->allocator, placed[1], 2, 1);
    CHECK(planar_holds(c, placed[0], 1, 1));
    CHECK(planar_holds(c, placed[1], 2, 1));
    const D3D12_RESOURCE_DESC got = ID3D12Resource_GetDesc(placed[1]);
    CHECK(got.Format == DXGI_FORMAT_NV12 && got.DepthOrArraySize == 2);
  }
  ID3D12Resource *reserved = NULL;
  CHECK(ID3D12Device_CreateReservedResource(dev, &nd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&reserved) == E_INVALIDARG && !reserved);
  for (int i = 0; i < 2; ++i) if (placed[i]) ID3D12Resource_Release(placed[i]);
  if (heap) ID3D12Heap_Release(heap);

  /* Views of several slices of a plane: a Texture2DArray shader reads each slice (NV12 and P010) */
  D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ID3D12DescriptorHeap *views = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&views));
  if (!views) return;
  const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(views);
  const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(views);
  const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  PlanarDraw array_draw, int_draw, int_write;
  const int have_array = planar_draw_make(c, &array_draw, dx_vs_vol_dxbc, sizeof dx_vs_vol_dxbc, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, DXGI_FORMAT_B8G8R8A8_UNORM, 1);
  const int have_int = planar_draw_make(c, &int_draw, dx_vs_dxbc, sizeof dx_vs_dxbc, dx_ps_int_load_dxbc, sizeof dx_ps_int_load_dxbc, DXGI_FORMAT_B8G8R8A8_UNORM, 1);
  const int have_write = planar_draw_make(c, &int_write, dx_vs_dxbc, sizeof dx_vs_dxbc, dx_ps_int_write_dxbc, sizeof dx_ps_int_write_dxbc, DXGI_FORMAT_R8_UINT, 0);
  CHECK(have_array && have_int && have_write);
  ID3D12Resource *nv12 = planar_texture(dev, DXGI_FORMAT_NV12, 3, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ID3D12Resource *p010 = planar_texture(dev, DXGI_FORMAT_P010, 3, D3D12_RESOURCE_FLAG_NONE);
  if (nv12) planar_fill(dev, c->queue, c->fence, c->fence_value, c->allocator, nv12, 0, 0);
  if (p010) planar_fill(dev, c->queue, c->fence, c->fence_value, c->allocator, p010, 0, 0);
  if (have_array && nv12 && p010) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = plane_srv_desc(DXGI_FORMAT_R8_UNORM, 0, 1, 0, 3);
    ID3D12Device_CreateShaderResourceView(dev, nv12, &sv, cpu0);
    sv = plane_srv_desc(DXGI_FORMAT_R8G8_UNORM, 1, 1, 1, 2);      /* (the chroma of slices 1 and 2) */
    ID3D12Device_CreateShaderResourceView(dev, nv12, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + inc});
    sv = plane_srv_desc(DXGI_FORMAT_R16_UNORM, 0, 1, 0, 3);
    ID3D12Device_CreateShaderResourceView(dev, p010, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + 2 * inc});
    int luma_ok = 1, chroma_ok = 1, p010_ok = 1;
    for (UINT s = 0; s < 3; ++s) {
      const float at[4] = {0.5f, 0.5f, (float)s, 1};
      UINT32 px = planar_draw(c, &array_draw, views, gpu0, at, NULL, 64, 64, 32, 32);
      printf("  NV12 luma slice %u through an array view: %d %d %d\n", s, px_red(px), px_green(px), px_blue(px));
      luma_ok &= abs(px_red(px) - (int)planar_value(0, s, 0, 0, 0, 0)) <= 1 && px_green(px) == 0 && px_blue(px) == 0;
      px = planar_draw(c, &array_draw, views, (D3D12_GPU_DESCRIPTOR_HANDLE){gpu0.ptr + 2 * inc}, at, NULL, 64, 64, 32, 32);
      p010_ok &= abs(px_red(px) - (int)planar_value(0, s, 0, 0, 0, 0)) <= 1 && px_green(px) == 0;
      if (s < 2) {
        px = planar_draw(c, &array_draw, views, (D3D12_GPU_DESCRIPTOR_HANDLE){gpu0.ptr + inc}, at, NULL, 64, 64, 32, 32);
        printf("  NV12 chroma slice %u through an array view of slices 1, 2: %d %d %d\n", s + 1, px_red(px), px_green(px), px_blue(px));
        chroma_ok &= abs(px_red(px) - (int)planar_value(0, s + 1, 1, 0, 0, 0)) <= 1 && abs(px_green(px) - (int)planar_value(0, s + 1, 1, 1, 0, 0)) <= 1 && px_blue(px) == 0;
      }
    }
    CHECK(luma_ok);
    CHECK(chroma_ok);
    CHECK(p010_ok);
    /* ... and what changes in a slice is seen the next time the view is read (the array the view gives the shader follows its slices) */
    planar_fill(dev, c->queue, c->fence, c->fence_value, c->allocator, nv12, 5, 0);
    const float at[4] = {0.5f, 0.5f, 2.0f, 1};
    const UINT32 px = planar_draw(c, &array_draw, views, gpu0, at, NULL, 64, 64, 32, 32);
    CHECK(abs(px_red(px) - (int)planar_value(5, 2, 0, 0, 0, 0)) <= 1);
  }

  /* Integer views of a plane: R8_UINT / R8G8_UINT / R16_UINT read the plane's values as integers; an R8_UINT render target view writes them */
  if (have_int && nv12 && p010) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = plane_srv_desc(DXGI_FORMAT_R8_UINT, 0, 1, 2, 1);      /* (slice 2's luma, read by a Texture2D<uint4> shader) */
    ID3D12Device_CreateShaderResourceView(dev, nv12, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + 3 * inc});
    sv = plane_srv_desc(DXGI_FORMAT_R8G8_UINT, 1, 1, 1, 1);
    ID3D12Device_CreateShaderResourceView(dev, nv12, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + 4 * inc});
    sv = plane_srv_desc(DXGI_FORMAT_R16_UINT, 0, 1, 1, 1);
    ID3D12Device_CreateShaderResourceView(dev, p010, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + 5 * inc});
    const float any[4] = {0, 0, 0, 1};
    UINT32 px = planar_draw(c, &int_draw, views, (D3D12_GPU_DESCRIPTOR_HANDLE){gpu0.ptr + 3 * inc}, any, NULL, 64, 64, 3, 3);
    printf("  R8_UINT of NV12 slice 2's luma: %d %d %d (alpha %u)\n", px_red(px), px_green(px), px_blue(px), (unsigned)(px >> 24));
    CHECK(px_red(px) == (int)planar_value(5, 2, 0, 0, 0, 0) && px_green(px) == 0 && px_blue(px) == 0 && (px >> 24) == 1);
    px = planar_draw(c, &int_draw, views, (D3D12_GPU_DESCRIPTOR_HANDLE){gpu0.ptr + 4 * inc}, any, NULL, 64, 64, 3, 3);
    printf("  R8G8_UINT of NV12 slice 1's chroma: %d %d %d\n", px_red(px), px_green(px), px_blue(px));
    CHECK(px_red(px) == (int)planar_value(5, 1, 1, 0, 0, 0) && px_green(px) == (int)planar_value(5, 1, 1, 1, 0, 0) && px_blue(px) == 0);
    px = planar_draw(c, &int_draw, views, (D3D12_GPU_DESCRIPTOR_HANDLE){gpu0.ptr + 5 * inc}, any, NULL, 64, 64, 3, 3);
    /* (P010's 16-bit value is the pattern in the high byte: the integer is that times 256, which over 255 is above 1 in every channel the shader writes) */
    printf("  R16_UINT of P010 slice 1's luma: %d %d %d\n", px_red(px), px_green(px), px_blue(px));
    CHECK(px_red(px) == 255 && px_green(px) == 0 && px_blue(px) == 0);
  }
  if (have_write && nv12) {
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap *rtvs = NULL;
    CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    if (rtvs) {
      const D3D12_CPU_DESCRIPTOR_HANDLE luma = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
      const D3D12_CPU_DESCRIPTOR_HANDLE chroma = {luma.ptr + ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
      const D3D12_CPU_DESCRIPTOR_HANDLE luma_unorm = {luma.ptr + 2 * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
      D3D12_RENDER_TARGET_VIEW_DESC rv;
      memset(&rv, 0, sizeof rv);
      rv.Format = DXGI_FORMAT_R8_UINT;
      rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      rv.Texture2DArray.FirstArraySlice = 1;
      rv.Texture2DArray.ArraySize = 1;
      ID3D12Device_CreateRenderTargetView(dev, nv12, &rv, luma);
      rv.Format = DXGI_FORMAT_R8_UNORM;
      ID3D12Device_CreateRenderTargetView(dev, nv12, &rv, luma_unorm);
      rv.Format = DXGI_FORMAT_R8G8_UINT;
      rv.Texture2DArray.FirstArraySlice = 0;
      rv.Texture2DArray.PlaneSlice = 1;
      ID3D12Device_CreateRenderTargetView(dev, nv12, &rv, chroma);
      /* (the plane drawn into by the GPU first - a clear of its R8_UNORM view to 128 -, then the left half by the integer view: the right half keeps the GPU's 128) */
      ID3D12GraphicsCommandList *cl = NULL;
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
      if (cl) {
        const float half[4] = {0.5f, 0, 0, 1};
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, luma_unorm, half, 0, NULL);
        scene_run(c, cl);
        ID3D12GraphicsCommandList_Release(cl);
      }
      const float colour[4] = {0.2f, 0.9f, 0.9f, 0.9f};      /* (written as uint4(c * 255 + 0.5): red 51) */
      planar_draw(c, &int_write, NULL, (D3D12_GPU_DESCRIPTOR_HANDLE){0}, colour, &luma, PL_W / 2, PL_H, 0, 0);
      ID3D12GraphicsCommandList *list = NULL;
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
      if (list) {
        const float uv[4] = {7, 9, 300, 4};      /* (the integers 7 and 9; the channels the view does not have stay the plane's) */
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, chroma, uv, 0, NULL);
        scene_run(c, list);
        ID3D12GraphicsCommandList_Release(list);
      }
      BYTE values[PL_W * PL_H];
      UINT row = 0;
      int luma_ok = planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, nv12, 1, values, &row), others_ok = 1, uv_ok = 1;
      for (UINT i = 0; luma_ok && i < PL_W * PL_H; ++i) luma_ok &= values[i] == ((i % PL_W) < PL_W / 2 ? 51 : 128);
      printf("  R8_UINT target of the left half of slice 1's luma: %u, the right half %u (want 51, 128)\n", values[0], values[PL_W - 1]);
      others_ok &= planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, nv12, 0, values, &row) && values[5] == (BYTE)planar_value(5, 0, 0, 0, 0, 0);
      uv_ok &= planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, nv12, 3, values, &row);
      for (UINT i = 0; uv_ok && i < row * (PL_H / 2); ++i) uv_ok &= values[i] == ((i & 1) ? 9 : 7);
      printf("  R8G8_UINT clear of slice 0's chroma: %u %u (want 7 9)\n", values[0], values[1]);
      others_ok &= planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, nv12, 4, values, &row) && values[0] == (BYTE)planar_value(5, 1, 1, 0, 0, 0);
      CHECK(luma_ok);
      CHECK(uv_ok);
      CHECK(others_ok);
      /* (and what the integer target wrote is what the integer view of it reads, through the mirror kept up to date) */
      if (have_int) {
        const float any[4] = {0, 0, 0, 1};
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = plane_srv_desc(DXGI_FORMAT_R8_UINT, 0, 1, 1, 1);
        ID3D12Device_CreateShaderResourceView(dev, nv12, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + 6 * inc});
        const UINT32 px = planar_draw(c, &int_draw, views, (D3D12_GPU_DESCRIPTOR_HANDLE){gpu0.ptr + 6 * inc}, any, NULL, 64, 64, 3, 3);
        CHECK(px_red(px) == 51);
      }
      ID3D12DescriptorHeap_Release(rtvs);
    }
  }
  /* (a view of slices or planes the texture does not have reads nothing: its descriptor is a null one) */
  if (have_array && nv12) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = plane_srv_desc(DXGI_FORMAT_R8_UNORM, 0, 1, 2, 2);
    ID3D12Device_CreateShaderResourceView(dev, nv12, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){cpu0.ptr + 7 * inc});
  }
  if (have_array) planar_draw_free(&array_draw);
  if (have_int) planar_draw_free(&int_draw);
  if (have_write) planar_draw_free(&int_write);
  if (nv12) ID3D12Resource_Release(nv12);
  if (p010) ID3D12Resource_Release(p010);
  ID3D12DescriptorHeap_Release(views);
}

/* ---- reserved textures of other texel sizes, CPU-visible planar and packed YUV textures, packed YUV mips and arrays ------------------------------------------------- */

/* Subresource `sub` of `tex` from `data` (rows of `row_bytes`, tight) through an upload buffer laid out as GetCopyableFootprints says, or (`down`) back into it. */
static int sub_copy(Scene *c, ID3D12Resource *tex, UINT sub, BYTE *data, int down) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  ID3D12Device_GetCopyableFootprints(c->dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
  ID3D12Resource *buf = make_buffer(c->dev, down ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE,
                                    down ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ);
  BYTE *p = NULL;
  if (!buf) return 0;
  if (!down) {
    if (FAILED(ID3D12Resource_Map(buf, 0, NULL, (void **)&p)) || !p) { ID3D12Resource_Release(buf); return 0; }
    for (UINT y = 0; y < rows * fp.Footprint.Depth; ++y) memcpy(p + fp.Offset + (UINT64)y * fp.Footprint.RowPitch, data + (UINT64)y * row_bytes, (SIZE_T)row_bytes);
    ID3D12Resource_Unmap(buf, 0, NULL);
  }
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) { ID3D12Resource_Release(buf); return 0; }
  D3D12_TEXTURE_COPY_LOCATION t, b;
  memset(&t, 0, sizeof t);
  memset(&b, 0, sizeof b);
  t.pResource = tex;
  t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  t.SubresourceIndex = sub;
  b.pResource = buf;
  b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  b.PlacedFootprint = fp;
  if (down) ID3D12GraphicsCommandList_CopyTextureRegion(list, &b, 0, 0, 0, &t, NULL);
  else ID3D12GraphicsCommandList_CopyTextureRegion(list, &t, 0, 0, 0, &b, NULL);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  int ok = 1;
  if (down) {
    ok = SUCCEEDED(ID3D12Resource_Map(buf, 0, NULL, (void **)&p)) && p;
    for (UINT y = 0; ok && y < rows * fp.Footprint.Depth; ++y) memcpy(data + (UINT64)y * row_bytes, p + fp.Offset + (UINT64)y * fp.Footprint.RowPitch, (SIZE_T)row_bytes);
    if (p) ID3D12Resource_Unmap(buf, 0, NULL);
  }
  ID3D12Resource_Release(buf);
  return ok;
}
static UINT64 sub_bytes(ID3D12Device *dev, ID3D12Resource *tex, UINT sub) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  UINT rows = 0;
  UINT64 row_bytes = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  ID3D12Device_GetCopyableFootprints(dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes, NULL);
  return row_bytes * rows;
}
static UINT32 format_support2(ID3D12Device *dev, DXGI_FORMAT f) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {f, 0, 0};
  return SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ? (UINT32)fs.Support2 : 0xFFFFFFFFu;
}
static UINT32 format_support1(ID3D12Device *dev, DXGI_FORMAT f) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {f, 0, 0};
  return SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ? (UINT32)fs.Support1 : 0;
}

/* Reserved textures of 1, 2, 8 and 16-byte texels: Direct3D's tile shapes, the packed levels over several tiles, the tiles' bytes the texels of the format (CopyTiles in, a
 * copy of the level out); the video formats are not tiled. */
static void test_tiled_formats(Scene *c) {
  ID3D12Device *dev = c->dev;
  static const struct { DXGI_FORMAT f; UINT w, h; } shapes[] = {
      {DXGI_FORMAT_R8_UNORM, 256, 256}, {DXGI_FORMAT_R8G8_UNORM, 256, 128}, {DXGI_FORMAT_R16_UNORM, 256, 128}, {DXGI_FORMAT_R16G16_UNORM, 128, 128},
      {DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128}, {DXGI_FORMAT_R16G16B16A16_UNORM, 128, 64}, {DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64}};
  for (UINT i = 0; i < sizeof shapes / sizeof shapes[0]; ++i) {
    CHECK(format_support2(dev, shapes[i].f) & D3D12_FORMAT_SUPPORT2_TILED);
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 300, 200, 1, 1, shapes[i].f, {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *r = NULL;
    CHECK_HR(ID3D12Device_CreateReservedResource(dev, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r));
    if (!r) continue;
    UINT total = 0, nsub = 1;
    D3D12_TILE_SHAPE shape;
    D3D12_SUBRESOURCE_TILING tiling;
    D3D12_PACKED_MIP_INFO packed;
    ID3D12Device_GetResourceTiling(dev, r, &total, &packed, &shape, &nsub, 0, &tiling);
    const UINT across = (300 + shapes[i].w - 1) / shapes[i].w, down = (200 + shapes[i].h - 1) / shapes[i].h;
    const int standard = 300 >= shapes[i].w && 200 >= shapes[i].h;
    printf("tiled format %d: %ux%u tiles of %ux%u, %u in all\n", (int)shapes[i].f, tiling.WidthInTiles, tiling.HeightInTiles, shape.WidthInTexels, shape.HeightInTexels, total);
    CHECK(shape.WidthInTexels == shapes[i].w && shape.HeightInTexels == shapes[i].h && shape.DepthInTexels == 1);
    if (standard) CHECK(tiling.WidthInTiles == across && tiling.HeightInTiles == down && total == across * down && packed.NumPackedMips == 0);
    else CHECK(packed.NumPackedMips == 1 && packed.NumStandardMips == 0 && total == packed.NumTilesForPackedMips && total >= 1);
    ID3D12Resource_Release(r);
  }
  /* not tiled: the video formats (DXGI's tables), block-compressed and depth formats */
  static const DXGI_FORMAT untiled[] = {DXGI_FORMAT_NV12, DXGI_FORMAT_P010, DXGI_FORMAT_P016, DXGI_FORMAT_AYUV, DXGI_FORMAT_Y410, DXGI_FORMAT_Y416, DXGI_FORMAT_YUY2,
                                        DXGI_FORMAT_Y210, DXGI_FORMAT_Y216, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_D32_FLOAT};
  for (UINT i = 0; i < sizeof untiled / sizeof untiled[0]; ++i) {
    CHECK(!(format_support2(dev, untiled[i]) & D3D12_FORMAT_SUPPORT2_TILED));
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 1, untiled[i], {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                              untiled[i] == DXGI_FORMAT_D32_FLOAT ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *r = NULL;
    const HRESULT hr = ID3D12Device_CreateReservedResource(dev, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r);
    if (untiled[i] != DXGI_FORMAT_D32_FLOAT) CHECK(hr == E_INVALIDARG && !r);
    else CHECK(FAILED(hr) && !r);
    if (r) ID3D12Resource_Release(r);
  }

  {
    /* (levels of a 64-bit format - the kernel's 16-bit images have them on a device with levels of every image: 128 x 64 tiles, 8 of level 0 and 2 of level 1, no packed level) */
    D3D12_RESOURCE_DESC wd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 2, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                              D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *r = NULL;
    CHECK_HR(ID3D12Device_CreateReservedResource(dev, &wd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r));
    if (r) {
      UINT tiles = 0, count = 2;
      D3D12_PACKED_MIP_INFO packed;
      D3D12_TILE_SHAPE shape;
      D3D12_SUBRESOURCE_TILING tiling[2];
      ID3D12Device_GetResourceTiling(dev, r, &tiles, &packed, &shape, &count, 0, tiling);
      CHECK(tiles == 10 && packed.NumPackedMips == 0 && shape.WidthInTexels == 128 && shape.HeightInTexels == 64 && tiling[1].WidthInTiles == 1 && tiling[1].HeightInTiles == 2);
      ID3D12Resource_Release(r);
    }
  }
  /* R8 4096 x 64, 7 levels: none is a whole 256 x 256 tile, so all are packed - 349504 bytes over six tiles */
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4096, 64, 1, 7, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *tall = NULL;
  CHECK_HR(ID3D12Device_CreateReservedResource(dev, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tall));
  /* R8 512 x 256: two standard tiles side by side */
  td.Width = 512;
  td.Height = 256;
  td.MipLevels = 1;
  ID3D12Resource *wide = NULL;
  CHECK_HR(ID3D12Device_CreateReservedResource(dev, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&wide));
  D3D12_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.SizeInBytes = 8 * 65536;
  hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  hd.Properties.CreationNodeMask = hd.Properties.VisibleNodeMask = 1;
  hd.Alignment = 65536;
  ID3D12Heap *heap = NULL;
  CHECK_HR(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  ID3D12Resource *upload = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 6 * 65536, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 6 * 65536, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!tall || !wide || !heap || !upload || !readback) { CHECK(0); goto done; }
  UINT total = 0, nsub = 7;
  D3D12_PACKED_MIP_INFO packed;
  D3D12_TILE_SHAPE shape;
  D3D12_SUBRESOURCE_TILING tilings[7];
  ID3D12Device_GetResourceTiling(dev, tall, &total, &packed, &shape, &nsub, 0, tilings);
  printf("R8 4096x64x7: %u tiles, %u standard + %u packed levels in %u tiles from %u\n", total, packed.NumStandardMips, packed.NumPackedMips, packed.NumTilesForPackedMips,
         packed.StartTileIndexInOverallResource);
  CHECK(total == 6 && packed.NumStandardMips == 0 && packed.NumPackedMips == 7 && packed.NumTilesForPackedMips == 6 && packed.StartTileIndexInOverallResource == 0);
  CHECK(nsub == 7 && tilings[0].StartTileIndexInOverallResource == 0xFFFFFFFFu && shape.WidthInTexels == 256 && shape.HeightInTexels == 256);
  {
    /* the six tiles to heap tiles 1 .. 6; CopyTiles of a pattern in; level 1 (2048 x 32, from byte 262144 of the packed run: tile 4) copied out as R8 */
    D3D12_TILED_RESOURCE_COORDINATE start = {0, 0, 0, 0};
    D3D12_TILE_REGION_SIZE size = {6, FALSE, 0, 0, 0};
    D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_start = 1, count = 6;
    ID3D12CommandQueue_UpdateTileMappings(c->queue, tall, 1, &start, &size, heap, 1, &flags, &heap_start, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    BYTE *p = NULL;
    CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void **)&p));
    if (p) {
      for (UINT i = 0; i < 6 * 65536; ++i) p[i] = (BYTE)(i * 13 + (i >> 16) * 7 + 1);
      ID3D12Resource_Unmap(upload, 0, NULL);
    }
    ID3D12GraphicsCommandList *list = NULL;
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (list) {
      ID3D12GraphicsCommandList_CopyTiles(list, tall, &start, &size, upload, 0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
    }
    static BYTE level1[2048 * 32];
    CHECK(sub_copy(c, tall, 1, level1, 1));
    int same = 1;
    for (UINT i = 0; i < sizeof level1; ++i) {
      const UINT at = 262144 + i;
      same &= level1[i] == (BYTE)(at * 13 + (at >> 16) * 7 + 1);
    }
    printf("R8 packed level 1 through CopyTiles: %s\n", same ? "same" : "differs");
    CHECK(same);
    /* and back out of the tiles: the packed run as it went in (the last tile's 21824 bytes of it) */
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (list) {
      ID3D12GraphicsCommandList_CopyTiles(list, tall, &start, &size, readback, 0, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
    }
    const BYTE *q = NULL;
    CHECK_HR(ID3D12Resource_Map(readback, 0, NULL, (void **)&q));
    same = q != NULL;
    for (UINT i = 0; q && i < 349504; ++i) same &= q[i] == (BYTE)(i * 13 + (i >> 16) * 7 + 1);
    if (q) ID3D12Resource_Unmap(readback, 0, NULL);
    CHECK(same);
  }
  {
    /* R8 512 x 256: tile 1 (the right half) to heap tile 7, written through a placed... through CopyTiles of a ramp; level 0 copied out: the right half is the ramp, row by row
     * 256 texels, the left half (tile 0, nowhere) zero */
    D3D12_TILED_RESOURCE_COORDINATE start = {1, 0, 0, 0};
    D3D12_TILE_REGION_SIZE size = {1, FALSE, 0, 0, 0};
    D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_start = 7, count = 1;
    ID3D12CommandQueue_UpdateTileMappings(c->queue, wide, 1, &start, &size, heap, 1, &flags, &heap_start, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    ID3D12GraphicsCommandList *list = NULL;
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (list) {
      ID3D12GraphicsCommandList_CopyTiles(list, wide, &start, &size, upload, 3 * 65536, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
    }
    static BYTE level0[512 * 256];
    CHECK(sub_copy(c, wide, 0, level0, 1));
    int same = 1;
    for (UINT y = 0; y < 256; ++y)
      for (UINT x = 0; x < 512; ++x) {
        const UINT at = 3 * 65536 + y * 256 + (x - 256);
        same &= level0[y * 512 + x] == (x < 256 ? 0 : (BYTE)(at * 13 + (at >> 16) * 7 + 1));
      }
    printf("R8 512x256, tile 1 mapped: %s\n", same ? "right" : "wrong");
    CHECK(same);
  }
done:
  if (readback) ID3D12Resource_Release(readback);
  if (upload) ID3D12Resource_Release(upload);
  if (heap) ID3D12Heap_Release(heap);
  if (wide) ID3D12Resource_Release(wide);
  if (tall) ID3D12Resource_Release(tall);
}

/* Planar, packed YUV and narrow textures in a custom heap the CPU sees: each plane of each slice (each slice of a packed YUV array) a subresource of WriteToSubresource /
 * ReadFromSubresource, in the plane's format; what the CPU wrote is what the GPU copies out. */
static void test_cpu_planar(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES hp = ID3D12Device_GetCustomHeapProperties(dev, 0, D3D12_HEAP_TYPE_READBACK);
  static const DXGI_FORMAT formats[] = {DXGI_FORMAT_NV12, DXGI_FORMAT_P010, DXGI_FORMAT_P208, DXGI_FORMAT_V408};
  for (UINT f = 0; f < sizeof formats / sizeof formats[0]; ++f) {
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, PL_W, PL_H, 2, 1, formats[f], {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *tex = NULL;
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tex));
    if (!tex) continue;
    D3D12_FEATURE_DATA_FORMAT_INFO info = {formats[f], 0};
    CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &info, sizeof info));
    const UINT subs = info.PlaneCount * 2;
    void *pointer = (void *)1;
    CHECK(ID3D12Resource_Map(tex, 0, NULL, &pointer) == E_INVALIDARG && pointer == NULL);      /* (the driver's layout: no address) */
    CHECK_HR(ID3D12Resource_Map(tex, subs - 1, NULL, NULL));
    CHECK(ID3D12Resource_Map(tex, subs, NULL, NULL) == E_INVALIDARG);
    static BYTE data[6][PL_W * PL_H * 4], back[PL_W * PL_H * 4];
    int same = 1;
    for (UINT s = 0; s < subs; ++s) {
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
      UINT rows = 0;
      UINT64 row = 0;
      ID3D12Device_GetCopyableFootprints(dev, &td, s, 1, 0, &fp, &rows, &row, NULL);
      for (UINT i = 0; i < row * rows; ++i) data[s][i] = (BYTE)(s * 37 + i * 3 + f);
      CHECK_HR(ID3D12Resource_WriteToSubresource(tex, s, NULL, data[s], (UINT)row, (UINT)(row * rows)));
    }
    /* a box of the second plane of slice 1: the row after the first 2 texels of the next one, written from rows 40 bytes apart */
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp1;
    UINT rows1 = 0;
    UINT64 row1 = 0;
    ID3D12Device_GetCopyableFootprints(dev, &td, 3, 1, 0, &fp1, &rows1, &row1, NULL);
    const UINT texel = (UINT)(row1 / fp1.Footprint.Width);
    BYTE patch[2 * 40];
    for (UINT i = 0; i < sizeof patch; ++i) patch[i] = (BYTE)(0xC0 + i);
    const D3D12_BOX box = {2, 1, 0, 4, 3, 1};
    CHECK_HR(ID3D12Resource_WriteToSubresource(tex, 3, &box, patch, 40, 80));
    for (UINT y = 1; y < 3; ++y) memcpy(data[3] + y * row1 + 2 * texel, patch + (y - 1) * 40, 2 * texel);
    const D3D12_BOX outside = {0, 0, 0, fp1.Footprint.Width + 1, 1, 1};
    CHECK(ID3D12Resource_WriteToSubresource(tex, 3, &outside, patch, 40, 80) == E_INVALIDARG);
    for (UINT s = 0; s < subs; ++s) {
      const UINT64 n = sub_bytes(dev, tex, s);
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
      UINT rows = 0;
      UINT64 row = 0;
      ID3D12Device_GetCopyableFootprints(dev, &td, s, 1, 0, &fp, &rows, &row, NULL);
      memset(back, 0, sizeof back);
      CHECK_HR(ID3D12Resource_ReadFromSubresource(tex, back, (UINT)row, (UINT)n, s, NULL));
      same &= memcmp(back, data[s], (SIZE_T)n) == 0;
      memset(back, 0, sizeof back);
      CHECK(sub_copy(c, tex, s, back, 1));      /* (the GPU's copy of the plane: the same texels) */
      same &= memcmp(back, data[s], (SIZE_T)n) == 0;
    }
    printf("CPU-visible %d texture of %u planes x 2 slices: %s\n", (int)formats[f], info.PlaneCount, same ? "same" : "differs");
    CHECK(same);
    ID3D12Resource_Unmap(tex, subs - 1, NULL);
    ID3D12Resource_Release(tex);
  }
  /* a narrow format (R8, R16G16): the texels written are what is read and copied, not the kernel's widened pixels (two levels of the 8-bit ones; the kernel's 16-bit
   * images have one) */
  static const DXGI_FORMAT narrow[] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R8G8_UNORM};
  for (UINT f = 0; f < sizeof narrow / sizeof narrow[0]; ++f) {
    const UINT levels = narrow[f] == DXGI_FORMAT_R16G16_UNORM ? 1 : 2;
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 12, 6, 1, (UINT16)levels, narrow[f], {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *tex = NULL;
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tex));
    if (!tex) continue;
    int same = 1;
    for (UINT s = 0; s < levels; ++s) {
      BYTE data[12 * 6 * 4], back[12 * 6 * 4];
      const UINT64 n = sub_bytes(dev, tex, s), row = n / (s ? 3 : 6);
      for (UINT i = 0; i < n; ++i) data[i] = (BYTE)(i * 5 + s + 11 * f);
      CHECK_HR(ID3D12Resource_WriteToSubresource(tex, s, NULL, data, (UINT)row, (UINT)n));
      memset(back, 0, sizeof back);
      CHECK_HR(ID3D12Resource_ReadFromSubresource(tex, back, (UINT)row, (UINT)n, s, NULL));
      same &= memcmp(back, data, (SIZE_T)n) == 0;
      memset(back, 0, sizeof back);
      CHECK(sub_copy(c, tex, s, back, 1));
      same &= memcmp(back, data, (SIZE_T)n) == 0;
    }
    printf("CPU-visible narrow %d texture: %s\n", (int)narrow[f], same ? "same" : "differs");
    CHECK(same);
    ID3D12Resource_Release(tex);
  }
  /* packed YUV: an AYUV array of three slices and a YUY2 texture (its boxes on whole pairs) in the CPU's heap */
  D3D12_RESOURCE_DESC ad = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 4, 3, 1, DXGI_FORMAT_AYUV, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *ayuv = NULL, *yuy2 = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&ayuv));
  if (ayuv) {
    int same = 1;
    for (UINT s = 0; s < 3; ++s) {
      UINT32 px[4][8], back[4][8];
      for (UINT i = 0; i < 32; ++i) px[i / 8][i % 8] = 0x01020304u * (s + 1) + i;
      CHECK_HR(ID3D12Resource_WriteToSubresource(ayuv, s, NULL, px, sizeof px[0], sizeof px));
      CHECK_HR(ID3D12Resource_ReadFromSubresource(ayuv, back, sizeof back[0], sizeof back, s, NULL));
      same &= memcmp(px, back, sizeof px) == 0;
      memset(back, 0, sizeof back);
      CHECK(sub_copy(c, ayuv, s, (BYTE *)back, 1));
      same &= memcmp(px, back, sizeof px) == 0;
    }
    CHECK(ID3D12Resource_WriteToSubresource(ayuv, 3, NULL, &same, 32, 128) == E_INVALIDARG);
    printf("CPU-visible AYUV array: %s\n", same ? "same" : "differs");
    CHECK(same);
    ID3D12Resource_Release(ayuv);
  }
  ad.DepthOrArraySize = 1;
  ad.Format = DXGI_FORMAT_YUY2;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&yuy2));
  if (yuy2) {
    BYTE row[16 * 4];
    for (UINT i = 0; i < sizeof row; ++i) row[i] = (BYTE)(i + 100);
    const D3D12_BOX odd = {1, 0, 0, 3, 1, 1}, pair = {2, 1, 0, 6, 2, 1};
    CHECK(ID3D12Resource_WriteToSubresource(yuy2, 0, &odd, row, 16, 16) == E_INVALIDARG);
    CHECK_HR(ID3D12Resource_WriteToSubresource(yuy2, 0, &pair, row, 16, 16));
    BYTE back[4][16];
    CHECK_HR(ID3D12Resource_ReadFromSubresource(yuy2, back, 16, 64, 0, NULL));
    CHECK(memcmp(&back[1][4], row, 8) == 0 && back[0][4] == 0);
    ID3D12Resource_Release(yuy2);
  }
}

/* Packed YUV textures: mip levels for AYUV and Y416 (the others none, as DXGI's tables have it); arrays placed in a heap; render target views of a slice of an AYUV array;
 * a Texture2DArray view of several slices of one. */
static void test_packed_yuv(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  CHECK(format_support1(dev, DXGI_FORMAT_AYUV) & D3D12_FORMAT_SUPPORT1_MIP);
  CHECK(format_support1(dev, DXGI_FORMAT_Y416) & D3D12_FORMAT_SUPPORT1_MIP);      /* (as Windows has them: the kernel's 16-bit images have levels on a device with levels of every image) */
  CHECK(!(format_support1(dev, DXGI_FORMAT_YUY2) & D3D12_FORMAT_SUPPORT1_MIP) && !(format_support1(dev, DXGI_FORMAT_Y410) & D3D12_FORMAT_SUPPORT1_MIP));
  /* (mips: 1 made, 0 refused as Windows refuses it - E_INVALIDARG -, 2 not supported here - DXGI_ERROR_UNSUPPORTED) */
  static const struct { DXGI_FORMAT f; int mips; UINT bytes; } kinds[] = {{DXGI_FORMAT_AYUV, 1, 4}, {DXGI_FORMAT_Y416, 1, 8}, {DXGI_FORMAT_Y410, 0, 4},
                                                                         {DXGI_FORMAT_YUY2, 0, 4}, {DXGI_FORMAT_Y210, 0, 8}, {DXGI_FORMAT_Y216, 0, 8}};
  for (UINT k = 0; k < sizeof kinds / sizeof kinds[0]; ++k) {
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 1, 3, kinds[k].f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *t = NULL;
    const HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t);
    if (kinds[k].mips != 1) { CHECK(hr == (kinds[k].mips ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG) && !t); continue; }
    CHECK_HR(hr);
    if (!t) continue;
    /* level 1 (8 x 4) and level 2 (4 x 2) written and read back: each its own */
    int same = 1;
    for (UINT level = 1; level < 3; ++level) {
      BYTE data[8 * 4 * 8], back[8 * 4 * 8];
      const UINT64 n = sub_bytes(dev, t, level);
      for (UINT i = 0; i < n; ++i) data[i] = (BYTE)(i * 7 + level * 50 + k);
      same &= sub_copy(c, t, level, data, 0);
      memset(back, 0, sizeof back);
      same &= sub_copy(c, t, level, back, 1) && memcmp(back, data, (SIZE_T)n) == 0;
    }
    printf("packed YUV %d with 3 levels: %s\n", (int)kinds[k].f, same ? "kept" : "lost");
    CHECK(same);
    ID3D12Resource_Release(t);
  }
  /* (an array of levels: one array of the kernel's, on a device with levels of layered images - level 1 of slice 1, subresource 3, written and read back on its own) */
  D3D12_RESOURCE_DESC ad = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 2, 2, DXGI_FORMAT_AYUV, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *levels = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&levels));
  if (levels) {
    BYTE data[8 * 4 * 4], back[8 * 4 * 4], other[8 * 4 * 4];
    for (UINT i = 0; i < sizeof data; ++i) data[i] = (BYTE)(i * 3 + 1);
    memset(back, 0, sizeof back);
    memset(other, 0xEE, sizeof other);
    CHECK(sub_copy(c, levels, 3, data, 0) && sub_copy(c, levels, 3, back, 1) && memcmp(back, data, sizeof data) == 0);
    CHECK(sub_copy(c, levels, 1, other, 1) && other[0] == 0);      /* (level 1 of slice 0: its own, zeros) */
    ID3D12Resource_Release(levels);
  }

  /* two Y416 arrays of three slices placed side by side in one heap keep their own slices */
  ad.Format = DXGI_FORMAT_Y416;
  ad.DepthOrArraySize = 3;
  ad.MipLevels = 1;
  const D3D12_RESOURCE_ALLOCATION_INFO info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &ad);
  CHECK(info.SizeInBytes >= 3 * 16 * 8 * 8 && info.SizeInBytes % 65536 == 0);
  D3D12_HEAP_DESC hd = {2 * info.SizeInBytes, {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1}, 0, D3D12_HEAP_FLAG_NONE};
  ID3D12Heap *heap = NULL;
  ID3D12Resource *placed[2] = {NULL, NULL};
  CHECK_HR(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  for (int i = 0; heap && i < 2; ++i)
    CHECK_HR(ID3D12Device_CreatePlacedResource(dev, heap, (UINT64)i * info.SizeInBytes, &ad, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&placed[i]));
  if (placed[0] && placed[1]) {
    static BYTE data[2][3][16 * 8 * 8], back[16 * 8 * 8];
    for (int i = 0; i < 2; ++i)
      for (UINT s = 0; s < 3; ++s) {
        for (UINT b = 0; b < sizeof data[i][s]; ++b) data[i][s][b] = (BYTE)(b * 3 + s * 40 + i * 101);
        CHECK(sub_copy(c, placed[i], s, data[i][s], 0));
      }
    int same = 1;
    for (int i = 0; i < 2; ++i)
      for (UINT s = 0; s < 3; ++s) same &= sub_copy(c, placed[i], s, back, 1) && memcmp(back, data[i][s], sizeof back) == 0;
    printf("placed Y416 arrays side by side: %s\n", same ? "apart" : "mixed");
    CHECK(same);
  }
  for (int i = 0; i < 2; ++i) if (placed[i]) ID3D12Resource_Release(placed[i]);
  if (heap) ID3D12Heap_Release(heap);

  /* an AYUV array of three slices: a render target view of slice 1 cleared, the others kept; a Texture2DArray view of slices 1 and 2 reads each */
  ad.Format = DXGI_FORMAT_AYUV;
  ad.Width = PL_W;
  ad.Height = PL_H;
  ad.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ID3D12Resource *arr = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&arr));
  D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ID3D12DescriptorHeap *rtvs = NULL, *srvs = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&srvs));
  if (arr && rtvs && srvs) {
    static UINT32 slices[3][PL_W * PL_H], back[PL_W * PL_H];
    for (UINT s = 0; s < 3; ++s) {
      for (UINT i = 0; i < PL_W * PL_H; ++i) slices[s][i] = 0xFF000000u | (0x30 + 0x40 * s) << 16 | (0x20 + 0x10 * s) << 8 | (0x10 + 0x20 * s);
      CHECK(sub_copy(c, arr, s, (BYTE *)slices[s], 0));
    }
    D3D12_RENDER_TARGET_VIEW_DESC rv;
    memset(&rv, 0, sizeof rv);
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rv.Texture2DArray.FirstArraySlice = 1;
    rv.Texture2DArray.ArraySize = 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, arr, &rv, rtv);
    ID3D12GraphicsCommandList *list = NULL;
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (list) {
      const float colour[4] = {1.0f, 0.0f, 0.5f, 1.0f};      /* (V 255, U 0, Y 128, A 255) */
      ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, colour, 0, NULL);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
    }
    int ok = sub_copy(c, arr, 1, (BYTE *)back, 1);
    printf("AYUV array slice 1 cleared through its render target view: %08x\n", (unsigned)back[0]);
    for (UINT i = 0; ok && i < PL_W * PL_H; ++i) ok &= (back[i] & 0xFF00FFFFu) == 0xFF0000FFu && ((back[i] >> 16) & 0xFF) >= 127 && ((back[i] >> 16) & 0xFF) <= 128;
    CHECK(ok);
    ok = sub_copy(c, arr, 0, (BYTE *)back, 1) && memcmp(back, slices[0], sizeof back) == 0 && sub_copy(c, arr, 2, (BYTE *)back, 1) && memcmp(back, slices[2], sizeof back) == 0;
    CHECK(ok);
    /* the array view: slices 1 and 2 through a Texture2DArray shader (the colour's z picks the slice of the view) */
    PlanarDraw draw;
    if (planar_draw_make(c, &draw, dx_vs_vol_dxbc, sizeof dx_vs_vol_dxbc, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, DXGI_FORMAT_B8G8R8A8_UNORM, 1)) {
      D3D12_SHADER_RESOURCE_VIEW_DESC sv = plane_srv_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 1, 2);
      ID3D12Device_CreateShaderResourceView(dev, arr, &sv, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srvs));
      const D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srvs);
      const float at0[4] = {0.5f, 0.5f, 0.0f, 1}, at1[4] = {0.5f, 0.5f, 1.0f, 1};
      const UINT32 p0 = planar_draw(c, &draw, srvs, gpu, at0, NULL, 64, 64, 32, 32), p1 = planar_draw(c, &draw, srvs, gpu, at1, NULL, 64, 64, 32, 32);
      printf("AYUV array view of slices 1, 2: %d %d %d / %d %d %d\n", px_red(p0), px_green(p0), px_blue(p0), px_red(p1), px_green(p1), px_blue(p1));
      /* (the B8G8R8A8 target shows the R8G8B8A8 view's red as red: slice 1 is the clear - V 255, U 0, Y 128 -, slice 2 its pattern) */
      CHECK(px_red(p0) == 255 && px_green(p0) == 0 && abs(px_blue(p0) - 128) <= 1);
      CHECK(px_red(p1) == (int)(slices[2][0] & 0xFF) && px_green(p1) == (int)((slices[2][0] >> 8) & 0xFF) && px_blue(p1) == (int)((slices[2][0] >> 16) & 0xFF));
      planar_draw_free(&draw);
    } else CHECK(0);
  }
  if (rtvs) ID3D12DescriptorHeap_Release(rtvs);
  if (srvs) ID3D12DescriptorHeap_Release(srvs);
  if (arr) ID3D12Resource_Release(arr);
}


/* ---- unordered access to textures, and drawing into layers ------------------------------------------------------------------------------------------------------------ */

/* A texture of the default heap: `dim` (TEXTURE1D / 2D / 3D), `layers` the array size or the depth. */
static ID3D12Resource *uav_texture(ID3D12Device *dev, D3D12_RESOURCE_DIMENSION dim, UINT w, UINT h, UINT layers, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {dim, 0, w, h, (UINT16)layers, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  ID3D12Resource *t = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t));
  return t;
}
/* Subresource `sub` read back: its rows (of a volume, every slice's) one after the other, `texel` bytes a texel, into `out`. */
static int texture_read(Scene *c, ID3D12Resource *tex, UINT sub, void *out, UINT texel) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  ID3D12Device_GetCopyableFootprints(c->dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
  ID3D12Resource *down = make_buffer(c->dev, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!down || !list) { if (down) ID3D12Resource_Release(down); if (list) ID3D12GraphicsCommandList_Release(list); return 0; }
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = down;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = fp;
  from.pResource = tex;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = sub;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  const BYTE *p = NULL;
  const int ok = SUCCEEDED(ID3D12Resource_Map(down, 0, NULL, (void **)&p)) && p;
  const UINT row = fp.Footprint.Width * texel;
  for (UINT z = 0; ok && z < fp.Footprint.Depth; ++z)
    for (UINT y = 0; y < rows; ++y) memcpy((BYTE *)out + ((SIZE_T)z * rows + y) * row, p + fp.Offset + ((UINT64)z * rows + y) * fp.Footprint.RowPitch, row);
  if (p) ID3D12Resource_Unmap(down, 0, NULL);
  ID3D12Resource_Release(down);
  return ok;
}
/* Rows of `texel`-byte texels into subresource `sub` (all of it). */
static void texture_write(Scene *c, ID3D12Resource *tex, UINT sub, const void *data, UINT texel) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  ID3D12Device_GetCopyableFootprints(c->dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
  ID3D12Resource *up = make_buffer(c->dev, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  BYTE *p = NULL;
  if (!up || FAILED(ID3D12Resource_Map(up, 0, NULL, (void **)&p)) || !p) { CHECK(0); if (up) ID3D12Resource_Release(up); return; }
  const UINT row = fp.Footprint.Width * texel;
  for (UINT z = 0; z < fp.Footprint.Depth; ++z)
    for (UINT y = 0; y < rows; ++y) memcpy(p + fp.Offset + ((UINT64)z * rows + y) * fp.Footprint.RowPitch, (const BYTE *)data + ((SIZE_T)z * rows + y) * row, row);
  ID3D12Resource_Unmap(up, 0, NULL);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (list) {
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    to.pResource = tex;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = sub;
    from.pResource = up;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
    scene_run(c, list);
    ID3D12GraphicsCommandList_Release(list);
  }
  ID3D12Resource_Release(up);
}
/* A root signature of a table of `count` unordered access views from u<first> (and, for a draw, the vertex shader's matrix first and the input layout). */
static ID3D12RootSignature *uav_signature(ID3D12Device *dev, UINT first, UINT count, int graphics) {
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, count, first, 0, 0};
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 16;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 0, NULL, graphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = NULL, *error = NULL;
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) {
    CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);
  }
  return rs;
}
static ID3D12PipelineState *compute_pso(ID3D12Device *dev, ID3D12RootSignature *rs, const void *code, SIZE_T bytes) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
  memset(&cd, 0, sizeof cd);
  cd.pRootSignature = rs;
  cd.CS.pShaderBytecode = code;
  cd.CS.BytecodeLength = bytes;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso));
  return pso;
}
/* A UAV of a texture into slot `i` of a shader-visible heap (and of the CPU heap ClearUnorderedAccessView* takes the same descriptor from). */
static void put_uav(ID3D12Device *dev, ID3D12DescriptorHeap *heap, ID3D12DescriptorHeap *cpu_heap, UINT i, ID3D12Resource *r, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc) {
  const UINT step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
  at.ptr += (SIZE_T)i * step;
  ID3D12Device_CreateUnorderedAccessView(dev, r, NULL, desc, at);
  if (cpu_heap) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    c.ptr += (SIZE_T)i * step;
    ID3D12Device_CreateUnorderedAccessView(dev, r, NULL, desc, c);
  }
}
static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(ID3D12Device *dev, ID3D12DescriptorHeap *heap, UINT i) {
  D3D12_GPU_DESCRIPTOR_HANDLE g = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
  g.ptr += (UINT64)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return g;
}
static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12Device *dev, ID3D12DescriptorHeap *heap, UINT i) {
  D3D12_CPU_DESCRIPTOR_HANDLE c = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
  c.ptr += (SIZE_T)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return c;
}
static ID3D12DescriptorHeap *view_heap(ID3D12Device *dev, UINT count, int visible) {
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, count, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *h = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&h));
  return h;
}
static BYTE unorm8(float v) { return (BYTE)(v <= 0 ? 0 : v >= 1 ? 255 : (UINT)(v * 255.0f + 0.5f)); }

/* The format support of unordered access to textures: typed loads and stores of the formats a texture view can have, the atomics of R32_UINT / R32_SINT. */
static void test_uav_format_support(ID3D12Device *dev) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT f = {DXGI_FORMAT_R32_UINT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  CHECK((f.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) &&
        (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD) && (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE));
  f.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  CHECK((f.Support2 & (D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) == (D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) &&
        !(f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD));
  f.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  CHECK(!(f.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && !(f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
  f.Format = DXGI_FORMAT_NV12;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  CHECK((f.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
  /* (TypedUAVLoadAdditionalFormats: typed loads of every format a typed unordered access view can have - the typed views of buffers read them all, where the adapter has them) */
  D3D12_FEATURE_DATA_D3D12_OPTIONS o;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o));
  f.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  CHECK(!o.TypedUAVLoadAdditionalFormats == !(f.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) && !o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation);
}

/* Compute shaders writing and reading a 2D array storage image, typed loads of R32_FLOAT, the atomics on R32_UINT / R32_SINT images of one, two and three dimensions, and
 * ClearUnorderedAccessView* of a texture's view (a rectangle of a layer). */
static void test_texture_uavs(Scene *c) {
  ID3D12Device *dev = c->dev;
  test_uav_format_support(dev);
  const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource *picture = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 3, DXGI_FORMAT_R8G8B8A8_UNORM, uav);
  ID3D12Resource *packed = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 1, DXGI_FORMAT_R32_UINT, uav);
  ID3D12Resource *halves = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 1, DXGI_FORMAT_R32_TYPELESS, uav);
  ID3D12Resource *counts = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 1, 1, DXGI_FORMAT_R32_UINT, uav);
  ID3D12Resource *least = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4, 1, 1, DXGI_FORMAT_R32_SINT, uav);
  ID3D12Resource *flags = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4, 1, 1, DXGI_FORMAT_R32_TYPELESS, uav);
  ID3D12Resource *most = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 2, 2, 2, DXGI_FORMAT_R32_SINT, uav);
  /* (no unordered access without the flag; none of an sRGB format) */
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC srgb = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, uav};
  ID3D12Resource *refused = NULL;
  CHECK(FAILED(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &srgb, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&refused)) && !refused);
  ID3D12RootSignature *rs4 = uav_signature(dev, 0, 4, 0);
  ID3D12PipelineState *fill = compute_pso(dev, rs4, dx_cs_uav_array_dxbc, sizeof dx_cs_uav_array_dxbc);
  ID3D12PipelineState *load = compute_pso(dev, rs4, dx_cs_uav_load_dxbc, sizeof dx_cs_uav_load_dxbc);
  ID3D12PipelineState *atomic = compute_pso(dev, rs4, dx_cs_uav_atomic_dxbc, sizeof dx_cs_uav_atomic_dxbc);
  ID3D12DescriptorHeap *heap = view_heap(dev, 16, 1), *cpu = view_heap(dev, 16, 0);
  if (!picture || !packed || !halves || !counts || !least || !flags || !most || !rs4 || !fill || !load || !atomic || !heap || !cpu) { CHECK(0); return; }
  /* halves starts as 2 * i at texel i */
  float start[64];
  for (UINT i = 0; i < 64; ++i) start[i] = 2.0f * (float)i;
  texture_write(c, halves, 0, start, 4);
  D3D12_UNORDERED_ACCESS_VIEW_DESC u;
  memset(&u, 0, sizeof u);
  u.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  u.Texture2DArray.ArraySize = 3;
  put_uav(dev, heap, cpu, 0, picture, &u);
  put_uav(dev, heap, cpu, 1, packed, NULL);
  u.Format = DXGI_FORMAT_R32_FLOAT;      /* (a typed view of a typeless texture) */
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  put_uav(dev, heap, cpu, 2, halves, &u);
  put_uav(dev, heap, cpu, 3, NULL, NULL);
  /* the atomics' views: 4 .. 7 */
  put_uav(dev, heap, cpu, 4, counts, NULL);
  put_uav(dev, heap, cpu, 5, least, NULL);
  u.Format = DXGI_FORMAT_R32_UINT;
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
  put_uav(dev, heap, cpu, 6, flags, &u);
  memset(&u, 0, sizeof u);
  u.Format = DXGI_FORMAT_R32_SINT;
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
  u.Texture3D.WSize = (UINT)-1;
  put_uav(dev, heap, cpu, 7, most, &u);
  /* a view of layer 1 of the picture, for a clear: 8 */
  memset(&u, 0, sizeof u);
  u.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  u.Texture2DArray.FirstArraySlice = 1;
  u.Texture2DArray.ArraySize = 1;
  put_uav(dev, heap, cpu, 8, picture, &u);

  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, fill, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  /* (the atomics' images cleared first: 0 counts and bits, the largest int for the minimum, the smallest for the maximum) */
  const UINT zero[4] = {0, 0, 0, 0}, big[4] = {0x7FFFFFFFu, 0, 0, 0}, small[4] = {0x80000000u, 0, 0, 0};
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 4), heap_cpu(dev, cpu, 4), counts, zero, 0, NULL);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 5), heap_cpu(dev, cpu, 5), least, big, 0, NULL);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 6), heap_cpu(dev, cpu, 6), flags, zero, 0, NULL);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 7), heap_cpu(dev, cpu, 7), most, small, 0, NULL);
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs4);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 3);
  D3D12_RESOURCE_BARRIER b;
  memset(&b, 0, sizeof b);
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = picture;
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
  ID3D12GraphicsCommandList_SetPipelineState(list, load);
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
  ID3D12GraphicsCommandList_SetPipelineState(list, atomic);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 4));
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
  /* a rectangle of layer 1 of the picture cleared to red */
  const float red[4] = {1, 0, 0, 1};
  const D3D12_RECT rect = {2, 2, 4, 4};
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, heap_gpu(dev, heap, 8), heap_cpu(dev, cpu, 8), picture, red, 1, &rect);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);

  BYTE texels[3][8 * 8 * 4];
  int wrong = 0;
  for (UINT layer = 0; layer < 3; ++layer) {
    CHECK(texture_read(c, picture, layer, texels[layer], 4));
    for (UINT y = 0; y < 8; ++y)
      for (UINT x = 0; x < 8; ++x) {
        const BYTE *t = &texels[layer][(y * 8 + x) * 4];
        const int cleared = layer == 1 && x >= 2 && x < 4 && y >= 2 && y < 4;
        const BYTE want[4] = {cleared ? 255 : unorm8(x / 8.0f), cleared ? 0 : unorm8(y / 8.0f), cleared ? 0 : unorm8(layer / 3.0f), 255};
        if (memcmp(t, want, 4) != 0) {
          if (!wrong++) printf("  storage image: layer %u (%u, %u) %u %u %u %u, want %u %u %u %u\n", layer, x, y, t[0], t[1], t[2], t[3], want[0], want[1], want[2], want[3]);
        }
      }
  }
  CHECK(wrong == 0);
  UINT words[64];
  float floats[64];
  CHECK(texture_read(c, packed, 0, words, 4));
  wrong = 0;
  for (UINT i = 0; i < 64; ++i) {
    const UINT x = i % 8, y = i / 8;
    const UINT want = unorm8(x / 8.0f) | (UINT)unorm8(y / 8.0f) << 8 | (UINT)unorm8(2 / 3.0f) << 16 | 255u << 24;
    if (words[i] != want && !wrong++) printf("  typed load: texel %u %08x, want %08x\n", i, words[i], want);
  }
  CHECK(wrong == 0);
  CHECK(texture_read(c, halves, 0, floats, 4));
  wrong = 0;
  for (UINT i = 0; i < 64; ++i)
    if (floats[i] != (float)i && !wrong++) printf("  R32_FLOAT halved: texel %u %g, want %u\n", i, floats[i], i);
  CHECK(wrong == 0);
  UINT got[8];
  INT signed_got[8];
  CHECK(texture_read(c, counts, 0, got, 4));
  printf("texture atomics: counts %u %u %u %u\n", got[0], got[1], got[2], got[3]);
  CHECK(got[0] == 16 && got[1] == 16 && got[2] == 16 && got[3] == 16);
  CHECK(texture_read(c, least, 0, signed_got, 4));
  printf("texture atomics: least %d %d %d %d\n", signed_got[0], signed_got[1], signed_got[2], signed_got[3]);
  CHECK(signed_got[0] == -60 && signed_got[1] == -61 && signed_got[2] == -62 && signed_got[3] == -63);
  CHECK(texture_read(c, flags, 0, got, 4));
  printf("texture atomics: bits %08x, compare-exchange %u, exchange %u (old %u)\n", got[0], got[1], got[2], got[3]);
  CHECK(got[0] == 0xFFFFFFFFu && got[1] >= 1 && got[1] <= 64 && got[2] >= 100 && got[2] <= 163 && (got[3] == 0 || (got[3] >= 100 && got[3] <= 162)));
  if (!g_hardware) CHECK(got[1] == 1 && got[2] == 163 && got[3] == 162);      /* (the CPU runs the threads in order) */
  CHECK(texture_read(c, most, 0, signed_got, 4));
  wrong = 0;
  for (UINT i = 0; i < 8; ++i)
    if (signed_got[i] != (INT)(56 + i) && !wrong++) printf("  3D maximum: %u %d\n", i, signed_got[i]);
  CHECK(wrong == 0);
  ID3D12DescriptorHeap_Release(cpu);
  ID3D12DescriptorHeap_Release(heap);
  ID3D12PipelineState_Release(atomic);
  ID3D12PipelineState_Release(load);
  ID3D12PipelineState_Release(fill);
  ID3D12RootSignature_Release(rs4);
  ID3D12Resource *all[] = {picture, packed, halves, counts, least, flags, most};
  for (UINT i = 0; i < sizeof all / sizeof all[0]; ++i) ID3D12Resource_Release(all[i]);
}

/* A pixel shader writing storage images beside its render target (u1, u2): every pixel it shades marks its place, and an atomic counts them. */
static void test_pixel_uavs(Scene *c) {
  ID3D12Device *dev = c->dev;
  const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource *marks = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 1, DXGI_FORMAT_R32_UINT, uav);
  ID3D12Resource *total = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 1, 1, DXGI_FORMAT_R32_UINT, uav);
  ID3D12RootSignature *rs = uav_signature(dev, 1, 2, 1);
  ID3D12DescriptorHeap *heap = view_heap(dev, 2, 1), *cpu = view_heap(dev, 2, 0);
  if (!marks || !total || !rs || !heap || !cpu) { CHECK(0); return; }
  put_uav(dev, heap, cpu, 0, marks, NULL);
  put_uav(dev, heap, cpu, 1, total, NULL);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_uav_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_uav_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!pso || !list) { CHECK(0); return; }
  const UINT zero[4] = {0, 0, 0, 0};
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 0), heap_cpu(dev, cpu, 0), marks, zero, 0, NULL);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 1), heap_cpu(dev, cpu, 1), total, zero, 0, NULL);
  scene_begin(c, list, 1);
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(c->vb), 256, 28};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_read_target(c, list);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  const int shaded = scene_coloured(c, 0, 0, 64, 64);
  UINT count = 0;
  static UINT placed[64 * 64];
  CHECK(texture_read(c, total, 0, &count, 4));
  CHECK(texture_read(c, marks, 0, placed, 4));
  const UINT32 *pixels = NULL;
  int wrong = 0, marked = 0;
  if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
    for (UINT i = 0; i < 64 * 64; ++i) {
      const int lit = (pixels[i] & 0xFFFFFF) != 0;
      marked += placed[i] != 0;
      if ((lit ? placed[i] != i + 1 : placed[i] != 0) && !wrong++) printf("  pixel shader storage image: pixel %u lit %d, mark %u\n", i, lit, placed[i]);
    }
    ID3D12Resource_Unmap(c->readback, 0, NULL);
  }
  printf("pixel shader storage images: %d pixels drawn, %u counted, %d marked\n", shaded, count, marked);
  CHECK(shaded > 1000 && count == (UINT)shaded && marked == shaded && wrong == 0);
  ID3D12PipelineState_Release(pso);
  ID3D12DescriptorHeap_Release(cpu);
  ID3D12DescriptorHeap_Release(heap);
  ID3D12RootSignature_Release(rs);
  ID3D12Resource_Release(total);
  ID3D12Resource_Release(marks);
}

/* ---- buffers: typed views, append and consume views and their counters, slots far up, draws that write only unordered access views -------------------------------------------- */

/* `bytes` of `data` into a buffer of the GPU's memory (by way of an upload buffer and a copy), and back out of one. */
static void buffer_upload(Scene *c, ID3D12Resource *to, UINT64 offset, const void *data, UINT bytes) {
  ID3D12Resource *up = make_buffer(c->dev, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *p = NULL;
  if (!up || FAILED(ID3D12Resource_Map(up, 0, NULL, &p)) || !p) { CHECK(0); return; }
  memcpy(p, data, bytes);
  ID3D12Resource_Unmap(up, 0, NULL);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (list) {
    ID3D12GraphicsCommandList_CopyBufferRegion(list, to, offset, up, 0, bytes);
    scene_run(c, list);
    ID3D12GraphicsCommandList_Release(list);
  }
  ID3D12Resource_Release(up);
}
static int buffer_download(Scene *c, ID3D12Resource *from, UINT64 offset, void *out, UINT bytes) {
  ID3D12Resource *back = make_buffer(c->dev, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList *list = NULL;
  int ok = 0;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (back && list) {
    ID3D12GraphicsCommandList_CopyBufferRegion(list, back, 0, from, offset, bytes);
    scene_run(c, list);
    void *p = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(back, 0, NULL, &p)) && p) { memcpy(out, p, bytes); ID3D12Resource_Unmap(back, 0, NULL); ok = 1; }
  }
  if (list) ID3D12GraphicsCommandList_Release(list);
  if (back) ID3D12Resource_Release(back);
  return ok;
}
/* A root signature of one table of `count` ranges (each one register of `types[k]` - SRV or UAV - at `regs[k]`, in table order) and 16 root constants first. */
static ID3D12RootSignature *ranges_signature(ID3D12Device *dev, UINT count, const D3D12_DESCRIPTOR_RANGE_TYPE *types, const UINT *regs, const UINT *sizes, int graphics) {
  D3D12_DESCRIPTOR_RANGE ranges[8];
  UINT offset = 0;
  for (UINT k = 0; k < count && k < 8; ++k) {
    D3D12_DESCRIPTOR_RANGE r = {types[k], sizes ? sizes[k] : 1, regs[k], 0, offset};
    ranges[k] = r;
    offset += r.NumDescriptors;
  }
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 16;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = count;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 0, NULL, graphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = NULL, *error = NULL;
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) {
    CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);
  }
  if (error) ID3D10Blob_Release(error);
  return rs;
}
static void buffer_srv(ID3D12Device *dev, ID3D12DescriptorHeap *heap, UINT i, ID3D12Resource *r, DXGI_FORMAT format, UINT first, UINT count, UINT stride, UINT flags) {
  D3D12_SHADER_RESOURCE_VIEW_DESC s;
  memset(&s, 0, sizeof s);
  s.Format = format;
  s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  s.Buffer.FirstElement = first;
  s.Buffer.NumElements = count;
  s.Buffer.StructureByteStride = stride;
  s.Buffer.Flags = flags;
  ID3D12Device_CreateShaderResourceView(dev, r, &s, heap_cpu(dev, heap, i));
}
static void buffer_uav(ID3D12Device *dev, ID3D12DescriptorHeap *heap, ID3D12DescriptorHeap *cpu, UINT i, ID3D12Resource *r, ID3D12Resource *counter, UINT64 counter_offset, DXGI_FORMAT format,
                       UINT first, UINT count, UINT stride, UINT flags) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC u;
  memset(&u, 0, sizeof u);
  u.Format = format;
  u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  u.Buffer.FirstElement = first;
  u.Buffer.NumElements = count;
  u.Buffer.StructureByteStride = stride;
  u.Buffer.CounterOffsetInBytes = counter_offset;
  u.Buffer.Flags = flags;
  ID3D12Device_CreateUnorderedAccessView(dev, r, counter, &u, heap_cpu(dev, heap, i));
  if (cpu) ID3D12Device_CreateUnorderedAccessView(dev, r, counter, &u, heap_cpu(dev, cpu, i));
}
static UINT16 half_of(float f) {
  /* (round to nearest even; the values here are normal) */
  UINT32 b;
  memcpy(&b, &f, 4);
  const UINT32 sign = (b >> 16) & 0x8000u;
  const INT32 exponent = (INT32)((b >> 23) & 0xFF) - 127 + 15;
  if (exponent <= 0) return (UINT16)sign;
  UINT32 h = sign | ((UINT32)exponent << 10) | ((b >> 13) & 0x3FF);
  const UINT32 rest = b & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;
  return (UINT16)h;
}
static float float_of_half(UINT16 h) {
  const UINT32 exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v = exponent == 0 ? (float)mantissa / 1024.0f / 16384.0f : (1.0f + (float)mantissa / 1024.0f) * (float)(1u << exponent) / 32768.0f;
  return (h & 0x8000) ? -v : v;
}

/* Typed views of buffers in a compute shader (cs_typed_buffer: R8G8B8A8_UNORM and R16G16_FLOAT shader resource views of t2 and t9, an R16G16B16A16_FLOAT unordered access view of u5
 * stored and loaded, atomics on an R32_UINT one of u1), the buffer formats' support, and a typed buffer view cleared with ClearUnorderedAccessViewUint. */
static void test_typed_buffers(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT f = {DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  const int typed = (f.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) != 0;
  if (!typed) { printf("typed buffers: not on this adapter\n"); return; }
  CHECK((f.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD));
  f.Format = DXGI_FORMAT_R32G32B32_FLOAT;      /* (read, not stored) */
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f));
  CHECK((f.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) && !(f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
  D3D12_FEATURE_DATA_D3D12_OPTIONS o;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o));
  CHECK(o.TypedUAVLoadAdditionalFormats);
  const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource *colours = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *halves = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 20, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *counts = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 32, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *results = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 40, uav, D3D12_RESOURCE_STATE_COMMON);
  const D3D12_DESCRIPTOR_RANGE_TYPE types[4] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV};
  const UINT regs[4] = {2, 9, 1, 5};
  ID3D12RootSignature *rs = ranges_signature(dev, 4, types, regs, NULL, 0);
  ID3D12PipelineState *pso = rs ? compute_pso(dev, rs, dx_cs_typed_buffer_dxbc, sizeof dx_cs_typed_buffer_dxbc) : NULL;
  ID3D12DescriptorHeap *heap = view_heap(dev, 4, 1), *cpu = view_heap(dev, 4, 0);
  if (!colours || !halves || !counts || !results || !rs || !pso || !heap || !cpu) { CHECK(0); return; }
  BYTE colour_bytes[16];
  UINT16 half_words[10] = {0};
  for (UINT i = 0; i < 4; ++i) {
    colour_bytes[i * 4] = (BYTE)(i * 40), colour_bytes[i * 4 + 1] = 100, colour_bytes[i * 4 + 2] = 0, colour_bytes[i * 4 + 3] = 255;
    half_words[2 + i * 2] = half_of(0.5f * (float)i), half_words[3 + i * 2] = half_of(2.0f + (float)i);
  }
  buffer_upload(c, colours, 0, colour_bytes, 16);
  buffer_upload(c, halves, 0, half_words, 20);
  buffer_srv(dev, heap, 0, colours, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 4, 0, 0);
  buffer_srv(dev, heap, 1, halves, DXGI_FORMAT_R16G16_FLOAT, 1, 4, 0, 0);
  buffer_uav(dev, heap, cpu, 2, counts, NULL, 0, DXGI_FORMAT_R32_UINT, 0, 8, 0, 0);
  buffer_uav(dev, heap, cpu, 3, results, NULL, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 4, 0, 0);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  /* (the typed views cleared: the counts to 0, the results to 7, elements of their format) */
  const UINT zero[4] = {0, 0, 0, 0};
  const float seven[4] = {7, 7, 7, 7};
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 2), heap_cpu(dev, cpu, 2), counts, zero, 0, NULL);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, heap_gpu(dev, heap, 3), heap_cpu(dev, cpu, 3), results, seven, 0, NULL);
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  UINT16 r[20];
  UINT k[8];
  CHECK(buffer_download(c, results, 0, r, 40) && buffer_download(c, counts, 0, k, 32));
  int wrong = 0;
  wrong += r[0] != 0 || r[1] != 0 || r[2] != 0 || r[3] != 0;      /* (element 0 is outside the view: not cleared, not written) */
  for (UINT i = 0; i < 4; ++i) {
    const UINT16 *e = &r[4 + i * 4];
    const float want_x = float_of_half(half_of((float)(i * 40) / 255.0f + 0.5f * (float)i));
    const float got[4] = {float_of_half(e[0]), float_of_half(e[1]), float_of_half(e[2]), float_of_half(e[3])};
    const int bad = fabsf(got[0] - want_x) > 0.002f || fabsf(got[1] - 200.0f / 255.0f) > 0.002f || got[2] != 2.0f + (float)i || got[3] != 4.0f ||
                    k[2 + i] != (UINT)(got[0] * 100.0f + 0.5f) + 4000;
    if (bad && !wrong) printf("  typed buffers: element %u %f %f %f %f, count %u\n", i, got[0], got[1], got[2], got[3], k[2 + i]);
    wrong += bad;
  }
  if (k[0] != 10 || k[1] != 30) printf("  typed buffers: atomics %u %u\n", k[0], k[1]);
  CHECK(wrong == 0 && k[0] == 10 && k[1] == 30 && k[6] == 0 && k[7] == 0);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D12DescriptorHeap_Release(cpu);
  ID3D12DescriptorHeap_Release(heap);
  ID3D12Resource_Release(results);
  ID3D12Resource_Release(counts);
  ID3D12Resource_Release(halves);
  ID3D12Resource_Release(colours);
}

/* Append, consume and counter views with their counters in a buffer of their own (cs_append: imm_atomic_alloc / imm_atomic_consume, bufinfo), a structured view of u60 and a raw one of
 * t100: the counters start where an upload put them and the counts the shader left are copied out (CopyBufferRegion of the counter resource). */
static void test_counters(Scene *c) {
  ID3D12Device *dev = c->dev;
  const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource *appended = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 64, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *consumed = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 32, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *counted = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 32, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *wide = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 40, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *distant = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 32, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *counters = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 3 * 4096, uav, D3D12_RESOURCE_STATE_COMMON);      /* (D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT apart) */
  const D3D12_DESCRIPTOR_RANGE_TYPE types[3] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV};
  const UINT regs[3] = {0, 60, 100}, sizes[3] = {3, 1, 1};
  ID3D12RootSignature *rs = ranges_signature(dev, 3, types, regs, sizes, 0);
  ID3D12PipelineState *pso = rs ? compute_pso(dev, rs, dx_cs_append_dxbc, sizeof dx_cs_append_dxbc) : NULL;
  ID3D12DescriptorHeap *heap = view_heap(dev, 5, 1);
  if (!appended || !consumed || !counted || !wide || !distant || !counters || !rs || !pso || !heap) { CHECK(0); return; }
  const UINT values[8] = {100, 101, 102, 103, 104, 105, 106, 107}, raw[8] = {1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007}, six = 6, nothing[16] = {0};
  buffer_upload(c, consumed, 0, values, 32);
  buffer_upload(c, distant, 0, raw, 32);
  buffer_upload(c, appended, 0, nothing, 64);
  buffer_upload(c, counted, 0, nothing, 32);
  buffer_upload(c, wide, 0, nothing, 40);
  buffer_upload(c, counters, 0, nothing, 4);
  buffer_upload(c, counters, 4096, &six, 4);
  buffer_upload(c, counters, 8192, nothing, 4);
  buffer_uav(dev, heap, NULL, 0, appended, counters, 0, DXGI_FORMAT_UNKNOWN, 0, 8, 8, 0);
  buffer_uav(dev, heap, NULL, 1, consumed, counters, 4096, DXGI_FORMAT_UNKNOWN, 0, 8, 4, 0);
  buffer_uav(dev, heap, NULL, 2, counted, counters, 8192, DXGI_FORMAT_UNKNOWN, 0, 8, 4, 0);
  buffer_uav(dev, heap, NULL, 3, wide, NULL, 0, DXGI_FORMAT_UNKNOWN, 0, 10, 4, 0);
  buffer_srv(dev, heap, 4, distant, DXGI_FORMAT_R32_TYPELESS, 0, 8, 0, D3D12_BUFFER_SRV_FLAG_RAW);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  UINT a[16], k[8], w[10], n[3];
  CHECK(buffer_download(c, appended, 0, a, 64) && buffer_download(c, counted, 0, k, 32) && buffer_download(c, wide, 0, w, 40));
  CHECK(buffer_download(c, counters, 0, &n[0], 4) && buffer_download(c, counters, 4096, &n[1], 4) && buffer_download(c, counters, 8192, &n[2], 4));
  /* (the threads take the values and places in an order of the GPU's: the six appended pairs are the six values 100 .. 105, each with a thread below 6, each thread once) */
  int wrong = 0;
  UINT seen_values = 0, seen_threads = 0, seen_places = 0;
  for (UINT i = 0; i < 6; ++i) {
    if (a[i * 2] < 100 || a[i * 2] > 105 || a[i * 2 + 1] > 5) { ++wrong; continue; }
    seen_values |= 1u << (a[i * 2] - 100);
    seen_threads |= 1u << a[i * 2 + 1];
  }
  for (UINT i = 0; i < 8; ++i) {
    if (k[i] % 3 != 1 || k[i] / 3 > 7) { ++wrong; continue; }
    seen_places |= 1u << (k[i] / 3);
  }
  for (UINT i = 0; i < 8; ++i) wrong += w[i] != 1007 + i;
  printf("append and consume: counters %u %u %u, bufinfo %u %u\n", n[0], n[1], n[2], w[8], w[9]);
  CHECK(wrong == 0 && seen_values == 0x3F && seen_threads == 0x3F && seen_places == 0xFF && n[0] == 6 && n[1] == 0 && n[2] == 8 && w[8] == 32 && w[9] == 8);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D12DescriptorHeap_Release(heap);
  ID3D12Resource_Release(counters);
  ID3D12Resource_Release(distant);
  ID3D12Resource_Release(wide);
  ID3D12Resource_Release(counted);
  ID3D12Resource_Release(consumed);
  ID3D12Resource_Release(appended);
}

/* A draw with no render target and no depth buffer (UAV-only rendering): a triangle over the 16 x 16 viewport whose pixel shader adds one to each pixel's element of a typed view, and
 * again with ForcedSampleCount 4 (where the device has four samples): still once a pixel. And a vertex shader that writes unordered access views (vs_uav). */
static void test_uav_only_draws(Scene *c) {
  ID3D12Device *dev = c->dev;
  const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource *hits = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256 * 4, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *seen = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *positions = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 48, uav, D3D12_RESOURCE_STATE_COMMON);
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 3 * 28, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  const D3D12_DESCRIPTOR_RANGE_TYPE types[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV};
  const UINT regs[1] = {0}, sizes[1] = {3};
  ID3D12RootSignature *rs = ranges_signature(dev, 1, types, regs, sizes, 1);
  ID3D12DescriptorHeap *heap = view_heap(dev, 3, 1), *cpu = view_heap(dev, 3, 0);
  if (!hits || !seen || !positions || !vb || !rs || !heap || !cpu) { CHECK(0); return; }
  const float tri[21] = {-1, -1, 0.5f, 1, 1, 1, 1, 3, -1, 0.5f, 1, 1, 1, 1, -1, 3, 0.5f, 1, 1, 1, 1};
  void *p = NULL;
  if (SUCCEEDED(ID3D12Resource_Map(vb, 0, NULL, &p)) && p) { memcpy(p, tri, sizeof tri); ID3D12Resource_Unmap(vb, 0, NULL); }
  buffer_uav(dev, heap, cpu, 0, hits, NULL, 0, DXGI_FORMAT_R32_UINT, 0, 256, 0, 0);
  buffer_uav(dev, heap, cpu, 1, seen, NULL, 0, DXGI_FORMAT_R32_UINT, 0, 4, 0, 0);
  buffer_uav(dev, heap, cpu, 2, positions, NULL, 0, DXGI_FORMAT_UNKNOWN, 0, 3, 16, 0);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = dx_ps_uav_only_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_uav_only_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 0;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL, *forced = NULL, *vertex = NULL, *refused = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {DXGI_FORMAT_B8G8R8A8_UNORM, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
  const int four = SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) && q.NumQualityLevels > 0;
  pd.RasterizerState.ForcedSampleCount = 4;
  if (four) CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&forced));
  /* (refused: a forced count of 3, and a forced count with a render target) */
  pd.RasterizerState.ForcedSampleCount = 3;
  CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&refused) == E_INVALIDARG && !refused);
  pd.RasterizerState.ForcedSampleCount = 4;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&refused) == E_INVALIDARG && !refused);
  /* (the vertex shader that writes u1 and u2, and the same pixel shader) */
  pd.RasterizerState.ForcedSampleCount = 0;
  pd.NumRenderTargets = 0;
  pd.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
  D3D12_INPUT_ELEMENT_DESC position[1] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  pd.InputLayout.pInputElementDescs = position;
  pd.InputLayout.NumElements = 1;
  pd.VS.pShaderBytecode = dx_vs_uav_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_uav_dxbc;
  const HRESULT vertex_made = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&vertex);
  if (!pso) return;
  const UINT zero[4] = {0, 0, 0, 0};
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  for (UINT i = 0; i < 3; ++i) ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, i), heap_cpu(dev, cpu, i), i == 0 ? hits : i == 1 ? seen : positions, zero, 0, NULL);
  D3D12_VIEWPORT vp = {0, 0, 16, 16, 0, 1};
  D3D12_RECT sc = {0, 0, 16, 16};
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 3 * 28, 28};
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  if (forced) {
    ID3D12GraphicsCommandList_SetPipelineState(list, forced);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  }
  if (vertex) {
    D3D12_VERTEX_BUFFER_VIEW pv = {ID3D12Resource_GetGPUVirtualAddress(vb), 48, 16};      /* (the first 48 bytes as three float4 positions) */
    ID3D12GraphicsCommandList_SetPipelineState(list, vertex);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &pv);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  }
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  UINT h[256], s[4];
  float pos[12];
  CHECK(buffer_download(c, hits, 0, h, sizeof h) && buffer_download(c, seen, 0, s, sizeof s) && buffer_download(c, positions, 0, pos, sizeof pos));
  /* (each pixel once a draw: by the first, the forced one, and - its triangle is the first 48 bytes of floats as positions - none or some of them by the vertex one) */
  const UINT draws = forced ? 2u : 1u;
  int wrong = 0, extra = 0;
  for (UINT i = 0; i < 256; ++i) {
    if (h[i] < draws || h[i] > draws + 1) { if (!wrong++) printf("  UAV-only draw: element %u = %u, want %u\n", i, h[i], draws); }
    extra += h[i] == draws + 1;
  }
  printf("UAV-only draws: %u draws, %d pixels of the vertex shader's triangle; vertex shader UAVs %s (%u vertices)\n", draws, extra, vertex ? "made" : "refused", s[0]);
  CHECK(wrong == 0);
  if (vertex) {
    int bad = s[0] != 3;
    for (UINT i = 0; i < 12; ++i) bad += pos[i] != tri[i];
    CHECK(bad == 0);
  } else {
    CHECK(FAILED(vertex_made));
  }
  if (vertex) ID3D12PipelineState_Release(vertex);
  if (forced) ID3D12PipelineState_Release(forced);
  ID3D12PipelineState_Release(pso);
  ID3D12DescriptorHeap_Release(cpu);
  ID3D12DescriptorHeap_Release(heap);
  ID3D12RootSignature_Release(rs);
  ID3D12Resource_Release(vb);
  ID3D12Resource_Release(positions);
  ID3D12Resource_Release(seen);
  ID3D12Resource_Release(hits);
}

/* A geometry shader sending triangle n to layer n (SV_RenderTargetArrayIndex) of a render target and a depth buffer of three slices, a clear of a view of two of them, and
 * a view that starts at slice 1. */
static void test_layers(Scene *c) {
  ID3D12Device *dev = c->dev;
  ID3D12Resource *slices = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 3, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ID3D12Resource *depths = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 3, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 9 * 28, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!slices || !depths || !vb) { CHECK(0); return; }
  /* three triangles: red over the left, green over the middle, blue over the right (each at depth 0.5) */
  float verts[9][7];
  for (UINT t = 0; t < 3; ++t) {
    const float x0 = -1.0f + 0.66f * t;
    const float tri[3][3] = {{x0, -0.9f, 0.5f}, {x0 + 0.6f, -0.9f, 0.5f}, {x0 + 0.3f, 0.9f, 0.5f}};
    for (UINT k = 0; k < 3; ++k) {
      float *v = verts[t * 3 + k];
      v[0] = tri[k][0], v[1] = tri[k][1], v[2] = tri[k][2];
      v[3] = t == 0, v[4] = t == 1, v[5] = t == 2, v[6] = 1;
    }
  }
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtvs = NULL, *dsvs = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvs));
  if (!rtvs || !dsvs) return;
  const UINT rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE all = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs), upper = all, dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsvs);
  upper.ptr += rtv_step;
  D3D12_RENDER_TARGET_VIEW_DESC rv;
  memset(&rv, 0, sizeof rv);
  rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
  rv.Texture2DArray.ArraySize = 3;
  ID3D12Device_CreateRenderTargetView(dev, slices, &rv, all);
  rv.Texture2DArray.FirstArraySlice = 1;
  rv.Texture2DArray.ArraySize = 2;
  ID3D12Device_CreateRenderTargetView(dev, slices, &rv, upper);
  D3D12_DEPTH_STENCIL_VIEW_DESC dv;
  memset(&dv, 0, sizeof dv);
  dv.Format = DXGI_FORMAT_D32_FLOAT;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
  dv.Texture2DArray.ArraySize = 3;
  ID3D12Device_CreateDepthStencilView(dev, depths, &dv, dsv);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = c->rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.GS.pShaderBytecode = dx_gs_layer_dxbc;
  pd.GS.BytecodeLength = sizeof dx_gs_layer_dxbc;
  pd.PS.pShaderBytecode = dx_ps_color_dxbc;
  pd.PS.BytecodeLength = sizeof dx_ps_color_dxbc;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.DepthStencilState.DepthEnable = TRUE;
  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!pso || !list) { CHECK(0); return; }
  const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
  D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  D3D12_RECT sc = {0, 0, 64, 64};
  D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 9 * 28, 28};
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, all, black, 0, NULL);
  ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &all, FALSE, &dsv);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 9, 1, 0, 0);
  scene_run(c, list);
  /* where each triangle's middle is: (column of triangle t, row 32) */
  static BYTE texels[3][64 * 64 * 4];
  int wrong = 0;
  for (UINT layer = 0; layer < 3; ++layer) {
    CHECK(texture_read(c, slices, layer, texels[layer], 4));
    for (UINT t = 0; t < 3; ++t) {
      const UINT x = (UINT)(((-1.0f + 0.66f * t + 0.3f) + 1.0f) * 32.0f);
      const BYTE *p = &texels[layer][(32 * 64 + x) * 4];      /* (B, G, R, A) */
      const BYTE want_r = t == layer && t == 0 ? 255 : 0, want_g = t == layer && t == 1 ? 255 : 0, want_b = t == layer && t == 2 ? 255 : 0;
      if ((p[2] != want_r || p[1] != want_g || p[0] != want_b) && !wrong++)
        printf("  layer %u, triangle %u: %u %u %u, want %u %u %u\n", layer, t, p[2], p[1], p[0], want_r, want_g, want_b);
    }
  }
  printf("layers: %s\n", wrong ? "wrong" : "each triangle in its slice");
  CHECK(wrong == 0);
  /* a clear of the view of slices 1 and 2 (slice 0 as it was) */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, pso));
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, upper, white, 0, NULL);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &upper, FALSE, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  scene_run(c, list);
  wrong = 0;
  for (UINT layer = 0; layer < 3; ++layer) {
    CHECK(texture_read(c, slices, layer, texels[layer], 4));
    const BYTE *corner = &texels[layer][(2 * 64 + 2) * 4];
    if ((layer == 0 ? corner[0] != 0 : corner[0] != 255) && !wrong++) printf("  clear of slices 1, 2: slice %u corner %u\n", layer, corner[0]);
  }
  CHECK(wrong == 0);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12PipelineState_Release(pso);
  ID3D12DescriptorHeap_Release(dsvs);
  ID3D12DescriptorHeap_Release(rtvs);
  ID3D12Resource_Release(vb);
  ID3D12Resource_Release(depths);
  ID3D12Resource_Release(slices);
}

/* Storage images of the planes of an NV12 texture (R8_UINT the luma, R8G8_UINT the chroma): what the shader writes is in the planes, the other values as they were. */
static void test_plane_uavs(Scene *c) {
  ID3D12Device *dev = c->dev;
  ID3D12Resource *tex = planar_texture(dev, DXGI_FORMAT_NV12, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12RootSignature *rs = uav_signature(dev, 0, 2, 0);
  ID3D12PipelineState *pso = rs ? compute_pso(dev, rs, dx_cs_plane_uav_dxbc, sizeof dx_cs_plane_uav_dxbc) : NULL;
  ID3D12DescriptorHeap *heap = view_heap(dev, 3, 1), *cpu = view_heap(dev, 3, 0);
  if (!tex || !rs || !pso || !heap || !cpu) { CHECK(0); return; }
  planar_fill(dev, c->queue, c->fence, c->fence_value, c->allocator, tex, 3, 1);
  D3D12_UNORDERED_ACCESS_VIEW_DESC u;
  memset(&u, 0, sizeof u);
  u.Format = DXGI_FORMAT_R8_UINT;
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  put_uav(dev, heap, cpu, 0, tex, &u);
  u.Format = DXGI_FORMAT_R8G8_UINT;
  u.Texture2D.PlaneSlice = 1;
  put_uav(dev, heap, cpu, 1, tex, &u);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_Dispatch(list, 2, 1, 1);
  scene_run(c, list);
  BYTE values[PL_W * PL_H];
  UINT row = 0;
  int wrong = 0;
  CHECK(planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, tex, 0, values, &row));
  for (UINT y = 0; y < PL_H; ++y)
    for (UINT x = 0; x < PL_W; ++x)
      if (values[y * row + x] != x + 16 * y && !wrong++) printf("  luma storage image: (%u, %u) %u, want %u\n", x, y, values[y * row + x], x + 16 * y);
  CHECK(planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, tex, 1, values, &row));
  for (UINT y = 0; y < PL_H / 2; ++y)
    for (UINT x = 0; x < PL_W / 2; ++x)
      if ((values[y * row + 2 * x] != x * 10 || values[y * row + 2 * x + 1] != y * 20 + 1) && !wrong++)
        printf("  chroma storage image: (%u, %u) %u %u, want %u %u\n", x, y, values[y * row + 2 * x], values[y * row + 2 * x + 1], x * 10, y * 20 + 1);
  printf("plane storage images: %s\n", wrong ? "wrong" : "the shader's values in both planes");
  CHECK(wrong == 0);
  /* a clear of a rectangle of the luma's view: those texels, the others as the shader left them */
  CHECK_HR(ID3D12GraphicsCommandList_Reset(list, c->allocator, NULL));
  const UINT seven[4] = {7, 0, 0, 0};
  const D3D12_RECT rect = {0, 0, 4, 2};
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, heap_gpu(dev, heap, 0), heap_cpu(dev, cpu, 0), tex, seven, 1, &rect);
  scene_run(c, list);
  CHECK(planar_read(dev, c->queue, c->fence, c->fence_value, c->allocator, tex, 0, values, &row));
  CHECK(values[0] == 7 && values[row + 3] == 7 && values[4] == 4 && values[2 * row] == 32);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12DescriptorHeap_Release(cpu);
  ID3D12DescriptorHeap_Release(heap);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  ID3D12Resource_Release(tex);
}

/* ---- depth buffers read as textures, comparison samplers, texture and sampler pairs ------------------------------------------------------------------------------------------- */

/* A pipeline of vs.hlsl and `ps` on `rs` (the scene's vertex layout), drawing into the scene's target (`colour`) or into a depth and stencil buffer of `dsv_format` alone, which
 * the depth pass tests (less), writes and stamps with the stencil reference. */
static ID3D12PipelineState *shadow_pipeline(Scene *c, ID3D12RootSignature *rs, const void *ps, SIZE_T ps_size, int colour, DXGI_FORMAT dsv_format) {
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_dxbc;
  pd.PS.pShaderBytecode = ps;
  pd.PS.BytecodeLength = ps_size;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (dsv_format != DXGI_FORMAT_UNKNOWN) {
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    if (dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
      pd.DepthStencilState.StencilEnable = TRUE;
      pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 0xFF;
      const D3D12_DEPTH_STENCILOP_DESC stamp = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
      pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = stamp;
    }
    pd.DSVFormat = dsv_format;
  }
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = colour ? 1 : 0;
  if (colour) pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(c->dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  return pso;
}

/* A root signature of a table of `views` shader resource views (t0 ..), a table of `samplers` samplers (s0 ..; none: 0), the static samplers given (after them), and the 16
 * constants of the vertex shader's matrix (the last parameter). */
static ID3D12RootSignature *shadow_signature(ID3D12Device *dev, UINT views, UINT samplers, const D3D12_STATIC_SAMPLER_DESC *statics, UINT static_count) {
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, views, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, samplers, 0, 0, 0}};
  D3D12_ROOT_PARAMETER params[3];
  memset(params, 0, sizeof params);
  UINT n = 0;
  params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[n].DescriptorTable.NumDescriptorRanges = 1;
  params[n++].DescriptorTable.pDescriptorRanges = &ranges[0];
  if (samplers) {
    params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[n].DescriptorTable.NumDescriptorRanges = 1;
    params[n++].DescriptorTable.pDescriptorRanges = &ranges[1];
  }
  params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[n++].Constants.Num32BitValues = 16;
  D3D12_ROOT_SIGNATURE_DESC rsd = {n, params, static_count, statics, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  if (blob) ID3D10Blob_Release(blob);
  if (error) ID3D10Blob_Release(error);
  return rs;
}

static D3D12_SAMPLER_DESC shadow_sampler(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address, D3D12_COMPARISON_FUNC func) {
  D3D12_SAMPLER_DESC s;
  memset(&s, 0, sizeof s);
  s.Filter = filter;
  s.AddressU = s.AddressV = s.AddressW = address;
  s.ComparisonFunc = func;
  s.MaxLOD = 1000.0f;
  return s;
}

/* Six vertices of a rectangle from (x0, y0) to (x1, y1) in clip space at depth z, coloured so that vs.hlsl's coordinate is (u, v) = 0, 0 at the top left to 1, 1 at the bottom right
 * of the target (plus `dv`), the colour's blue `reference`. */
static void shadow_rect(float (*v)[7], float x0, float y0, float x1, float y1, float z, float dv, float reference) {
  const float corners[6][2] = {{x0, y1}, {x1, y1}, {x0, y0}, {x0, y0}, {x1, y1}, {x1, y0}};
  for (int i = 0; i < 6; ++i) {
    const float x = corners[i][0], y = corners[i][1];
    v[i][0] = x, v[i][1] = y, v[i][2] = z;
    v[i][3] = (x + 1) / 4, v[i][4] = ((1 - y) / 2 + dv) / 2, v[i][5] = reference, v[i][6] = 1;
  }
}

static int shadow_near(UINT value, UINT want, UINT within) { return value + within >= want && value <= want + within; }

static void shadow_transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b;
  memset(&b, 0, sizeof b);
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* A shadow map, the way Direct3D 12 programs make one: a D24S8 buffer (R24G8_TYPELESS) drawn into by a depth-only pass - its top half at depth 0.25 and stencil 5, the rest
 * cleared to 1 and 0 -, then read by shaders: compared by two comparison samplers of a table and sampled by a static one (three texture and sampler pairs of t0: ps_shadow), its
 * stencil plane loaded (ps_stencil_only), and then compared after the stencil was read (a GPU's texture may keep one plane's mode), compared through a linear comparison sampler (SampleCmp, GatherCmp: ps_shadow_pcf). Then a pass that draws into a swap chain's back
 * buffer - an image kept for showing - with a depth buffer that is a texture, read afterwards (the rows of the two must not come out the other way round), an array of depth, one
 * texture with two samplers, and the formats. */
static void test_shadow_maps(Scene *c) {
  ID3D12Device *dev = c->dev;
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  /* (what the formats say: the views of depth buffers sampled, compared and gathered, their stencil loaded) */
  const UINT32 x8 = format_support1(dev, DXGI_FORMAT_R24_UNORM_X8_TYPELESS), g8 = format_support1(dev, DXGI_FORMAT_X24_TYPELESS_G8_UINT), r32 = format_support1(dev, DXGI_FORMAT_R32_FLOAT);
  CHECK((x8 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON) && (x8 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON) && (x8 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
  CHECK((g8 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) && !(g8 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) && (r32 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON));
  CHECK(format_support1(dev, DXGI_FORMAT_D16_UNORM) & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL);
  D3D12_RESOURCE_DESC dd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  ID3D12Resource *depth = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  D3D12_DESCRIPTOR_HEAP_DESC smp_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ID3D12DescriptorHeap *dsv_heap = NULL, *srv_heap = NULL, *smp_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &dsv_desc, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &srv_desc, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &smp_desc, &IID_ID3D12DescriptorHeap, (void **)&smp_heap));
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!depth || !dsv_heap || !srv_heap || !smp_heap || !vb) { CHECK(0); return; }
  const UINT view_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sampler_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
  const D3D12_CPU_DESCRIPTOR_HANDLE srv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap), smp0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(smp_heap);
  const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap), smp_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(smp_heap);
  D3D12_DEPTH_STENCIL_VIEW_DESC dv;
  memset(&dv, 0, sizeof dv);
  dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  ID3D12Device_CreateDepthStencilView(dev, depth, &dv, dsv);
  /* (t0 the depth, t1 the stencil, t2 a depth view of a wrong format: a null view) */
  D3D12_SHADER_RESOURCE_VIEW_DESC sv;
  memset(&sv, 0, sizeof sv);
  sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  ID3D12Device_CreateShaderResourceView(dev, depth, &sv, srv0);
  sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
  sv.Texture2D.PlaneSlice = 1;
  ID3D12Device_CreateShaderResourceView(dev, depth, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){srv0.ptr + view_step});
  D3D12_SAMPLER_DESC less_equal = shadow_sampler(D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_COMPARISON_FUNC_LESS_EQUAL);
  D3D12_SAMPLER_DESC greater = shadow_sampler(D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_COMPARISON_FUNC_GREATER);
  ID3D12Device_CreateSampler(dev, &less_equal, smp0);
  ID3D12Device_CreateSampler(dev, &greater, (D3D12_CPU_DESCRIPTOR_HANDLE){smp0.ptr + sampler_step});
  D3D12_STATIC_SAMPLER_DESC plain;
  memset(&plain, 0, sizeof plain);
  plain.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  plain.AddressU = plain.AddressV = plain.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  plain.MaxLOD = 1000.0f;
  plain.ShaderRegister = 2;
  D3D12_STATIC_SAMPLER_DESC pcf = plain;
  pcf.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  pcf.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pcf.ShaderRegister = 0;
  ID3D12RootSignature *rs_shadow = shadow_signature(dev, 1, 2, &plain, 1), *rs_stencil = shadow_signature(dev, 2, 0, NULL, 0), *rs_pcf = shadow_signature(dev, 1, 0, &pcf, 1);
  ID3D12PipelineState *depth_pso = shadow_pipeline(c, c->rs, dx_ps_color_dxbc, sizeof dx_ps_color_dxbc, 0, DXGI_FORMAT_D24_UNORM_S8_UINT);
  ID3D12PipelineState *shadow_pso = rs_shadow ? shadow_pipeline(c, rs_shadow, dx_ps_shadow_dxbc, sizeof dx_ps_shadow_dxbc, 1, DXGI_FORMAT_UNKNOWN) : NULL;
  ID3D12PipelineState *stencil_pso = rs_stencil ? shadow_pipeline(c, rs_stencil, dx_ps_stencil_only_dxbc, sizeof dx_ps_stencil_only_dxbc, 1, DXGI_FORMAT_UNKNOWN) : NULL;
  ID3D12PipelineState *pcf_pso = rs_pcf ? shadow_pipeline(c, rs_pcf, dx_ps_shadow_pcf_dxbc, sizeof dx_ps_shadow_pcf_dxbc, 1, DXGI_FORMAT_UNKNOWN) : NULL;
  if (!depth_pso || !shadow_pso || !stencil_pso || !pcf_pso) { CHECK(0); return; }
  /* the vertices: the top half at 0.25 (0), the whole target with the reference 0.5 (6), the same a quarter of a texel down (12) */
  float verts[18][7];
  shadow_rect(&verts[0], -1, 0, 1, 1, 0.25f, 0, 0);
  shadow_rect(&verts[6], -1, -1, 1, 1, 0.5f, 0, 0.5f);
  shadow_rect(&verts[12], -1, -1, 1, 1, 0.5f, 0.25f / 64, 0.5f);
  void *mapped = NULL;
  CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
  if (mapped) memcpy(mapped, verts, sizeof verts);
  ID3D12Resource_Unmap(vb, 0, NULL);
  const D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), sizeof verts, 28};
  const D3D12_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  const D3D12_RECT sc = {0, 0, 64, 64};
  const float black[4] = {0, 0, 0, 0};
  ID3D12DescriptorHeap *heaps[2] = {srv_heap, smp_heap};
  /* Draws the full target (first vertex `first`) with `pso` on `rs` reading the depth buffer and gives the pixels at the points. */
#define SHADOW_READ(pso_, rs_, samplers_, first_, out_, ...)                                                                              \
  do {                                                                                                                                     \
    ID3D12GraphicsCommandList *l_ = NULL;                                                                                                  \
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));                                                                                  \
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso_, &IID_ID3D12GraphicsCommandList, (void **)&l_)); \
    if (!l_) break;                                                                                                                        \
    ID3D12GraphicsCommandList_OMSetRenderTargets(l_, 1, &c->rtv, FALSE, NULL);                                                            \
    ID3D12GraphicsCommandList_ClearRenderTargetView(l_, c->rtv, black, 0, NULL);                                                           \
    ID3D12GraphicsCommandList_RSSetViewports(l_, 1, &vp);                                                                                 \
    ID3D12GraphicsCommandList_RSSetScissorRects(l_, 1, &sc);                                                                              \
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l_, rs_);                                                                           \
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l_, 2, heaps);                                                                            \
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l_, 0, srv_gpu);                                                              \
    if (samplers_) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l_, 1, smp_gpu);                                              \
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l_, (samplers_) ? 2 : 1, 16, kIdentity, 0);                                   \
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l_, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);                                             \
    ID3D12GraphicsCommandList_IASetVertexBuffers(l_, 0, 1, &vbv);                                                                         \
    ID3D12GraphicsCommandList_DrawInstanced(l_, 6, 1, first_, 0);                                                                         \
    scene_read_target(c, l_);                                                                                                              \
    scene_run(c, l_);                                                                                                                      \
    ID3D12GraphicsCommandList_Release(l_);                                                                                                 \
    const UINT32 *px_ = NULL;                                                                                                              \
    const int at_[][2] = {__VA_ARGS__};                                                                                                    \
    if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&px_)) && px_) {                                                       \
      for (UINT k_ = 0; k_ < sizeof at_ / sizeof at_[0]; ++k_) (out_)[k_] = px_[at_[k_][1] * 64 + at_[k_][0]];                             \
      ID3D12Resource_Unmap(c->readback, 0, NULL);                                                                                          \
    }                                                                                                                                      \
  } while (0)
  /* the depth pass */
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, depth_pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) { CHECK(0); return; }
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &dsv);
  ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_OMSetStencilRef(list, 5);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
  ID3D12GraphicsCommandList_DrawInstanced(list, 6, 1, 0, 0);
  shadow_transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  UINT32 px[4] = {0};
  /* compared with 0.5: less-or-equal fails above (0.25) and passes below (1), greater the other way; the static sampler gives the depth */
  SHADOW_READ(shadow_pso, rs_shadow, 1, 6, px, {10, 10}, {50, 50});
  printf("shadow maps: compared %08x (top) %08x (bottom)\n", (unsigned)px[0], (unsigned)px[1]);
  CHECK(px_red(px[0]) == 0 && px_green(px[0]) == 255 && shadow_near((UINT)px_blue(px[0]), 64, 2));
  CHECK(px_red(px[1]) == 255 && px_green(px[1]) == 0 && px_blue(px[1]) == 255);
  /* the stencil plane (a draw that reads it alone: a GPU's texture may read one plane at a time) */
  SHADOW_READ(stencil_pso, rs_stencil, 0, 6, px, {10, 10}, {50, 50});
  printf("shadow maps: stencil %08x (top) %08x (bottom)\n", (unsigned)px[0], (unsigned)px[1]);
  CHECK(px_green(px[0]) == 5 && px_green(px[1]) == 0);
  /* the depth again after the stencil (the host's texture is put back to its depth) */
  SHADOW_READ(shadow_pso, rs_shadow, 1, 6, px, {10, 10}, {50, 50});
  CHECK(px_red(px[0]) == 0 && px_green(px[0]) == 255 && px_red(px[1]) == 255 && px_blue(px[1]) == 255);
  /* filtered: a quarter of a texel down, row 31 takes a quarter of row 32 (which passes); GatherCmp gives two of each there */
  SHADOW_READ(pcf_pso, rs_pcf, 0, 12, px, {20, 31}, {20, 10}, {20, 50});
  printf("shadow maps: filtered %08x (the edge) %08x %08x\n", (unsigned)px[0], (unsigned)px[1], (unsigned)px[2]);
  CHECK(shadow_near((UINT)px_red(px[0]), 64, 6) && shadow_near((UINT)px_green(px[0]), 64, 6) && shadow_near((UINT)px_blue(px[0]), 128, 4));
  CHECK((px[1] & 0xFFFFFF) == 0 && (px[2] & 0xFFFFFF) == 0xFFFFFF);

  /* A pass into a swap chain's back buffer (kept for showing) with the depth buffer, a texture, drawing its top half near: then the depth read and the back buffer copied out. */
  IDXGIFactory4 *factory = NULL;
  CHECK_HR(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
  IDXGISwapChain1 *chain = NULL;
  ID3D12Resource *back = NULL;
  if (factory) {
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = 64;
    sd.Height = 64;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    CHECK_HR(IDXGIFactory4_CreateSwapChainForComposition(factory, (IUnknown *)c->queue, &sd, NULL, &chain));
    if (chain) CHECK_HR(IDXGISwapChain1_GetBuffer(chain, 0, &IID_ID3D12Resource, (void **)&back));
  }
  ID3D12PipelineState *front_pso = shadow_pipeline(c, c->rs, dx_ps_color_dxbc, sizeof dx_ps_color_dxbc, 1, DXGI_FORMAT_D24_UNORM_S8_UINT);
  if (back && front_pso) {
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap *rtv_heap = NULL;
    CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &rtv_desc, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap ? ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap) : (D3D12_CPU_DESCRIPTOR_HANDLE){0};
    if (rtv_heap) ID3D12Device_CreateRenderTargetView(dev, back, NULL, rtv);
    /* (the bottom half near this time: 0.25 below, 1 above) */
    float bottom[6][7];
    shadow_rect(bottom, -1, -1, 1, 0, 0.25f, 0, 0);
    CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
    if (mapped) memcpy((BYTE *)mapped + sizeof verts, bottom, sizeof bottom);
    ID3D12Resource_Unmap(vb, 0, NULL);
    const float grey[4] = {0.25f, 0.25f, 0.25f, 1};
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, front_pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (list && rtv_heap) {
      shadow_transition(list, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      shadow_transition(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
      ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
      ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
      ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
      ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
      ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
      ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, c->rs);
      ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
      ID3D12GraphicsCommandList_OMSetStencilRef(list, 9);
      ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
      ID3D12GraphicsCommandList_DrawInstanced(list, 6, 1, 18, 0);
      shadow_transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      shadow_transition(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION to, from;
      memset(&to, 0, sizeof to);
      memset(&from, 0, sizeof from);
      to.pResource = c->readback;
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      to.PlacedFootprint.Footprint.Width = to.PlacedFootprint.Footprint.Height = 64;
      to.PlacedFootprint.Footprint.Depth = 1;
      to.PlacedFootprint.Footprint.RowPitch = 256;
      from.pResource = back;
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
      shadow_transition(list, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      scene_run(c, list);
      const UINT32 *pixels = NULL;
      if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
        /* (the back buffer: the grey clear above, the vertex colour below - red and green the coordinate's halves, (0.39, 0.39, 0) at (50, 50)) */
        printf("shadow maps: back buffer %08x (top) %08x (bottom)\n", (unsigned)pixels[10 * 64 + 10], (unsigned)pixels[50 * 64 + 50]);
        CHECK(shadow_near((UINT)px_red(pixels[10 * 64 + 10]), 64, 2) && shadow_near((UINT)px_blue(pixels[10 * 64 + 10]), 64, 2));
        CHECK(shadow_near((UINT)px_red(pixels[50 * 64 + 50]), 0x65, 3) && shadow_near((UINT)px_green(pixels[50 * 64 + 50]), 0x65, 3) && px_blue(pixels[50 * 64 + 50]) == 0);
        ID3D12Resource_Unmap(c->readback, 0, NULL);
      }
      ID3D12GraphicsCommandList_Release(list);
      list = NULL;
      SHADOW_READ(shadow_pso, rs_shadow, 1, 6, px, {10, 10}, {50, 50});
      printf("shadow maps: drawn with a back buffer %08x (top) %08x (bottom)\n", (unsigned)px[0], (unsigned)px[1]);
      CHECK(px_red(px[0]) == 255 && px_green(px[0]) == 0 && px_blue(px[0]) == 255);
      CHECK(px_red(px[1]) == 0 && px_green(px[1]) == 255 && shadow_near((UINT)px_blue(px[1]), 64, 2));
      SHADOW_READ(stencil_pso, rs_stencil, 0, 6, px, {10, 10}, {50, 50});
      CHECK(px_green(px[0]) == 0 && px_green(px[1]) == 9);
    }
    if (rtv_heap) ID3D12DescriptorHeap_Release(rtv_heap);
  }
  if (front_pso) ID3D12PipelineState_Release(front_pso);
  if (back) ID3D12Resource_Release(back);
  if (chain) IDXGISwapChain1_Release(chain);
  if (factory) IDXGIFactory4_Release(factory);

  /* D16 (R16_TYPELESS: an R16_UNORM view) and D32 with stencil (R32G8X24_TYPELESS: an R32_FLOAT_X8X24_TYPELESS view) depth buffers, cleared to 0.25 and compared */
  {
    const DXGI_FORMAT made[2][3] = {{DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM},
                                    {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS}};
    for (int k = 0; k < 2; ++k) {
      D3D12_RESOURCE_DESC kd = dd;
      kd.Format = made[k][0];
      ID3D12Resource *other = NULL;
      CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &kd, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&other));
      if (!other) continue;
      D3D12_DEPTH_STENCIL_VIEW_DESC ov = dv;
      ov.Format = made[k][1];
      const D3D12_CPU_DESCRIPTOR_HANDLE at = {dsv.ptr + ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
      ID3D12Device_CreateDepthStencilView(dev, other, &ov, at);
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
      if (list) {
        ID3D12GraphicsCommandList_ClearDepthStencilView(list, at, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, NULL);
        shadow_transition(list, other, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        scene_run(c, list);
        ID3D12GraphicsCommandList_Release(list);
        list = NULL;
      }
      D3D12_SHADER_RESOURCE_VIEW_DESC kv = sv;
      kv.Format = made[k][2];
      kv.Texture2D.PlaneSlice = 0;
      ID3D12Device_CreateShaderResourceView(dev, other, &kv, srv0);
      SHADOW_READ(shadow_pso, rs_shadow, 1, 6, px, {32, 32});
      printf("shadow maps: %s %08x\n", k ? "D32S8" : "D16", (unsigned)px[0]);
      CHECK(px_red(px[0]) == 0 && px_green(px[0]) == 255 && shadow_near((UINT)px_blue(px[0]), 64, 2));
      ID3D12Resource_Release(other);
    }
  }

  /* One texture with two samplers - wrap and clamp - and a second texture with the second (ps_pair): two units for t0. */
  {
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *ramp = NULL, *second = NULL;
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&ramp));
    td.Width = 1;
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&second));
    ID3D12Resource *upload = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 1024, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_SAMPLER_DESC wrap = shadow_sampler(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_COMPARISON_FUNC_NEVER);
    D3D12_SAMPLER_DESC clamp = shadow_sampler(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_COMPARISON_FUNC_NEVER);
    ID3D12RootSignature *rs_pair = shadow_signature(dev, 2, 2, NULL, 0);
    ID3D12PipelineState *pair_pso = rs_pair ? shadow_pipeline(c, rs_pair, dx_ps_pair_dxbc, sizeof dx_ps_pair_dxbc, 1, DXGI_FORMAT_UNKNOWN) : NULL;
    BYTE *up = NULL;
    if (ramp && second && upload && pair_pso && SUCCEEDED(ID3D12Resource_Map(upload, 0, NULL, (void **)&up)) && up) {
      const UINT32 texels[4] = {0xFF200000u, 0xFF600000u, 0xFFA00000u, 0xFFE00000u}, one = 0xFF00C000u;
      memcpy(up, texels, sizeof texels);
      memcpy(up + 512, &one, 4);
      ID3D12Resource_Unmap(upload, 0, NULL);
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
      for (int i = 0; list && i < 2; ++i) {
        D3D12_TEXTURE_COPY_LOCATION to, from;
        memset(&to, 0, sizeof to);
        memset(&from, 0, sizeof from);
        to.pResource = i ? second : ramp;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = upload;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = i ? 512 : 0;
        from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        from.PlacedFootprint.Footprint.Width = i ? 1 : 4;
        from.PlacedFootprint.Footprint.Height = from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = 256;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
        shadow_transition(list, i ? second : ramp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      if (list) {
        scene_run(c, list);
        ID3D12GraphicsCommandList_Release(list);
        list = NULL;
      }
      sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      sv.Texture2D.PlaneSlice = 0;
      ID3D12Device_CreateShaderResourceView(dev, ramp, &sv, srv0);
      ID3D12Device_CreateShaderResourceView(dev, second, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){srv0.ptr + view_step});
      ID3D12Device_CreateSampler(dev, &wrap, smp0);
      ID3D12Device_CreateSampler(dev, &clamp, (D3D12_CPU_DESCRIPTOR_HANDLE){smp0.ptr + sampler_step});
      /* (u 0 .. 2 across the target: the wide rectangle's coordinate is twice the other's) */
      float wide[6][7];
      shadow_rect(wide, -1, -1, 1, 1, 0.5f, 0, 0);
      for (int i = 0; i < 6; ++i) wide[i][3] *= 2;
      CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
      if (mapped) memcpy((BYTE *)mapped + sizeof verts, wide, sizeof wide);
      ID3D12Resource_Unmap(vb, 0, NULL);
      SHADOW_READ(pair_pso, rs_pair, 1, 18, px, {4, 10}, {36, 10});
      printf("shadow maps: one texture, two samplers %08x %08x\n", (unsigned)px[0], (unsigned)px[1]);
      CHECK(px_red(px[0]) == 0x20 && px_green(px[0]) == 0x20 && px_blue(px[0]) == 0xC0);
      CHECK(px_red(px[1]) == 0x20 && px_green(px[1]) == 0xE0 && px_blue(px[1]) == 0xC0);
    } else {
      CHECK(0);
    }
    if (pair_pso) ID3D12PipelineState_Release(pair_pso);
    if (rs_pair) ID3D12RootSignature_Release(rs_pair);
    if (upload) ID3D12Resource_Release(upload);
    if (second) ID3D12Resource_Release(second);
    if (ramp) ID3D12Resource_Release(ramp);
  }

  /* An array of depth (on a device that draws into layers): two slices of 32 x 32, cleared to 0.75 and 0.25, compared with 0.5 and the second loaded. */
  D3D12_FEATURE_DATA_D3D12_OPTIONS opts;
  memset(&opts, 0, sizeof opts);
  D3D12_RESOURCE_DESC ad = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 32, 2, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  ID3D12Resource *layers = NULL;
  const HRESULT made = ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&layers);
  CHECK(made == S_OK || made == DXGI_ERROR_UNSUPPORTED);
  ID3D12RootSignature *rs_array = layers ? shadow_signature(dev, 1, 2, NULL, 0) : NULL;
  ID3D12PipelineState *array_pso = rs_array ? shadow_pipeline(c, rs_array, dx_ps_shadow_array_dxbc, sizeof dx_ps_shadow_array_dxbc, 1, DXGI_FORMAT_UNKNOWN) : NULL;
  if (layers && array_pso) {
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    for (UINT slice = 0; list && slice < 2; ++slice) {
      D3D12_DEPTH_STENCIL_VIEW_DESC av;
      memset(&av, 0, sizeof av);
      av.Format = DXGI_FORMAT_D32_FLOAT;
      av.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
      av.Texture2DArray.FirstArraySlice = slice;
      av.Texture2DArray.ArraySize = 1;
      const D3D12_CPU_DESCRIPTOR_HANDLE at = {dsv.ptr + ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
      ID3D12Device_CreateDepthStencilView(dev, layers, &av, at);
      ID3D12GraphicsCommandList_ClearDepthStencilView(list, at, D3D12_CLEAR_FLAG_DEPTH, slice ? 0.25f : 0.75f, 0, 0, NULL);
      if (slice == 0) {
        /* (the clear of one slice is carried out before the view is written again) */
        scene_run(c, list);
        ID3D12GraphicsCommandList_Release(list);
        list = NULL;
        CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
        CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
      }
    }
    if (list) {
      shadow_transition(list, layers, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
      list = NULL;
    }
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = 2;
    ID3D12Device_CreateShaderResourceView(dev, layers, &sv, srv0);
    ID3D12Device_CreateSampler(dev, &less_equal, smp0);
    SHADOW_READ(array_pso, rs_array, 1, 6, px, {20, 20});
    printf("shadow maps: an array of depth %08x\n", (unsigned)px[0]);
    CHECK(px_red(px[0]) == 255 && px_green(px[0]) == 0 && shadow_near((UINT)px_blue(px[0]), 64, 2));
  } else {
    printf("shadow maps: no arrays of depth here\n");
  }
  if (array_pso) ID3D12PipelineState_Release(array_pso);
  if (rs_array) ID3D12RootSignature_Release(rs_array);
  if (layers) ID3D12Resource_Release(layers);
  /* (a depth buffer the program says is never read has no shader resource views: its view is a null one) */
  dd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  ID3D12Resource *denied = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&denied));
  if (denied) ID3D12Resource_Release(denied);
#undef SHADOW_READ
  ID3D12PipelineState_Release(pcf_pso);
  ID3D12PipelineState_Release(stencil_pso);
  ID3D12PipelineState_Release(shadow_pso);
  ID3D12PipelineState_Release(depth_pso);
  ID3D12RootSignature_Release(rs_pcf);
  ID3D12RootSignature_Release(rs_stencil);
  ID3D12RootSignature_Release(rs_shadow);
  ID3D12Resource_Release(vb);
  ID3D12DescriptorHeap_Release(smp_heap);
  ID3D12DescriptorHeap_Release(srv_heap);
  ID3D12DescriptorHeap_Release(dsv_heap);
  ID3D12Resource_Release(depth);
}

/* ---- levels and layers (the kernel's kFeature2MipmappedLayers) ---------------------------------------------------------------------------------------------------------- */

/* The colour (B8G8R8A8 as a 0xAARRGGBB word) a test writes into layer `layer` of level `level`. */
static UINT32 ml_colour(UINT level, UINT layer) { return 0xFF000000u | (0x20u + level * 0x40u) << 16 | (0x10u + layer * 0x20u) << 8 | 0x33u; }
static ID3D12Resource *ml_texture(ID3D12Device *dev, D3D12_RESOURCE_DIMENSION dim, UINT w, UINT h, UINT layers, UINT mips, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {dim, 0, w, h, (UINT16)layers, (UINT16)mips, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  ID3D12Resource *t = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t));
  return t;
}
/* Every subresource of a B8G8R8A8 texture its colour (a volume's level: every slice of it the colour of its slice number). */
static void ml_fill(Scene *c, ID3D12Resource *tex) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  const UINT volume = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D, layers = volume ? 1 : rd.DepthOrArraySize;
  static UINT32 texels[64 * 64 * 8];
  for (UINT layer = 0; layer < layers; ++layer)
    for (UINT level = 0; level < rd.MipLevels; ++level) {
      const UINT w = (UINT)rd.Width >> level ? (UINT)rd.Width >> level : 1, h = rd.Height >> level ? rd.Height >> level : 1;
      const UINT d = volume ? (rd.DepthOrArraySize >> level ? rd.DepthOrArraySize >> level : 1) : 1;
      for (UINT z = 0; z < d; ++z)
        for (UINT i = 0; i < w * h; ++i) texels[z * w * h + i] = ml_colour(level, volume ? z : layer);
      texture_write(c, tex, level + layer * rd.MipLevels, texels, 4);
    }
}
/* The texel (x, y) of subresource `sub` (of slice `z` of a volume's level), as the 0xAARRGGBB word of B8G8R8A8. */
static UINT32 ml_texel(Scene *c, ID3D12Resource *tex, UINT sub, UINT x, UINT y, UINT z) {
  static UINT32 texels[64 * 64 * 8];
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(tex);
  const UINT level = sub % rd.MipLevels, w = (UINT)rd.Width >> level ? (UINT)rd.Width >> level : 1, h = rd.Height >> level ? rd.Height >> level : 1;
  memset(texels, 0, sizeof texels);
  if (!texture_read(c, tex, sub, texels, 4)) return 0xDEADDEADu;
  return texels[(z * h + y) * w + x];
}
/* A draw of the whole target with vs_vol (its texture coordinate is the vertex colour: (u, v, w) and alpha `a` everywhere) and `ps`, sampling `tex` through `view` (point
 * filtering, the levels by the nearest); the colour of the target's middle. 0xDEADDEAD when the draw could not be made. */
static UINT32 ml_draw(Scene *c, const void *ps, SIZE_T ps_bytes, ID3D12Resource *tex, const D3D12_SHADER_RESOURCE_VIEW_DESC *view, float u, float v, float w, float a) {
  ID3D12Device *dev = c->dev;
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 16;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  D3D12_STATIC_SAMPLER_DESC ss;
  memset(&ss, 0, sizeof ss);
  ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  ss.MaxLOD = 1000.0f;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  if (blob) ID3D10Blob_Release(blob);
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_vol_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_vol_dxbc;
  pd.PS.pShaderBytecode = ps;
  pd.PS.BytecodeLength = ps_bytes;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = layout;
  pd.InputLayout.NumElements = 2;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  if (rs) CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12DescriptorHeap *heap = view_heap(dev, 1, 1);
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 3 * 28, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12GraphicsCommandList *list = NULL;
  UINT32 result = 0xDEADDEADu;
  if (pso && heap && vb) {
    ID3D12Device_CreateShaderResourceView(dev, tex, view, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap));
    const float verts[3][7] = {{-1, -1, 0.5f, u, v, w, a}, {3, -1, 0.5f, u, v, w, a}, {-1, 3, 0.5f, u, v, w, a}};
    void *mapped = NULL;
    CHECK_HR(ID3D12Resource_Map(vb, 0, NULL, &mapped));
    if (mapped) memcpy(mapped, verts, sizeof verts);
    ID3D12Resource_Unmap(vb, 0, NULL);
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  }
  if (list) {
    D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 3 * 28, 28};
    scene_begin(c, list, 1);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 16, kIdentity, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    scene_read_target(c, list);
    scene_run(c, list);
    const UINT32 *pixels = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(c->readback, 0, NULL, (void **)&pixels)) && pixels) {
      result = pixels[32 * 64 + 32];
      ID3D12Resource_Unmap(c->readback, 0, NULL);
    }
    ID3D12GraphicsCommandList_Release(list);
  }
  if (vb) ID3D12Resource_Release(vb);
  if (heap) ID3D12DescriptorHeap_Release(heap);
  if (pso) ID3D12PipelineState_Release(pso);
  if (rs) ID3D12RootSignature_Release(rs);
  return result;
}

static void test_mip_layers(Scene *c) {
  ID3D12Device *dev = c->dev;
  /* (what the device has: levels of arrays, volumes, cubes and the wide formats, render targets and depth buffers of levels) */
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs));
  CHECK(fs.Support1 & D3D12_FORMAT_SUPPORT1_MIP);
  ID3D12Resource *array = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 3, 4, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ID3D12Resource *box = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 8, 8, 4, 3, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *six = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 6, 2, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *twelve = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 12, 2, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *wide = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 2, 3, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ID3D12Resource *depth = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 2, 2, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
  if (!array || !box || !six || !twelve || !wide || !depth) { CHECK(0); return; }
  ml_fill(c, array);
  ml_fill(c, box);
  ml_fill(c, six);
  ml_fill(c, twelve);
  /* every subresource back as it was written: level 2 of layer 1, the second slice of level 1 of the volume */
  CHECK(ml_texel(c, array, 2 + 1 * 4, 1, 1, 0) == ml_colour(2, 1));
  CHECK(ml_texel(c, box, 1, 3, 3, 1) == ml_colour(1, 1));

  /* Shader resource views: an array view of layers 1 .. 2 from level 1 (SampleLevel 1 of it is level 2); a single slice; a 2D view of the volume's levels. */
  D3D12_SHADER_RESOURCE_VIEW_DESC sv;
  memset(&sv, 0, sizeof sv);
  sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  sv.Texture2DArray.MostDetailedMip = 1;
  sv.Texture2DArray.MipLevels = (UINT)-1;
  sv.Texture2DArray.FirstArraySlice = 1;
  sv.Texture2DArray.ArraySize = 2;
  CHECK(ml_draw(c, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, array, &sv, 0.5f, 0.5f, 1.0f, 1) == ml_colour(1, 2));
  CHECK(ml_draw(c, dx_ps_texarray_lod_dxbc, sizeof dx_ps_texarray_lod_dxbc, array, &sv, 0.5f, 0.5f, 0.0f, 1) == ml_colour(2, 1));
  CHECK(ml_draw(c, dx_ps_texarray_lod_dxbc, sizeof dx_ps_texarray_lod_dxbc, array, NULL, 0.5f, 0.5f, 2.0f, 1) == ml_colour(1, 2));
  sv.Texture2DArray.MipLevels = 1;
  CHECK(ml_draw(c, dx_ps_texarray_lod_dxbc, sizeof dx_ps_texarray_lod_dxbc, array, &sv, 0.5f, 0.5f, 0.0f, 1) == ml_colour(1, 1));   /* (one level: the lod clamped to it) */
  /* (GetDimensions of level 1 of the view - level 2 of the array: 2 x 2 -, its 2 layers and 3 levels; each / 16 - B8G8R8A8: 0xAARRGGBB) */
  sv.Texture2DArray.MipLevels = (UINT)-1;
  CHECK(ml_draw(c, dx_ps_texelarray_level_dxbc, sizeof dx_ps_texelarray_level_dxbc, array, &sv, 0.0f, 0.0f, 0.0f, 0) == 0x30202020u);
  CHECK(ml_draw(c, dx_ps_texelarray_level_dxbc, sizeof dx_ps_texelarray_level_dxbc, array, &sv, 0.0f, 0.0f, 1.0f, 1) == ml_colour(2, 2));   /* (Load of level 1, layer 1 of the view) */
  memset(&sv.Texture2DArray, 0, sizeof sv.Texture2DArray);
  sv.Texture2DArray.MipLevels = 1;
  sv.Texture2DArray.FirstArraySlice = 2;
  sv.Texture2DArray.ArraySize = 1;
  CHECK(ml_draw(c, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, array, &sv, 0.5f, 0.5f, 0.0f, 1) == ml_colour(0, 2));
  D3D12_SHADER_RESOURCE_VIEW_DESC vv;
  memset(&vv, 0, sizeof vv);
  vv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  vv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  vv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  vv.Texture3D.MipLevels = (UINT)-1;
  CHECK(ml_draw(c, dx_ps_tex3d_lod_dxbc, sizeof dx_ps_tex3d_lod_dxbc, box, &vv, 0.5f, 0.5f, 0.75f, 1) == ml_colour(1, 1));
  vv.Texture3D.MostDetailedMip = 1;
  CHECK(ml_draw(c, dx_ps_tex3d_lod_dxbc, sizeof dx_ps_tex3d_lod_dxbc, box, &vv, 0.5f, 0.5f, 0.25f, 1) == ml_colour(2, 0));
  /* An array of six seen as a cube, of twelve as a cube array (Direct3D 12 tells them apart by their views alone): +Z is the fifth face; cube 1 of the array. */
  D3D12_SHADER_RESOURCE_VIEW_DESC cv;
  memset(&cv, 0, sizeof cv);
  cv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  cv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  cv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  cv.TextureCube.MipLevels = (UINT)-1;
  CHECK(ml_draw(c, dx_ps_texcube_dxbc, sizeof dx_ps_texcube_dxbc, six, &cv, 0.1f, 0.15f, 1.0f, 1) == ml_colour(0, 4));
  cv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
  cv.TextureCubeArray.MipLevels = (UINT)-1;
  cv.TextureCubeArray.NumCubes = 2;
  CHECK(ml_draw(c, dx_ps_texcubearray_dxbc, sizeof dx_ps_texcubearray_dxbc, twelve, &cv, 0.55f, 0.57f, 1.0f, 0.1f) == ml_colour(0, 10));
  CHECK(ml_draw(c, dx_ps_texcubearray_lod_dxbc, sizeof dx_ps_texcubearray_lod_dxbc, twelve, &cv, 0.55f, 0.57f, 1.0f, 0.1f) == ml_colour(1, 10));
  cv.TextureCubeArray.First2DArrayFace = 6;
  cv.TextureCubeArray.NumCubes = 1;
  CHECK(ml_draw(c, dx_ps_texcubearray_dxbc, sizeof dx_ps_texcubearray_dxbc, twelve, &cv, 0.55f, 0.57f, 1.0f, 0.0f) == ml_colour(0, 10));

  /* Render target and depth views of a level: a clear of layer 2 of level 1, a draw into level 2 of layer 0 with a depth buffer of its size (level 1 of the 64 x 64 one is 32 x 32:
   * the draw's own target is a level of a 64 x 64 texture). */
  ID3D12Resource *targets = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 128, 128, 3, 3, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtvs = NULL, *dsvs = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = 1;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvs));
  if (!targets || !rtvs || !dsvs) { CHECK(0); return; }
  D3D12_CPU_DESCRIPTOR_HANDLE level1 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs), level2 = level1, dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsvs);
  level2.ptr += ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_RENDER_TARGET_VIEW_DESC rv;
  memset(&rv, 0, sizeof rv);
  rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
  rv.Texture2DArray.MipSlice = 1;
  rv.Texture2DArray.FirstArraySlice = 2;
  rv.Texture2DArray.ArraySize = 1;
  ID3D12Device_CreateRenderTargetView(dev, targets, &rv, level1);
  rv.Texture2DArray.MipSlice = 2;
  rv.Texture2DArray.FirstArraySlice = 0;
  ID3D12Device_CreateRenderTargetView(dev, targets, &rv, level2);
  D3D12_DEPTH_STENCIL_VIEW_DESC dv;
  memset(&dv, 0, sizeof dv);
  dv.Format = DXGI_FORMAT_D32_FLOAT;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
  dv.Texture2DArray.MipSlice = 1;
  dv.Texture2DArray.FirstArraySlice = 1;
  dv.Texture2DArray.ArraySize = 1;
  ID3D12Device_CreateDepthStencilView(dev, depth, &dv, dsv);
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, c->pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, level1, white, 0, NULL);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, level2, black, 0, NULL);
  ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &level2, FALSE, &dsv);
  D3D12_VIEWPORT vp = {0, 0, 32, 32, 0, 1};
  D3D12_RECT sc = {0, 0, 32, 32};
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  scene_state(c, list, ID3D12Resource_GetGPUVirtualAddress(c->vb));
  ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  /* (the scene's triangle covers the middle of the 32 x 32 level; its corner stays black; level 1 of layer 2 is white, of layer 0 as it was: zeros) */
  const UINT32 middle = ml_texel(c, targets, 2, 16, 20, 0), corner = ml_texel(c, targets, 2, 0, 0, 0);
  printf("level targets: middle %08x corner %08x, level 1 of layer 2 %08x\n", (unsigned)middle, (unsigned)corner, (unsigned)ml_texel(c, targets, 1 + 2 * 3, 5, 5, 0));
  CHECK((middle & 0xFFFFFF) != 0 && corner == 0xFF000000u && ml_texel(c, targets, 1 + 2 * 3, 5, 5, 0) == 0xFFFFFFFFu && ml_texel(c, targets, 1, 5, 5, 0) == 0);

  /* Copies: level 1 of layer 2 of the array into level 0 of a 4 x 4 texture; the whole array into another (CopyResource: every level of every layer); slice 1 of level 1 of
   * the volume into slice 0. */
  ID3D12Resource *flat = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *copy = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 3, 4, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *box2 = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 8, 8, 4, 3, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  if (!flat || !copy || !box2) { CHECK(0); return; }
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.Type = from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = flat;
  from.pResource = array;
  from.SubresourceIndex = 1 + 2 * 4;
  const D3D12_BOX two = {0, 0, 0, 2, 2, 1};
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 1, 1, 0, &from, &two);
  ID3D12GraphicsCommandList_CopyResource(list, copy, array);
  to.pResource = box2;
  to.SubresourceIndex = 1;
  from.pResource = box;
  from.SubresourceIndex = 1;
  const D3D12_BOX slice1 = {0, 0, 1, 4, 4, 2};
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, &slice1);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  CHECK(ml_texel(c, flat, 0, 1, 1, 0) == ml_colour(1, 2) && ml_texel(c, flat, 0, 2, 2, 0) == ml_colour(1, 2) && ml_texel(c, flat, 0, 3, 3, 0) == 0);
  int same = 1;
  for (UINT layer = 0; layer < 3; ++layer)
    for (UINT level = 0; level < 4; ++level) same &= ml_texel(c, copy, level + layer * 4, 0, 0, 0) == ml_colour(level, layer);
  CHECK(same);
  CHECK(ml_texel(c, box2, 1, 2, 2, 0) == ml_colour(1, 1) && ml_texel(c, box2, 1, 2, 2, 1) == 0);

  /* Unordered access to level 1 of the array's layers (cs_uav_array writes (x / 8, y / 8, layer / layers, 1)). */
  ID3D12Resource *storage = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 16, 16, 2, 3, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12RootSignature *rs = uav_signature(dev, 0, 1, 0);
  ID3D12PipelineState *fill = rs ? compute_pso(dev, rs, dx_cs_uav_array_dxbc, sizeof dx_cs_uav_array_dxbc) : NULL;
  ID3D12DescriptorHeap *heap = view_heap(dev, 1, 1);
  if (!storage || !rs || !fill || !heap) { CHECK(0); return; }
  D3D12_UNORDERED_ACCESS_VIEW_DESC u;
  memset(&u, 0, sizeof u);
  u.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  u.Texture2DArray.MipSlice = 1;
  u.Texture2DArray.ArraySize = 2;
  put_uav(dev, heap, NULL, 0, storage, &u);
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, fill, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 2);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  BYTE texels[8 * 8 * 4];
  CHECK(texture_read(c, storage, 1 + 1 * 3, texels, 4));
  /* (R8G8B8A8: texel (4, 0) of layer 1 is (0.5, 0, 0.5, 1)) */
  CHECK(texels[4 * 4] == 128 && texels[4 * 4 + 1] == 0 && texels[4 * 4 + 2] == 128 && texels[4 * 4 + 3] == 255);
  CHECK(texture_read(c, storage, 1, texels, 4) && texels[4 * 4] == 128 && texels[4 * 4 + 2] == 0);
  /* ... and to level 1 of the layers of a narrow array (R8: the kernel keeps it widened; the view goes through a mirror of that level's layers that gives the red back) */
  ID3D12Resource *narrow = ml_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 16, 16, 2, 3, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!narrow) { CHECK(0); return; }
  u.Format = DXGI_FORMAT_R8_UNORM;
  put_uav(dev, heap, NULL, 0, narrow, &u);
  CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, fill, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) return;
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
  ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 16, kIdentity, 0);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, heap_gpu(dev, heap, 0));
  ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 2);
  scene_run(c, list);
  ID3D12GraphicsCommandList_Release(list);
  BYTE reds[16 * 16];
  memset(reds, 0xEE, sizeof reds);
  CHECK(texture_read(c, narrow, 1 + 1 * 3, reds, 1) && reds[4] == 128 && reds[0] == 0 && reds[8 * 7 + 7] == 223);
  memset(reds, 0xEE, sizeof reds);
  CHECK(texture_read(c, narrow, 0 + 1 * 3, reds, 1) && reds[4] == 0 && reds[100] == 0);      /* (level 0 as it was) */
  ID3D12Resource_Release(narrow);
  printf("levels and layers: views, targets of levels, copies and storage checked\n");
  ID3D12DescriptorHeap_Release(heap);
  ID3D12PipelineState_Release(fill);
  ID3D12RootSignature_Release(rs);
  ID3D12Resource_Release(storage);
  ID3D12Resource_Release(box2);
  ID3D12Resource_Release(copy);
  ID3D12Resource_Release(flat);
  ID3D12DescriptorHeap_Release(dsvs);
  ID3D12DescriptorHeap_Release(rtvs);
  ID3D12Resource_Release(targets);
  ID3D12Resource_Release(depth);
  ID3D12Resource_Release(wide);
  ID3D12Resource_Release(twelve);
  ID3D12Resource_Release(six);
  ID3D12Resource_Release(box);
  ID3D12Resource_Release(array);
}

/* ---- the texture formats -------------------------------------------------------------------------------------------------------------------------------------------------- */

/* (generated by Graphics/BlockCompression.hpp's decoder, which BlockCompressionTest checks against Mesa's: one 4 x 4 block of each format and its texels as floats) */
typedef struct BcCase { DXGI_FORMAT format; BYTE block[16]; UINT32 texels[16][4]; } BcCase;
static const BcCase g_bc_cases[] = {
    {DXGI_FORMAT_BC4_UNORM, {0x2A, 0x80, 0x78, 0x94, 0x31, 0x2F, 0x12, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {{0x3E28A8A9u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F800000u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F008081u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E6DBA87u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F008081u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E996633u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3EBBEF22u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F008081u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F800000u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3EDE7812u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E28A8A9u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F008081u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F008081u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F800000u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F008081u, 0x00000000u, 0x00000000u, 0x3F800000u}}},
    {DXGI_FORMAT_BC4_SNORM, {0x43, 0xD4, 0xE6, 0x87, 0x99, 0x75, 0x07, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {{0xBDC61E86u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E1CA705u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0xBE62EA67u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E8E4114u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F070E1Cu, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E8E4114u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0xBDC61E86u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E1CA705u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3CE65F07u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0xBDC61E86u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3CE65F07u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E8E4114u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3F070E1Cu, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3ECE2EA6u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3CE65F07u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3ECE2EA6u, 0x00000000u, 0x00000000u, 0x3F800000u}}},
    {DXGI_FORMAT_BC5_UNORM, {0x0A, 0x63, 0xE6, 0x9B, 0xDF, 0xB0, 0xB0, 0xC9, 0xDE, 0xDE, 0x88, 0x4C, 0xD5, 0x44, 0x6B, 0xB4}, {{0x00000000u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3E7E9832u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3F800000u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3EA30970u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3EC6C6C7u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3F800000u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3F800000u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3D20A0A1u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x00000000u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3DDF45ACu, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3D20A0A1u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3E371D84u, 0x00000000u, 0x00000000u, 0x3F800000u}, {0x3E371D84u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x3DDF45ACu, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}, {0x00000000u, 0x3F5EDEDFu, 0x00000000u, 0x3F800000u}}},
    {DXGI_FORMAT_BC5_SNORM, {0x16, 0xDE, 0xF3, 0x58, 0xC5, 0x7A, 0x55, 0x8E, 0xE2, 0x77, 0x3D, 0xBB, 0x5E, 0x87, 0x6F, 0xE1}, {{0x3D418306u, 0x3F33CE02u, 0x00000000u, 0x3F800000u}, {0xBE112245u, 0x3F800000u, 0x00000000u, 0x3F800000u}, {0x3D418306u, 0x3EEF788Bu, 0x00000000u, 0x3F800000u}, {0xBC810204u, 0x3F33CE02u, 0x00000000u, 0x3F800000u}, {0xBDA14285u, 0x3E6EAA21u, 0x00000000u, 0x3F800000u}, {0x3DE1C387u, 0x3F33CE02u, 0x00000000u, 0x3F800000u}, {0xBE891224u, 0x3F800000u, 0x00000000u, 0x3F800000u}, {0xBE112245u, 0xBACE69A0u, 0x00000000u, 0x3F800000u}, {0x3DE1C387u, 0x3F800000u, 0x00000000u, 0x3F800000u}, {0xBE51A347u, 0xBE71E3C8u, 0x00000000u, 0x3F800000u}, {0xBDA14285u, 0xBF800000u, 0x00000000u, 0x3F800000u}, {0x3DE1C387u, 0x3F800000u, 0x00000000u, 0x3F800000u}, {0xBDA14285u, 0xBF800000u, 0x00000000u, 0x3F800000u}, {0xBC810204u, 0xBACE69A0u, 0x00000000u, 0x3F800000u}, {0x3D418306u, 0xBE71E3C8u, 0x00000000u, 0x3F800000u}, {0xBC810204u, 0x3F800000u, 0x00000000u, 0x3F800000u}}},
    {DXGI_FORMAT_BC6H_UF16, {0xC3, 0x2B, 0x53, 0x7B, 0x2D, 0xDD, 0xC9, 0xB4, 0x92, 0x1D, 0xF3, 0x4E, 0xAF, 0xCE, 0xC9, 0xDA}, {{0x3DDB6000u, 0x422FE000u, 0x424BE000u, 0x3F800000u}, {0x428B8000u, 0x417AA000u, 0x3F900000u, 0x3F800000u}, {0x44E38000u, 0x41202000u, 0x3E320000u, 0x3F800000u}, {0x3DDB6000u, 0x422FE000u, 0x424BE000u, 0x3F800000u}, {0x3F190000u, 0x41FFE000u, 0x4192A000u, 0x3F800000u}, {0x46214000u, 0x40F02000u, 0x3D78C000u, 0x3F800000u}, {0x45940000u, 0x41058000u, 0x3DCB2000u, 0x3F800000u}, {0x3FA64000u, 0x41EA8000u, 0x41404000u, 0x3F800000u}, {0x46214000u, 0x40F02000u, 0x3D78C000u, 0x3F800000u}, {0x433C0000u, 0x41600000u, 0x3F290000u, 0x3F800000u}, {0x45940000u, 0x41058000u, 0x3DCB2000u, 0x3F800000u}, {0x44566000u, 0x41356000u, 0x3E846000u, 0x3F800000u}, {0x428B8000u, 0x417AA000u, 0x3F900000u, 0x3F800000u}, {0x44566000u, 0x41356000u, 0x3E846000u, 0x3F800000u}, {0x433C0000u, 0x41600000u, 0x3F290000u, 0x3F800000u}, {0x44E38000u, 0x41202000u, 0x3E320000u, 0x3F800000u}}},
    {DXGI_FORMAT_BC6H_SF16, {0x03, 0xA1, 0xB3, 0x01, 0xE8, 0x7D, 0x74, 0x04, 0x00, 0x14, 0x1C, 0xBB, 0x92, 0x23, 0xD7, 0xA9}, {{0x4001E000u, 0xBCA5A000u, 0x00000000u, 0x3F800000u}, {0x4001E000u, 0xBCA5A000u, 0x00000000u, 0x3F800000u}, {0x3D566000u, 0xBC2A0000u, 0x370C0000u, 0x3F800000u}, {0x3F610000u, 0xBC888000u, 0x36000000u, 0x3F800000u}, {0xB4400000u, 0xBB330000u, 0x37D20000u, 0x3F800000u}, {0x3F610000u, 0xBC888000u, 0x36000000u, 0x3F800000u}, {0x38A04000u, 0xBB502000u, 0x37C18000u, 0x3F800000u}, {0x38A04000u, 0xBB502000u, 0x37C18000u, 0x3F800000u}, {0x3E980000u, 0xBC642000u, 0x36940000u, 0x3F800000u}, {0x3A0A2000u, 0xBB918000u, 0x379C0000u, 0x3F800000u}, {0x3DF72000u, 0xBC472000u, 0x36D60000u, 0x3F800000u}, {0x3E980000u, 0xBC642000u, 0x36940000u, 0x3F800000u}, {0x3B4BC000u, 0xBBCBA000u, 0x37770000u, 0x3F800000u}, {0xB8A14000u, 0xBB160000u, 0x37E28000u, 0x3F800000u}, {0x3A0A2000u, 0xBB918000u, 0x379C0000u, 0x3F800000u}, {0x39412000u, 0xBB6D2000u, 0x37B10000u, 0x3F800000u}}},
    {DXGI_FORMAT_BC7_UNORM, {0xC0, 0x06, 0xCE, 0x07, 0xDA, 0x9E, 0x6E, 0xBD, 0x95, 0xB6, 0xBE, 0xBA, 0xAC, 0x59, 0xAE, 0x34}, {{0x3E1C9C9Du, 0x3EEAEAEBu, 0x3F28A8A9u, 0x3EE2E2E3u}, {0x3E9C9C9Du, 0x3EB2B2B3u, 0x3EF2F2F3u, 0x3EECECEDu}, {0x3E78F8F9u, 0x3ECACACBu, 0x3F0D8D8Eu, 0x3EE8E8E9u}, {0x3EB4B4B5u, 0x3EA2A2A3u, 0x3ED6D6D7u, 0x3EF0F0F1u}, {0x3ED8D8D9u, 0x3E8A8A8Bu, 0x3EACACADu, 0x3EF4F4F5u}, {0x3EB4B4B5u, 0x3EA2A2A3u, 0x3ED6D6D7u, 0x3EF0F0F1u}, {0x3EAAAAABu, 0x3EAAAAABu, 0x3EE2E2E3u, 0x3EEEEEEFu}, {0x3EB4B4B5u, 0x3EA2A2A3u, 0x3ED6D6D7u, 0x3EF0F0F1u}, {0x3EC0C0C1u, 0x3E9A9A9Bu, 0x3EC8C8C9u, 0x3EF2F2F3u}, {0x3EAAAAABu, 0x3EAAAAABu, 0x3EE2E2E3u, 0x3EEEEEEFu}, {0x3E9C9C9Du, 0x3EB2B2B3u, 0x3EF2F2F3u, 0x3EECECEDu}, {0x3E5CDCDDu, 0x3ED2D2D3u, 0x3F159596u, 0x3EE6E6E7u}, {0x3ED8D8D9u, 0x3E8A8A8Bu, 0x3EACACADu, 0x3EF4F4F5u}, {0x3EAAAAABu, 0x3EAAAAABu, 0x3EE2E2E3u, 0x3EEEEEEFu}, {0x3E48C8C9u, 0x3EDADADBu, 0x3F1B9B9Cu, 0x3EE4E4E5u}, {0x3E30B0B1u, 0x3EE2E2E3u, 0x3F22A2A3u, 0x3EE2E2E3u}}},
    {DXGI_FORMAT_BC7_UNORM_SRGB, {0x40, 0xDB, 0x7B, 0x99, 0x90, 0x77, 0x8F, 0x5D, 0xA6, 0xC0, 0x85, 0xDE, 0x15, 0x5C, 0x2E, 0x09}, {{0x3F038384u, 0x3EF6F6F7u, 0x3F5BDBDCu, 0x3F179798u}, {0x3F39B9BAu, 0x3E74F4F5u, 0x3F48C8C9u, 0x3F2CACADu}, {0x3ED8D8D9u, 0x3F169697u, 0x3F64E4E5u, 0x3F0E8E8Fu}, {0x3F47C7C8u, 0x3E34B4B5u, 0x3F43C3C4u, 0x3F31B1B2u}, {0x3F119192u, 0x3ED6D6D7u, 0x3F56D6D7u, 0x3F1C9C9Du}, {0x3F29A9AAu, 0x3EA0A0A1u, 0x3F4ECECFu, 0x3F25A5A6u}, {0x3F57D7D8u, 0x3DD0D0D1u, 0x3F3DBDBEu, 0x3F37B7B8u}, {0x3F4ECECFu, 0x3E149495u, 0x3F40C0C1u, 0x3F34B4B5u}, {0x3F119192u, 0x3ED6D6D7u, 0x3F56D6D7u, 0x3F1C9C9Du}, {0x3EE6E6E7u, 0x3F0E8E8Fu, 0x3F61E1E2u, 0x3F119192u}, {0x3F47C7C8u, 0x3E34B4B5u, 0x3F43C3C4u, 0x3F31B1B2u}, {0x3F119192u, 0x3ED6D6D7u, 0x3F56D6D7u, 0x3F1C9C9Du}, {0x3F57D7D8u, 0x3DD0D0D1u, 0x3F3DBDBEu, 0x3F37B7B8u}, {0x3EF8F8F9u, 0x3F038384u, 0x3F5EDEDFu, 0x3F149495u}, {0x3F30B0B1u, 0x3E909091u, 0x3F4BCBCCu, 0x3F28A8A9u}, {0x3ED8D8D9u, 0x3F169697u, 0x3F64E4E5u, 0x3F0E8E8Fu}}},
};

/* An sRGB code as the linear value it means ((c + 0.055) / 1.055 to the 2.4, by a square times a fifth root found by Newton's iteration: no maths library). */
static float srgb_linear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  const double p = (c + 0.055) / 1.055, p2 = p * p;
  double r = p;
  for (int k = 0; k < 40; ++k) { const double r4 = r * r * r * r; r = r - (r4 * r - p2) / (5.0 * r4); }
  return (float)(p2 * r);
}
static float half_float(UINT16 h) {
  const UINT32 sign = (UINT32)(h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 0x3FFu;
  UINT32 b;
  if (e == 0) {
    if (!m) b = sign;
    else { UINT32 ex = 113, mm = m; while (!(mm & 0x400u)) { mm <<= 1; --ex; } b = sign | (ex << 23) | ((mm & 0x3FFu) << 13); }
  } else b = e == 31 ? sign | 0x7F800000u | (m << 13) : sign | ((e + 112) << 23) | (m << 13);
  float f;
  memcpy(&f, &b, 4);
  return f;
}
static float small_float(UINT32 v, UINT mb) {      /* (R11G11B10's: 5 exponent bits, `mb` of mantissa) */
  const UINT32 e = v >> mb, m = v & ((1u << mb) - 1);
  return half_float((UINT16)((e << 10) | (m << (10 - mb))));
}
static float bits_of(UINT32 v) { float f; memcpy(&f, &v, 4); return f; }
static UINT32 float_bits_(float f) { UINT32 v; memcpy(&v, &f, 4); return v; }

/* What a shader's Load of a texel of `format` holding the bytes `t` gives (integers as their bits). */
typedef enum { FK_UNORM, FK_SNORM, FK_UINT, FK_SINT, FK_FLOAT } FormatKind;
typedef struct LoadCase { DXGI_FORMAT format; UINT bytes, channels, bits; FormatKind kind; int load; } LoadCase;      /* (load: 0 float, 1 uint, 2 sint view) */
static void reference_texel(const LoadCase *f, const BYTE *t, float out[4]) {
  const int integer = f->kind == FK_UINT || f->kind == FK_SINT;
  out[0] = out[1] = out[2] = 0;
  out[3] = integer ? bits_of(1) : 1.0f;
  UINT32 w = 0;
  memcpy(&w, t, f->bytes < 4 ? f->bytes : 4);
  switch (f->format) {
    case DXGI_FORMAT_R11G11B10_FLOAT: out[0] = small_float(w & 0x7FF, 6); out[1] = small_float((w >> 11) & 0x7FF, 6); out[2] = small_float(w >> 22, 5); return;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: {
      float scale = 1.0f;
      for (int e = (int)(w >> 27) - 24; e > 0; --e) scale *= 2.0f;
      for (int e = (int)(w >> 27) - 24; e < 0; ++e) scale *= 0.5f;
      out[0] = (float)(w & 511) * scale; out[1] = (float)((w >> 9) & 511) * scale; out[2] = (float)((w >> 18) & 511) * scale;
      return;
    }
    case DXGI_FORMAT_R10G10B10A2_UINT: out[0] = bits_of(w & 1023); out[1] = bits_of((w >> 10) & 1023); out[2] = bits_of((w >> 20) & 1023); out[3] = bits_of(w >> 30); return;
    case DXGI_FORMAT_B5G6R5_UNORM: out[0] = (w >> 11 & 31) / 31.0f; out[1] = (w >> 5 & 63) / 63.0f; out[2] = (w & 31) / 31.0f; return;
    case DXGI_FORMAT_B5G5R5A1_UNORM: out[0] = (w >> 10 & 31) / 31.0f; out[1] = (w >> 5 & 31) / 31.0f; out[2] = (w & 31) / 31.0f; out[3] = (float)(w >> 15 & 1); return;
    case DXGI_FORMAT_B4G4R4A4_UNORM: out[0] = (w >> 8 & 15) / 15.0f; out[1] = (w >> 4 & 15) / 15.0f; out[2] = (w & 15) / 15.0f; out[3] = (w >> 12 & 15) / 15.0f; return;
    case DXGI_FORMAT_A8_UNORM: out[3] = t[0] / 255.0f; return;
    default: break;
  }
  for (UINT c = 0; c < f->channels; ++c) {
    UINT32 v = 0;
    memcpy(&v, t + c * (f->bits / 8), f->bits / 8);
    const INT32 s = f->bits == 8 ? (INT8)v : f->bits == 16 ? (INT16)v : (INT32)v;
    const float max = f->bits == 8 ? 255.0f : 65535.0f, smax = f->bits == 8 ? 127.0f : 32767.0f;
    switch (f->kind) {
      case FK_UNORM: out[c] = (float)v / max; break;
      case FK_SNORM: out[c] = (float)s / smax < -1.0f ? -1.0f : (float)s / smax; break;
      case FK_UINT: out[c] = bits_of(v); break;
      case FK_SINT: out[c] = bits_of((UINT32)s); break;
      case FK_FLOAT: out[c] = f->bits == 16 ? half_float((UINT16)v) : bits_of(v); break;
    }
  }
}
static int near_value(float got, float want, float tolerance) {
  if (want != want) return got != got;
  if (got != got) return 0;
  const float d = got > want ? got - want : want - got;
  return d <= tolerance || (got == want);
}

/* A compute shader (`code`) reading a 4 x 4 texture through its shader resource view (t0, with a bilinear clamping sampler s0) into a buffer of 16 float4s (u0). */
/* (the same through a view whose first level is `level`) */
static int format_dispatch_level(Scene *c, ID3D12RootSignature *rs, const void *code, SIZE_T size, ID3D12Resource *tex, DXGI_FORMAT view, UINT level, float out[16][4]) {
  ID3D12Device *dev = c->dev;
  ID3D12PipelineState *pso = compute_pso(dev, rs, code, size);
  ID3D12Resource *buf = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12DescriptorHeap *heap = view_heap(dev, 2, 1);
  ID3D12GraphicsCommandList *list = NULL;
  int ok = 0;
  if (pso && buf && down && heap) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = view;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MostDetailedMip = level;
    sd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, tex, &sd, heap_cpu(dev, heap, 0));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 16;
    ud.Buffer.StructureByteStride = 16;
    ID3D12Device_CreateUnorderedAccessView(dev, buf, NULL, &ud, heap_cpu(dev, heap, 1));
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
  }
  if (list) {
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, heap_gpu(dev, heap, 0));
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = buf;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, down, 0, buf, 0, 256);
    scene_run(c, list);
    ID3D12GraphicsCommandList_Release(list);
    const float *p = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(down, 0, NULL, (void **)&p)) && p) {
      memcpy(out, p, 256);
      ok = 1;
      ID3D12Resource_Unmap(down, 0, NULL);
    }
  }
  if (pso) ID3D12PipelineState_Release(pso);
  if (buf) ID3D12Resource_Release(buf);
  if (down) ID3D12Resource_Release(down);
  if (heap) ID3D12DescriptorHeap_Release(heap);
  return ok;
}
static int format_dispatch(Scene *c, ID3D12RootSignature *rs, const void *code, SIZE_T size, ID3D12Resource *tex, DXGI_FORMAT view, float out[16][4]) {
  return format_dispatch_level(c, rs, code, size, tex, view, 0, out);
}
static ID3D12RootSignature *format_signature(ID3D12Device *dev) {
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
  D3D12_ROOT_PARAMETER param;
  memset(&param, 0, sizeof param);
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable.NumDescriptorRanges = 2;
  param.DescriptorTable.pDescriptorRanges = ranges;
  D3D12_STATIC_SAMPLER_DESC ss;
  memset(&ss, 0, sizeof ss);
  ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  ss.MaxLOD = 16;
  ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = NULL, *error = NULL;
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) {
    CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);
  }
  return rs;
}
static D3D12_FORMAT_SUPPORT1 fmt_support(ID3D12Device *dev, DXGI_FORMAT f, D3D12_FORMAT_SUPPORT2 *s2) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT s = {f, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof s));
  if (s2) *s2 = s.Support2;
  return s.Support1;
}
static int texture_makes(ID3D12Device *dev, DXGI_FORMAT f, UINT mips, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, (UINT16)mips, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  ID3D12Resource *t = NULL;
  const HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t);
  if (t) ID3D12Resource_Release(t);
  return SUCCEEDED(hr);
}
/* The bytes a render target of `format` keeps of `v` (the Direct3D conversions; integers as the values' bits), and how far a GPU's rounding may be from them, in codes. */
static UINT target_bytes(DXGI_FORMAT format, const float v[4], BYTE out[16]) {
  UINT32 u[4];
  for (int k = 0; k < 4; ++k) u[k] = float_bits_(v[k]);
  UINT32 w = 0;
  switch (format) {
    case DXGI_FORMAT_R8_UNORM: out[0] = unorm8(v[0]); return 1;
    case DXGI_FORMAT_R8G8_UNORM: out[0] = unorm8(v[0]); out[1] = unorm8(v[1]); return 2;
    case DXGI_FORMAT_A8_UNORM: out[0] = unorm8(v[3]); return 1;
    case DXGI_FORMAT_R8_SNORM: out[0] = (BYTE)(INT8)(v[0] * 127.0f + (v[0] < 0 ? -0.5f : 0.5f)); return 1;
    case DXGI_FORMAT_R8G8B8A8_SNORM: for (int k = 0; k < 4; ++k) out[k] = (BYTE)(INT8)(v[k] * 127.0f + (v[k] < 0 ? -0.5f : 0.5f)); return 4;
    case DXGI_FORMAT_R8_UINT: out[0] = (BYTE)(u[0] > 255 ? 255 : u[0]); return 1;
    case DXGI_FORMAT_R8G8B8A8_SINT: for (int k = 0; k < 4; ++k) out[k] = (BYTE)(INT8)((INT32)u[k] > 127 ? 127 : (INT32)u[k] < -128 ? -128 : (INT32)u[k]); return 4;
    case DXGI_FORMAT_R16_UINT: w = u[0] > 65535 ? 65535 : u[0]; memcpy(out, &w, 2); return 2;
    case DXGI_FORMAT_R16G16_SNORM: for (int k = 0; k < 2; ++k) { const INT16 s = (INT16)(v[k] * 32767.0f + (v[k] < 0 ? -0.5f : 0.5f)); memcpy(out + 2 * k, &s, 2); } return 4;
    case DXGI_FORMAT_R16G16_FLOAT: { const UINT16 h[2] = {0x3C00, 0xC000}; memcpy(out, h, 4); return 4; }      /* (v is 1, -2) */
    case DXGI_FORMAT_R16_FLOAT: { const UINT16 h = 0x3800; memcpy(out, &h, 2); return 2; }      /* (0.5) */
    case DXGI_FORMAT_R32G32_FLOAT: memcpy(out, v, 8); return 8;
    case DXGI_FORMAT_R32G32_UINT: memcpy(out, u, 8); return 8;
    case DXGI_FORMAT_R11G11B10_FLOAT: w = (15u << 6) | (14u << 6 | 32u) << 11 | (16u << 5) << 22; memcpy(out, &w, 4); return 4;      /* (1, 0.75, 2) */
    case DXGI_FORMAT_R10G10B10A2_UINT: w = (u[0] & 1023) | (u[1] & 1023) << 10 | (u[2] & 1023) << 20 | (u[3] & 3) << 30; memcpy(out, &w, 4); return 4;
    case DXGI_FORMAT_B5G6R5_UNORM: w = (UINT32)(v[2] * 31 + 0.5f) | (UINT32)(v[1] * 63 + 0.5f) << 5 | (UINT32)(v[0] * 31 + 0.5f) << 11; memcpy(out, &w, 2); return 2;
    case DXGI_FORMAT_B5G5R5A1_UNORM: w = (UINT32)(v[2] * 31 + 0.5f) | (UINT32)(v[1] * 31 + 0.5f) << 5 | (UINT32)(v[0] * 31 + 0.5f) << 10 | (v[3] >= 0.5f ? 1u : 0u) << 15; memcpy(out, &w, 2); return 2;
    case DXGI_FORMAT_B4G4R4A4_UNORM: w = (UINT32)(v[2] * 15 + 0.5f) | (UINT32)(v[1] * 15 + 0.5f) << 4 | (UINT32)(v[0] * 15 + 0.5f) << 8 | (UINT32)(v[3] * 15 + 0.5f) << 12; memcpy(out, &w, 2); return 2;
    default: return 0;
  }
}
/* One draw of `colour` (through ps, the float, unsigned or signed shader) into a 4 x 4 render target of `format`; its texel (1, 1) back into `out`. */
static int format_draw(Scene *c, DXGI_FORMAT format, const void *ps, SIZE_T ps_size, const float colour[4], BYTE out[16], UINT bytes) {
  ID3D12Device *dev = c->dev;
  ID3D12Resource *t = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  D3D12_ROOT_PARAMETER param;
  memset(&param, 0, sizeof param);
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  param.Constants.Num32BitValues = 4;
  D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (blob) {
    CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    ID3D10Blob_Release(blob);
  }
  /* (a triangle over the whole target) */
  ID3D12Resource *vb = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  const float corners[6] = {-1, 1, 3, 1, -1, -3};
  void *mapped = NULL;
  if (vb && SUCCEEDED(ID3D12Resource_Map(vb, 0, NULL, &mapped)) && mapped) {
    memcpy(mapped, corners, sizeof corners);
    ID3D12Resource_Unmap(vb, 0, NULL);
  }
  D3D12_INPUT_ELEMENT_DESC layout = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = dx_vs_fmt_quad_dxbc;
  pd.VS.BytecodeLength = sizeof dx_vs_fmt_quad_dxbc;
  pd.PS.pShaderBytecode = ps;
  pd.PS.BytecodeLength = ps_size;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.InputLayout.pInputElementDescs = &layout;
  pd.InputLayout.NumElements = 1;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = format;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = NULL;
  if (rs) CHECK_HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *rtvs = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
  ID3D12GraphicsCommandList *list = NULL;
  int ok = 0;
  if (t && pso && rtvs && vb) {
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, t, NULL, rtv);
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (list) {
      D3D12_RESOURCE_BARRIER b;
      memset(&b, 0, sizeof b);
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = t;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
      const D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
      const D3D12_RECT sc = {0, 0, 4, 4};
      ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
      ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
      ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
      ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
      ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, colour, 0);
      ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      const D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), 256, 8};
      ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
      BYTE texels[16 * 16];
      ok = texture_read(c, t, 0, texels, bytes);
      if (ok) memcpy(out, texels + (1 * 4 + 1) * bytes, bytes);
    }
  }
  if (t) ID3D12Resource_Release(t);
  if (vb) ID3D12Resource_Release(vb);
  if (pso) ID3D12PipelineState_Release(pso);
  if (rs) ID3D12RootSignature_Release(rs);
  if (rtvs) ID3D12DescriptorHeap_Release(rtvs);
  return ok;
}

/* Every colour format a texture can have, with its real channels and encoding: what CheckFeatureSupport says is what textures can be made (sampled, drawn into, unordered
 * access, levels); the texels written are what a shader's Load reads (normalised, signed, integer, half and single floats, the packed floats, 5:6:5 and the other 16-bit ones,
 * alpha alone, 96-bit), copies give them back as written; render targets of each kind keep what a pixel shader writes in their own encoding; BC4 .. BC7 blocks are read as their
 * decoded texels (BC7's sRGB form linearised, and filtered on the linear values); textures of the largest size the adapter says it has are made, larger ones refused. */
static void test_formats(Scene *c) {
  ID3D12Device *dev = c->dev;
  static const DXGI_FORMAT all[] = {
      DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_UINT,
      DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT,
      DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_FLOAT,
      DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32_FLOAT,
      DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM,
      DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16, DXGI_FORMAT_BC7_UNORM,
      DXGI_FORMAT_BC7_UNORM_SRGB};
  /* (what the support says is what can be made; the formats every Direct3D 11 device samples are sampled here too) */
  int sampled = 0;
  for (UINT i = 0; i < sizeof all / sizeof all[0]; ++i) {
    D3D12_FORMAT_SUPPORT2 s2;
    const D3D12_FORMAT_SUPPORT1 s1 = fmt_support(dev, all[i], &s2);
    const int load = (s1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
    sampled += load;
    if (texture_makes(dev, all[i], 1, D3D12_RESOURCE_FLAG_NONE) != load) { printf("  %u: texture %d, support %08x\n", all[i], !load, (UINT)s1); CHECK(0); }
    if (texture_makes(dev, all[i], 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != ((s1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0)) { printf("  %u: render target\n", all[i]); CHECK(0); }
    if (texture_makes(dev, all[i], 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != ((s1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0)) { printf("  %u: uav\n", all[i]); CHECK(0); }
    if (load && texture_makes(dev, all[i], 4, D3D12_RESOURCE_FLAG_NONE) != ((s1 & D3D12_FORMAT_SUPPORT1_MIP) != 0)) { printf("  %u: levels\n", all[i]); CHECK(0); }
    if ((s1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) && !(s1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) CHECK(0);
  }
  if (!g_hardware) CHECK(sampled == (int)(sizeof all / sizeof all[0]));      /* (the CPU renderer has every one) */
  printf("formats (Load, Sample, render Target, Blend, Uav, Mips):");
  for (UINT i = 0; i < sizeof all / sizeof all[0]; ++i) {
    const D3D12_FORMAT_SUPPORT1 s1 = fmt_support(dev, all[i], NULL);
    printf(" %u:%s%s%s%s%s%s", all[i], (s1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ? "L" : "", (s1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ? "S" : "",
           (s1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ? "T" : "", (s1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) ? "B" : "", (s1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ? "U" : "",
           (s1 & D3D12_FORMAT_SUPPORT1_MIP) ? "M" : "");
  }
  printf("\n");
  D3D12_FORMAT_SUPPORT2 s2;
  CHECK(!(fmt_support(dev, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, &s2) & D3D12_FORMAT_SUPPORT1_RENDER_TARGET));      /* (never a render target in Direct3D) */
  CHECK(!(fmt_support(dev, DXGI_FORMAT_R32G32B32_FLOAT, &s2) & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
  CHECK(fmt_support(dev, DXGI_FORMAT_R32G32_FLOAT, &s2) & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER);
  if (!g_hardware) {
    CHECK((fmt_support(dev, DXGI_FORMAT_R11G11B10_FLOAT, &s2) & (D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                                                                   D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) == (D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE |
                                                                   D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW));
    CHECK(fmt_support(dev, DXGI_FORMAT_B5G6R5_UNORM, &s2) & D3D12_FORMAT_SUPPORT1_MIP);
    CHECK((fmt_support(dev, DXGI_FORMAT_BC7_UNORM, &s2) & (D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_MIP)) == (D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_MIP));
    CHECK(!(fmt_support(dev, DXGI_FORMAT_R8_UINT, &s2) & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) && (fmt_support(dev, DXGI_FORMAT_R8_UINT, &s2) & D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
  }

  /* (Load of the texels written: every kind of format) */
  ID3D12RootSignature *rs = format_signature(dev);
  if (!rs) { CHECK(0); return; }
  static const LoadCase loads[] = {
      {DXGI_FORMAT_R8_UNORM, 1, 1, 8, FK_UNORM, 0}, {DXGI_FORMAT_R8_SNORM, 1, 1, 8, FK_SNORM, 0}, {DXGI_FORMAT_R8_UINT, 1, 1, 8, FK_UINT, 1}, {DXGI_FORMAT_R8_SINT, 1, 1, 8, FK_SINT, 2},
      {DXGI_FORMAT_A8_UNORM, 1, 0, 8, FK_UNORM, 0}, {DXGI_FORMAT_R8G8_UNORM, 2, 2, 8, FK_UNORM, 0}, {DXGI_FORMAT_R8G8_SNORM, 2, 2, 8, FK_SNORM, 0}, {DXGI_FORMAT_R8G8_UINT, 2, 2, 8, FK_UINT, 1},
      {DXGI_FORMAT_R8G8_SINT, 2, 2, 8, FK_SINT, 2}, {DXGI_FORMAT_R8G8B8A8_SNORM, 4, 4, 8, FK_SNORM, 0}, {DXGI_FORMAT_R8G8B8A8_SINT, 4, 4, 8, FK_SINT, 2},
      {DXGI_FORMAT_R16_UNORM, 2, 1, 16, FK_UNORM, 0}, {DXGI_FORMAT_R16_SNORM, 2, 1, 16, FK_SNORM, 0}, {DXGI_FORMAT_R16_UINT, 2, 1, 16, FK_UINT, 1}, {DXGI_FORMAT_R16_SINT, 2, 1, 16, FK_SINT, 2},
      {DXGI_FORMAT_R16_FLOAT, 2, 1, 16, FK_FLOAT, 0}, {DXGI_FORMAT_R16G16_UNORM, 4, 2, 16, FK_UNORM, 0}, {DXGI_FORMAT_R16G16_SNORM, 4, 2, 16, FK_SNORM, 0},
      {DXGI_FORMAT_R16G16_UINT, 4, 2, 16, FK_UINT, 1}, {DXGI_FORMAT_R16G16_SINT, 4, 2, 16, FK_SINT, 2}, {DXGI_FORMAT_R16G16_FLOAT, 4, 2, 16, FK_FLOAT, 0},
      {DXGI_FORMAT_R32G32_UINT, 8, 2, 32, FK_UINT, 1}, {DXGI_FORMAT_R32G32_SINT, 8, 2, 32, FK_SINT, 2}, {DXGI_FORMAT_R32G32_FLOAT, 8, 2, 32, FK_FLOAT, 0},
      {DXGI_FORMAT_R32G32B32_UINT, 12, 3, 32, FK_UINT, 1}, {DXGI_FORMAT_R32G32B32_SINT, 12, 3, 32, FK_SINT, 2}, {DXGI_FORMAT_R32G32B32_FLOAT, 12, 3, 32, FK_FLOAT, 0},
      {DXGI_FORMAT_R11G11B10_FLOAT, 4, 3, 0, FK_FLOAT, 0}, {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 4, 3, 0, FK_FLOAT, 0}, {DXGI_FORMAT_R10G10B10A2_UINT, 4, 4, 0, FK_UINT, 1},
      {DXGI_FORMAT_B5G6R5_UNORM, 2, 3, 0, FK_UNORM, 0}, {DXGI_FORMAT_B5G5R5A1_UNORM, 2, 4, 0, FK_UNORM, 0}, {DXGI_FORMAT_B4G4R4A4_UNORM, 2, 4, 0, FK_UNORM, 0}};
  for (UINT i = 0; i < sizeof loads / sizeof loads[0]; ++i) {
    const LoadCase *f = &loads[i];
    if (!(fmt_support(dev, f->format, NULL) & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) continue;      /* (the adapter has no such textures: refused above, as it says) */
    BYTE texels[16 * 12], back[16 * 12];
    for (UINT k = 0; k < 16 * f->bytes; ++k) texels[k] = (BYTE)(k * 37 + i * 11 + 5);
    if (f->kind == FK_FLOAT && f->bits)      /* (finite floats: a NaN reads as some NaN) */
      for (UINT k = f->bits / 8 - 1; k < 16 * f->bytes; k += f->bits / 8) texels[k] &= 0x3F;
    ID3D12Resource *t = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, f->format, D3D12_RESOURCE_FLAG_NONE);
    if (!t) { CHECK(0); continue; }
    texture_write(c, t, 0, texels, f->bytes);
    CHECK(texture_read(c, t, 0, back, f->bytes) && memcmp(back, texels, 16 * f->bytes) == 0);      /* (copies keep the bytes) */
    float got[16][4];
    const void *code = f->load == 1 ? dx_cs_fmt_loadu_dxbc : f->load == 2 ? dx_cs_fmt_loadi_dxbc : dx_cs_fmt_load_dxbc;
    const SIZE_T size = f->load == 1 ? sizeof dx_cs_fmt_loadu_dxbc : f->load == 2 ? sizeof dx_cs_fmt_loadi_dxbc : sizeof dx_cs_fmt_load_dxbc;
    if (format_dispatch(c, rs, code, size, t, f->format, got)) {
      int wrong = 0;
      for (UINT k = 0; k < 16; ++k) {
        float want[4];
        reference_texel(f, texels + k * f->bytes, want);
        for (UINT ch = 0; ch < 4; ++ch) {
          const int integer = f->kind == FK_UINT || f->kind == FK_SINT;
          const int ok = integer ? float_bits_(got[k][ch]) == float_bits_(want[ch]) : near_value(got[k][ch], want[ch], f->kind == FK_FLOAT ? 0.0f : 1e-5f);
          if (!ok && wrong++ < 2) printf("  format %u texel %u channel %u: %08x, not %08x\n", f->format, k, ch, float_bits_(got[k][ch]), float_bits_(want[ch]));
        }
      }
      CHECK(wrong == 0);
    } else CHECK(0);
    ID3D12Resource_Release(t);
  }

  /* (BC4 .. BC7: the decoded texels, the sRGB one linearised - Load and a bilinear sample half way between two texels, filtered on the linear values) */
  for (UINT i = 0; i < sizeof g_bc_cases / sizeof g_bc_cases[0]; ++i) {
    const BcCase *bc = &g_bc_cases[i];
    if (!(fmt_support(dev, bc->format, NULL) & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) continue;
    const UINT block = bc->format == DXGI_FORMAT_BC4_UNORM || bc->format == DXGI_FORMAT_BC4_SNORM ? 8 : 16;
    ID3D12Resource *t = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, bc->format, D3D12_RESOURCE_FLAG_NONE);
    if (!t) { CHECK(0); continue; }
    texture_write(c, t, 0, bc->block, block / 4);      /* (one row of blocks: "4 texels" of a quarter block each) */
    const int srgb = bc->format == DXGI_FORMAT_BC7_UNORM_SRGB, bc6 = bc->format == DXGI_FORMAT_BC6H_UF16 || bc->format == DXGI_FORMAT_BC6H_SF16;
    float want[16][4], got[16][4];
    for (UINT k = 0; k < 16; ++k)
      for (UINT ch = 0; ch < 4; ++ch) want[k][ch] = srgb && ch < 3 ? srgb_linear(bits_of(bc->texels[k][ch])) : bits_of(bc->texels[k][ch]);
    /* (a GPU decodes BC4 / BC5 in 8 bits or in floats: within a code of 8 bits - of the signed ones, a step of 1 / 127) */
    const int bc_snorm = bc->format == DXGI_FORMAT_BC4_SNORM || bc->format == DXGI_FORMAT_BC5_SNORM;
    const float tolerance = bc6 ? 0.0f : bc_snorm ? 1.0f / 127.0f : 1.0f / 254.0f;
    int wrong = 0;
    if (format_dispatch(c, rs, dx_cs_fmt_load_dxbc, sizeof dx_cs_fmt_load_dxbc, t, bc->format, got)) {
      for (UINT k = 0; k < 16; ++k)
        for (UINT ch = 0; ch < 4; ++ch)
          if (!near_value(got[k][ch], want[k][ch], tolerance + (srgb ? 1e-4f : 0.0f)) && wrong++ < 2)
            printf("  format %u texel %u channel %u: %g, not %g\n", bc->format, k, ch, got[k][ch], want[k][ch]);
    } else CHECK(0);
    CHECK(wrong == 0);
    if (format_dispatch(c, rs, dx_cs_fmt_sample_dxbc, sizeof dx_cs_fmt_sample_dxbc, t, bc->format, got)) {
      wrong = 0;
      for (UINT y = 0; y < 4; ++y)
        for (UINT x = 0; x < 4; ++x)
          for (UINT ch = 0; ch < 4; ++ch) {
            const float mid = (want[y * 4 + x][ch] + want[y * 4 + (x < 3 ? x + 1 : 3)][ch]) * 0.5f;
            const float scale = mid < 0 ? -mid : mid;
            if (!near_value(got[y * 4 + x][ch], mid, tolerance + 2e-3f * (scale > 1 ? scale : 1)) && wrong++ < 2)
              printf("  format %u sample %u,%u channel %u: %g, not %g\n", bc->format, x, y, ch, got[y * 4 + x][ch], mid);
          }
      CHECK(wrong == 0);
    } else CHECK(0);
    ID3D12Resource_Release(t);
  }

  /* (levels of the formats whose texture says it has them - SUPPORT1_MIP: the wide ones and the block-compressed ones decoded into floats too, on a device with levels of every
   * image -: an 8 x 8 texture of two levels, level 1 read through a view that begins there, level 0 holding other texels) */
  static const LoadCase level_loads[] = {
      {DXGI_FORMAT_R16_FLOAT, 2, 1, 16, FK_FLOAT, 0}, {DXGI_FORMAT_R16G16_UNORM, 4, 2, 16, FK_UNORM, 0}, {DXGI_FORMAT_R32G32_UINT, 8, 2, 32, FK_UINT, 1},
      {DXGI_FORMAT_R32G32B32_FLOAT, 12, 3, 32, FK_FLOAT, 0}, {DXGI_FORMAT_R11G11B10_FLOAT, 4, 3, 0, FK_FLOAT, 0}, {DXGI_FORMAT_R8G8_SINT, 2, 2, 8, FK_SINT, 2},
      {DXGI_FORMAT_B5G6R5_UNORM, 2, 3, 0, FK_UNORM, 0}};
  int level_formats = 0;
  for (UINT i = 0; i < sizeof level_loads / sizeof level_loads[0]; ++i) {
    const LoadCase *f = &level_loads[i];
    if (!(fmt_support(dev, f->format, NULL) & D3D12_FORMAT_SUPPORT1_MIP)) continue;
    ++level_formats;
    D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 2, f->format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *t = NULL;
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t));
    if (!t) continue;
    BYTE base[64 * 12], texels[16 * 12], back[16 * 12];
    for (UINT k = 0; k < 64 * f->bytes; ++k) base[k] = (BYTE)(k * 13 + 1);
    for (UINT k = 0; k < 16 * f->bytes; ++k) texels[k] = (BYTE)(k * 29 + i * 7 + 3);
    if (f->kind == FK_FLOAT && f->bits) {      /* (finite floats) */
      for (UINT k = f->bits / 8 - 1; k < 64 * f->bytes; k += f->bits / 8) base[k] &= 0x3F;
      for (UINT k = f->bits / 8 - 1; k < 16 * f->bytes; k += f->bits / 8) texels[k] &= 0x3F;
    }
    texture_write(c, t, 0, base, f->bytes);
    texture_write(c, t, 1, texels, f->bytes);
    CHECK(texture_read(c, t, 1, back, f->bytes) && memcmp(back, texels, 16 * f->bytes) == 0);      /* (the level's copy keeps its bytes) */
    float got[16][4];
    const void *code = f->load == 1 ? dx_cs_fmt_loadu_dxbc : f->load == 2 ? dx_cs_fmt_loadi_dxbc : dx_cs_fmt_load_dxbc;
    const SIZE_T size = f->load == 1 ? sizeof dx_cs_fmt_loadu_dxbc : f->load == 2 ? sizeof dx_cs_fmt_loadi_dxbc : sizeof dx_cs_fmt_load_dxbc;
    if (format_dispatch_level(c, rs, code, size, t, f->format, 1, got)) {
      int wrong = 0;
      for (UINT k = 0; k < 16; ++k) {
        float want[4];
        reference_texel(f, texels + k * f->bytes, want);
        for (UINT ch = 0; ch < 4; ++ch) {
          const int integer = f->kind == FK_UINT || f->kind == FK_SINT;
          const int ok = integer ? float_bits_(got[k][ch]) == float_bits_(want[ch]) : near_value(got[k][ch], want[ch], f->kind == FK_FLOAT ? 0.0f : 1e-5f);
          if (!ok && wrong++ < 2) printf("  format %u level 1 texel %u channel %u: %08x, not %08x\n", f->format, k, ch, float_bits_(got[k][ch]), float_bits_(want[ch]));
        }
      }
      CHECK(wrong == 0);
    } else CHECK(0);
    ID3D12Resource_Release(t);
  }
  /* (... and of the block-compressed ones: level 1 of an 8 x 8 texture is one block; level 0 is four blocks of another case) */
  for (UINT i = 0; i < sizeof g_bc_cases / sizeof g_bc_cases[0]; ++i) {
    const BcCase *bc = &g_bc_cases[i];
    if (!(fmt_support(dev, bc->format, NULL) & D3D12_FORMAT_SUPPORT1_MIP)) continue;
    ++level_formats;
    const UINT block = bc->format == DXGI_FORMAT_BC4_UNORM || bc->format == DXGI_FORMAT_BC4_SNORM ? 8 : 16;
    D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 2, bc->format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *t = NULL;
    CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t));
    if (!t) continue;
    BYTE four[4 * 16];
    for (UINT k = 0; k < 4 * block; ++k) four[k] = (BYTE)(k * 7 + 0x41);
    texture_write(c, t, 0, four, block / 4);      /* (two rows of two blocks: "8 texels" of a quarter block each) */
    texture_write(c, t, 1, bc->block, block / 4);
    const int srgb = bc->format == DXGI_FORMAT_BC7_UNORM_SRGB, bc6 = bc->format == DXGI_FORMAT_BC6H_UF16 || bc->format == DXGI_FORMAT_BC6H_SF16;
    const int bc_snorm = bc->format == DXGI_FORMAT_BC4_SNORM || bc->format == DXGI_FORMAT_BC5_SNORM;
    const float tolerance = bc6 ? 0.0f : bc_snorm ? 1.0f / 127.0f : 1.0f / 254.0f;
    float got[16][4];
    int wrong = 0;
    if (format_dispatch_level(c, rs, dx_cs_fmt_load_dxbc, sizeof dx_cs_fmt_load_dxbc, t, bc->format, 1, got)) {
      for (UINT k = 0; k < 16; ++k)
        for (UINT ch = 0; ch < 4; ++ch) {
          const float want = srgb && ch < 3 ? srgb_linear(bits_of(bc->texels[k][ch])) : bits_of(bc->texels[k][ch]);
          if (!near_value(got[k][ch], want, tolerance + (srgb ? 1e-4f : 0.0f)) && wrong++ < 2) printf("  format %u level 1 texel %u channel %u: %g, not %g\n", bc->format, k, ch, got[k][ch], want);
        }
    } else CHECK(0);
    CHECK(wrong == 0);
    ID3D12Resource_Release(t);
  }
  /* (the CPU renderer has levels of every format) */
  if (!g_hardware) CHECK(level_formats == (int)(sizeof level_loads / sizeof level_loads[0] + sizeof g_bc_cases / sizeof g_bc_cases[0]));

  /* (render targets: what a pixel shader writes, in the target's encoding) */
  static const struct { DXGI_FORMAT f; float v[4]; int shader; } draws[] = {
      {DXGI_FORMAT_R8_UNORM, {0.25f, 0.5f, 0.75f, 1}, 0}, {DXGI_FORMAT_R8G8_UNORM, {0.25f, 0.5f, 0.75f, 1}, 0}, {DXGI_FORMAT_A8_UNORM, {0.25f, 0.5f, 0.75f, 0.4f}, 0},
      {DXGI_FORMAT_R8_SNORM, {-0.5f, 0, 0, 0}, 0}, {DXGI_FORMAT_R8G8B8A8_SNORM, {-1.0f, 0.5f, 0.25f, -0.25f}, 0}, {DXGI_FORMAT_R16G16_SNORM, {-0.75f, 0.125f, 0, 0}, 0},
      {DXGI_FORMAT_R16G16_FLOAT, {1.0f, -2.0f, 0, 0}, 0}, {DXGI_FORMAT_R16_FLOAT, {0.5f, 0, 0, 0}, 0}, {DXGI_FORMAT_R32G32_FLOAT, {1.5f, -1e-3f, 0, 0}, 0},
      {DXGI_FORMAT_R11G11B10_FLOAT, {1.0f, 0.75f, 2.0f, 0}, 0}, {DXGI_FORMAT_B5G6R5_UNORM, {1.0f, 0.5f, 0.0f, 1}, 0}, {DXGI_FORMAT_B5G5R5A1_UNORM, {0.0f, 1.0f, 0.5f, 1}, 0},
      {DXGI_FORMAT_B4G4R4A4_UNORM, {0.2f, 0.4f, 0.6f, 0.8f}, 0}};
  for (UINT i = 0; i < sizeof draws / sizeof draws[0]; ++i) {
    if (!(fmt_support(dev, draws[i].f, NULL) & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) continue;
    BYTE want[16], got[16];
    const UINT bytes = target_bytes(draws[i].f, draws[i].v, want);
    if (!format_draw(c, draws[i].f, dx_ps_fmt_out_dxbc, sizeof dx_ps_fmt_out_dxbc, draws[i].v, got, bytes)) { CHECK(0); continue; }
    if (memcmp(got, want, bytes) != 0) {
      printf("  render target %u:", draws[i].f);
      for (UINT k = 0; k < bytes; ++k) printf(" %02x/%02x", got[k], want[k]);
      printf("\n");
      CHECK(0);
    }
  }
  /* (integer render targets: the bits, saturated to the format) */
  static const struct { DXGI_FORMAT f; UINT32 v[4]; int sint; } int_draws[] = {
      {DXGI_FORMAT_R8_UINT, {300, 0, 0, 0}, 0}, {DXGI_FORMAT_R16_UINT, {1234, 0, 0, 0}, 0}, {DXGI_FORMAT_R32G32_UINT, {0xDEADBEEFu, 7, 0, 0}, 0},
      {DXGI_FORMAT_R10G10B10A2_UINT, {1023, 512, 3, 2}, 0}, {DXGI_FORMAT_R8G8B8A8_SINT, {(UINT32)-5, 100, (UINT32)-200, 300}, 1}};
  for (UINT i = 0; i < sizeof int_draws / sizeof int_draws[0]; ++i) {
    if (!(fmt_support(dev, int_draws[i].f, NULL) & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) continue;
    float v[4];
    memcpy(v, int_draws[i].v, 16);
    BYTE want[16], got[16];
    const UINT bytes = target_bytes(int_draws[i].f, v, want);
    const void *ps = int_draws[i].sint ? dx_ps_fmt_outi_dxbc : dx_ps_fmt_outu_dxbc;
    if (!format_draw(c, int_draws[i].f, ps, int_draws[i].sint ? sizeof dx_ps_fmt_outi_dxbc : sizeof dx_ps_fmt_outu_dxbc, v, got, bytes)) { CHECK(0); continue; }
    if (memcmp(got, want, bytes) != 0) {
      printf("  integer render target %u:", int_draws[i].f);
      for (UINT k = 0; k < bytes; ++k) printf(" %02x/%02x", got[k], want[k]);
      printf("\n");
      CHECK(0);
    }
  }

  /* (a typed unordered access clear of R11G11B10: the floats as its small floats) */
  if (fmt_support(dev, DXGI_FORMAT_R11G11B10_FLOAT, NULL) & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) {
    ID3D12Resource *t = uav_texture(dev, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heap = view_heap(dev, 1, 1), *cpu = view_heap(dev, 1, 0);
    ID3D12GraphicsCommandList *list = NULL;
    if (t && heap && cpu) {
      put_uav(dev, heap, cpu, 0, t, NULL);
      CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
      CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    }
    if (list) {
      const float v[4] = {1.0f, 0.75f, 2.0f, 0};
      ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
      ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, heap_gpu(dev, heap, 0), heap_cpu(dev, cpu, 0), t, v, 0, NULL);
      scene_run(c, list);
      ID3D12GraphicsCommandList_Release(list);
      UINT32 texels[16];
      const UINT32 want = (15u << 6) | (14u << 6 | 32u) << 11 | (16u << 5) << 22;
      CHECK(texture_read(c, t, 0, texels, 4) && texels[0] == want && texels[15] == want);
    } else CHECK(0);
    if (t) ID3D12Resource_Release(t);
    if (heap) ID3D12DescriptorHeap_Release(heap);
    if (cpu) ID3D12DescriptorHeap_Release(cpu);
  }

  /* (the largest textures: the size Direct3D 11 / 12 allow where the adapter has it - the CPU renderer does -, a larger one never) */
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC big = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16384, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *t = NULL;
  const HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &big, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t);
  if (!g_hardware) CHECK(SUCCEEDED(hr) && t);
  if (t) {
    /* (a texel at the far end goes in and comes back) */
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &big, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    CHECK(fp.Footprint.Width == 16384 && row_bytes == 16384 * 4);
    ID3D12Resource_Release(t);
  }
  big.Width = 16385;
  t = NULL;
  CHECK(FAILED(ID3D12Device_CreateCommittedResource(dev, &defaults, D3D12_HEAP_FLAG_NONE, &big, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t)) && !t);
  ID3D12RootSignature_Release(rs);
}

static void test_extras(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  Scene c;
  memset(&c, 0, sizeof c);
  c.dev = dev;
  c.queue = queue;
  c.fence = fence;
  c.fence_value = fence_value;
  if (!scene_make(&c)) { CHECK(0); return; }
  test_queries(&c);
  test_indirect(&c);
  test_bundle(&c);
  test_tiled(&c);
  test_predication(&c);
  test_stream_output(&c);
  test_mip_tails(&c);
  test_vertex_formats(&c);
  test_depth_only(&c);
  test_multisample(&c);
  test_limits(&c);
  test_blend_factor(&c);
  test_planar(&c);
  test_tiled_formats(&c);
  test_cpu_planar(&c);
  test_packed_yuv(&c);
  test_texture_uavs(&c);
  test_typed_buffers(&c);
  test_counters(&c);
  test_uav_only_draws(&c);
  test_pixel_uavs(&c);
  test_layers(&c);
  test_plane_uavs(&c);
  test_shadow_maps(&c);
  test_mip_layers(&c);
  test_formats(&c);
}

/* ---- the interfaces after ID3D12Device and ID3D12GraphicsCommandList ---------------------------------------------------------------------------------------------------- */

static void run_list(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value, ID3D12GraphicsCommandList *list) {
  CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)list};
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
  wait_fence(queue, fence, fence_value);
}

static void test_later_queries(ID3D12Device *dev) {
  /* the later versions of the interfaces are the same objects */
  static const IID *const device_iids[] = {&IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4, &IID_ID3D12Device5,
                                           &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9, &IID_ID3D12Device10};
  for (UINT i = 0; i < sizeof device_iids / sizeof device_iids[0]; ++i) {
    IUnknown *u = NULL;
    CHECK_HR(ID3D12Device_QueryInterface(dev, device_iids[i], (void **)&u));
    CHECK(u == (IUnknown *)dev);
    if (u) IUnknown_Release(u);
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3));
  CHECK(o3.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT);
  CHECK(o3.ViewInstancingTier == D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED);
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5));
  CHECK(o5.RenderPassesTier == D3D12_RENDER_PASS_TIER_0 && o5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12));
  CHECK(o12.EnhancedBarriersSupported);
  CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12 - 4) == E_INVALIDARG);
  D3D12_FEATURE_DATA_SHADER_CACHE sc;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_CACHE, &sc, sizeof sc));
  CHECK((sc.SupportFlags & (D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) == (D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY));
  D3D12_FEATURE_DATA_ROOT_SIGNATURE rsv = {D3D_ROOT_SIGNATURE_VERSION_1_0};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ROOT_SIGNATURE, &rsv, sizeof rsv));
  CHECK(rsv.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_0);      /* (the asked-for version when it is lower) */
  D3D12_FEATURE_DATA_FORMAT_INFO fi = {DXGI_FORMAT_D24_UNORM_S8_UINT, 0};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &fi, sizeof fi));
  CHECK(fi.PlaneCount == 2);
  D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT pc = {0, 7};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, &pc, sizeof pc));
  CHECK(pc.Count == 0);
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7));
  CHECK(o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
}

static void test_later_objects(ID3D12Device *dev, ID3D12CommandQueue *queue) {
  ID3D12Device8 *dev8 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device8, (void **)&dev8));
  if (!dev8) return;
  /* CreateCommittedResource2 and the description with D3D12_RESOURCE_DESC1's fields */
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC1 rd;
  memset(&rd, 0, sizeof rd);
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = 8;
  rd.Height = 4;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  ID3D12Resource2 *r2 = NULL;
  CHECK_HR(ID3D12Device8_CreateCommittedResource2(dev8, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, NULL, &IID_ID3D12Resource2, (void **)&r2));
  if (r2) {
    D3D12_RESOURCE_DESC1 got = ID3D12Resource2_GetDesc1(r2);
    CHECK(got.Width == 8 && got.Height == 4 && got.Format == DXGI_FORMAT_R8G8B8A8_UNORM && got.SamplerFeedbackMipRegion.Width == 0);
    void *session = (void *)1;
    CHECK(ID3D12Resource2_GetProtectedResourceSession(r2, &IID_ID3D12ProtectedResourceSession, &session) == DXGI_ERROR_NOT_FOUND && session == NULL);
    ID3D12Resource2_Release(r2);
  }
  /* a sampler feedback mip region is refused (no sampler feedback) */
  rd.SamplerFeedbackMipRegion.Width = 4;
  rd.SamplerFeedbackMipRegion.Height = 4;
  rd.SamplerFeedbackMipRegion.Depth = 1;
  r2 = NULL;
  CHECK(FAILED(ID3D12Device8_CreateCommittedResource2(dev8, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, NULL, &IID_ID3D12Resource2, (void **)&r2)) && !r2);
  /* GetResourceAllocationInfo2 reports what GetResourceAllocationInfo does */
  rd.SamplerFeedbackMipRegion.Width = rd.SamplerFeedbackMipRegion.Height = rd.SamplerFeedbackMipRegion.Depth = 0;
  D3D12_RESOURCE_ALLOCATION_INFO1 info1;
  D3D12_RESOURCE_ALLOCATION_INFO all = ID3D12Device8_GetResourceAllocationInfo2(dev8, 0, 1, &rd, &info1);
  CHECK(all.SizeInBytes >= 8 * 4 * 4 && all.Alignment != 0 && info1.SizeInBytes == all.SizeInBytes && info1.Offset == 0);

  /* Fence1 */
  ID3D12Fence1 *f1 = NULL;
  CHECK_HR(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence1, (void **)&f1));
  if (f1) { CHECK(ID3D12Fence1_GetCreationFlags(f1) == D3D12_FENCE_FLAG_NONE); }
  /* Heap1 (CreateHeap1 without a session) */
  ID3D12Heap1 *heap1 = NULL;
  D3D12_HEAP_DESC hd = {65536, hp, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  CHECK_HR(ID3D12Device8_CreateHeap1(dev8, &hd, NULL, &IID_ID3D12Heap1, (void **)&heap1));
  if (heap1) {
    void *session = (void *)1;
    CHECK(ID3D12Heap1_GetProtectedResourceSession(heap1, &IID_ID3D12ProtectedResourceSession, &session) == DXGI_ERROR_NOT_FOUND && session == NULL);
    ID3D12Heap1_Release(heap1);
  }

  /* SetEventOnMultipleFenceCompletion: any and all */
  ID3D12Fence *a = NULL, *b = NULL;
  CHECK_HR(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&a));
  CHECK_HR(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&b));
  if (a && b) {
    ID3D12Fence *both[2] = {a, b};
    const UINT64 values[2] = {2, 3};
    HANDLE any = CreateEventW(NULL, FALSE, FALSE, NULL), every = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK_HR(ID3D12Device8_SetEventOnMultipleFenceCompletion(dev8, both, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, any));
    CHECK_HR(ID3D12Device8_SetEventOnMultipleFenceCompletion(dev8, both, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, every));
    CHECK(WaitForSingleObject(any, 0) == WAIT_TIMEOUT);
    CHECK_HR(ID3D12Fence_Signal(a, 2));
    CHECK(WaitForSingleObject(any, 10000) == WAIT_OBJECT_0);
    Sleep(50);
    CHECK(WaitForSingleObject(every, 0) == WAIT_TIMEOUT);
    CHECK_HR(ID3D12Fence_Signal(b, 3));
    CHECK(WaitForSingleObject(every, 10000) == WAIT_OBJECT_0);
    /* EnqueueMakeResident signals the fence when the objects are resident (they always are) */
    ID3D12Pageable *pages[1] = {(ID3D12Pageable *)a};
    CHECK_HR(ID3D12Device8_EnqueueMakeResident(dev8, D3D12_RESIDENCY_FLAG_NONE, 1, pages, b, 7));
    CHECK(ID3D12Fence_GetCompletedValue(b) == 7);
    CloseHandle(any);
    CloseHandle(every);
  }
  if (a) ID3D12Fence_Release(a);
  if (b) ID3D12Fence_Release(b);

  /* CreateCommandList1: a closed list that is Reset before it records */
  ID3D12GraphicsCommandList *closed = NULL;
  CHECK_HR(ID3D12Device8_CreateCommandList1(dev8, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, &IID_ID3D12GraphicsCommandList, (void **)&closed));
  if (closed) {
    ID3D12CommandAllocator *allocator = NULL;
    CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
    CHECK(FAILED(ID3D12GraphicsCommandList_Close(closed)));      /* (already closed) */
    if (allocator) CHECK_HR(ID3D12GraphicsCommandList_Reset(closed, allocator, NULL));
    CHECK_HR(ID3D12GraphicsCommandList_Close(closed));
    ID3D12GraphicsCommandList7 *l7 = NULL;
    CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(closed, &IID_ID3D12GraphicsCommandList7, (void **)&l7));
    if (l7) ID3D12GraphicsCommandList7_Release(l7);
    ID3D12GraphicsCommandList_Release(closed);
    if (allocator) ID3D12CommandAllocator_Release(allocator);
  }
  /* what the device does not have: meta commands, protected sessions, state objects */
  UINT meta = 5;
  CHECK_HR(ID3D12Device8_EnumerateMetaCommands(dev8, &meta, NULL));
  CHECK(meta == 0);
  if (f1) ID3D12Fence1_Release(f1);
  ID3D12Device8_Release(dev8);
  (void)queue;
}

static void test_write_immediate(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  ID3D12Resource *gpu = make_buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  ID3D12GraphicsCommandList2 *l2 = NULL;
  if (list) CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList2, (void **)&l2));
  ID3D12DescriptorHeap *cpu_heap = NULL;
  D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
  dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ID3D12DescriptorHeap *gpu_heap = NULL;
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
  if (!gpu || !down || !l2 || !cpu_heap || !gpu_heap) { CHECK(0); return; }
  const UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap), cpu1 = cpu0;
  cpu1.ptr += inc;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(gpu_heap), gpu1 = gpu0;
  gpu1.ptr += inc;
  /* a raw view of bytes 0 .. 127 and a typed R8G8B8A8_UNORM view of bytes 128 .. 191 */
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw;
  memset(&raw, 0, sizeof raw);
  raw.Format = DXGI_FORMAT_R32_TYPELESS;
  raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  raw.Buffer.NumElements = 32;
  raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  ID3D12Device_CreateUnorderedAccessView(dev, gpu, NULL, &raw, cpu0);
  D3D12_UNORDERED_ACCESS_VIEW_DESC typed = raw;
  typed.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  typed.Buffer.FirstElement = 32;
  typed.Buffer.NumElements = 16;
  typed.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
  ID3D12Device_CreateUnorderedAccessView(dev, gpu, NULL, &typed, cpu1);
  /* a structured view of 16-byte elements over bytes 224 .. 255, for a rectangle of element 1 */
  D3D12_UNORDERED_ACCESS_VIEW_DESC structured = raw;
  structured.Format = DXGI_FORMAT_UNKNOWN;
  structured.Buffer.FirstElement = 14;
  structured.Buffer.NumElements = 2;
  structured.Buffer.StructureByteStride = 16;
  structured.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu2 = cpu0;
  cpu2.ptr += 2 * inc;
  ID3D12Device_CreateUnorderedAccessView(dev, gpu, NULL, &structured, cpu2);
  ID3D12Device_CopyDescriptorsSimple(dev, 3, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu_heap), cpu0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &gpu_heap);
  const UINT clear_u[4] = {0x11223344u, 0, 0, 0};
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu0, cpu0, gpu, clear_u, 0, NULL);
  const float clear_f[4] = {1.0f, 0.0f, 0.5f, 1.0f};
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, gpu1, cpu1, gpu, clear_f, 0, NULL);
  /* a rectangle of the raw view: elements 4 .. 7 */
  const UINT clear_r[4] = {0xA5A5A5A5u, 0, 0, 0};
  const D3D12_RECT part = {4, 0, 8, 1};
  ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu0, cpu0, gpu, clear_r, 1, &part);
  {
    const UINT zero[4] = {0, 0, 0, 0}, seven[4] = {7, 0, 0, 0};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu2 = gpu0;
    gpu2.ptr += 2 * inc;
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu2, cpu2, gpu, zero, 0, NULL);
    const D3D12_RECT second = {1, 0, 2, 1};
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu2, cpu2, gpu, seven, 1, &second);
  }
  /* WriteBufferImmediate after the clears, in their order, then an atomic copy of 8 bytes */
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = {{ID3D12Resource_GetGPUVirtualAddress(gpu) + 200, 0xCAFEF00Du}, {ID3D12Resource_GetGPUVirtualAddress(gpu) + 204, 0x0BADBEEFu}};
  ID3D12GraphicsCommandList2_WriteBufferImmediate(l2, 2, params, NULL);
  ID3D12GraphicsCommandList1 *l1 = (ID3D12GraphicsCommandList1 *)l2;
  ID3D12GraphicsCommandList1_AtomicCopyBufferUINT64(l1, gpu, 208, gpu, 200, 0, NULL, NULL);
  ID3D12GraphicsCommandList_CopyBufferRegion(list, down, 0, gpu, 0, 256);
  run_list(queue, fence, fence_value, list);
  const UINT32 *w = NULL;
  CHECK_HR(ID3D12Resource_Map(down, 0, NULL, (void **)&w));
  if (w) {
    int raw_ok = 1;
    for (int i = 0; i < 32; ++i) raw_ok &= w[i] == (i >= 4 && i < 8 ? 0xA5A5A5A5u : 0x11223344u);
    CHECK(raw_ok);
    int typed_ok = 1;
    for (int i = 32; i < 48; ++i) typed_ok &= w[i] == 0xFF8000FFu;      /* (R 255, G 0, B 128, A 255 as bytes) */
    CHECK(typed_ok);
    if (!typed_ok) printf("typed clear: %08x\n", (unsigned)w[32]);
    CHECK(w[50] == 0xCAFEF00Du && w[51] == 0x0BADBEEFu);
    CHECK(w[52] == 0xCAFEF00Du && w[53] == 0x0BADBEEFu);
    CHECK(w[56] == 0 && w[59] == 0 && w[60] == 7 && w[63] == 7);      /* (element 0 of the structured view zeroed, element 1 - bytes 240 .. 255 - sevens) */
    ID3D12Resource_Unmap(down, 0, NULL);
  }
  ID3D12GraphicsCommandList2_Release(l2);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12DescriptorHeap_Release(cpu_heap);
  ID3D12DescriptorHeap_Release(gpu_heap);
  ID3D12Resource_Release(gpu);
  ID3D12Resource_Release(down);
}

static void test_render_pass(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  /* a render pass whose beginning access clears the target, then a copy of what it left */
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource *target = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&target));
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12DescriptorHeap *rtv_heap = NULL;
  D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList4 *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&list));
  if (!target || !down || !rtv_heap || !list) { CHECK(0); return; }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  ID3D12Device_CreateRenderTargetView(dev, target, NULL, rtv);
  D3D12_RENDER_PASS_RENDER_TARGET_DESC rt;
  memset(&rt, 0, sizeof rt);
  rt.cpuDescriptor = rtv;
  rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
  rt.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rt.BeginningAccess.Clear.ClearValue.Color[0] = 0.0f;
  rt.BeginningAccess.Clear.ClearValue.Color[1] = 1.0f;
  rt.BeginningAccess.Clear.ClearValue.Color[2] = 0.0f;
  rt.BeginningAccess.Clear.ClearValue.Color[3] = 1.0f;
  rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
  ID3D12GraphicsCommandList4_BeginRenderPass(list, 1, &rt, NULL, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
  ID3D12GraphicsCommandList4_EndRenderPass(list);
  /* (resumed: its beginning access - a clear to red here - is not done again) */
  D3D12_RENDER_PASS_RENDER_TARGET_DESC again = rt;
  again.BeginningAccess.Clear.ClearValue.Color[0] = 1.0f;
  again.BeginningAccess.Clear.ClearValue.Color[1] = 0.0f;
  ID3D12GraphicsCommandList4_BeginRenderPass(list, 1, &again, NULL, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
  ID3D12GraphicsCommandList4_EndRenderPass(list);
  /* the enhanced barrier into the copy source layout: nothing to transition */
  ID3D12GraphicsCommandList7 *l7 = NULL;
  CHECK_HR(ID3D12GraphicsCommandList4_QueryInterface(list, &IID_ID3D12GraphicsCommandList7, (void **)&l7));
  if (l7) {
    D3D12_TEXTURE_BARRIER tb;
    memset(&tb, 0, sizeof tb);
    tb.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
    tb.SyncAfter = D3D12_BARRIER_SYNC_COPY;
    tb.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    tb.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    tb.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    tb.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    tb.pResource = target;
    tb.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
    D3D12_BARRIER_GROUP group;
    memset(&group, 0, sizeof group);
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &tb;
    ID3D12GraphicsCommandList7_Barrier(l7, 1, &group);
    ID3D12GraphicsCommandList7_Release(l7);
  }
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = down;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  to.PlacedFootprint.Footprint.Width = 16;
  to.PlacedFootprint.Footprint.Height = 16;
  to.PlacedFootprint.Footprint.Depth = 1;
  to.PlacedFootprint.Footprint.RowPitch = 256;
  from.pResource = target;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  ID3D12GraphicsCommandList4_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
  run_list(queue, fence, fence_value, (ID3D12GraphicsCommandList *)list);
  const UINT32 *px = NULL;
  CHECK_HR(ID3D12Resource_Map(down, 0, NULL, (void **)&px));
  if (px) {
    CHECK(px[0] == 0xFF00FF00u && px[15 * 64 + 15] == 0xFF00FF00u);
    ID3D12Resource_Unmap(down, 0, NULL);
  }
  ID3D12GraphicsCommandList4_Release(list);
  ID3D12CommandAllocator_Release(allocator);
  ID3D12DescriptorHeap_Release(rtv_heap);
  ID3D12Resource_Release(down);
  ID3D12Resource_Release(target);
}

static void test_deserializers(void) {
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER params[2];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 3;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler;
  memset(&sampler, 0, sizeof sampler);
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = 1000.0f;
  sampler.ShaderRegister = 5;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  if (!blob) return;
  ID3D12RootSignatureDeserializer *d = NULL;
  CHECK_HR(D3D12CreateRootSignatureDeserializer(ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignatureDeserializer, (void **)&d));
  if (d) {
    const D3D12_ROOT_SIGNATURE_DESC *got = ID3D12RootSignatureDeserializer_GetRootSignatureDesc(d);
    CHECK(got && got->NumParameters == 2 && got->NumStaticSamplers == 1 && got->Flags == D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    if (got && got->NumParameters == 2) {
      CHECK(got->pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && got->pParameters[0].Descriptor.ShaderRegister == 3 &&
            got->pParameters[0].ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX);
      CHECK(got->pParameters[1].DescriptorTable.NumDescriptorRanges == 1 && got->pParameters[1].DescriptorTable.pDescriptorRanges[0].NumDescriptors == 2 &&
            got->pParameters[1].DescriptorTable.pDescriptorRanges[0].BaseShaderRegister == 1);
      CHECK(got->pStaticSamplers[0].ShaderRegister == 5 && got->pStaticSamplers[0].MaxLOD == 1000.0f);
    }
    ID3D12RootSignatureDeserializer_Release(d);
  }
  ID3D12VersionedRootSignatureDeserializer *v = NULL;
  CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v));
  if (v) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *un = ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(v);
    CHECK(un && un->Version == D3D_ROOT_SIGNATURE_VERSION_1_0);
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *v11 = NULL;
    CHECK_HR(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, D3D_ROOT_SIGNATURE_VERSION_1_1, &v11));
    if (v11) {
      CHECK(v11->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && v11->Desc_1_1.NumParameters == 2);
      /* (1.0 converted: the ranges are volatile, as are root descriptors' data) */
      CHECK(v11->Desc_1_1.pParameters[1].DescriptorTable.pDescriptorRanges[0].Flags ==
            (D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE));
      CHECK(v11->Desc_1_1.pParameters[0].Descriptor.Flags == D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *bad = NULL;
    CHECK(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, (D3D_ROOT_SIGNATURE_VERSION)7, &bad) == E_INVALIDARG);
    ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  /* garbage is refused */
  const BYTE junk[16] = {1, 2, 3, 4};
  d = NULL;
  CHECK(FAILED(D3D12CreateRootSignatureDeserializer(junk, sizeof junk, &IID_ID3D12RootSignatureDeserializer, (void **)&d)) && d == NULL);
  ID3D10Blob_Release(blob);
}

static void test_pipeline_library(ID3D12Device *dev) {
  ID3D12Device1 *dev1 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device1, (void **)&dev1));
  if (!dev1) return;
  D3D12_ROOT_PARAMETER params[4];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 4;
  for (int i = 1; i < 4; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[i].Descriptor.ShaderRegister = (UINT)(i - 1);
  }
  D3D12_ROOT_SIGNATURE_DESC rsd = {4, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error));
  ID3D12RootSignature *rs = NULL;
  if (blob) CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
  memset(&cd, 0, sizeof cd);
  cd.pRootSignature = rs;
  cd.CS.pShaderBytecode = dx_cs_fill_dxbc;
  cd.CS.BytecodeLength = sizeof dx_cs_fill_dxbc;
  ID3D12PipelineState *pso = NULL;
  CHECK_HR(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso));
  ID3D12PipelineLibrary *lib = NULL;
  CHECK_HR(ID3D12Device1_CreatePipelineLibrary(dev1, NULL, 0, &IID_ID3D12PipelineLibrary, (void **)&lib));
  if (lib && pso) {
    CHECK_HR(ID3D12PipelineLibrary_StorePipeline(lib, L"fill", pso));
    CHECK(ID3D12PipelineLibrary_StorePipeline(lib, L"fill", pso) == E_INVALIDARG);      /* (the name is taken) */
    const SIZE_T size = ID3D12PipelineLibrary_GetSerializedSize(lib);
    CHECK(size > 0 && size < 4096);
    BYTE data[4096];
    CHECK_HR(ID3D12PipelineLibrary_Serialize(lib, data, size));
    CHECK(ID3D12PipelineLibrary_Serialize(lib, data, size - 1) == E_INVALIDARG);
    ID3D12PipelineLibrary *again = NULL;
    CHECK_HR(ID3D12Device1_CreatePipelineLibrary(dev1, data, size, &IID_ID3D12PipelineLibrary, (void **)&again));
    if (again) {
      ID3D12PipelineState *loaded = NULL;
      CHECK_HR(ID3D12PipelineLibrary_LoadComputePipeline(again, L"fill", &cd, &IID_ID3D12PipelineState, (void **)&loaded));
      if (loaded) ID3D12PipelineState_Release(loaded);
      loaded = NULL;
      CHECK(ID3D12PipelineLibrary_LoadComputePipeline(again, L"other", &cd, &IID_ID3D12PipelineState, (void **)&loaded) == E_INVALIDARG && !loaded);
      /* another description under the stored name */
      D3D12_COMPUTE_PIPELINE_STATE_DESC other = cd;
      other.NodeMask = 0;
      other.CS.BytecodeLength = sizeof dx_cs_fill_dxbc - 4;
      CHECK(ID3D12PipelineLibrary_LoadComputePipeline(again, L"fill", &other, &IID_ID3D12PipelineState, (void **)&loaded) == E_INVALIDARG && !loaded);
      /* the stream form of the same description */
      struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t0; ID3D12RootSignature *rs; D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t1; D3D12_SHADER_BYTECODE cs; } stream;
      memset(&stream, 0, sizeof stream);
      stream.t0 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
      stream.rs = rs;
      stream.t1 = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
      stream.cs = cd.CS;
      D3D12_PIPELINE_STATE_STREAM_DESC sd = {sizeof stream, &stream};
      ID3D12PipelineLibrary1 *lib1 = NULL;
      CHECK_HR(ID3D12PipelineLibrary_QueryInterface(again, &IID_ID3D12PipelineLibrary1, (void **)&lib1));
      if (lib1) {
        CHECK_HR(ID3D12PipelineLibrary1_LoadPipeline(lib1, L"fill", &sd, &IID_ID3D12PipelineState, (void **)&loaded));
        if (loaded) ID3D12PipelineState_Release(loaded);
        ID3D12PipelineLibrary1_Release(lib1);
      }
      ID3D12Device2 *dev2 = NULL;
      CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device2, (void **)&dev2));
      if (dev2) {
        loaded = NULL;
        CHECK_HR(ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState, (void **)&loaded));
        if (loaded) ID3D12PipelineState_Release(loaded);
        ID3D12Device2_Release(dev2);
      }
      ID3D12PipelineLibrary_Release(again);
    }
    /* a blob that is not one of this driver's */
    BYTE foreign[64];
    memset(foreign, 0x5A, sizeof foreign);
    ID3D12PipelineLibrary *bad = NULL;
    CHECK(ID3D12Device1_CreatePipelineLibrary(dev1, foreign, sizeof foreign, &IID_ID3D12PipelineLibrary, (void **)&bad) == D3D12_ERROR_DRIVER_VERSION_MISMATCH && !bad);
  }
  if (lib) ID3D12PipelineLibrary_Release(lib);
  if (pso) ID3D12PipelineState_Release(pso);
  if (rs) ID3D12RootSignature_Release(rs);
  if (blob) ID3D10Blob_Release(blob);
  ID3D12Device1_Release(dev1);
}

static void test_shader_cache(ID3D12Device *dev) {
  ID3D12Device9 *dev9 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device9, (void **)&dev9));
  if (!dev9) return;
  D3D12_SHADER_CACHE_SESSION_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.Identifier.Data1 = 0x7C05u;
  sd.Mode = D3D12_SHADER_CACHE_MODE_MEMORY;
  ID3D12ShaderCacheSession *cache = NULL;
  CHECK_HR(ID3D12Device9_CreateShaderCacheSession(dev9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&cache));
  if (cache) {
    const char key[] = "key", value[] = "the value";
    CHECK_HR(ID3D12ShaderCacheSession_StoreValue(cache, key, sizeof key, value, sizeof value));
    CHECK(ID3D12ShaderCacheSession_StoreValue(cache, key, sizeof key, value, sizeof value) == DXGI_ERROR_ALREADY_EXISTS);
    UINT size = 0;
    CHECK_HR(ID3D12ShaderCacheSession_FindValue(cache, key, sizeof key, NULL, &size));
    CHECK(size == sizeof value);
    char got[32] = {0};
    size = 2;
    CHECK(ID3D12ShaderCacheSession_FindValue(cache, key, sizeof key, got, &size) == DXGI_ERROR_MORE_DATA);
    size = sizeof got;
    CHECK_HR(ID3D12ShaderCacheSession_FindValue(cache, key, sizeof key, got, &size));
    CHECK(size == sizeof value && memcmp(got, value, sizeof value) == 0);
    CHECK(ID3D12ShaderCacheSession_FindValue(cache, "nope", 4, got, &size) == DXGI_ERROR_NOT_FOUND);
    D3D12_SHADER_CACHE_SESSION_DESC back = ID3D12ShaderCacheSession_GetDesc(cache);
    CHECK(back.Identifier.Data1 == 0x7C05u && back.Mode == D3D12_SHADER_CACHE_MODE_MEMORY);
    /* clearing the application's caches empties the session */
    CHECK_HR(ID3D12Device9_ShaderCacheControl(dev9, D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED, D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR));
    CHECK(ID3D12ShaderCacheSession_FindValue(cache, key, sizeof key, NULL, &size) == DXGI_ERROR_NOT_FOUND);
    ID3D12ShaderCacheSession_Release(cache);
  }
  sd.Flags = D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;      /* (a flag of disk caches) */
  cache = NULL;
  CHECK(ID3D12Device9_CreateShaderCacheSession(dev9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&cache) == E_INVALIDARG && !cache);
  CHECK(ID3D12Device9_ShaderCacheControl(dev9, D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED, D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE | D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) == E_INVALIDARG);
  ID3D12Device9_Release(dev9);
}

static void test_cpu_texture(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  /* a texture in a custom heap the CPU sees, written and read with WriteToSubresource / ReadFromSubresource; the GPU sees what the CPU wrote */
  D3D12_HEAP_PROPERTIES hp = ID3D12Device_GetCustomHeapProperties(dev, 0, D3D12_HEAP_TYPE_READBACK);
  CHECK(hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
  D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 4, 1, 2, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *tex = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tex));
  /* (an upload heap holds no textures) */
  D3D12_HEAP_PROPERTIES up = {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  ID3D12Resource *bad = NULL;
  CHECK(ID3D12Device_CreateCommittedResource(dev, &up, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&bad) == E_INVALIDARG && !bad);
  if (!tex) return;
  void *pointer = (void *)1;
  CHECK(ID3D12Resource_Map(tex, 0, NULL, &pointer) == E_INVALIDARG && pointer == NULL);      /* (no address of a texture of the driver's layout) */
  CHECK_HR(ID3D12Resource_Map(tex, 0, NULL, NULL));
  CHECK_HR(ID3D12Resource_Map(tex, 1, NULL, NULL));
  UINT32 level0[4][8], level1[2][4];
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x) level0[y][x] = 0xFF000000u | (UINT32)(y * 16 + x);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 4; ++x) level1[y][x] = 0x80000000u | (UINT32)(0x100 * (y * 4 + x + 1));
  CHECK_HR(ID3D12Resource_WriteToSubresource(tex, 0, NULL, level0, sizeof level0[0], sizeof level0));
  CHECK_HR(ID3D12Resource_WriteToSubresource(tex, 1, NULL, level1, sizeof level1[0], sizeof level1));
  /* a box: texels 2 .. 5 of rows 1 .. 2 of level 0, from rows 64 bytes apart */
  UINT32 patch[2][16];
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 16; ++x) patch[y][x] = 0xAA000000u | (UINT32)(y * 4 + x);
  const D3D12_BOX box = {2, 1, 0, 6, 3, 1};
  CHECK_HR(ID3D12Resource_WriteToSubresource(tex, 0, &box, patch, sizeof patch[0], sizeof patch));
  const D3D12_BOX outside = {6, 0, 0, 9, 1, 1};
  CHECK(ID3D12Resource_WriteToSubresource(tex, 0, &outside, patch, sizeof patch[0], sizeof patch) == E_INVALIDARG);
  CHECK(ID3D12Resource_WriteToSubresource(tex, 2, NULL, level0, sizeof level0[0], sizeof level0) == E_INVALIDARG);      /* (two subresources) */
  UINT32 back0[4][8], back1[2][4];
  memset(back0, 0, sizeof back0);
  memset(back1, 0, sizeof back1);
  CHECK_HR(ID3D12Resource_ReadFromSubresource(tex, back0, sizeof back0[0], sizeof back0, 0, NULL));
  CHECK_HR(ID3D12Resource_ReadFromSubresource(tex, back1, sizeof back1[0], sizeof back1, 1, NULL));
  int same = 1;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x) {
      const int in_box = x >= 2 && x < 6 && y >= 1 && y < 3;
      same &= back0[y][x] == (in_box ? patch[y - 1][x - 2] : level0[y][x]);
    }
  CHECK(same);
  CHECK(memcmp(back1, level1, sizeof level1) == 0);
  ID3D12Resource_Unmap(tex, 0, NULL);
  ID3D12Resource_Unmap(tex, 1, NULL);
  /* the GPU copies what the CPU wrote */
  ID3D12Resource *down = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 2, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (down && list) {
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    to.pResource = down;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    to.PlacedFootprint.Footprint.Width = 4;
    to.PlacedFootprint.Footprint.Height = 2;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = 256;
    from.pResource = tex;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 1;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
    run_list(queue, fence, fence_value, list);
    const UINT32 *px = NULL;
    CHECK_HR(ID3D12Resource_Map(down, 0, NULL, (void **)&px));
    if (px) {
      CHECK(px[0] == level1[0][0] && px[3] == level1[0][3] && px[64] == level1[1][0] && px[67] == level1[1][3]);
      ID3D12Resource_Unmap(down, 0, NULL);
    }
  }
  if (list) ID3D12GraphicsCommandList_Release(list);
  if (allocator) ID3D12CommandAllocator_Release(allocator);
  if (down) ID3D12Resource_Release(down);
  /* a buffer is not written with WriteToSubresource */
  ID3D12Resource *buffer = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (buffer) {
    CHECK(ID3D12Resource_WriteToSubresource(buffer, 0, NULL, level0, 32, 128) == E_INVALIDARG);
    ID3D12Resource_Release(buffer);
  }
  ID3D12Resource_Release(tex);

  /* the 64 KiB standard swizzle (StandardSwizzle64KBSupported): a CPU-visible texture of that layout is mapped in it - 2D 32-bit: x bytes 0xAA8F, y 0x5570 */
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  memset(&options, 0, sizeof options);
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options));
  CHECK(options.StandardSwizzle64KBSupported && options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_1 &&
        options.ConservativeRasterizationTier <= D3D12_CONSERVATIVE_RASTERIZATION_TIER_1);
  D3D12_RESOURCE_DESC sd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *sw = NULL;
  CHECK_HR(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&sw));
  sd.Format = DXGI_FORMAT_D32_FLOAT;
  sd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_HEAP_PROPERTIES dp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  bad = NULL;
  CHECK(ID3D12Device_CreateCommittedResource(dev, &dp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&bad) == E_INVALIDARG && !bad);
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.Flags = D3D12_RESOURCE_FLAG_NONE;
  sd.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;      /* (a reserved resource's) */
  CHECK(ID3D12Device_CreateCommittedResource(dev, &dp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&bad) == E_INVALIDARG && !bad);
  if (sw) {
    UINT32 texels[16][16], seen[16][16];
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) texels[y][x] = 0xFF000000u | (UINT32)(y * 0x10203 + x * 0x30201);
    CHECK_HR(ID3D12Resource_WriteToSubresource(sw, 0, NULL, texels, sizeof texels[0], sizeof texels));
    BYTE *mapped = NULL;
    CHECK_HR(ID3D12Resource_Map(sw, 0, NULL, (void **)&mapped));
    if (mapped) {
      int same = 1;
      for (UINT y = 0; y < 16; ++y)
        for (UINT x = 0; x < 16; ++x) {
          UINT off = 0, xb = x * 4, bit = 0;
          for (UINT m = 0xAA8Fu, b = 1; m; b <<= 1) { const UINT low = m & (0u - m); if (xb & b) off |= low; m &= m - 1; }
          for (UINT m = 0x5570u, b = 1; m; b <<= 1) { const UINT low = m & (0u - m); if (y & b) off |= low; m &= m - 1; }
          (void)bit;
          UINT32 v;
          memcpy(&v, mapped + off, 4);
          same &= v == texels[y][x];
        }
      CHECK(same);
      const UINT32 marker = 0xFF0A0B0Cu;
      memcpy(mapped, &marker, 4);      /* (texel 0, 0 is at offset 0) */
      ID3D12Resource_Unmap(sw, 0, NULL);
      memset(seen, 0, sizeof seen);
      CHECK_HR(ID3D12Resource_ReadFromSubresource(sw, seen, sizeof seen[0], sizeof seen, 0, NULL));
      CHECK(seen[0][0] == marker && seen[0][1] == texels[0][1] && seen[15][15] == texels[15][15]);
    }
    ID3D12Resource_Release(sw);
  }
}

/* ---- the interfaces and descriptions of the later Agility SDKs (d3d12_types.h) ------------------------------------------------------------------------------------------ */

/* A pipeline-state stream as CD3DX12 lays one out: each subobject its type and then its contents at their own alignment, the whole of it aligned to a pointer. */
typedef struct Stream { BYTE bytes[1024]; SIZE_T size; } Stream;
static void stream_add(Stream *s, UINT type, const void *data, SIZE_T size, SIZE_T align) {
  const SIZE_T word = sizeof(void *);      /* (alignas(void *): 8 bytes in a 64-bit program, 4 in a 32-bit one) */
  SIZE_T at = (s->size + word - 1) & ~(word - 1);
  memcpy(s->bytes + at, &type, 4);
  SIZE_T inner = (at + 4 + align - 1) & ~(align - 1);
  memcpy(s->bytes + inner, data, size);
  s->size = (inner + size + word - 1) & ~(word - 1);
}
static HRESULT stream_make(ID3D12Device2 *dev2, const Stream *s, ID3D12PipelineState **out) {
  D3D12_PIPELINE_STATE_STREAM_DESC sd = {s->size, (void *)s->bytes};
  *out = NULL;
  return ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState, (void **)out);
}

static void test_root_signature_12(ID3D12Device *dev) {
  D3D12_FEATURE_DATA_ROOT_SIGNATURE rsv = {D3D_ROOT_SIGNATURE_VERSION_1_2};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ROOT_SIGNATURE, &rsv, sizeof rsv));
  CHECK(rsv.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_2);
  D3D12_ROOT_PARAMETER1 param;
  memset(&param, 0, sizeof param);
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  param.Constants.Num32BitValues = 4;
  D3D12_STATIC_SAMPLER_DESC1 ss[2];
  memset(ss, 0, sizeof ss);
  for (int i = 0; i < 2; ++i) {
    ss[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    ss[i].MaxLOD = 1000.0f;
    ss[i].ShaderRegister = (UINT)i;
    ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  ss[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT;
  ss[0].Flags = D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR;
  ss[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 vd;
  memset(&vd, 0, sizeof vd);
  vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
  vd.Desc_1_2.NumParameters = 1;
  vd.Desc_1_2.pParameters = &param;
  vd.Desc_1_2.NumStaticSamplers = 2;
  vd.Desc_1_2.pStaticSamplers = ss;
  ID3DBlob *b12 = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeVersionedRootSignature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&vd, &b12, &error));
  if (!b12) return;
  const BYTE *bytes = ID3D10Blob_GetBufferPointer(b12);
  const SIZE_T size = ID3D10Blob_GetBufferSize(b12);
  CHECK(size == 24 + 12 + 12 + 2 * 56 && bytes[0] == 3);      /* (version 3; a sampler of 1.2 is 56 bytes) */
  ID3D12VersionedRootSignatureDeserializer *v = NULL;
  CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(bytes, size, &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v));
  if (v) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *un = (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *)ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(v);
    CHECK(un && un->Version == D3D_ROOT_SIGNATURE_VERSION_1_2 && un->Desc_1_2.NumStaticSamplers == 2 && un->Desc_1_2.NumParameters == 1);
    if (un && un->Desc_1_2.NumStaticSamplers == 2) {
      CHECK(un->Desc_1_2.pStaticSamplers[0].Flags == D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR && un->Desc_1_2.pStaticSamplers[0].BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT);
      CHECK(un->Desc_1_2.pStaticSamplers[1].Flags == 0 && un->Desc_1_2.pStaticSamplers[1].ShaderRegister == 1 && un->Desc_1_2.pParameters[0].Constants.Num32BitValues == 4);
    }
    /* (a sampler with flags has no 1.1 or 1.0 description) */
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *down = (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)1;
    CHECK(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, D3D_ROOT_SIGNATURE_VERSION_1_1, &down) == E_INVALIDARG && down == NULL);
    ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  ID3D12RootSignatureDeserializer *plain = NULL;
  CHECK(FAILED(D3D12CreateRootSignatureDeserializer(bytes, size, &IID_ID3D12RootSignatureDeserializer, (void **)&plain)) && !plain);
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, bytes, size, &IID_ID3D12RootSignature, (void **)&rs));
  if (rs) ID3D12RootSignature_Release(rs);
  /* every truncation and every flipped bit: refused or read, never a crash; what is read gives each version's description or a refusal */
  for (SIZE_T n = 0; n <= size; ++n) {
    v = NULL;
    if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(bytes, n, &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v)) && v) ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  BYTE flipped[256];
  int read_back = 0;
  for (SIZE_T bit = 0; bit < size * 8; ++bit) {
    memcpy(flipped, bytes, size);
    flipped[bit / 8] ^= (BYTE)(1u << (bit % 8));
    v = NULL;
    if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(flipped, size, &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v)) && v) {
      for (UINT k = 1; k <= 3; ++k) {
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *at = NULL;
        if (SUCCEEDED(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, (D3D_ROOT_SIGNATURE_VERSION)k, &at)) && at) ++read_back;
      }
      ID3D12VersionedRootSignatureDeserializer_Release(v);
    }
  }
  CHECK(read_back > 0);
  ID3D10Blob_Release(b12);
  /* without sampler flags a 1.2 root signature reads back at 1.0 and 1.1 */
  ss[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  ss[0].Flags = 0;
  CHECK_HR(D3D12SerializeVersionedRootSignature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&vd, &b12, &error));
  if (b12) {
    v = NULL;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(ID3D10Blob_GetBufferPointer(b12), ID3D10Blob_GetBufferSize(b12), &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v));
    if (v) {
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d10 = NULL, *d11 = NULL;
      CHECK_HR(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, D3D_ROOT_SIGNATURE_VERSION_1_0, &d10));
      CHECK_HR(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, D3D_ROOT_SIGNATURE_VERSION_1_1, &d11));
      CHECK(d10 && d10->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 && d10->Desc_1_0.NumStaticSamplers == 2 && d10->Desc_1_0.pStaticSamplers[1].ShaderRegister == 1);
      CHECK(d11 && d11->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && d11->Desc_1_1.NumParameters == 1 && d11->Desc_1_1.pParameters[0].Constants.Num32BitValues == 4);
      ID3D12VersionedRootSignatureDeserializer_Release(v);
    }
    ID3D10Blob_Release(b12);
  }
  /* what the serializer refuses: an integer colour without its flag, the flag with a float colour, a flag that does not exist, an integer colour before 1.2 */
  ss[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK_UINT;
  b12 = NULL;
  CHECK(D3D12SerializeVersionedRootSignature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&vd, &b12, &error) == E_INVALIDARG && !b12);
  ss[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  ss[0].Flags = D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR;
  CHECK(D3D12SerializeVersionedRootSignature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&vd, &b12, &error) == E_INVALIDARG && !b12);
  ss[0].Flags = (D3D12_SAMPLER_FLAGS)4;
  CHECK(D3D12SerializeVersionedRootSignature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&vd, &b12, &error) == E_INVALIDARG && !b12);
  D3D12_STATIC_SAMPLER_DESC old;
  memcpy(&old, &ss[1], sizeof old);
  old.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC v11;
  memset(&v11, 0, sizeof v11);
  v11.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  v11.Desc_1_1.NumParameters = 1;
  v11.Desc_1_1.pParameters = &param;
  v11.Desc_1_1.NumStaticSamplers = 1;
  v11.Desc_1_1.pStaticSamplers = &old;
  CHECK(D3D12SerializeVersionedRootSignature(&v11, &b12, &error) == E_INVALIDARG && !b12);
  /* coordinates in texels: written, but no device here samples with them (NonNormalizedCoordinateSamplersSupported is FALSE) */
  D3D12_FEATURE_DATA_D3D12_OPTIONS17 o17;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS17, &o17, sizeof o17));
  CHECK(!o17.NonNormalizedCoordinateSamplersSupported);
  ss[0].Flags = D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
  CHECK_HR(D3D12SerializeVersionedRootSignature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&vd, &b12, &error));
  if (b12) {
    rs = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(b12), ID3D10Blob_GetBufferSize(b12), &IID_ID3D12RootSignature, (void **)&rs) == E_INVALIDARG && !rs);
    ID3D10Blob_Release(b12);
  }
  /* a 1.0 root signature read at 1.2: the flags of a 1.0 root signature, no sampler flags */
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER p10;
  memset(&p10, 0, sizeof p10);
  p10.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p10.DescriptorTable.NumDescriptorRanges = 1;
  p10.DescriptorTable.pDescriptorRanges = &range;
  D3D12_ROOT_SIGNATURE_DESC d10 = {1, &p10, 1, &old, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  old.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
  ID3DBlob *b10 = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&d10, D3D_ROOT_SIGNATURE_VERSION_1_0, &b10, &error));
  if (b10) {
    v = NULL;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(ID3D10Blob_GetBufferPointer(b10), ID3D10Blob_GetBufferSize(b10), &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v));
    if (v) {
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *at = NULL;
      CHECK_HR(ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(v, D3D_ROOT_SIGNATURE_VERSION_1_2, &at));
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *at12 = (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *)at;
      CHECK(at12 && at12->Version == D3D_ROOT_SIGNATURE_VERSION_1_2 && at12->Desc_1_2.NumStaticSamplers == 1 && at12->Desc_1_2.pStaticSamplers[0].Flags == 0 &&
            at12->Desc_1_2.pStaticSamplers[0].BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);
      CHECK(at12 && at12->Desc_1_2.pParameters[0].DescriptorTable.pDescriptorRanges[0].Flags ==
                        (D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE));
      ID3D12VersionedRootSignatureDeserializer_Release(v);
    }
    ID3D10Blob_Release(b10);
  }
}

/* The root signatures of a DXIL library (rootsig_lib.hlsl, made by dxc as lib_6_3: a GlobalRootSignature "grs", a LocalRootSignature "lrs" and a StateObjectConfig "cfg"). */
static void test_library_root_signatures(ID3D12Device *dev) {
  /* (an export of the Agility SDK's d3d12.dll that mingw's import library does not have) */
  typedef HRESULT(WINAPI * FromLibrary)(const void *, SIZE_T, LPCWSTR, REFIID, void **);
  const FromLibrary from_library = (FromLibrary)(void *)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary");
  CHECK(from_library != NULL);
  if (!from_library) return;
  ID3D12VersionedRootSignatureDeserializer *v = NULL;
  CHECK_HR(from_library(dx_rootsig_lib_dxbc, sizeof dx_rootsig_lib_dxbc, L"grs",
                                                                              &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v));
  if (v) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *un = ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(v);
    CHECK(un && un->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && un->Desc_1_1.NumParameters == 2 && un->Desc_1_1.NumStaticSamplers == 1);
    if (un && un->Desc_1_1.NumParameters == 2) {
      CHECK(un->Desc_1_1.pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && un->Desc_1_1.pParameters[0].Constants.Num32BitValues == 4);
      CHECK(un->Desc_1_1.pParameters[1].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && un->Desc_1_1.pParameters[1].DescriptorTable.NumDescriptorRanges == 2);
    }
    ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  v = NULL;
  CHECK_HR(from_library(dx_rootsig_lib_dxbc, sizeof dx_rootsig_lib_dxbc, L"lrs",
                                                                              &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v));
  if (v) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *un = ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(v);
    CHECK(un && un->Desc_1_1.NumParameters == 1 && un->Desc_1_1.pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV &&
          un->Desc_1_1.pParameters[0].Descriptor.ShaderRegister == 3);
    ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  static const WCHAR *const not_there[] = {L"cfg", L"gr", L"grsx", L"none"};
  for (UINT i = 0; i < 4; ++i) {
    v = NULL;
    CHECK(from_library(dx_rootsig_lib_dxbc, sizeof dx_rootsig_lib_dxbc, not_there[i],
                                                                             &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v) == E_INVALIDARG && !v);
  }
  /* (a shader with no library part, a library cut short, and every 7th bit flipped: refused or read, never a crash) */
  v = NULL;
  CHECK(FAILED(from_library(dx_cs_fill_dxbc, sizeof dx_cs_fill_dxbc, L"grs", &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v)));
  for (SIZE_T n = 0; n < sizeof dx_rootsig_lib_dxbc; n += 13) {
    v = NULL;
    if (SUCCEEDED(from_library(dx_rootsig_lib_dxbc, n, L"grs", &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v)) && v)
      ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  static BYTE lib[sizeof dx_rootsig_lib_dxbc];
  for (SIZE_T bit = 0; bit < sizeof lib * 8; bit += 7) {
    memcpy(lib, dx_rootsig_lib_dxbc, sizeof lib);
    lib[bit / 8] ^= (BYTE)(1u << (bit % 8));
    v = NULL;
    if (SUCCEEDED(from_library(lib, sizeof lib, L"grs", &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&v)) && v)
      ID3D12VersionedRootSignatureDeserializer_Release(v);
  }
  ID3D12Device14 *dev14 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device14, (void **)&dev14));
  if (dev14) {
    ID3D12RootSignature *rs = NULL;
    CHECK_HR(ID3D12Device14_CreateRootSignatureFromSubobjectInLibrary(dev14, 0, dx_rootsig_lib_dxbc, sizeof dx_rootsig_lib_dxbc, L"grs", &IID_ID3D12RootSignature, (void **)&rs));
    if (rs) ID3D12RootSignature_Release(rs);
    rs = NULL;
    CHECK(ID3D12Device14_CreateRootSignatureFromSubobjectInLibrary(dev14, 0, dx_rootsig_lib_dxbc, sizeof dx_rootsig_lib_dxbc, L"cfg", &IID_ID3D12RootSignature, (void **)&rs) == E_INVALIDARG && !rs);
    ID3D12Device14_Release(dev14);
  }
}

static void test_agility_streams(ID3D12Device *dev) {
  ID3D12Device2 *dev2 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device2, (void **)&dev2));
  if (!dev2) return;
  /* (root signatures as blobs: the compute one of the pipeline library test, and one with the 16 constants the vertex shader reads) */
  D3D12_ROOT_PARAMETER params[4];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 4;
  for (int i = 1; i < 4; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[i].Descriptor.ShaderRegister = (UINT)(i - 1);
  }
  D3D12_ROOT_SIGNATURE_DESC crsd = {4, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *cblob = NULL, *gblob = NULL, *error = NULL;
  CHECK_HR(D3D12SerializeRootSignature(&crsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &cblob, &error));
  D3D12_ROOT_PARAMETER gp;
  memset(&gp, 0, sizeof gp);
  gp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  gp.Constants.Num32BitValues = 16;
  D3D12_ROOT_SIGNATURE_DESC grsd = {1, &gp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  CHECK_HR(D3D12SerializeRootSignature(&grsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &gblob, &error));
  if (!cblob || !gblob) { ID3D12Device2_Release(dev2); return; }
  D3D12_SERIALIZED_ROOT_SIGNATURE_DESC csr = {ID3D10Blob_GetBufferPointer(cblob), ID3D10Blob_GetBufferSize(cblob)};
  D3D12_SERIALIZED_ROOT_SIGNATURE_DESC gsr = {ID3D10Blob_GetBufferPointer(gblob), ID3D10Blob_GetBufferSize(gblob)};
  D3D12_SHADER_BYTECODE cs = {dx_cs_fill_dxbc, sizeof dx_cs_fill_dxbc}, vs = {dx_vs_dxbc, sizeof dx_vs_dxbc}, ps = {dx_ps_color_dxbc, sizeof dx_ps_color_dxbc};
  /* a compute pipeline with its root signature as a blob */
  Stream s;
  ID3D12PipelineState *pso = NULL;
  memset(&s, 0, sizeof s);
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE, &csr, sizeof csr, sizeof(void *));
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cs, sizeof cs, sizeof(void *));
  CHECK_HR(stream_make(dev2, &s, &pso));
  if (pso) {
    ID3D12RootSignature *got = NULL;
    /* (the pipeline holds the root signature it made: the device is still the one) */
    ID3D12Device *owner = NULL;
    CHECK_HR(ID3D12PipelineState_GetDevice(pso, &IID_ID3D12Device, (void **)&owner));
    if (owner) ID3D12Device_Release(owner);
    (void)got;
    ID3D12PipelineState_Release(pso);
  }
  /* ... and with a root signature object as well: two of one kind */
  ID3D12RootSignature *rs = NULL;
  CHECK_HR(ID3D12Device_CreateRootSignature(dev, 0, csr.pSerializedBlob, csr.SerializedBlobSizeInBytes, &IID_ID3D12RootSignature, (void **)&rs));
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &rs, sizeof rs, sizeof(void *));
  CHECK(stream_make(dev2, &s, &pso) == E_INVALIDARG && !pso);
  /* (a blob that is not a root signature) */
  memset(&s, 0, sizeof s);
  D3D12_SERIALIZED_ROOT_SIGNATURE_DESC junk = {dx_cs_fill_dxbc, 16};
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE, &junk, sizeof junk, sizeof(void *));
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cs, sizeof cs, sizeof(void *));
  CHECK(FAILED(stream_make(dev2, &s, &pso)) && !pso);
  /* graphics pipelines with RASTERIZER2 / RASTERIZER1 and DEPTH_STENCIL2 */
  D3D12_INPUT_ELEMENT_DESC layout[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_INPUT_LAYOUT_DESC il = {layout, 2};
  const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  struct D3D12_RT_FORMAT_ARRAY rt;
  memset(&rt, 0, sizeof rt);
  rt.NumRenderTargets = 1;
  rt.RTFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  D3D12_RASTERIZER_DESC2 r2;
  memset(&r2, 0, sizeof r2);
  r2.FillMode = D3D12_FILL_MODE_SOLID;
  r2.CullMode = D3D12_CULL_MODE_NONE;
  r2.DepthBias = 2.4f;
  r2.DepthClipEnable = TRUE;
  r2.LineRasterizationMode = D3D12_LINE_RASTERIZATION_MODE_ALIASED;
  D3D12_DEPTH_STENCIL_DESC2 ds2;
  memset(&ds2, 0, sizeof ds2);
  ds2.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  ds2.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  const D3D12_DEPTH_STENCILOP_DESC1 keep = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS, 0xFF, 0xFF};
  ds2.FrontFace = ds2.BackFace = keep;
  for (int variant = 0; variant < 9; ++variant) {
    memset(&s, 0, sizeof s);
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE, &gsr, sizeof gsr, sizeof(void *));
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &vs, sizeof vs, sizeof(void *));
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &ps, sizeof ps, sizeof(void *));
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &il, sizeof il, sizeof(void *));
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &topology, 4, 4);
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rt, sizeof rt, 4);
    D3D12_RASTERIZER_DESC2 r = r2;
    D3D12_DEPTH_STENCIL_DESC2 d = ds2;
    HRESULT want = S_OK;
    if (variant == 1) { r.LineRasterizationMode = D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW; want = E_INVALIDARG; }      /* (NarrowQuadrilateralLinesSupported FALSE) */
    if (variant == 2) { r.LineRasterizationMode = (D3D12_LINE_RASTERIZATION_MODE)7; want = E_INVALIDARG; }
    if (variant == 3) { d.StencilEnable = TRUE; d.BackFace.StencilReadMask = 0x0F; want = E_INVALIDARG; }      /* (IndependentFrontAndBackStencilRefMaskSupported FALSE) */
    if (variant == 4) { d.BackFace.StencilReadMask = 0x0F; }      /* (masks that differ with the stencil test off) */
    if (variant == 5) { d.DepthBoundsTestEnable = TRUE; want = E_INVALIDARG; }
    if (variant == 7) r.LineRasterizationMode = D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE;
    if (variant == 6) {
      /* RASTERIZER1 in place of RASTERIZER2, with RASTERIZER as well: two of one kind */
      D3D12_RASTERIZER_DESC1 r1;
      memset(&r1, 0, sizeof r1);
      r1.FillMode = D3D12_FILL_MODE_SOLID;
      r1.CullMode = D3D12_CULL_MODE_BACK;
      r1.DepthBias = -1.5f;
      r1.DepthClipEnable = TRUE;
      stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1, &r1, sizeof r1, 4);
      ID3D12PipelineState *one = NULL;
      CHECK_HR(stream_make(dev2, &s, &one));
      if (one) ID3D12PipelineState_Release(one);
      D3D12_RASTERIZER_DESC r0;
      memset(&r0, 0, sizeof r0);
      r0.FillMode = D3D12_FILL_MODE_SOLID;
      r0.CullMode = D3D12_CULL_MODE_BACK;
      stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &r0, sizeof r0, 4);
      want = E_INVALIDARG;
    } else {
      stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, &r, sizeof r, 4);
    }
    stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, &d, sizeof d, 4);
    if (variant == 8) {
      /* DEPTH_STENCIL1 after DEPTH_STENCIL2: two of one kind */
      D3D12_DEPTH_STENCIL_DESC1 d1;
      memset(&d1, 0, sizeof d1);
      stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &d1, sizeof d1, 4);
      want = E_INVALIDARG;
    }
    pso = NULL;
    const HRESULT hr = stream_make(dev2, &s, &pso);
    printf("pipeline stream variant %d: %08lx\n", variant, (unsigned long)hr);
    CHECK(hr == want && (SUCCEEDED(hr) ? pso != NULL : pso == NULL));
    if (pso) ID3D12PipelineState_Release(pso);
  }
  /* a subobject type of a later SDK than this one knows (PSDB), and the dynamic depth bias and strip cut, which this device has not (OPTIONS16 / 15) */
  memset(&s, 0, sizeof s);
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE, &csr, sizeof csr, sizeof(void *));
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cs, sizeof cs, sizeof(void *));
  Stream with_flags = s;
  const UINT junk_psdb[4] = {0};
  stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PSDB, junk_psdb, sizeof junk_psdb, sizeof(void *));
  CHECK(stream_make(dev2, &s, &pso) == E_INVALIDARG && !pso);
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16));
  CHECK(!o16.DynamicDepthBiasSupported);
  const D3D12_PIPELINE_STATE_FLAGS dynamic = D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
  stream_add(&with_flags, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, &dynamic, 4, 4);
  CHECK(stream_make(dev2, &with_flags, &pso) == E_INVALIDARG && !pso);
  /* the library takes the stream with a serialized root signature as the description with the object: the same fingerprint */
  ID3D12Device1 *dev1 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device1, (void **)&dev1));
  if (dev1 && rs) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
    memset(&cd, 0, sizeof cd);
    cd.pRootSignature = rs;
    cd.CS = cs;
    ID3D12PipelineState *stored = NULL;
    CHECK_HR(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&stored));
    ID3D12PipelineLibrary1 *lib = NULL;
    CHECK_HR(ID3D12Device1_CreatePipelineLibrary(dev1, NULL, 0, &IID_ID3D12PipelineLibrary1, (void **)&lib));
    if (lib && stored) {
      CHECK_HR(ID3D12PipelineLibrary1_StorePipeline(lib, L"fill", stored));
      memset(&s, 0, sizeof s);
      stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE, &csr, sizeof csr, sizeof(void *));
      stream_add(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cs, sizeof cs, sizeof(void *));
      D3D12_PIPELINE_STATE_STREAM_DESC sd = {s.size, s.bytes};
      ID3D12PipelineState *loaded = NULL;
      CHECK_HR(ID3D12PipelineLibrary1_LoadPipeline(lib, L"fill", &sd, &IID_ID3D12PipelineState, (void **)&loaded));
      if (loaded) ID3D12PipelineState_Release(loaded);
    }
    if (lib) ID3D12PipelineLibrary1_Release(lib);
    if (stored) ID3D12PipelineState_Release(stored);
  }
  if (dev1) ID3D12Device1_Release(dev1);
  if (rs) ID3D12RootSignature_Release(rs);
  ID3D10Blob_Release(cblob);
  ID3D10Blob_Release(gblob);
  ID3D12Device2_Release(dev2);
}

/* Sessions of one identifier are of one store: on one device the same session, on another a session of the same store, and two disk sessions write one file. */
static void test_shared_shader_cache(ID3D12Device *dev) {
  ID3D12Device9 *dev9 = NULL, *other9 = NULL;
  ID3D12Device *other = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device9, (void **)&dev9));
  LUID luid = ID3D12Device_GetAdapterLuid(dev);
  IDXGIFactory4 *factory = NULL;
  IDXGIAdapter *adapter = NULL;
  CHECK_HR(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory));
  if (factory) CHECK_HR(IDXGIFactory4_EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter, (void **)&adapter));
  if (adapter) CHECK_HR(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&other));
  if (other) CHECK_HR(ID3D12Device_QueryInterface(other, &IID_ID3D12Device9, (void **)&other9));
  if (!dev9 || !other9) goto done;
  for (int mode = 0; mode < 2; ++mode) {
    D3D12_SHADER_CACHE_SESSION_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Identifier.Data1 = 0x5A4E0000u + (UINT)mode;
    sd.Identifier.Data2 = (USHORT)GetCurrentProcessId();
    sd.Mode = mode ? D3D12_SHADER_CACHE_MODE_DISK : D3D12_SHADER_CACHE_MODE_MEMORY;
    if (mode) sd.Flags = D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
    sd.Version = 3;
    ID3D12ShaderCacheSession *a = NULL, *b = NULL, *c = NULL, *mismatch = NULL;
    CHECK_HR(ID3D12Device9_CreateShaderCacheSession(dev9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&a));
    CHECK_HR(ID3D12Device9_CreateShaderCacheSession(dev9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&b));
    CHECK_HR(ID3D12Device9_CreateShaderCacheSession(other9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&c));
    CHECK(a && a == b && c && c != a);      /* (the same session on one device, another session of the store on the other) */
    D3D12_SHADER_CACHE_SESSION_DESC changed = sd;
    changed.Version = 4;
    CHECK(ID3D12Device9_CreateShaderCacheSession(dev9, &changed, &IID_ID3D12ShaderCacheSession, (void **)&mismatch) == DXGI_ERROR_ALREADY_EXISTS && !mismatch);
    CHECK(ID3D12Device9_CreateShaderCacheSession(other9, &changed, &IID_ID3D12ShaderCacheSession, (void **)&mismatch) == DXGI_ERROR_ALREADY_EXISTS && !mismatch);
    if (a && c) {
      UINT size = 0;
      CHECK_HR(ID3D12ShaderCacheSession_StoreValue(a, "one", 4, "first", 6));
      CHECK_HR(ID3D12ShaderCacheSession_StoreValue(c, "two", 4, "second", 7));
      CHECK(ID3D12ShaderCacheSession_StoreValue(c, "one", 4, "again", 6) == DXGI_ERROR_ALREADY_EXISTS);
      CHECK_HR(ID3D12ShaderCacheSession_FindValue(a, "two", 4, NULL, &size));
      CHECK(size == 7);
      CHECK_HR(ID3D12ShaderCacheSession_FindValue(c, "one", 4, NULL, &size));
      CHECK(size == 6);
    }
    if (a) ID3D12ShaderCacheSession_Release(a);
    if (b) ID3D12ShaderCacheSession_Release(b);
    if (c) ID3D12ShaderCacheSession_Release(c);
    /* every session gone: a memory store is gone with them; a disk store's file has what both devices stored */
    a = NULL;
    CHECK_HR(ID3D12Device9_CreateShaderCacheSession(other9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&a));
    if (a) {
      char got[16];
      UINT size = sizeof got;
      const HRESULT one = ID3D12ShaderCacheSession_FindValue(a, "one", 4, got, &size);
      size = sizeof got;
      const HRESULT two = ID3D12ShaderCacheSession_FindValue(a, "two", 4, got, &size);
      printf("shader cache (%s) reopened: %08lx %08lx\n", mode ? "disk" : "memory", (unsigned long)one, (unsigned long)two);
      if (mode) CHECK(one == S_OK && two == S_OK && size == 7 && memcmp(got, "second", 7) == 0);
      else CHECK(one == DXGI_ERROR_NOT_FOUND && two == DXGI_ERROR_NOT_FOUND);
      ID3D12ShaderCacheSession_SetDeleteOnDestroy(a);
      ID3D12ShaderCacheSession_Release(a);
    }
    if (mode) {
      /* (deleted with its last session: a new one starts empty) */
      a = NULL;
      CHECK_HR(ID3D12Device9_CreateShaderCacheSession(dev9, &sd, &IID_ID3D12ShaderCacheSession, (void **)&a));
      if (a) {
        UINT size = 0;
        CHECK(ID3D12ShaderCacheSession_FindValue(a, "one", 4, NULL, &size) == DXGI_ERROR_NOT_FOUND);
        ID3D12ShaderCacheSession_SetDeleteOnDestroy(a);
        ID3D12ShaderCacheSession_Release(a);
      }
    }
  }
done:
  if (other9) ID3D12Device9_Release(other9);
  if (other) ID3D12Device_Release(other);
  if (adapter) IDXGIAdapter_Release(adapter);
  if (factory) IDXGIFactory4_Release(factory);
  if (dev9) ID3D12Device9_Release(dev9);
}

/* ID3D12Device11 .. 14, ID3D12GraphicsCommandList8 .. 10, the features of OPTIONS12 .. 21, and an aliasing done with an enhanced barrier. */
static void test_agility_interfaces(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  static const IID *const iids[] = {&IID_ID3D12Device11, &IID_ID3D12Device12, &IID_ID3D12Device13, &IID_ID3D12Device14};
  for (UINT i = 0; i < 4; ++i) {
    IUnknown *u = NULL;
    CHECK_HR(ID3D12Device_QueryInterface(dev, iids[i], (void **)&u));
    CHECK(u == (IUnknown *)dev);
    if (u) IUnknown_Release(u);
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12;
  D3D12_FEATURE_DATA_D3D12_OPTIONS19 o19;
  D3D12_FEATURE_DATA_D3D12_OPTIONS20 o20;
  D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12));
  CHECK(o12.EnhancedBarriersSupported && !o12.RelaxedFormatCastingSupported);
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS19, &o19, sizeof o19));
  CHECK(o19.RasterizerDesc2Supported && !o19.NarrowQuadrilateralLinesSupported && o19.MaxSamplerDescriptorHeapSize == 2048 && o19.MaxViewDescriptorHeapSize == 1000000);
  CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS19, &o19, sizeof o19 - 4) == E_INVALIDARG);
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS20, &o20, sizeof o20));
  CHECK(o20.RecreateAtTier == D3D12_RECREATE_AT_TIER_NOT_SUPPORTED);
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof o21));
  CHECK(o21.WorkGraphsTier == D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED && o21.ExecuteIndirectTier == D3D12_EXECUTE_INDIRECT_TIER_1_0);
  ID3D12Device11 *dev11 = NULL;
  ID3D12Device12 *dev12 = NULL;
  ID3D12Device13 *dev13 = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device11, (void **)&dev11));
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device12, (void **)&dev12));
  CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device13, (void **)&dev13));
  if (dev11) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap *heap = NULL;
    CHECK_HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    if (heap) {
      D3D12_SAMPLER_DESC2 s2;
      memset(&s2, 0, sizeof s2);
      s2.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      s2.AddressU = s2.AddressV = s2.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
      s2.UintBorderColor[0] = s2.UintBorderColor[3] = 255;
      s2.MaxLOD = 1000.0f;
      s2.Flags = D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR;
      ID3D12Device11_CreateSampler2(dev11, &s2, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap));
      ID3D12DescriptorHeap_Release(heap);
    }
    ID3D12Device11_Release(dev11);
  }
  if (dev12) {
    D3D12_RESOURCE_DESC1 rd;
    memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = 64;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    const UINT32 none = 0, one = 1;
    const DXGI_FORMAT cast = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, *casts = &cast;
    D3D12_RESOURCE_ALLOCATION_INFO i2 = ID3D12Device12_GetResourceAllocationInfo2(dev12, 0, 1, &rd, NULL);
    D3D12_RESOURCE_ALLOCATION_INFO i3 = ID3D12Device12_GetResourceAllocationInfo3(dev12, 0, 1, &rd, &none, &casts, NULL);
    CHECK(i3.SizeInBytes == i2.SizeInBytes && i3.Alignment == i2.Alignment && i2.SizeInBytes != ~(UINT64)0);
    i3 = ID3D12Device12_GetResourceAllocationInfo3(dev12, 0, 1, &rd, &one, &casts, NULL);
    CHECK(i3.SizeInBytes == ~(UINT64)0);      /* (RelaxedFormatCastingSupported is FALSE) */
    ID3D12Device12_Release(dev12);
  }
  if (dev13) {
    ID3D12Heap *heap = NULL;
    static BYTE memory[65536];
    CHECK(ID3D12Device13_OpenExistingHeapFromAddress1(dev13, memory, sizeof memory, &IID_ID3D12Heap, (void **)&heap) == DXGI_ERROR_UNSUPPORTED && !heap);
    CHECK(ID3D12Device13_OpenExistingHeapFromAddress1(dev13, NULL, 0, &IID_ID3D12Heap, (void **)&heap) == E_INVALIDARG && !heap);
    ID3D12Device13_Release(dev13);
  }
  /* the later lists, and an aliasing barrier said the enhanced way: a placed buffer written, given up (access NO_ACCESS), and the buffer placed at the same bytes read */
  ID3D12CommandAllocator *allocator = NULL;
  ID3D12GraphicsCommandList *list = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocator));
  if (allocator) CHECK_HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
  if (!list) { if (allocator) ID3D12CommandAllocator_Release(allocator); return; }
  ID3D12GraphicsCommandList8 *l8 = NULL;
  ID3D12GraphicsCommandList9 *l9 = NULL;
  ID3D12GraphicsCommandList10 *l10 = NULL;
  CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList8, (void **)&l8));
  CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList9, (void **)&l9));
  CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList10, (void **)&l10));
  CHECK((void *)l8 == (void *)list && (void *)l9 == (void *)list && (void *)l10 == (void *)list);
  if (l8) { ID3D12GraphicsCommandList8_OMSetFrontAndBackStencilRef(l8, 3, 3); ID3D12GraphicsCommandList8_Release(l8); }
  if (l9) ID3D12GraphicsCommandList9_Release(l9);
  if (l10) ID3D12GraphicsCommandList10_Release(l10);
  D3D12_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.SizeInBytes = 65536;
  hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ID3D12Heap *heap = NULL;
  CHECK_HR(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&heap));
  D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *first = NULL, *second = NULL;
  if (heap) {
    CHECK_HR(ID3D12Device_CreatePlacedResource(dev, heap, 0, &bd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&first));
    CHECK_HR(ID3D12Device_CreatePlacedResource(dev, heap, 0, &bd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&second));
  }
  ID3D12Resource *upload = make_buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *readback = make_buffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList7 *l7 = NULL;
  CHECK_HR(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList7, (void **)&l7));
  if (first && second && upload && readback && l7) {
    UINT32 *p = NULL;
    CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void **)&p));
    if (p) for (UINT i = 0; i < 64; ++i) p[i] = 0xA1A50000u + i;
    ID3D12Resource_Unmap(upload, 0, NULL);
    /* (the second buffer used first, so its device object holds bytes of its own) */
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, second, 0, 256);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, first, 0, upload, 0, 256);
    D3D12_BUFFER_BARRIER bb[2];
    memset(bb, 0, sizeof bb);
    bb[0].SyncBefore = D3D12_BARRIER_SYNC_COPY;
    bb[0].SyncAfter = D3D12_BARRIER_SYNC_NONE;
    bb[0].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
    bb[0].AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
    bb[0].pResource = first;
    bb[0].Size = ~(UINT64)0;
    bb[1].SyncBefore = D3D12_BARRIER_SYNC_NONE;
    bb[1].SyncAfter = D3D12_BARRIER_SYNC_COPY;
    bb[1].AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
    bb[1].AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    bb[1].pResource = second;
    bb[1].Size = ~(UINT64)0;
    D3D12_BARRIER_GROUP group = {D3D12_BARRIER_TYPE_BUFFER, 2, {.pBufferBarriers = bb}};
    ID3D12GraphicsCommandList7_Barrier(l7, 1, &group);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, second, 0, 256);
    run_list(queue, fence, fence_value, list);
    UINT32 *got = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(readback, 0, NULL, (void **)&got)) && got) {
      int same = 1;
      for (UINT i = 0; i < 64; ++i) same &= got[i] == 0xA1A50000u + i;
      printf("enhanced barrier aliasing: %s\n", same ? "the second buffer has the first's bytes" : "the second buffer kept its own");
      CHECK(same);
      ID3D12Resource_Unmap(readback, 0, NULL);
    }
  } else {
    CHECK_HR(ID3D12GraphicsCommandList_Close(list));
  }
  /* a GPU upload heap (OPTIONS16): the CPU's writes reach the GPU, and the GPU's the CPU */
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16;
  CHECK_HR(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16));
  CHECK(o16.GPUUploadHeapSupported);
  const D3D12_HEAP_PROPERTIES custom = ID3D12Device_GetCustomHeapProperties(dev, 0, D3D12_HEAP_TYPE_GPU_UPLOAD);
  CHECK(custom.Type == D3D12_HEAP_TYPE_CUSTOM && custom.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE && custom.MemoryPoolPreference == D3D12_MEMORY_POOL_L1);
  ID3D12Resource *gpu_upload = make_buffer(dev, D3D12_HEAP_TYPE_GPU_UPLOAD, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  CHECK(gpu_upload != NULL);
  if (gpu_upload && upload && readback && allocator) {
    D3D12_HEAP_PROPERTIES hp;
    D3D12_HEAP_FLAGS hf;
    CHECK_HR(ID3D12Resource_GetHeapProperties(gpu_upload, &hp, &hf));
    CHECK(hp.Type == D3D12_HEAP_TYPE_GPU_UPLOAD);
    UINT32 *p = NULL;
    CHECK_HR(ID3D12Resource_Map(gpu_upload, 0, NULL, (void **)&p));
    if (p) { for (UINT i = 0; i < 64; ++i) p[i] = 0x6B000000u + i; ID3D12Resource_Unmap(gpu_upload, 0, NULL); }
    p = NULL;
    CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void **)&p));
    if (p) { for (UINT i = 0; i < 64; ++i) p[i] = 0x5C000000u + i; ID3D12Resource_Unmap(upload, 0, NULL); }
    CHECK_HR(ID3D12CommandAllocator_Reset(allocator));
    CHECK_HR(ID3D12GraphicsCommandList_Reset(list, allocator, NULL));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, gpu_upload, 0, 256);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, gpu_upload, 128, upload, 0, 128);
    run_list(queue, fence, fence_value, list);
    UINT32 *got = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(readback, 0, NULL, (void **)&got)) && got) {
      int same = 1;
      for (UINT i = 0; i < 64; ++i) same &= got[i] == 0x6B000000u + i;
      CHECK(same);      /* (the CPU's writes, as the GPU saw them) */
      ID3D12Resource_Unmap(readback, 0, NULL);
    }
    got = NULL;
    if (SUCCEEDED(ID3D12Resource_Map(gpu_upload, 0, NULL, (void **)&got)) && got) {
      int same = 1;
      for (UINT i = 0; i < 32; ++i) same &= got[i] == 0x6B000000u + i && got[32 + i] == 0x5C000000u + i;
      printf("GPU upload heap: %s\n", same ? "the CPU and the GPU see each other's writes" : "a write is missing");
      CHECK(same);      /* (the GPU's copy into it, as the CPU reads it) */
      ID3D12Resource_Unmap(gpu_upload, 0, NULL);
    }
  }
  if (gpu_upload) ID3D12Resource_Release(gpu_upload);
  if (l7) ID3D12GraphicsCommandList7_Release(l7);
  if (upload) ID3D12Resource_Release(upload);
  if (readback) ID3D12Resource_Release(readback);
  if (first) ID3D12Resource_Release(first);
  if (second) ID3D12Resource_Release(second);
  if (heap) ID3D12Heap_Release(heap);
  ID3D12GraphicsCommandList_Release(list);
  ID3D12CommandAllocator_Release(allocator);
}

static void test_agility(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  test_root_signature_12(dev);
  test_library_root_signatures(dev);
  test_agility_streams(dev);
  test_shared_shader_cache(dev);
  test_agility_interfaces(dev, queue, fence, fence_value);
}

static void test_later(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  test_later_queries(dev);
  test_later_objects(dev, queue);
  test_write_immediate(dev, queue, fence, fence_value);
  test_render_pass(dev, queue, fence, fence_value);
  test_deserializers();
  test_pipeline_library(dev);
  test_shader_cache(dev);
  test_cpu_texture(dev, queue, fence, fence_value);
  test_agility(dev, queue, fence, fence_value);
}

int main(void) {
  IDXGIAdapter1 *adapter = NULL;
  test_dxgi(&adapter);
  ID3D12Device *dev = NULL;
  CHECK(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL) == S_FALSE);
  CHECK_HR(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
  CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_1, &IID_ID3D12Device, (void **)&dev) == DXGI_ERROR_UNSUPPORTED || dev != NULL);
  if (!dev) {
    printf("d3d12test: no device\n");
    printf("d3d12test: %d checks, %d failed\n", checks, failed + 1);
    return 1;
  }
  test_device_queries(dev);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *queue = NULL;
  CHECK_HR(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
  ID3D12Fence *fence = NULL;
  CHECK_HR(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
  UINT64 value = 0;
  if (queue && fence) {
    test_fences(dev, queue);
    test_copies(dev, queue, fence, &value);
    test_triangle(dev, queue, fence, &value);
    test_texture(dev, queue, fence, &value);
    test_address_modes(dev, queue, fence, &value);
    test_mipmaps(dev, queue, fence, &value);
    test_depth(dev, queue, fence, &value);
    test_compute(dev, queue, fence, &value);
    test_extras(dev, queue, fence, &value);
    test_later(dev, queue, fence, &value);
    test_swap_chain(adapter, dev, queue);
  }
  if (fence) ID3D12Fence_Release(fence);
  if (queue) ID3D12CommandQueue_Release(queue);
  ID3D12Device_Release(dev);
  if (adapter) IDXGIAdapter1_Release(adapter);
  printf("d3d12test: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
