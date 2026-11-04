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
#include <d3d12.h>
#include <dxgi1_6.h>
#include "dxfixtures.h"

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)
#define CHECK_HR(x) do { HRESULT hr_ = (x); ++checks; if (FAILED(hr_)) { ++failed; printf("FAILED line %d: %s = %08lx\n", __LINE__, #x, (unsigned long)hr_); } } while (0)

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


static void test_texture(ID3D12Device *dev, ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 *fence_value) {
  D3D12_HEAP_PROPERTIES defaults = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rt_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_RESOURCE_DESC tex_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
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
  memcpy(p, &texels[0], 8);
  memcpy(p + 256, &texels[2], 8);
  ID3D12Resource_Unmap(upload, 0, NULL);
  const float uv[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
  float verts[6][7];
  for (int i = 0; i < 6; ++i) {
    verts[i][0] = uv[i][0] * 2 - 1; verts[i][1] = 1 - uv[i][1] * 2; verts[i][2] = 0.5f;
    verts[i][3] = uv[i][0] / 2; verts[i][4] = uv[i][1] / 2; verts[i][5] = 1; verts[i][6] = 1;
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
  sv.Texture2D.MipLevels = 1;
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
  ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
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
  if (pixels) {
    const UINT32 q00 = pixels[8 * 32 + 8], q10 = pixels[8 * 32 + 24], q01 = pixels[24 * 32 + 8], q11 = pixels[24 * 32 + 24];
    printf("texture: quadrants %08x %08x %08x %08x\n", (unsigned)q00, (unsigned)q10, (unsigned)q01, (unsigned)q11);
    /* (R8G8B8A8: 0xAABBGGRR) red (scaled by the colour), green, blue, white */
    CHECK((q00 & 0xFF) > 0 && ((q00 >> 8) & 0xFF) == 0 && ((q00 >> 16) & 0xFF) == 0);
    CHECK((q10 & 0xFF) == 0 && ((q10 >> 8) & 0xFF) > 0 && ((q10 >> 16) & 0xFF) == 0);
    CHECK((q01 & 0xFF) == 0 && ((q01 >> 8) & 0xFF) == 0 && ((q01 >> 16) & 0xFF) > 200);
    CHECK((q11 & 0xFF) > 0 && ((q11 >> 8) & 0xFF) > 0 && ((q11 >> 16) & 0xFF) > 200);
    ID3D12Resource_Unmap(readback, 0, NULL);
  }
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
    test_depth(dev, queue, fence, &value);
    test_compute(dev, queue, fence, &value);
    test_swap_chain(adapter, dev, queue);
  }
  if (fence) ID3D12Fence_Release(fence);
  if (queue) ID3D12CommandQueue_Release(queue);
  ID3D12Device_Release(dev);
  if (adapter) IDXGIAdapter1_Release(adapter);
  printf("d3d12test: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
