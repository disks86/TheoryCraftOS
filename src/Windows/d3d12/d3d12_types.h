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
/* What the later Agility SDKs' d3d12.h has and mingw's does not: root signature version 1.2 (static samplers with flags), the second sampler description, the
 * pipeline-stream subobjects DEPTH_STENCIL2 / RASTERIZER1 / RASTERIZER2 / SERIALIZED_ROOT_SIGNATURE, the feature structures OPTIONS19 .. 21, the GPU upload heap,
 * and the interfaces ID3D12Device11 .. 14 and ID3D12GraphicsCommandList8 .. 10. Laid out as Microsoft's DirectX-Headers (MIT) define them; the vtables in widl's C
 * form (each one whole, as widl writes it), which scripts/gen-dx-vtables.py reads. Included after <d3d12.h>, by d3d12.dll and by the test programs. */
#pragma once

#define D3D_ROOT_SIGNATURE_VERSION_1_2 ((D3D_ROOT_SIGNATURE_VERSION)0x3)
#define D3D12_HEAP_TYPE_GPU_UPLOAD ((D3D12_HEAP_TYPE)5)

#define D3D12_FEATURE_D3D12_OPTIONS19 ((D3D12_FEATURE)48)
#define D3D12_FEATURE_D3D12_OPTIONS20 ((D3D12_FEATURE)49)
#define D3D12_FEATURE_PREDICATION ((D3D12_FEATURE)50)
#define D3D12_FEATURE_PLACED_RESOURCE_SUPPORT_INFO ((D3D12_FEATURE)51)
#define D3D12_FEATURE_HARDWARE_COPY ((D3D12_FEATURE)52)
#define D3D12_FEATURE_D3D12_OPTIONS21 ((D3D12_FEATURE)53)

#define D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2 ((D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)26)
#define D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1 ((D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)27)
#define D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2 ((D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)28)
#define D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE ((D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)29)
#define D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PSDB ((D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)30)

#define D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS ((D3D12_PIPELINE_STATE_FLAGS)0x4)
#define D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT ((D3D12_PIPELINE_STATE_FLAGS)0x8)
#define D3D12_PIPELINE_STATE_FLAG_DISABLE_CACHED_BLOB ((D3D12_PIPELINE_STATE_FLAGS)0x10)

#define D3D12_HEAP_FLAG_TOOLS_USE_MANUAL_WRITE_TRACKING ((D3D12_HEAP_FLAGS)0x2000)

typedef enum D3D12_SAMPLER_FLAGS {
  D3D12_SAMPLER_FLAG_NONE = 0, D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR = 0x1, D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES = 0x2
} D3D12_SAMPLER_FLAGS;
#define D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK_UINT ((D3D12_STATIC_BORDER_COLOR)3)
#define D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT ((D3D12_STATIC_BORDER_COLOR)4)

typedef struct D3D12_SAMPLER_DESC2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union { FLOAT FloatBorderColor[4]; UINT UintBorderColor[4]; };
  FLOAT MinLOD, MaxLOD;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_SAMPLER_DESC2;

typedef struct D3D12_STATIC_SAMPLER_DESC1 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  D3D12_STATIC_BORDER_COLOR BorderColor;
  FLOAT MinLOD, MaxLOD;
  UINT ShaderRegister, RegisterSpace;
  D3D12_SHADER_VISIBILITY ShaderVisibility;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_STATIC_SAMPLER_DESC1;

typedef struct D3D12_ROOT_SIGNATURE_DESC2 {
  UINT NumParameters;
  const D3D12_ROOT_PARAMETER1 *pParameters;
  UINT NumStaticSamplers;
  const D3D12_STATIC_SAMPLER_DESC1 *pStaticSamplers;
  D3D12_ROOT_SIGNATURE_FLAGS Flags;
} D3D12_ROOT_SIGNATURE_DESC2;

/* (the Agility SDK's versioned description: mingw's has no Desc_1_2; the same layout with it, used where 1.2 is read or written) */
typedef struct D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 {
  D3D_ROOT_SIGNATURE_VERSION Version;
  union { D3D12_ROOT_SIGNATURE_DESC Desc_1_0; D3D12_ROOT_SIGNATURE_DESC1 Desc_1_1; D3D12_ROOT_SIGNATURE_DESC2 Desc_1_2; };
} D3D12_VERSIONED_ROOT_SIGNATURE_DESC2;

typedef struct D3D12_DEPTH_STENCILOP_DESC1 {
  D3D12_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp;
  D3D12_COMPARISON_FUNC StencilFunc;
  UINT8 StencilReadMask, StencilWriteMask;
} D3D12_DEPTH_STENCILOP_DESC1;
typedef struct D3D12_DEPTH_STENCIL_DESC2 {
  BOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  BOOL StencilEnable;
  D3D12_DEPTH_STENCILOP_DESC1 FrontFace, BackFace;
  BOOL DepthBoundsTestEnable;
} D3D12_DEPTH_STENCIL_DESC2;

typedef struct D3D12_RASTERIZER_DESC1 {
  D3D12_FILL_MODE FillMode;
  D3D12_CULL_MODE CullMode;
  BOOL FrontCounterClockwise;
  FLOAT DepthBias, DepthBiasClamp, SlopeScaledDepthBias;
  BOOL DepthClipEnable, MultisampleEnable, AntialiasedLineEnable;
  UINT ForcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
} D3D12_RASTERIZER_DESC1;
typedef enum D3D12_LINE_RASTERIZATION_MODE {
  D3D12_LINE_RASTERIZATION_MODE_ALIASED = 0, D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED = 1, D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE = 2,
  D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW = 3
} D3D12_LINE_RASTERIZATION_MODE;
typedef struct D3D12_RASTERIZER_DESC2 {
  D3D12_FILL_MODE FillMode;
  D3D12_CULL_MODE CullMode;
  BOOL FrontCounterClockwise;
  FLOAT DepthBias, DepthBiasClamp, SlopeScaledDepthBias;
  BOOL DepthClipEnable;
  D3D12_LINE_RASTERIZATION_MODE LineRasterizationMode;
  UINT ForcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
} D3D12_RASTERIZER_DESC2;

typedef struct D3D12_SERIALIZED_ROOT_SIGNATURE_DESC {
  const void *pSerializedBlob;
  SIZE_T SerializedBlobSizeInBytes;
} D3D12_SERIALIZED_ROOT_SIGNATURE_DESC;

typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS19 {
  BOOL MismatchingOutputDimensionsSupported;
  UINT SupportedSampleCountsWithNoOutputs;
  BOOL PointSamplingAddressesNeverRoundUp;
  BOOL RasterizerDesc2Supported;
  BOOL NarrowQuadrilateralLinesSupported;
  BOOL AnisoFilterWithPointMipSupported;
  UINT MaxSamplerDescriptorHeapSize;
  UINT MaxSamplerDescriptorHeapSizeWithStaticSamplers;
  UINT MaxViewDescriptorHeapSize;
  BOOL ComputeOnlyCustomHeapSupported;
} D3D12_FEATURE_DATA_D3D12_OPTIONS19;
typedef enum D3D12_RECREATE_AT_TIER { D3D12_RECREATE_AT_TIER_NOT_SUPPORTED = 0, D3D12_RECREATE_AT_TIER_1 = 1 } D3D12_RECREATE_AT_TIER;
typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS20 {
  BOOL ComputeOnlyWriteWatchSupported;
  D3D12_RECREATE_AT_TIER RecreateAtTier;
} D3D12_FEATURE_DATA_D3D12_OPTIONS20;
typedef enum D3D12_WORK_GRAPHS_TIER { D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED = 0, D3D12_WORK_GRAPHS_TIER_1_0 = 10 } D3D12_WORK_GRAPHS_TIER;
typedef enum D3D12_EXECUTE_INDIRECT_TIER { D3D12_EXECUTE_INDIRECT_TIER_1_0 = 10, D3D12_EXECUTE_INDIRECT_TIER_1_1 = 11 } D3D12_EXECUTE_INDIRECT_TIER;
typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS21 {
  D3D12_WORK_GRAPHS_TIER WorkGraphsTier;
  D3D12_EXECUTE_INDIRECT_TIER ExecuteIndirectTier;
  BOOL SampleCmpGradientAndBiasSupported;
  BOOL ExtendedCommandInfoSupported;
} D3D12_FEATURE_DATA_D3D12_OPTIONS21;

/* (the work graph descriptions are only named: ID3D12GraphicsCommandList10's calls take them, and no work graph can be made here - WorkGraphsTier is NOT_SUPPORTED) */
typedef struct D3D12_SET_PROGRAM_DESC D3D12_SET_PROGRAM_DESC;
typedef struct D3D12_DISPATCH_GRAPH_DESC D3D12_DISPATCH_GRAPH_DESC;

#ifndef __ID3D12Device11_INTERFACE_DEFINED__
#define __ID3D12Device11_INTERFACE_DEFINED__
typedef interface ID3D12Device11 ID3D12Device11;
DEFINE_GUID(IID_ID3D12Device11, 0x5405c344, 0xd457, 0x444e, 0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39);
typedef struct ID3D12Device11Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12Device11 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12Device11 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12Device11 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12Device11 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12Device11 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12Device11 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12Device11 *This,
        const WCHAR *name);

    /*** ID3D12Device methods ***/
    UINT (STDMETHODCALLTYPE *GetNodeCount)(
        ID3D12Device11 *This);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue)(
        ID3D12Device11 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid,
        void **command_queue);

    HRESULT (STDMETHODCALLTYPE *CreateCommandAllocator)(
        ID3D12Device11 *This,
        D3D12_COMMAND_LIST_TYPE type,
        REFIID riid,
        void **command_allocator);

    HRESULT (STDMETHODCALLTYPE *CreateGraphicsPipelineState)(
        ID3D12Device11 *This,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineState)(
        ID3D12Device11 *This,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateCommandList)(
        ID3D12Device11 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *command_allocator,
        ID3D12PipelineState *initial_pipeline_state,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(
        ID3D12Device11 *This,
        D3D12_FEATURE feature,
        void *feature_data,
        UINT feature_data_size);

    HRESULT (STDMETHODCALLTYPE *CreateDescriptorHeap)(
        ID3D12Device11 *This,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid,
        void **descriptor_heap);

    UINT (STDMETHODCALLTYPE *GetDescriptorHandleIncrementSize)(
        ID3D12Device11 *This,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateRootSignature)(
        ID3D12Device11 *This,
        UINT node_mask,
        const void *bytecode,
        SIZE_T bytecode_length,
        REFIID riid,
        void **root_signature);

    void (STDMETHODCALLTYPE *CreateConstantBufferView)(
        ID3D12Device11 *This,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateShaderResourceView)(
        ID3D12Device11 *This,
        ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateUnorderedAccessView)(
        ID3D12Device11 *This,
        ID3D12Resource *resource,
        ID3D12Resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateRenderTargetView)(
        ID3D12Device11 *This,
        ID3D12Resource *resource,
        const D3D12_RENDER_TARGET_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateDepthStencilView)(
        ID3D12Device11 *This,
        ID3D12Resource *resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateSampler)(
        ID3D12Device11 *This,
        const D3D12_SAMPLER_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CopyDescriptors)(
        ID3D12Device11 *This,
        UINT dst_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
        const UINT *dst_descriptor_range_sizes,
        UINT src_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
        const UINT *src_descriptor_range_sizes,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    void (STDMETHODCALLTYPE *CopyDescriptorsSimple)(
        ID3D12Device11 *This,
        UINT descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
        const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo)(
        ID3D12Device11 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT reource_desc_count,
        const D3D12_RESOURCE_DESC *resource_descs);

    D3D12_HEAP_PROPERTIES * (STDMETHODCALLTYPE *GetCustomHeapProperties)(
        ID3D12Device11 *This,
        D3D12_HEAP_PROPERTIES *__ret,
        UINT node_mask,
        D3D12_HEAP_TYPE heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource)(
        ID3D12Device11 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap)(
        ID3D12Device11 *This,
        const D3D12_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource)(
        ID3D12Device11 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource)(
        ID3D12Device11 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateSharedHandle)(
        ID3D12Device11 *This,
        ID3D12DeviceChild *object,
        const SECURITY_ATTRIBUTES *attributes,
        DWORD access,
        const WCHAR *name,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandle)(
        ID3D12Device11 *This,
        HANDLE handle,
        REFIID riid,
        void **object);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandleByName)(
        ID3D12Device11 *This,
        const WCHAR *name,
        DWORD access,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *MakeResident)(
        ID3D12Device11 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *Evict)(
        ID3D12Device11 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *CreateFence)(
        ID3D12Device11 *This,
        UINT64 initial_value,
        D3D12_FENCE_FLAGS flags,
        REFIID riid,
        void **fence);

    HRESULT (STDMETHODCALLTYPE *GetDeviceRemovedReason)(
        ID3D12Device11 *This);

    void (STDMETHODCALLTYPE *GetCopyableFootprints)(
        ID3D12Device11 *This,
        const D3D12_RESOURCE_DESC *desc,
        UINT first_sub_resource,
        UINT sub_resource_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_count,
        UINT64 *row_size,
        UINT64 *total_bytes);

    HRESULT (STDMETHODCALLTYPE *CreateQueryHeap)(
        ID3D12Device11 *This,
        const D3D12_QUERY_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *SetStablePowerState)(
        ID3D12Device11 *This,
        WINBOOL enable);

    HRESULT (STDMETHODCALLTYPE *CreateCommandSignature)(
        ID3D12Device11 *This,
        const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *root_signature,
        REFIID riid,
        void **command_signature);

    void (STDMETHODCALLTYPE *GetResourceTiling)(
        ID3D12Device11 *This,
        ID3D12Resource *resource,
        UINT *total_tile_count,
        D3D12_PACKED_MIP_INFO *packed_mip_info,
        D3D12_TILE_SHAPE *standard_tile_shape,
        UINT *sub_resource_tiling_count,
        UINT first_sub_resource_tiling,
        D3D12_SUBRESOURCE_TILING *sub_resource_tilings);

    LUID * (STDMETHODCALLTYPE *GetAdapterLuid)(
        ID3D12Device11 *This,
        LUID *__ret);

    /*** ID3D12Device1 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineLibrary)(
        ID3D12Device11 *This,
        const void *blob,
        SIZE_T blob_size,
        REFIID iid,
        void **lib);

    HRESULT (STDMETHODCALLTYPE *SetEventOnMultipleFenceCompletion)(
        ID3D12Device11 *This,
        ID3D12Fence *const *fences,
        const UINT64 *values,
        UINT fence_count,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
        HANDLE event);

    HRESULT (STDMETHODCALLTYPE *SetResidencyPriority)(
        ID3D12Device11 *This,
        UINT object_count,
        ID3D12Pageable *const *objects,
        const D3D12_RESIDENCY_PRIORITY *priorities);

    /*** ID3D12Device2 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineState)(
        ID3D12Device11 *This,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    /*** ID3D12Device3 methods ***/
    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromAddress)(
        ID3D12Device11 *This,
        const void *address,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromFileMapping)(
        ID3D12Device11 *This,
        HANDLE file_mapping,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *EnqueueMakeResident)(
        ID3D12Device11 *This,
        D3D12_RESIDENCY_FLAGS flags,
        UINT num_objects,
        ID3D12Pageable *const *objects,
        ID3D12Fence *fence,
        UINT64 fence_value);

    /*** ID3D12Device4 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommandList1)(
        ID3D12Device11 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession)(
        ID3D12Device11 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource1)(
        ID3D12Device11 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap1)(
        ID3D12Device11 *This,
        const D3D12_HEAP_DESC *desc,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource1)(
        ID3D12Device11 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **resource);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo1)(
        ID3D12Device11 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    /*** ID3D12Device5 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateLifetimeTracker)(
        ID3D12Device11 *This,
        ID3D12LifetimeOwner *owner,
        REFIID riid,
        void **tracker);

    void (STDMETHODCALLTYPE *RemoveDevice)(
        ID3D12Device11 *This);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommands)(
        ID3D12Device11 *This,
        UINT *meta_commands_count,
        D3D12_META_COMMAND_DESC *descs);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommandParameters)(
        ID3D12Device11 *This,
        REFGUID command_id,
        D3D12_META_COMMAND_PARAMETER_STAGE stage,
        UINT *total_structure_size_in_bytes,
        UINT *parameter_count,
        D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs);

    HRESULT (STDMETHODCALLTYPE *CreateMetaCommand)(
        ID3D12Device11 *This,
        REFGUID command_id,
        UINT node_mask,
        const void *creation_parameters_data,
        SIZE_T creation_parameters_data_size_in_bytes,
        REFIID riid,
        void **meta_command);

    HRESULT (STDMETHODCALLTYPE *CreateStateObject)(
        ID3D12Device11 *This,
        const D3D12_STATE_OBJECT_DESC *desc,
        REFIID riid,
        void **state_object);

    void (STDMETHODCALLTYPE *GetRaytracingAccelerationStructurePrebuildInfo)(
        ID3D12Device11 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info);

    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS (STDMETHODCALLTYPE *CheckDriverMatchingIdentifier)(
        ID3D12Device11 *This,
        D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check);

    /*** ID3D12Device6 methods ***/
    HRESULT (STDMETHODCALLTYPE *SetBackgroundProcessingMode)(
        ID3D12Device11 *This,
        D3D12_BACKGROUND_PROCESSING_MODE mode,
        D3D12_MEASUREMENTS_ACTION action,
        HANDLE event,
        WINBOOL *further_measurements_desired);

    /*** ID3D12Device7 methods ***/
    HRESULT (STDMETHODCALLTYPE *AddToStateObject)(
        ID3D12Device11 *This,
        const D3D12_STATE_OBJECT_DESC *addition,
        ID3D12StateObject *state_object_to_grow_from,
        REFIID riid,
        void **new_state_object);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession1)(
        ID3D12Device11 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
        REFIID riid,
        void **session);

    /*** ID3D12Device8 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo2)(
        ID3D12Device11 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource2)(
        ID3D12Device11 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource1)(
        ID3D12Device11 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    void (STDMETHODCALLTYPE *CreateSamplerFeedbackUnorderedAccessView)(
        ID3D12Device11 *This,
        ID3D12Resource *targeted_resource,
        ID3D12Resource *feedback_resource,
        D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor);

    void (STDMETHODCALLTYPE *GetCopyableFootprints1)(
        ID3D12Device11 *This,
        const D3D12_RESOURCE_DESC1 *resource_desc,
        UINT first_subresource,
        UINT subresources_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *rows_count,
        UINT64 *row_size_in_bytes,
        UINT64 *total_bytes);

    /*** ID3D12Device9 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateShaderCacheSession)(
        ID3D12Device11 *This,
        const D3D12_SHADER_CACHE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *ShaderCacheControl)(
        ID3D12Device11 *This,
        D3D12_SHADER_CACHE_KIND_FLAGS kinds,
        D3D12_SHADER_CACHE_CONTROL_FLAGS control);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue1)(
        ID3D12Device11 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID creator_id,
        REFIID riid,
        void **command_queue);

    /*** ID3D12Device10 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource3)(
        ID3D12Device11 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource2)(
        ID3D12Device11 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource2)(
        ID3D12Device11 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    /*** ID3D12Device11 methods ***/
    void (STDMETHODCALLTYPE *CreateSampler2)(
        ID3D12Device11 *This,
        const D3D12_SAMPLER_DESC2 *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dst);

    END_INTERFACE
} ID3D12Device11Vtbl;
interface ID3D12Device11 { CONST_VTBL ID3D12Device11Vtbl *lpVtbl; };
#endif

#ifndef __ID3D12Device12_INTERFACE_DEFINED__
#define __ID3D12Device12_INTERFACE_DEFINED__
typedef interface ID3D12Device12 ID3D12Device12;
DEFINE_GUID(IID_ID3D12Device12, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5,0x41,0x15,0xa4,0x05,0x39,0x5f,0xc5);
typedef struct ID3D12Device12Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12Device12 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12Device12 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12Device12 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12Device12 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12Device12 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12Device12 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12Device12 *This,
        const WCHAR *name);

    /*** ID3D12Device methods ***/
    UINT (STDMETHODCALLTYPE *GetNodeCount)(
        ID3D12Device12 *This);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue)(
        ID3D12Device12 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid,
        void **command_queue);

    HRESULT (STDMETHODCALLTYPE *CreateCommandAllocator)(
        ID3D12Device12 *This,
        D3D12_COMMAND_LIST_TYPE type,
        REFIID riid,
        void **command_allocator);

    HRESULT (STDMETHODCALLTYPE *CreateGraphicsPipelineState)(
        ID3D12Device12 *This,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineState)(
        ID3D12Device12 *This,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateCommandList)(
        ID3D12Device12 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *command_allocator,
        ID3D12PipelineState *initial_pipeline_state,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(
        ID3D12Device12 *This,
        D3D12_FEATURE feature,
        void *feature_data,
        UINT feature_data_size);

    HRESULT (STDMETHODCALLTYPE *CreateDescriptorHeap)(
        ID3D12Device12 *This,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid,
        void **descriptor_heap);

    UINT (STDMETHODCALLTYPE *GetDescriptorHandleIncrementSize)(
        ID3D12Device12 *This,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateRootSignature)(
        ID3D12Device12 *This,
        UINT node_mask,
        const void *bytecode,
        SIZE_T bytecode_length,
        REFIID riid,
        void **root_signature);

    void (STDMETHODCALLTYPE *CreateConstantBufferView)(
        ID3D12Device12 *This,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateShaderResourceView)(
        ID3D12Device12 *This,
        ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateUnorderedAccessView)(
        ID3D12Device12 *This,
        ID3D12Resource *resource,
        ID3D12Resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateRenderTargetView)(
        ID3D12Device12 *This,
        ID3D12Resource *resource,
        const D3D12_RENDER_TARGET_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateDepthStencilView)(
        ID3D12Device12 *This,
        ID3D12Resource *resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateSampler)(
        ID3D12Device12 *This,
        const D3D12_SAMPLER_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CopyDescriptors)(
        ID3D12Device12 *This,
        UINT dst_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
        const UINT *dst_descriptor_range_sizes,
        UINT src_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
        const UINT *src_descriptor_range_sizes,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    void (STDMETHODCALLTYPE *CopyDescriptorsSimple)(
        ID3D12Device12 *This,
        UINT descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
        const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo)(
        ID3D12Device12 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT reource_desc_count,
        const D3D12_RESOURCE_DESC *resource_descs);

    D3D12_HEAP_PROPERTIES * (STDMETHODCALLTYPE *GetCustomHeapProperties)(
        ID3D12Device12 *This,
        D3D12_HEAP_PROPERTIES *__ret,
        UINT node_mask,
        D3D12_HEAP_TYPE heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource)(
        ID3D12Device12 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap)(
        ID3D12Device12 *This,
        const D3D12_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource)(
        ID3D12Device12 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource)(
        ID3D12Device12 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateSharedHandle)(
        ID3D12Device12 *This,
        ID3D12DeviceChild *object,
        const SECURITY_ATTRIBUTES *attributes,
        DWORD access,
        const WCHAR *name,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandle)(
        ID3D12Device12 *This,
        HANDLE handle,
        REFIID riid,
        void **object);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandleByName)(
        ID3D12Device12 *This,
        const WCHAR *name,
        DWORD access,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *MakeResident)(
        ID3D12Device12 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *Evict)(
        ID3D12Device12 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *CreateFence)(
        ID3D12Device12 *This,
        UINT64 initial_value,
        D3D12_FENCE_FLAGS flags,
        REFIID riid,
        void **fence);

    HRESULT (STDMETHODCALLTYPE *GetDeviceRemovedReason)(
        ID3D12Device12 *This);

    void (STDMETHODCALLTYPE *GetCopyableFootprints)(
        ID3D12Device12 *This,
        const D3D12_RESOURCE_DESC *desc,
        UINT first_sub_resource,
        UINT sub_resource_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_count,
        UINT64 *row_size,
        UINT64 *total_bytes);

    HRESULT (STDMETHODCALLTYPE *CreateQueryHeap)(
        ID3D12Device12 *This,
        const D3D12_QUERY_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *SetStablePowerState)(
        ID3D12Device12 *This,
        WINBOOL enable);

    HRESULT (STDMETHODCALLTYPE *CreateCommandSignature)(
        ID3D12Device12 *This,
        const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *root_signature,
        REFIID riid,
        void **command_signature);

    void (STDMETHODCALLTYPE *GetResourceTiling)(
        ID3D12Device12 *This,
        ID3D12Resource *resource,
        UINT *total_tile_count,
        D3D12_PACKED_MIP_INFO *packed_mip_info,
        D3D12_TILE_SHAPE *standard_tile_shape,
        UINT *sub_resource_tiling_count,
        UINT first_sub_resource_tiling,
        D3D12_SUBRESOURCE_TILING *sub_resource_tilings);

    LUID * (STDMETHODCALLTYPE *GetAdapterLuid)(
        ID3D12Device12 *This,
        LUID *__ret);

    /*** ID3D12Device1 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineLibrary)(
        ID3D12Device12 *This,
        const void *blob,
        SIZE_T blob_size,
        REFIID iid,
        void **lib);

    HRESULT (STDMETHODCALLTYPE *SetEventOnMultipleFenceCompletion)(
        ID3D12Device12 *This,
        ID3D12Fence *const *fences,
        const UINT64 *values,
        UINT fence_count,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
        HANDLE event);

    HRESULT (STDMETHODCALLTYPE *SetResidencyPriority)(
        ID3D12Device12 *This,
        UINT object_count,
        ID3D12Pageable *const *objects,
        const D3D12_RESIDENCY_PRIORITY *priorities);

    /*** ID3D12Device2 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineState)(
        ID3D12Device12 *This,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    /*** ID3D12Device3 methods ***/
    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromAddress)(
        ID3D12Device12 *This,
        const void *address,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromFileMapping)(
        ID3D12Device12 *This,
        HANDLE file_mapping,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *EnqueueMakeResident)(
        ID3D12Device12 *This,
        D3D12_RESIDENCY_FLAGS flags,
        UINT num_objects,
        ID3D12Pageable *const *objects,
        ID3D12Fence *fence,
        UINT64 fence_value);

    /*** ID3D12Device4 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommandList1)(
        ID3D12Device12 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession)(
        ID3D12Device12 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource1)(
        ID3D12Device12 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap1)(
        ID3D12Device12 *This,
        const D3D12_HEAP_DESC *desc,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource1)(
        ID3D12Device12 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **resource);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo1)(
        ID3D12Device12 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    /*** ID3D12Device5 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateLifetimeTracker)(
        ID3D12Device12 *This,
        ID3D12LifetimeOwner *owner,
        REFIID riid,
        void **tracker);

    void (STDMETHODCALLTYPE *RemoveDevice)(
        ID3D12Device12 *This);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommands)(
        ID3D12Device12 *This,
        UINT *meta_commands_count,
        D3D12_META_COMMAND_DESC *descs);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommandParameters)(
        ID3D12Device12 *This,
        REFGUID command_id,
        D3D12_META_COMMAND_PARAMETER_STAGE stage,
        UINT *total_structure_size_in_bytes,
        UINT *parameter_count,
        D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs);

    HRESULT (STDMETHODCALLTYPE *CreateMetaCommand)(
        ID3D12Device12 *This,
        REFGUID command_id,
        UINT node_mask,
        const void *creation_parameters_data,
        SIZE_T creation_parameters_data_size_in_bytes,
        REFIID riid,
        void **meta_command);

    HRESULT (STDMETHODCALLTYPE *CreateStateObject)(
        ID3D12Device12 *This,
        const D3D12_STATE_OBJECT_DESC *desc,
        REFIID riid,
        void **state_object);

    void (STDMETHODCALLTYPE *GetRaytracingAccelerationStructurePrebuildInfo)(
        ID3D12Device12 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info);

    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS (STDMETHODCALLTYPE *CheckDriverMatchingIdentifier)(
        ID3D12Device12 *This,
        D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check);

    /*** ID3D12Device6 methods ***/
    HRESULT (STDMETHODCALLTYPE *SetBackgroundProcessingMode)(
        ID3D12Device12 *This,
        D3D12_BACKGROUND_PROCESSING_MODE mode,
        D3D12_MEASUREMENTS_ACTION action,
        HANDLE event,
        WINBOOL *further_measurements_desired);

    /*** ID3D12Device7 methods ***/
    HRESULT (STDMETHODCALLTYPE *AddToStateObject)(
        ID3D12Device12 *This,
        const D3D12_STATE_OBJECT_DESC *addition,
        ID3D12StateObject *state_object_to_grow_from,
        REFIID riid,
        void **new_state_object);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession1)(
        ID3D12Device12 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
        REFIID riid,
        void **session);

    /*** ID3D12Device8 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo2)(
        ID3D12Device12 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource2)(
        ID3D12Device12 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource1)(
        ID3D12Device12 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    void (STDMETHODCALLTYPE *CreateSamplerFeedbackUnorderedAccessView)(
        ID3D12Device12 *This,
        ID3D12Resource *targeted_resource,
        ID3D12Resource *feedback_resource,
        D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor);

    void (STDMETHODCALLTYPE *GetCopyableFootprints1)(
        ID3D12Device12 *This,
        const D3D12_RESOURCE_DESC1 *resource_desc,
        UINT first_subresource,
        UINT subresources_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *rows_count,
        UINT64 *row_size_in_bytes,
        UINT64 *total_bytes);

    /*** ID3D12Device9 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateShaderCacheSession)(
        ID3D12Device12 *This,
        const D3D12_SHADER_CACHE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *ShaderCacheControl)(
        ID3D12Device12 *This,
        D3D12_SHADER_CACHE_KIND_FLAGS kinds,
        D3D12_SHADER_CACHE_CONTROL_FLAGS control);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue1)(
        ID3D12Device12 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID creator_id,
        REFIID riid,
        void **command_queue);

    /*** ID3D12Device10 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource3)(
        ID3D12Device12 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource2)(
        ID3D12Device12 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource2)(
        ID3D12Device12 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    /*** ID3D12Device11 methods ***/
    void (STDMETHODCALLTYPE *CreateSampler2)(
        ID3D12Device12 *This,
        const D3D12_SAMPLER_DESC2 *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dst);

    /*** ID3D12Device12 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo3)(
        ID3D12Device12 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        const UINT32 *castable_formats_counts,
        const DXGI_FORMAT *const *castable_formats,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    END_INTERFACE
} ID3D12Device12Vtbl;
interface ID3D12Device12 { CONST_VTBL ID3D12Device12Vtbl *lpVtbl; };
#endif

#ifndef __ID3D12Device13_INTERFACE_DEFINED__
#define __ID3D12Device13_INTERFACE_DEFINED__
typedef interface ID3D12Device13 ID3D12Device13;
DEFINE_GUID(IID_ID3D12Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1,0x18,0x5c,0x81,0x6f,0x45,0x69,0x5e);
typedef struct ID3D12Device13Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12Device13 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12Device13 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12Device13 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12Device13 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12Device13 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12Device13 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12Device13 *This,
        const WCHAR *name);

    /*** ID3D12Device methods ***/
    UINT (STDMETHODCALLTYPE *GetNodeCount)(
        ID3D12Device13 *This);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue)(
        ID3D12Device13 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid,
        void **command_queue);

    HRESULT (STDMETHODCALLTYPE *CreateCommandAllocator)(
        ID3D12Device13 *This,
        D3D12_COMMAND_LIST_TYPE type,
        REFIID riid,
        void **command_allocator);

    HRESULT (STDMETHODCALLTYPE *CreateGraphicsPipelineState)(
        ID3D12Device13 *This,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineState)(
        ID3D12Device13 *This,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateCommandList)(
        ID3D12Device13 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *command_allocator,
        ID3D12PipelineState *initial_pipeline_state,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(
        ID3D12Device13 *This,
        D3D12_FEATURE feature,
        void *feature_data,
        UINT feature_data_size);

    HRESULT (STDMETHODCALLTYPE *CreateDescriptorHeap)(
        ID3D12Device13 *This,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid,
        void **descriptor_heap);

    UINT (STDMETHODCALLTYPE *GetDescriptorHandleIncrementSize)(
        ID3D12Device13 *This,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateRootSignature)(
        ID3D12Device13 *This,
        UINT node_mask,
        const void *bytecode,
        SIZE_T bytecode_length,
        REFIID riid,
        void **root_signature);

    void (STDMETHODCALLTYPE *CreateConstantBufferView)(
        ID3D12Device13 *This,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateShaderResourceView)(
        ID3D12Device13 *This,
        ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateUnorderedAccessView)(
        ID3D12Device13 *This,
        ID3D12Resource *resource,
        ID3D12Resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateRenderTargetView)(
        ID3D12Device13 *This,
        ID3D12Resource *resource,
        const D3D12_RENDER_TARGET_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateDepthStencilView)(
        ID3D12Device13 *This,
        ID3D12Resource *resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateSampler)(
        ID3D12Device13 *This,
        const D3D12_SAMPLER_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CopyDescriptors)(
        ID3D12Device13 *This,
        UINT dst_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
        const UINT *dst_descriptor_range_sizes,
        UINT src_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
        const UINT *src_descriptor_range_sizes,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    void (STDMETHODCALLTYPE *CopyDescriptorsSimple)(
        ID3D12Device13 *This,
        UINT descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
        const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo)(
        ID3D12Device13 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT reource_desc_count,
        const D3D12_RESOURCE_DESC *resource_descs);

    D3D12_HEAP_PROPERTIES * (STDMETHODCALLTYPE *GetCustomHeapProperties)(
        ID3D12Device13 *This,
        D3D12_HEAP_PROPERTIES *__ret,
        UINT node_mask,
        D3D12_HEAP_TYPE heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource)(
        ID3D12Device13 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap)(
        ID3D12Device13 *This,
        const D3D12_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource)(
        ID3D12Device13 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource)(
        ID3D12Device13 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateSharedHandle)(
        ID3D12Device13 *This,
        ID3D12DeviceChild *object,
        const SECURITY_ATTRIBUTES *attributes,
        DWORD access,
        const WCHAR *name,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandle)(
        ID3D12Device13 *This,
        HANDLE handle,
        REFIID riid,
        void **object);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandleByName)(
        ID3D12Device13 *This,
        const WCHAR *name,
        DWORD access,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *MakeResident)(
        ID3D12Device13 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *Evict)(
        ID3D12Device13 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *CreateFence)(
        ID3D12Device13 *This,
        UINT64 initial_value,
        D3D12_FENCE_FLAGS flags,
        REFIID riid,
        void **fence);

    HRESULT (STDMETHODCALLTYPE *GetDeviceRemovedReason)(
        ID3D12Device13 *This);

    void (STDMETHODCALLTYPE *GetCopyableFootprints)(
        ID3D12Device13 *This,
        const D3D12_RESOURCE_DESC *desc,
        UINT first_sub_resource,
        UINT sub_resource_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_count,
        UINT64 *row_size,
        UINT64 *total_bytes);

    HRESULT (STDMETHODCALLTYPE *CreateQueryHeap)(
        ID3D12Device13 *This,
        const D3D12_QUERY_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *SetStablePowerState)(
        ID3D12Device13 *This,
        WINBOOL enable);

    HRESULT (STDMETHODCALLTYPE *CreateCommandSignature)(
        ID3D12Device13 *This,
        const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *root_signature,
        REFIID riid,
        void **command_signature);

    void (STDMETHODCALLTYPE *GetResourceTiling)(
        ID3D12Device13 *This,
        ID3D12Resource *resource,
        UINT *total_tile_count,
        D3D12_PACKED_MIP_INFO *packed_mip_info,
        D3D12_TILE_SHAPE *standard_tile_shape,
        UINT *sub_resource_tiling_count,
        UINT first_sub_resource_tiling,
        D3D12_SUBRESOURCE_TILING *sub_resource_tilings);

    LUID * (STDMETHODCALLTYPE *GetAdapterLuid)(
        ID3D12Device13 *This,
        LUID *__ret);

    /*** ID3D12Device1 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineLibrary)(
        ID3D12Device13 *This,
        const void *blob,
        SIZE_T blob_size,
        REFIID iid,
        void **lib);

    HRESULT (STDMETHODCALLTYPE *SetEventOnMultipleFenceCompletion)(
        ID3D12Device13 *This,
        ID3D12Fence *const *fences,
        const UINT64 *values,
        UINT fence_count,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
        HANDLE event);

    HRESULT (STDMETHODCALLTYPE *SetResidencyPriority)(
        ID3D12Device13 *This,
        UINT object_count,
        ID3D12Pageable *const *objects,
        const D3D12_RESIDENCY_PRIORITY *priorities);

    /*** ID3D12Device2 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineState)(
        ID3D12Device13 *This,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    /*** ID3D12Device3 methods ***/
    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromAddress)(
        ID3D12Device13 *This,
        const void *address,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromFileMapping)(
        ID3D12Device13 *This,
        HANDLE file_mapping,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *EnqueueMakeResident)(
        ID3D12Device13 *This,
        D3D12_RESIDENCY_FLAGS flags,
        UINT num_objects,
        ID3D12Pageable *const *objects,
        ID3D12Fence *fence,
        UINT64 fence_value);

    /*** ID3D12Device4 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommandList1)(
        ID3D12Device13 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession)(
        ID3D12Device13 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource1)(
        ID3D12Device13 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap1)(
        ID3D12Device13 *This,
        const D3D12_HEAP_DESC *desc,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource1)(
        ID3D12Device13 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **resource);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo1)(
        ID3D12Device13 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    /*** ID3D12Device5 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateLifetimeTracker)(
        ID3D12Device13 *This,
        ID3D12LifetimeOwner *owner,
        REFIID riid,
        void **tracker);

    void (STDMETHODCALLTYPE *RemoveDevice)(
        ID3D12Device13 *This);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommands)(
        ID3D12Device13 *This,
        UINT *meta_commands_count,
        D3D12_META_COMMAND_DESC *descs);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommandParameters)(
        ID3D12Device13 *This,
        REFGUID command_id,
        D3D12_META_COMMAND_PARAMETER_STAGE stage,
        UINT *total_structure_size_in_bytes,
        UINT *parameter_count,
        D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs);

    HRESULT (STDMETHODCALLTYPE *CreateMetaCommand)(
        ID3D12Device13 *This,
        REFGUID command_id,
        UINT node_mask,
        const void *creation_parameters_data,
        SIZE_T creation_parameters_data_size_in_bytes,
        REFIID riid,
        void **meta_command);

    HRESULT (STDMETHODCALLTYPE *CreateStateObject)(
        ID3D12Device13 *This,
        const D3D12_STATE_OBJECT_DESC *desc,
        REFIID riid,
        void **state_object);

    void (STDMETHODCALLTYPE *GetRaytracingAccelerationStructurePrebuildInfo)(
        ID3D12Device13 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info);

    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS (STDMETHODCALLTYPE *CheckDriverMatchingIdentifier)(
        ID3D12Device13 *This,
        D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check);

    /*** ID3D12Device6 methods ***/
    HRESULT (STDMETHODCALLTYPE *SetBackgroundProcessingMode)(
        ID3D12Device13 *This,
        D3D12_BACKGROUND_PROCESSING_MODE mode,
        D3D12_MEASUREMENTS_ACTION action,
        HANDLE event,
        WINBOOL *further_measurements_desired);

    /*** ID3D12Device7 methods ***/
    HRESULT (STDMETHODCALLTYPE *AddToStateObject)(
        ID3D12Device13 *This,
        const D3D12_STATE_OBJECT_DESC *addition,
        ID3D12StateObject *state_object_to_grow_from,
        REFIID riid,
        void **new_state_object);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession1)(
        ID3D12Device13 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
        REFIID riid,
        void **session);

    /*** ID3D12Device8 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo2)(
        ID3D12Device13 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource2)(
        ID3D12Device13 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource1)(
        ID3D12Device13 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    void (STDMETHODCALLTYPE *CreateSamplerFeedbackUnorderedAccessView)(
        ID3D12Device13 *This,
        ID3D12Resource *targeted_resource,
        ID3D12Resource *feedback_resource,
        D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor);

    void (STDMETHODCALLTYPE *GetCopyableFootprints1)(
        ID3D12Device13 *This,
        const D3D12_RESOURCE_DESC1 *resource_desc,
        UINT first_subresource,
        UINT subresources_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *rows_count,
        UINT64 *row_size_in_bytes,
        UINT64 *total_bytes);

    /*** ID3D12Device9 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateShaderCacheSession)(
        ID3D12Device13 *This,
        const D3D12_SHADER_CACHE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *ShaderCacheControl)(
        ID3D12Device13 *This,
        D3D12_SHADER_CACHE_KIND_FLAGS kinds,
        D3D12_SHADER_CACHE_CONTROL_FLAGS control);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue1)(
        ID3D12Device13 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID creator_id,
        REFIID riid,
        void **command_queue);

    /*** ID3D12Device10 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource3)(
        ID3D12Device13 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource2)(
        ID3D12Device13 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource2)(
        ID3D12Device13 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    /*** ID3D12Device11 methods ***/
    void (STDMETHODCALLTYPE *CreateSampler2)(
        ID3D12Device13 *This,
        const D3D12_SAMPLER_DESC2 *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dst);

    /*** ID3D12Device12 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo3)(
        ID3D12Device13 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        const UINT32 *castable_formats_counts,
        const DXGI_FORMAT *const *castable_formats,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    /*** ID3D12Device13 methods ***/
    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromAddress1)(
        ID3D12Device13 *This,
        const void *address,
        SIZE_T size,
        REFIID riid,
        void **heap);

    END_INTERFACE
} ID3D12Device13Vtbl;
interface ID3D12Device13 { CONST_VTBL ID3D12Device13Vtbl *lpVtbl; };
#endif

#ifndef __ID3D12Device14_INTERFACE_DEFINED__
#define __ID3D12Device14_INTERFACE_DEFINED__
typedef interface ID3D12Device14 ID3D12Device14;
DEFINE_GUID(IID_ID3D12Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e,0x4a,0x88,0xad,0x49,0x26,0xd3,0x23);
typedef struct ID3D12Device14Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12Device14 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12Device14 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12Device14 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12Device14 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12Device14 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12Device14 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12Device14 *This,
        const WCHAR *name);

    /*** ID3D12Device methods ***/
    UINT (STDMETHODCALLTYPE *GetNodeCount)(
        ID3D12Device14 *This);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue)(
        ID3D12Device14 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid,
        void **command_queue);

    HRESULT (STDMETHODCALLTYPE *CreateCommandAllocator)(
        ID3D12Device14 *This,
        D3D12_COMMAND_LIST_TYPE type,
        REFIID riid,
        void **command_allocator);

    HRESULT (STDMETHODCALLTYPE *CreateGraphicsPipelineState)(
        ID3D12Device14 *This,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineState)(
        ID3D12Device14 *This,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    HRESULT (STDMETHODCALLTYPE *CreateCommandList)(
        ID3D12Device14 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *command_allocator,
        ID3D12PipelineState *initial_pipeline_state,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(
        ID3D12Device14 *This,
        D3D12_FEATURE feature,
        void *feature_data,
        UINT feature_data_size);

    HRESULT (STDMETHODCALLTYPE *CreateDescriptorHeap)(
        ID3D12Device14 *This,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid,
        void **descriptor_heap);

    UINT (STDMETHODCALLTYPE *GetDescriptorHandleIncrementSize)(
        ID3D12Device14 *This,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateRootSignature)(
        ID3D12Device14 *This,
        UINT node_mask,
        const void *bytecode,
        SIZE_T bytecode_length,
        REFIID riid,
        void **root_signature);

    void (STDMETHODCALLTYPE *CreateConstantBufferView)(
        ID3D12Device14 *This,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateShaderResourceView)(
        ID3D12Device14 *This,
        ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateUnorderedAccessView)(
        ID3D12Device14 *This,
        ID3D12Resource *resource,
        ID3D12Resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateRenderTargetView)(
        ID3D12Device14 *This,
        ID3D12Resource *resource,
        const D3D12_RENDER_TARGET_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateDepthStencilView)(
        ID3D12Device14 *This,
        ID3D12Resource *resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CreateSampler)(
        ID3D12Device14 *This,
        const D3D12_SAMPLER_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

    void (STDMETHODCALLTYPE *CopyDescriptors)(
        ID3D12Device14 *This,
        UINT dst_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
        const UINT *dst_descriptor_range_sizes,
        UINT src_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
        const UINT *src_descriptor_range_sizes,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    void (STDMETHODCALLTYPE *CopyDescriptorsSimple)(
        ID3D12Device14 *This,
        UINT descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
        const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo)(
        ID3D12Device14 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT reource_desc_count,
        const D3D12_RESOURCE_DESC *resource_descs);

    D3D12_HEAP_PROPERTIES * (STDMETHODCALLTYPE *GetCustomHeapProperties)(
        ID3D12Device14 *This,
        D3D12_HEAP_PROPERTIES *__ret,
        UINT node_mask,
        D3D12_HEAP_TYPE heap_type);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource)(
        ID3D12Device14 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap)(
        ID3D12Device14 *This,
        const D3D12_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource)(
        ID3D12Device14 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource)(
        ID3D12Device14 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateSharedHandle)(
        ID3D12Device14 *This,
        ID3D12DeviceChild *object,
        const SECURITY_ATTRIBUTES *attributes,
        DWORD access,
        const WCHAR *name,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandle)(
        ID3D12Device14 *This,
        HANDLE handle,
        REFIID riid,
        void **object);

    HRESULT (STDMETHODCALLTYPE *OpenSharedHandleByName)(
        ID3D12Device14 *This,
        const WCHAR *name,
        DWORD access,
        HANDLE *handle);

    HRESULT (STDMETHODCALLTYPE *MakeResident)(
        ID3D12Device14 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *Evict)(
        ID3D12Device14 *This,
        UINT object_count,
        ID3D12Pageable *const *objects);

    HRESULT (STDMETHODCALLTYPE *CreateFence)(
        ID3D12Device14 *This,
        UINT64 initial_value,
        D3D12_FENCE_FLAGS flags,
        REFIID riid,
        void **fence);

    HRESULT (STDMETHODCALLTYPE *GetDeviceRemovedReason)(
        ID3D12Device14 *This);

    void (STDMETHODCALLTYPE *GetCopyableFootprints)(
        ID3D12Device14 *This,
        const D3D12_RESOURCE_DESC *desc,
        UINT first_sub_resource,
        UINT sub_resource_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_count,
        UINT64 *row_size,
        UINT64 *total_bytes);

    HRESULT (STDMETHODCALLTYPE *CreateQueryHeap)(
        ID3D12Device14 *This,
        const D3D12_QUERY_HEAP_DESC *desc,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *SetStablePowerState)(
        ID3D12Device14 *This,
        WINBOOL enable);

    HRESULT (STDMETHODCALLTYPE *CreateCommandSignature)(
        ID3D12Device14 *This,
        const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *root_signature,
        REFIID riid,
        void **command_signature);

    void (STDMETHODCALLTYPE *GetResourceTiling)(
        ID3D12Device14 *This,
        ID3D12Resource *resource,
        UINT *total_tile_count,
        D3D12_PACKED_MIP_INFO *packed_mip_info,
        D3D12_TILE_SHAPE *standard_tile_shape,
        UINT *sub_resource_tiling_count,
        UINT first_sub_resource_tiling,
        D3D12_SUBRESOURCE_TILING *sub_resource_tilings);

    LUID * (STDMETHODCALLTYPE *GetAdapterLuid)(
        ID3D12Device14 *This,
        LUID *__ret);

    /*** ID3D12Device1 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineLibrary)(
        ID3D12Device14 *This,
        const void *blob,
        SIZE_T blob_size,
        REFIID iid,
        void **lib);

    HRESULT (STDMETHODCALLTYPE *SetEventOnMultipleFenceCompletion)(
        ID3D12Device14 *This,
        ID3D12Fence *const *fences,
        const UINT64 *values,
        UINT fence_count,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
        HANDLE event);

    HRESULT (STDMETHODCALLTYPE *SetResidencyPriority)(
        ID3D12Device14 *This,
        UINT object_count,
        ID3D12Pageable *const *objects,
        const D3D12_RESIDENCY_PRIORITY *priorities);

    /*** ID3D12Device2 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreatePipelineState)(
        ID3D12Device14 *This,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        REFIID riid,
        void **pipeline_state);

    /*** ID3D12Device3 methods ***/
    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromAddress)(
        ID3D12Device14 *This,
        const void *address,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromFileMapping)(
        ID3D12Device14 *This,
        HANDLE file_mapping,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *EnqueueMakeResident)(
        ID3D12Device14 *This,
        D3D12_RESIDENCY_FLAGS flags,
        UINT num_objects,
        ID3D12Pageable *const *objects,
        ID3D12Fence *fence,
        UINT64 fence_value);

    /*** ID3D12Device4 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommandList1)(
        ID3D12Device14 *This,
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid,
        void **command_list);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession)(
        ID3D12Device14 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource1)(
        ID3D12Device14 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateHeap1)(
        ID3D12Device14 *This,
        const D3D12_HEAP_DESC *desc,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **heap);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource1)(
        ID3D12Device14 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid,
        void **resource);

    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo1)(
        ID3D12Device14 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    /*** ID3D12Device5 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateLifetimeTracker)(
        ID3D12Device14 *This,
        ID3D12LifetimeOwner *owner,
        REFIID riid,
        void **tracker);

    void (STDMETHODCALLTYPE *RemoveDevice)(
        ID3D12Device14 *This);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommands)(
        ID3D12Device14 *This,
        UINT *meta_commands_count,
        D3D12_META_COMMAND_DESC *descs);

    HRESULT (STDMETHODCALLTYPE *EnumerateMetaCommandParameters)(
        ID3D12Device14 *This,
        REFGUID command_id,
        D3D12_META_COMMAND_PARAMETER_STAGE stage,
        UINT *total_structure_size_in_bytes,
        UINT *parameter_count,
        D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs);

    HRESULT (STDMETHODCALLTYPE *CreateMetaCommand)(
        ID3D12Device14 *This,
        REFGUID command_id,
        UINT node_mask,
        const void *creation_parameters_data,
        SIZE_T creation_parameters_data_size_in_bytes,
        REFIID riid,
        void **meta_command);

    HRESULT (STDMETHODCALLTYPE *CreateStateObject)(
        ID3D12Device14 *This,
        const D3D12_STATE_OBJECT_DESC *desc,
        REFIID riid,
        void **state_object);

    void (STDMETHODCALLTYPE *GetRaytracingAccelerationStructurePrebuildInfo)(
        ID3D12Device14 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info);

    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS (STDMETHODCALLTYPE *CheckDriverMatchingIdentifier)(
        ID3D12Device14 *This,
        D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check);

    /*** ID3D12Device6 methods ***/
    HRESULT (STDMETHODCALLTYPE *SetBackgroundProcessingMode)(
        ID3D12Device14 *This,
        D3D12_BACKGROUND_PROCESSING_MODE mode,
        D3D12_MEASUREMENTS_ACTION action,
        HANDLE event,
        WINBOOL *further_measurements_desired);

    /*** ID3D12Device7 methods ***/
    HRESULT (STDMETHODCALLTYPE *AddToStateObject)(
        ID3D12Device14 *This,
        const D3D12_STATE_OBJECT_DESC *addition,
        ID3D12StateObject *state_object_to_grow_from,
        REFIID riid,
        void **new_state_object);

    HRESULT (STDMETHODCALLTYPE *CreateProtectedResourceSession1)(
        ID3D12Device14 *This,
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
        REFIID riid,
        void **session);

    /*** ID3D12Device8 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo2)(
        ID3D12Device14 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource2)(
        ID3D12Device14 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_resource_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource1)(
        ID3D12Device14 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource);

    void (STDMETHODCALLTYPE *CreateSamplerFeedbackUnorderedAccessView)(
        ID3D12Device14 *This,
        ID3D12Resource *targeted_resource,
        ID3D12Resource *feedback_resource,
        D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor);

    void (STDMETHODCALLTYPE *GetCopyableFootprints1)(
        ID3D12Device14 *This,
        const D3D12_RESOURCE_DESC1 *resource_desc,
        UINT first_subresource,
        UINT subresources_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *rows_count,
        UINT64 *row_size_in_bytes,
        UINT64 *total_bytes);

    /*** ID3D12Device9 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateShaderCacheSession)(
        ID3D12Device14 *This,
        const D3D12_SHADER_CACHE_SESSION_DESC *desc,
        REFIID riid,
        void **session);

    HRESULT (STDMETHODCALLTYPE *ShaderCacheControl)(
        ID3D12Device14 *This,
        D3D12_SHADER_CACHE_KIND_FLAGS kinds,
        D3D12_SHADER_CACHE_CONTROL_FLAGS control);

    HRESULT (STDMETHODCALLTYPE *CreateCommandQueue1)(
        ID3D12Device14 *This,
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID creator_id,
        REFIID riid,
        void **command_queue);

    /*** ID3D12Device10 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateCommittedResource3)(
        ID3D12Device14 *This,
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid_resource,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreatePlacedResource2)(
        ID3D12Device14 *This,
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC1 *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    HRESULT (STDMETHODCALLTYPE *CreateReservedResource2)(
        ID3D12Device14 *This,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_BARRIER_LAYOUT initial_layout,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        ID3D12ProtectedResourceSession *protected_session,
        UINT32 castable_formats_count,
        DXGI_FORMAT *castable_formats,
        REFIID riid,
        void **resource);

    /*** ID3D12Device11 methods ***/
    void (STDMETHODCALLTYPE *CreateSampler2)(
        ID3D12Device14 *This,
        const D3D12_SAMPLER_DESC2 *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE dst);

    /*** ID3D12Device12 methods ***/
    D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo3)(
        ID3D12Device14 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT resource_descs_count,
        const D3D12_RESOURCE_DESC1 *resource_descs,
        const UINT32 *castable_formats_counts,
        const DXGI_FORMAT *const *castable_formats,
        D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1);

    /*** ID3D12Device13 methods ***/
    HRESULT (STDMETHODCALLTYPE *OpenExistingHeapFromAddress1)(
        ID3D12Device14 *This,
        const void *address,
        SIZE_T size,
        REFIID riid,
        void **heap);

    /*** ID3D12Device14 methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateRootSignatureFromSubobjectInLibrary)(
        ID3D12Device14 *This,
        UINT node_mask,
        const void *library_blob,
        SIZE_T blob_length,
        LPCWSTR subobject_name,
        REFIID riid,
        void **root_signature);

    END_INTERFACE
} ID3D12Device14Vtbl;
interface ID3D12Device14 { CONST_VTBL ID3D12Device14Vtbl *lpVtbl; };
#endif

#ifndef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
typedef interface ID3D12GraphicsCommandList8 ID3D12GraphicsCommandList8;
DEFINE_GUID(IID_ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93,0x8e,0x23,0xc4,0xad,0x05,0xce,0x51);
typedef struct ID3D12GraphicsCommandList8Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12GraphicsCommandList8 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12GraphicsCommandList8 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12GraphicsCommandList8 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12GraphicsCommandList8 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12GraphicsCommandList8 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12GraphicsCommandList8 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12GraphicsCommandList8 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12GraphicsCommandList8 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12GraphicsCommandList8 *This);

    /*** ID3D12GraphicsCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12GraphicsCommandList8 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12CommandAllocator *allocator,
        ID3D12PipelineState *initial_state);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12PipelineState *pipeline_state);

    void (STDMETHODCALLTYPE *DrawInstanced)(
        ID3D12GraphicsCommandList8 *This,
        UINT vertex_count_per_instance,
        UINT instance_count,
        UINT start_vertex_location,
        UINT start_instance_location);

    void (STDMETHODCALLTYPE *DrawIndexedInstanced)(
        ID3D12GraphicsCommandList8 *This,
        UINT index_count_per_instance,
        UINT instance_count,
        UINT start_vertex_location,
        INT base_vertex_location,
        UINT start_instance_location);

    void (STDMETHODCALLTYPE *Dispatch)(
        ID3D12GraphicsCommandList8 *This,
        UINT x,
        UINT u,
        UINT z);

    void (STDMETHODCALLTYPE *CopyBufferRegion)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT64 byte_count);

    void (STDMETHODCALLTYPE *CopyTextureRegion)(
        ID3D12GraphicsCommandList8 *This,
        const D3D12_TEXTURE_COPY_LOCATION *dst,
        UINT dst_x,
        UINT dst_y,
        UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION *src,
        const D3D12_BOX *src_box);

    void (STDMETHODCALLTYPE *CopyResource)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *dst_resource,
        ID3D12Resource *src_resource);

    void (STDMETHODCALLTYPE *CopyTiles)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *tiled_resource,
        const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
        const D3D12_TILE_REGION_SIZE *tile_region_size,
        ID3D12Resource *buffer,
        UINT64 buffer_offset,
        D3D12_TILE_COPY_FLAGS flags);

    void (STDMETHODCALLTYPE *ResolveSubresource)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *dst_resource,
        UINT dst_sub_resource,
        ID3D12Resource *src_resource,
        UINT src_sub_resource,
        DXGI_FORMAT format);

    void (STDMETHODCALLTYPE *IASetPrimitiveTopology)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

    void (STDMETHODCALLTYPE *RSSetViewports)(
        ID3D12GraphicsCommandList8 *This,
        UINT viewport_count,
        const D3D12_VIEWPORT *viewports);

    void (STDMETHODCALLTYPE *RSSetScissorRects)(
        ID3D12GraphicsCommandList8 *This,
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *OMSetBlendFactor)(
        ID3D12GraphicsCommandList8 *This,
        const FLOAT blend_factor[4]);

    void (STDMETHODCALLTYPE *OMSetStencilRef)(
        ID3D12GraphicsCommandList8 *This,
        UINT stencil_ref);

    void (STDMETHODCALLTYPE *SetPipelineState)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12PipelineState *pipeline_state);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12GraphicsCommandList8 *This,
        UINT barrier_count,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *ExecuteBundle)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12GraphicsCommandList *command_list);

    void (STDMETHODCALLTYPE *SetDescriptorHeaps)(
        ID3D12GraphicsCommandList8 *This,
        UINT heap_count,
        ID3D12DescriptorHeap *const *heaps);

    void (STDMETHODCALLTYPE *SetComputeRootSignature)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12RootSignature *root_signature);

    void (STDMETHODCALLTYPE *SetGraphicsRootSignature)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12RootSignature *root_signature);

    void (STDMETHODCALLTYPE *SetComputeRootDescriptorTable)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

    void (STDMETHODCALLTYPE *SetGraphicsRootDescriptorTable)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

    void (STDMETHODCALLTYPE *SetComputeRoot32BitConstant)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        UINT data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetGraphicsRoot32BitConstant)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        UINT data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetComputeRoot32BitConstants)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        UINT constant_count,
        const void *data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetGraphicsRoot32BitConstants)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        UINT constant_count,
        const void *data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetComputeRootConstantBufferView)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootConstantBufferView)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetComputeRootShaderResourceView)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootShaderResourceView)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetComputeRootUnorderedAccessView)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootUnorderedAccessView)(
        ID3D12GraphicsCommandList8 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *IASetIndexBuffer)(
        ID3D12GraphicsCommandList8 *This,
        const D3D12_INDEX_BUFFER_VIEW *view);

    void (STDMETHODCALLTYPE *IASetVertexBuffers)(
        ID3D12GraphicsCommandList8 *This,
        UINT start_slot,
        UINT view_count,
        const D3D12_VERTEX_BUFFER_VIEW *views);

    void (STDMETHODCALLTYPE *SOSetTargets)(
        ID3D12GraphicsCommandList8 *This,
        UINT start_slot,
        UINT view_count,
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views);

    void (STDMETHODCALLTYPE *OMSetRenderTargets)(
        ID3D12GraphicsCommandList8 *This,
        UINT render_target_descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
        WINBOOL single_descriptor_handle,
        const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor);

    void (STDMETHODCALLTYPE *ClearDepthStencilView)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        D3D12_CLEAR_FLAGS flags,
        FLOAT depth,
        UINT8 stencil,
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearRenderTargetView)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        const FLOAT color[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewUint)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        ID3D12Resource *resource,
        const UINT values[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewFloat)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        ID3D12Resource *resource,
        const float values[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT query_count,
        ID3D12Resource *dst_buffer,
        UINT64 aligned_dst_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12GraphicsCommandList8 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12GraphicsCommandList8 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12GraphicsCommandList8 *This);

    void (STDMETHODCALLTYPE *ExecuteIndirect)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12CommandSignature *command_signature,
        UINT max_command_count,
        ID3D12Resource *arg_buffer,
        UINT64 arg_buffer_offset,
        ID3D12Resource *count_buffer,
        UINT64 count_buffer_offset);

    /*** ID3D12GraphicsCommandList1 methods ***/
    void (STDMETHODCALLTYPE *AtomicCopyBufferUINT)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT dependent_resource_count,
        ID3D12Resource *const *dependent_resources,
        const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges);

    void (STDMETHODCALLTYPE *AtomicCopyBufferUINT64)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT dependent_resource_count,
        ID3D12Resource *const *dependent_resources,
        const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges);

    void (STDMETHODCALLTYPE *OMSetDepthBounds)(
        ID3D12GraphicsCommandList8 *This,
        FLOAT min,
        FLOAT max);

    void (STDMETHODCALLTYPE *SetSamplePositions)(
        ID3D12GraphicsCommandList8 *This,
        UINT sample_count,
        UINT pixel_count,
        D3D12_SAMPLE_POSITION *sample_positions);

    void (STDMETHODCALLTYPE *ResolveSubresourceRegion)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *dst_resource,
        UINT dst_sub_resource_idx,
        UINT dst_x,
        UINT dst_y,
        ID3D12Resource *src_resource,
        UINT src_sub_resource_idx,
        D3D12_RECT *src_rect,
        DXGI_FORMAT format,
        D3D12_RESOLVE_MODE mode);

    void (STDMETHODCALLTYPE *SetViewInstanceMask)(
        ID3D12GraphicsCommandList8 *This,
        UINT mask);

    /*** ID3D12GraphicsCommandList2 methods ***/
    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12GraphicsCommandList8 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    /*** ID3D12GraphicsCommandList3 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12ProtectedResourceSession *protected_resource_session);

    /*** ID3D12GraphicsCommandList4 methods ***/
    void (STDMETHODCALLTYPE *BeginRenderPass)(
        ID3D12GraphicsCommandList8 *This,
        UINT render_targets_count,
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
        D3D12_RENDER_PASS_FLAGS flags);

    void (STDMETHODCALLTYPE *EndRenderPass)(
        ID3D12GraphicsCommandList8 *This);

    void (STDMETHODCALLTYPE *InitializeMetaCommand)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12MetaCommand *meta_command,
        const void *initialization_parameters_data,
        SIZE_T initialization_parameters_data_size_in_bytes);

    void (STDMETHODCALLTYPE *ExecuteMetaCommand)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12MetaCommand *meta_command,
        const void *execution_parameters_data,
        SIZE_T execution_parameters_data_size_in_bytes);

    void (STDMETHODCALLTYPE *BuildRaytracingAccelerationStructure)(
        ID3D12GraphicsCommandList8 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
        UINT postbuild_info_descs_count,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs);

    void (STDMETHODCALLTYPE *EmitRaytracingAccelerationStructurePostbuildInfo)(
        ID3D12GraphicsCommandList8 *This,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
        UINT src_acceleration_structures_count,
        const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data);

    void (STDMETHODCALLTYPE *CopyRaytracingAccelerationStructure)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data,
        D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode);

    void (STDMETHODCALLTYPE *SetPipelineState1)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12StateObject *state_object);

    void (STDMETHODCALLTYPE *DispatchRays)(
        ID3D12GraphicsCommandList8 *This,
        const D3D12_DISPATCH_RAYS_DESC *desc);

    /*** ID3D12GraphicsCommandList5 methods ***/
    void (STDMETHODCALLTYPE *RSSetShadingRate)(
        ID3D12GraphicsCommandList8 *This,
        D3D12_SHADING_RATE base_shading_rate,
        const D3D12_SHADING_RATE_COMBINER *combiners);

    void (STDMETHODCALLTYPE *RSSetShadingRateImage)(
        ID3D12GraphicsCommandList8 *This,
        ID3D12Resource *shading_rate_image);

    /*** ID3D12GraphicsCommandList6 methods ***/
    void (STDMETHODCALLTYPE *DispatchMesh)(
        ID3D12GraphicsCommandList8 *This,
        UINT thread_group_count_x,
        UINT thread_group_count_y,
        UINT thread_group_count_z);

    /*** ID3D12GraphicsCommandList7 methods ***/
    void (STDMETHODCALLTYPE *Barrier)(
        ID3D12GraphicsCommandList8 *This,
        UINT32 barrier_groups_count,
        const D3D12_BARRIER_GROUP *barrier_groups);

    /*** ID3D12GraphicsCommandList8 methods ***/
    void (STDMETHODCALLTYPE *OMSetFrontAndBackStencilRef)(
        ID3D12GraphicsCommandList8 *This,
        UINT front_stencil_ref,
        UINT back_stencil_ref);

    END_INTERFACE
} ID3D12GraphicsCommandList8Vtbl;
interface ID3D12GraphicsCommandList8 { CONST_VTBL ID3D12GraphicsCommandList8Vtbl *lpVtbl; };
#endif

#ifndef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
typedef interface ID3D12GraphicsCommandList9 ID3D12GraphicsCommandList9;
DEFINE_GUID(IID_ID3D12GraphicsCommandList9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1,0x1a,0xca,0xbd,0x2b,0x0c,0x59,0xe1);
typedef struct ID3D12GraphicsCommandList9Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12GraphicsCommandList9 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12GraphicsCommandList9 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12GraphicsCommandList9 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12GraphicsCommandList9 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12GraphicsCommandList9 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12GraphicsCommandList9 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12GraphicsCommandList9 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12GraphicsCommandList9 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12GraphicsCommandList9 *This);

    /*** ID3D12GraphicsCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12GraphicsCommandList9 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12CommandAllocator *allocator,
        ID3D12PipelineState *initial_state);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12PipelineState *pipeline_state);

    void (STDMETHODCALLTYPE *DrawInstanced)(
        ID3D12GraphicsCommandList9 *This,
        UINT vertex_count_per_instance,
        UINT instance_count,
        UINT start_vertex_location,
        UINT start_instance_location);

    void (STDMETHODCALLTYPE *DrawIndexedInstanced)(
        ID3D12GraphicsCommandList9 *This,
        UINT index_count_per_instance,
        UINT instance_count,
        UINT start_vertex_location,
        INT base_vertex_location,
        UINT start_instance_location);

    void (STDMETHODCALLTYPE *Dispatch)(
        ID3D12GraphicsCommandList9 *This,
        UINT x,
        UINT u,
        UINT z);

    void (STDMETHODCALLTYPE *CopyBufferRegion)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT64 byte_count);

    void (STDMETHODCALLTYPE *CopyTextureRegion)(
        ID3D12GraphicsCommandList9 *This,
        const D3D12_TEXTURE_COPY_LOCATION *dst,
        UINT dst_x,
        UINT dst_y,
        UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION *src,
        const D3D12_BOX *src_box);

    void (STDMETHODCALLTYPE *CopyResource)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *dst_resource,
        ID3D12Resource *src_resource);

    void (STDMETHODCALLTYPE *CopyTiles)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *tiled_resource,
        const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
        const D3D12_TILE_REGION_SIZE *tile_region_size,
        ID3D12Resource *buffer,
        UINT64 buffer_offset,
        D3D12_TILE_COPY_FLAGS flags);

    void (STDMETHODCALLTYPE *ResolveSubresource)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *dst_resource,
        UINT dst_sub_resource,
        ID3D12Resource *src_resource,
        UINT src_sub_resource,
        DXGI_FORMAT format);

    void (STDMETHODCALLTYPE *IASetPrimitiveTopology)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

    void (STDMETHODCALLTYPE *RSSetViewports)(
        ID3D12GraphicsCommandList9 *This,
        UINT viewport_count,
        const D3D12_VIEWPORT *viewports);

    void (STDMETHODCALLTYPE *RSSetScissorRects)(
        ID3D12GraphicsCommandList9 *This,
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *OMSetBlendFactor)(
        ID3D12GraphicsCommandList9 *This,
        const FLOAT blend_factor[4]);

    void (STDMETHODCALLTYPE *OMSetStencilRef)(
        ID3D12GraphicsCommandList9 *This,
        UINT stencil_ref);

    void (STDMETHODCALLTYPE *SetPipelineState)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12PipelineState *pipeline_state);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12GraphicsCommandList9 *This,
        UINT barrier_count,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *ExecuteBundle)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12GraphicsCommandList *command_list);

    void (STDMETHODCALLTYPE *SetDescriptorHeaps)(
        ID3D12GraphicsCommandList9 *This,
        UINT heap_count,
        ID3D12DescriptorHeap *const *heaps);

    void (STDMETHODCALLTYPE *SetComputeRootSignature)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12RootSignature *root_signature);

    void (STDMETHODCALLTYPE *SetGraphicsRootSignature)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12RootSignature *root_signature);

    void (STDMETHODCALLTYPE *SetComputeRootDescriptorTable)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

    void (STDMETHODCALLTYPE *SetGraphicsRootDescriptorTable)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

    void (STDMETHODCALLTYPE *SetComputeRoot32BitConstant)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        UINT data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetGraphicsRoot32BitConstant)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        UINT data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetComputeRoot32BitConstants)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        UINT constant_count,
        const void *data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetGraphicsRoot32BitConstants)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        UINT constant_count,
        const void *data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetComputeRootConstantBufferView)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootConstantBufferView)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetComputeRootShaderResourceView)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootShaderResourceView)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetComputeRootUnorderedAccessView)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootUnorderedAccessView)(
        ID3D12GraphicsCommandList9 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *IASetIndexBuffer)(
        ID3D12GraphicsCommandList9 *This,
        const D3D12_INDEX_BUFFER_VIEW *view);

    void (STDMETHODCALLTYPE *IASetVertexBuffers)(
        ID3D12GraphicsCommandList9 *This,
        UINT start_slot,
        UINT view_count,
        const D3D12_VERTEX_BUFFER_VIEW *views);

    void (STDMETHODCALLTYPE *SOSetTargets)(
        ID3D12GraphicsCommandList9 *This,
        UINT start_slot,
        UINT view_count,
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views);

    void (STDMETHODCALLTYPE *OMSetRenderTargets)(
        ID3D12GraphicsCommandList9 *This,
        UINT render_target_descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
        WINBOOL single_descriptor_handle,
        const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor);

    void (STDMETHODCALLTYPE *ClearDepthStencilView)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        D3D12_CLEAR_FLAGS flags,
        FLOAT depth,
        UINT8 stencil,
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearRenderTargetView)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        const FLOAT color[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewUint)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        ID3D12Resource *resource,
        const UINT values[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewFloat)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        ID3D12Resource *resource,
        const float values[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT query_count,
        ID3D12Resource *dst_buffer,
        UINT64 aligned_dst_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12GraphicsCommandList9 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12GraphicsCommandList9 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12GraphicsCommandList9 *This);

    void (STDMETHODCALLTYPE *ExecuteIndirect)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12CommandSignature *command_signature,
        UINT max_command_count,
        ID3D12Resource *arg_buffer,
        UINT64 arg_buffer_offset,
        ID3D12Resource *count_buffer,
        UINT64 count_buffer_offset);

    /*** ID3D12GraphicsCommandList1 methods ***/
    void (STDMETHODCALLTYPE *AtomicCopyBufferUINT)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT dependent_resource_count,
        ID3D12Resource *const *dependent_resources,
        const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges);

    void (STDMETHODCALLTYPE *AtomicCopyBufferUINT64)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT dependent_resource_count,
        ID3D12Resource *const *dependent_resources,
        const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges);

    void (STDMETHODCALLTYPE *OMSetDepthBounds)(
        ID3D12GraphicsCommandList9 *This,
        FLOAT min,
        FLOAT max);

    void (STDMETHODCALLTYPE *SetSamplePositions)(
        ID3D12GraphicsCommandList9 *This,
        UINT sample_count,
        UINT pixel_count,
        D3D12_SAMPLE_POSITION *sample_positions);

    void (STDMETHODCALLTYPE *ResolveSubresourceRegion)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *dst_resource,
        UINT dst_sub_resource_idx,
        UINT dst_x,
        UINT dst_y,
        ID3D12Resource *src_resource,
        UINT src_sub_resource_idx,
        D3D12_RECT *src_rect,
        DXGI_FORMAT format,
        D3D12_RESOLVE_MODE mode);

    void (STDMETHODCALLTYPE *SetViewInstanceMask)(
        ID3D12GraphicsCommandList9 *This,
        UINT mask);

    /*** ID3D12GraphicsCommandList2 methods ***/
    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12GraphicsCommandList9 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    /*** ID3D12GraphicsCommandList3 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12ProtectedResourceSession *protected_resource_session);

    /*** ID3D12GraphicsCommandList4 methods ***/
    void (STDMETHODCALLTYPE *BeginRenderPass)(
        ID3D12GraphicsCommandList9 *This,
        UINT render_targets_count,
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
        D3D12_RENDER_PASS_FLAGS flags);

    void (STDMETHODCALLTYPE *EndRenderPass)(
        ID3D12GraphicsCommandList9 *This);

    void (STDMETHODCALLTYPE *InitializeMetaCommand)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12MetaCommand *meta_command,
        const void *initialization_parameters_data,
        SIZE_T initialization_parameters_data_size_in_bytes);

    void (STDMETHODCALLTYPE *ExecuteMetaCommand)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12MetaCommand *meta_command,
        const void *execution_parameters_data,
        SIZE_T execution_parameters_data_size_in_bytes);

    void (STDMETHODCALLTYPE *BuildRaytracingAccelerationStructure)(
        ID3D12GraphicsCommandList9 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
        UINT postbuild_info_descs_count,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs);

    void (STDMETHODCALLTYPE *EmitRaytracingAccelerationStructurePostbuildInfo)(
        ID3D12GraphicsCommandList9 *This,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
        UINT src_acceleration_structures_count,
        const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data);

    void (STDMETHODCALLTYPE *CopyRaytracingAccelerationStructure)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data,
        D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode);

    void (STDMETHODCALLTYPE *SetPipelineState1)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12StateObject *state_object);

    void (STDMETHODCALLTYPE *DispatchRays)(
        ID3D12GraphicsCommandList9 *This,
        const D3D12_DISPATCH_RAYS_DESC *desc);

    /*** ID3D12GraphicsCommandList5 methods ***/
    void (STDMETHODCALLTYPE *RSSetShadingRate)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_SHADING_RATE base_shading_rate,
        const D3D12_SHADING_RATE_COMBINER *combiners);

    void (STDMETHODCALLTYPE *RSSetShadingRateImage)(
        ID3D12GraphicsCommandList9 *This,
        ID3D12Resource *shading_rate_image);

    /*** ID3D12GraphicsCommandList6 methods ***/
    void (STDMETHODCALLTYPE *DispatchMesh)(
        ID3D12GraphicsCommandList9 *This,
        UINT thread_group_count_x,
        UINT thread_group_count_y,
        UINT thread_group_count_z);

    /*** ID3D12GraphicsCommandList7 methods ***/
    void (STDMETHODCALLTYPE *Barrier)(
        ID3D12GraphicsCommandList9 *This,
        UINT32 barrier_groups_count,
        const D3D12_BARRIER_GROUP *barrier_groups);

    /*** ID3D12GraphicsCommandList8 methods ***/
    void (STDMETHODCALLTYPE *OMSetFrontAndBackStencilRef)(
        ID3D12GraphicsCommandList9 *This,
        UINT front_stencil_ref,
        UINT back_stencil_ref);

    /*** ID3D12GraphicsCommandList9 methods ***/
    void (STDMETHODCALLTYPE *RSSetDepthBias)(
        ID3D12GraphicsCommandList9 *This,
        FLOAT depth_bias,
        FLOAT depth_bias_clamp,
        FLOAT slope_scaled_depth_bias);
    void (STDMETHODCALLTYPE *IASetIndexBufferStripCutValue)(
        ID3D12GraphicsCommandList9 *This,
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value);

    END_INTERFACE
} ID3D12GraphicsCommandList9Vtbl;
interface ID3D12GraphicsCommandList9 { CONST_VTBL ID3D12GraphicsCommandList9Vtbl *lpVtbl; };
#endif

#ifndef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
typedef interface ID3D12GraphicsCommandList10 ID3D12GraphicsCommandList10;
DEFINE_GUID(IID_ID3D12GraphicsCommandList10, 0x7013c015, 0xd161, 0x4b63, 0xa0,0x8c,0x23,0x85,0x52,0xdd,0x8a,0xcc);
typedef struct ID3D12GraphicsCommandList10Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12GraphicsCommandList10 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12GraphicsCommandList10 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12GraphicsCommandList10 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12GraphicsCommandList10 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12GraphicsCommandList10 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12GraphicsCommandList10 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12GraphicsCommandList10 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12GraphicsCommandList10 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12GraphicsCommandList10 *This);

    /*** ID3D12GraphicsCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12GraphicsCommandList10 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12CommandAllocator *allocator,
        ID3D12PipelineState *initial_state);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12PipelineState *pipeline_state);

    void (STDMETHODCALLTYPE *DrawInstanced)(
        ID3D12GraphicsCommandList10 *This,
        UINT vertex_count_per_instance,
        UINT instance_count,
        UINT start_vertex_location,
        UINT start_instance_location);

    void (STDMETHODCALLTYPE *DrawIndexedInstanced)(
        ID3D12GraphicsCommandList10 *This,
        UINT index_count_per_instance,
        UINT instance_count,
        UINT start_vertex_location,
        INT base_vertex_location,
        UINT start_instance_location);

    void (STDMETHODCALLTYPE *Dispatch)(
        ID3D12GraphicsCommandList10 *This,
        UINT x,
        UINT u,
        UINT z);

    void (STDMETHODCALLTYPE *CopyBufferRegion)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT64 byte_count);

    void (STDMETHODCALLTYPE *CopyTextureRegion)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_TEXTURE_COPY_LOCATION *dst,
        UINT dst_x,
        UINT dst_y,
        UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION *src,
        const D3D12_BOX *src_box);

    void (STDMETHODCALLTYPE *CopyResource)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *dst_resource,
        ID3D12Resource *src_resource);

    void (STDMETHODCALLTYPE *CopyTiles)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *tiled_resource,
        const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
        const D3D12_TILE_REGION_SIZE *tile_region_size,
        ID3D12Resource *buffer,
        UINT64 buffer_offset,
        D3D12_TILE_COPY_FLAGS flags);

    void (STDMETHODCALLTYPE *ResolveSubresource)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *dst_resource,
        UINT dst_sub_resource,
        ID3D12Resource *src_resource,
        UINT src_sub_resource,
        DXGI_FORMAT format);

    void (STDMETHODCALLTYPE *IASetPrimitiveTopology)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

    void (STDMETHODCALLTYPE *RSSetViewports)(
        ID3D12GraphicsCommandList10 *This,
        UINT viewport_count,
        const D3D12_VIEWPORT *viewports);

    void (STDMETHODCALLTYPE *RSSetScissorRects)(
        ID3D12GraphicsCommandList10 *This,
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *OMSetBlendFactor)(
        ID3D12GraphicsCommandList10 *This,
        const FLOAT blend_factor[4]);

    void (STDMETHODCALLTYPE *OMSetStencilRef)(
        ID3D12GraphicsCommandList10 *This,
        UINT stencil_ref);

    void (STDMETHODCALLTYPE *SetPipelineState)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12PipelineState *pipeline_state);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12GraphicsCommandList10 *This,
        UINT barrier_count,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *ExecuteBundle)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12GraphicsCommandList *command_list);

    void (STDMETHODCALLTYPE *SetDescriptorHeaps)(
        ID3D12GraphicsCommandList10 *This,
        UINT heap_count,
        ID3D12DescriptorHeap *const *heaps);

    void (STDMETHODCALLTYPE *SetComputeRootSignature)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12RootSignature *root_signature);

    void (STDMETHODCALLTYPE *SetGraphicsRootSignature)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12RootSignature *root_signature);

    void (STDMETHODCALLTYPE *SetComputeRootDescriptorTable)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

    void (STDMETHODCALLTYPE *SetGraphicsRootDescriptorTable)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

    void (STDMETHODCALLTYPE *SetComputeRoot32BitConstant)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        UINT data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetGraphicsRoot32BitConstant)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        UINT data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetComputeRoot32BitConstants)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        UINT constant_count,
        const void *data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetGraphicsRoot32BitConstants)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        UINT constant_count,
        const void *data,
        UINT dst_offset);

    void (STDMETHODCALLTYPE *SetComputeRootConstantBufferView)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootConstantBufferView)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetComputeRootShaderResourceView)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootShaderResourceView)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetComputeRootUnorderedAccessView)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *SetGraphicsRootUnorderedAccessView)(
        ID3D12GraphicsCommandList10 *This,
        UINT root_parameter_index,
        D3D12_GPU_VIRTUAL_ADDRESS address);

    void (STDMETHODCALLTYPE *IASetIndexBuffer)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_INDEX_BUFFER_VIEW *view);

    void (STDMETHODCALLTYPE *IASetVertexBuffers)(
        ID3D12GraphicsCommandList10 *This,
        UINT start_slot,
        UINT view_count,
        const D3D12_VERTEX_BUFFER_VIEW *views);

    void (STDMETHODCALLTYPE *SOSetTargets)(
        ID3D12GraphicsCommandList10 *This,
        UINT start_slot,
        UINT view_count,
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views);

    void (STDMETHODCALLTYPE *OMSetRenderTargets)(
        ID3D12GraphicsCommandList10 *This,
        UINT render_target_descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
        WINBOOL single_descriptor_handle,
        const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor);

    void (STDMETHODCALLTYPE *ClearDepthStencilView)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        D3D12_CLEAR_FLAGS flags,
        FLOAT depth,
        UINT8 stencil,
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearRenderTargetView)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        const FLOAT color[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewUint)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        ID3D12Resource *resource,
        const UINT values[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *ClearUnorderedAccessViewFloat)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        ID3D12Resource *resource,
        const float values[4],
        UINT rect_count,
        const D3D12_RECT *rects);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12QueryHeap *heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT query_count,
        ID3D12Resource *dst_buffer,
        UINT64 aligned_dst_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12GraphicsCommandList10 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12GraphicsCommandList10 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12GraphicsCommandList10 *This);

    void (STDMETHODCALLTYPE *ExecuteIndirect)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12CommandSignature *command_signature,
        UINT max_command_count,
        ID3D12Resource *arg_buffer,
        UINT64 arg_buffer_offset,
        ID3D12Resource *count_buffer,
        UINT64 count_buffer_offset);

    /*** ID3D12GraphicsCommandList1 methods ***/
    void (STDMETHODCALLTYPE *AtomicCopyBufferUINT)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT dependent_resource_count,
        ID3D12Resource *const *dependent_resources,
        const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges);

    void (STDMETHODCALLTYPE *AtomicCopyBufferUINT64)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *dst_buffer,
        UINT64 dst_offset,
        ID3D12Resource *src_buffer,
        UINT64 src_offset,
        UINT dependent_resource_count,
        ID3D12Resource *const *dependent_resources,
        const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges);

    void (STDMETHODCALLTYPE *OMSetDepthBounds)(
        ID3D12GraphicsCommandList10 *This,
        FLOAT min,
        FLOAT max);

    void (STDMETHODCALLTYPE *SetSamplePositions)(
        ID3D12GraphicsCommandList10 *This,
        UINT sample_count,
        UINT pixel_count,
        D3D12_SAMPLE_POSITION *sample_positions);

    void (STDMETHODCALLTYPE *ResolveSubresourceRegion)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *dst_resource,
        UINT dst_sub_resource_idx,
        UINT dst_x,
        UINT dst_y,
        ID3D12Resource *src_resource,
        UINT src_sub_resource_idx,
        D3D12_RECT *src_rect,
        DXGI_FORMAT format,
        D3D12_RESOLVE_MODE mode);

    void (STDMETHODCALLTYPE *SetViewInstanceMask)(
        ID3D12GraphicsCommandList10 *This,
        UINT mask);

    /*** ID3D12GraphicsCommandList2 methods ***/
    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12GraphicsCommandList10 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    /*** ID3D12GraphicsCommandList3 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12ProtectedResourceSession *protected_resource_session);

    /*** ID3D12GraphicsCommandList4 methods ***/
    void (STDMETHODCALLTYPE *BeginRenderPass)(
        ID3D12GraphicsCommandList10 *This,
        UINT render_targets_count,
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
        D3D12_RENDER_PASS_FLAGS flags);

    void (STDMETHODCALLTYPE *EndRenderPass)(
        ID3D12GraphicsCommandList10 *This);

    void (STDMETHODCALLTYPE *InitializeMetaCommand)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12MetaCommand *meta_command,
        const void *initialization_parameters_data,
        SIZE_T initialization_parameters_data_size_in_bytes);

    void (STDMETHODCALLTYPE *ExecuteMetaCommand)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12MetaCommand *meta_command,
        const void *execution_parameters_data,
        SIZE_T execution_parameters_data_size_in_bytes);

    void (STDMETHODCALLTYPE *BuildRaytracingAccelerationStructure)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
        UINT postbuild_info_descs_count,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs);

    void (STDMETHODCALLTYPE *EmitRaytracingAccelerationStructurePostbuildInfo)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
        UINT src_acceleration_structures_count,
        const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data);

    void (STDMETHODCALLTYPE *CopyRaytracingAccelerationStructure)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data,
        D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode);

    void (STDMETHODCALLTYPE *SetPipelineState1)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12StateObject *state_object);

    void (STDMETHODCALLTYPE *DispatchRays)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_DISPATCH_RAYS_DESC *desc);

    /*** ID3D12GraphicsCommandList5 methods ***/
    void (STDMETHODCALLTYPE *RSSetShadingRate)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_SHADING_RATE base_shading_rate,
        const D3D12_SHADING_RATE_COMBINER *combiners);

    void (STDMETHODCALLTYPE *RSSetShadingRateImage)(
        ID3D12GraphicsCommandList10 *This,
        ID3D12Resource *shading_rate_image);

    /*** ID3D12GraphicsCommandList6 methods ***/
    void (STDMETHODCALLTYPE *DispatchMesh)(
        ID3D12GraphicsCommandList10 *This,
        UINT thread_group_count_x,
        UINT thread_group_count_y,
        UINT thread_group_count_z);

    /*** ID3D12GraphicsCommandList7 methods ***/
    void (STDMETHODCALLTYPE *Barrier)(
        ID3D12GraphicsCommandList10 *This,
        UINT32 barrier_groups_count,
        const D3D12_BARRIER_GROUP *barrier_groups);

    /*** ID3D12GraphicsCommandList8 methods ***/
    void (STDMETHODCALLTYPE *OMSetFrontAndBackStencilRef)(
        ID3D12GraphicsCommandList10 *This,
        UINT front_stencil_ref,
        UINT back_stencil_ref);

    /*** ID3D12GraphicsCommandList9 methods ***/
    void (STDMETHODCALLTYPE *RSSetDepthBias)(
        ID3D12GraphicsCommandList10 *This,
        FLOAT depth_bias,
        FLOAT depth_bias_clamp,
        FLOAT slope_scaled_depth_bias);
    void (STDMETHODCALLTYPE *IASetIndexBufferStripCutValue)(
        ID3D12GraphicsCommandList10 *This,
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value);

    /*** ID3D12GraphicsCommandList10 methods ***/
    void (STDMETHODCALLTYPE *SetProgram)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_SET_PROGRAM_DESC *desc);
    void (STDMETHODCALLTYPE *DispatchGraph)(
        ID3D12GraphicsCommandList10 *This,
        const D3D12_DISPATCH_GRAPH_DESC *desc);

    END_INTERFACE
} ID3D12GraphicsCommandList10Vtbl;
interface ID3D12GraphicsCommandList10 { CONST_VTBL ID3D12GraphicsCommandList10Vtbl *lpVtbl; };
#endif

/* ---- C wrappers (COBJMACROS) */
#ifdef COBJMACROS
static inline HRESULT ID3D12Device11_QueryInterface(ID3D12Device11 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12Device11_AddRef(ID3D12Device11 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12Device11_Release(ID3D12Device11 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12Device11_GetPrivateData(ID3D12Device11 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device11_SetPrivateData(ID3D12Device11 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device11_SetPrivateDataInterface(ID3D12Device11 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12Device11_SetName(ID3D12Device11 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline UINT ID3D12Device11_GetNodeCount(ID3D12Device11 *This) { return This->lpVtbl->GetNodeCount(This); }
static inline HRESULT ID3D12Device11_CreateCommandQueue(ID3D12Device11 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue(This, desc, riid, command_queue); }
static inline HRESULT ID3D12Device11_CreateCommandAllocator(ID3D12Device11 *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **command_allocator) { return This->lpVtbl->CreateCommandAllocator(This, type, riid, command_allocator); }
static inline HRESULT ID3D12Device11_CreateGraphicsPipelineState(ID3D12Device11 *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateGraphicsPipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device11_CreateComputePipelineState(ID3D12Device11 *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateComputePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device11_CreateCommandList(ID3D12Device11 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *command_allocator, ID3D12PipelineState *initial_pipeline_state, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList(This, node_mask, type, command_allocator, initial_pipeline_state, riid, command_list); }
static inline HRESULT ID3D12Device11_CheckFeatureSupport(ID3D12Device11 *This, D3D12_FEATURE feature, void *feature_data, UINT feature_data_size) { return This->lpVtbl->CheckFeatureSupport(This, feature, feature_data, feature_data_size); }
static inline HRESULT ID3D12Device11_CreateDescriptorHeap(ID3D12Device11 *This, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **descriptor_heap) { return This->lpVtbl->CreateDescriptorHeap(This, desc, riid, descriptor_heap); }
static inline UINT ID3D12Device11_GetDescriptorHandleIncrementSize(ID3D12Device11 *This, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { return This->lpVtbl->GetDescriptorHandleIncrementSize(This, descriptor_heap_type); }
static inline HRESULT ID3D12Device11_CreateRootSignature(ID3D12Device11 *This, UINT node_mask, const void *bytecode, SIZE_T bytecode_length, REFIID riid, void **root_signature) { return This->lpVtbl->CreateRootSignature(This, node_mask, bytecode, bytecode_length, riid, root_signature); }
static inline void ID3D12Device11_CreateConstantBufferView(ID3D12Device11 *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateConstantBufferView(This, desc, descriptor); }
static inline void ID3D12Device11_CreateShaderResourceView(ID3D12Device11 *This, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateShaderResourceView(This, resource, desc, descriptor); }
static inline void ID3D12Device11_CreateUnorderedAccessView(ID3D12Device11 *This, ID3D12Resource *resource, ID3D12Resource *counter_resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateUnorderedAccessView(This, resource, counter_resource, desc, descriptor); }
static inline void ID3D12Device11_CreateRenderTargetView(ID3D12Device11 *This, ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateRenderTargetView(This, resource, desc, descriptor); }
static inline void ID3D12Device11_CreateDepthStencilView(ID3D12Device11 *This, ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateDepthStencilView(This, resource, desc, descriptor); }
static inline void ID3D12Device11_CreateSampler(ID3D12Device11 *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateSampler(This, desc, descriptor); }
static inline void ID3D12Device11_CopyDescriptors(ID3D12Device11 *This, UINT dst_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets, const UINT *dst_descriptor_range_sizes, UINT src_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets, const UINT *src_descriptor_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptors(This, dst_descriptor_range_count, dst_descriptor_range_offsets, dst_descriptor_range_sizes, src_descriptor_range_count, src_descriptor_range_offsets, src_descriptor_range_sizes, descriptor_heap_type); }
static inline void ID3D12Device11_CopyDescriptorsSimple(ID3D12Device11 *This, UINT descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset, const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptorsSimple(This, descriptor_count, dst_descriptor_range_offset, src_descriptor_range_offset, descriptor_heap_type); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device11_GetResourceAllocationInfo(ID3D12Device11 *This, UINT visible_mask, UINT reource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo(This, &__ret, visible_mask, reource_desc_count, resource_descs); }
static inline D3D12_HEAP_PROPERTIES ID3D12Device11_GetCustomHeapProperties(ID3D12Device11 *This, UINT node_mask, D3D12_HEAP_TYPE heap_type) { D3D12_HEAP_PROPERTIES __ret; return *This->lpVtbl->GetCustomHeapProperties(This, &__ret, node_mask, heap_type); }
static inline HRESULT ID3D12Device11_CreateCommittedResource(ID3D12Device11 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateCommittedResource(This, heap_properties, heap_flags, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device11_CreateHeap(ID3D12Device11 *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device11_CreatePlacedResource(ID3D12Device11 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device11_CreateReservedResource(ID3D12Device11 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource(This, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device11_CreateSharedHandle(ID3D12Device11 *This, ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) { return This->lpVtbl->CreateSharedHandle(This, object, attributes, access, name, handle); }
static inline HRESULT ID3D12Device11_OpenSharedHandle(ID3D12Device11 *This, HANDLE handle, REFIID riid, void **object) { return This->lpVtbl->OpenSharedHandle(This, handle, riid, object); }
static inline HRESULT ID3D12Device11_OpenSharedHandleByName(ID3D12Device11 *This, const WCHAR *name, DWORD access, HANDLE *handle) { return This->lpVtbl->OpenSharedHandleByName(This, name, access, handle); }
static inline HRESULT ID3D12Device11_MakeResident(ID3D12Device11 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->MakeResident(This, object_count, objects); }
static inline HRESULT ID3D12Device11_Evict(ID3D12Device11 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->Evict(This, object_count, objects); }
static inline HRESULT ID3D12Device11_CreateFence(ID3D12Device11 *This, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **fence) { return This->lpVtbl->CreateFence(This, initial_value, flags, riid, fence); }
static inline HRESULT ID3D12Device11_GetDeviceRemovedReason(ID3D12Device11 *This) { return This->lpVtbl->GetDeviceRemovedReason(This); }
static inline void ID3D12Device11_GetCopyableFootprints(ID3D12Device11 *This, const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource, UINT sub_resource_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count, UINT64 *row_size, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints(This, desc, first_sub_resource, sub_resource_count, base_offset, layouts, row_count, row_size, total_bytes); }
static inline HRESULT ID3D12Device11_CreateQueryHeap(ID3D12Device11 *This, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateQueryHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device11_SetStablePowerState(ID3D12Device11 *This, WINBOOL enable) { return This->lpVtbl->SetStablePowerState(This, enable); }
static inline HRESULT ID3D12Device11_CreateCommandSignature(ID3D12Device11 *This, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root_signature, REFIID riid, void **command_signature) { return This->lpVtbl->CreateCommandSignature(This, desc, root_signature, riid, command_signature); }
static inline void ID3D12Device11_GetResourceTiling(ID3D12Device11 *This, ID3D12Resource *resource, UINT *total_tile_count, D3D12_PACKED_MIP_INFO *packed_mip_info, D3D12_TILE_SHAPE *standard_tile_shape, UINT *sub_resource_tiling_count, UINT first_sub_resource_tiling, D3D12_SUBRESOURCE_TILING *sub_resource_tilings) { This->lpVtbl->GetResourceTiling(This, resource, total_tile_count, packed_mip_info, standard_tile_shape, sub_resource_tiling_count, first_sub_resource_tiling, sub_resource_tilings); }
static inline LUID ID3D12Device11_GetAdapterLuid(ID3D12Device11 *This) { LUID __ret; return *This->lpVtbl->GetAdapterLuid(This, &__ret); }
static inline HRESULT ID3D12Device11_CreatePipelineLibrary(ID3D12Device11 *This, const void *blob, SIZE_T blob_size, REFIID iid, void **lib) { return This->lpVtbl->CreatePipelineLibrary(This, blob, blob_size, iid, lib); }
static inline HRESULT ID3D12Device11_SetEventOnMultipleFenceCompletion(ID3D12Device11 *This, ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) { return This->lpVtbl->SetEventOnMultipleFenceCompletion(This, fences, values, fence_count, flags, event); }
static inline HRESULT ID3D12Device11_SetResidencyPriority(ID3D12Device11 *This, UINT object_count, ID3D12Pageable *const *objects, const D3D12_RESIDENCY_PRIORITY *priorities) { return This->lpVtbl->SetResidencyPriority(This, object_count, objects, priorities); }
static inline HRESULT ID3D12Device11_CreatePipelineState(ID3D12Device11 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreatePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device11_OpenExistingHeapFromAddress(ID3D12Device11 *This, const void *address, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromAddress(This, address, riid, heap); }
static inline HRESULT ID3D12Device11_OpenExistingHeapFromFileMapping(ID3D12Device11 *This, HANDLE file_mapping, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromFileMapping(This, file_mapping, riid, heap); }
static inline HRESULT ID3D12Device11_EnqueueMakeResident(ID3D12Device11 *This, D3D12_RESIDENCY_FLAGS flags, UINT num_objects, ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 fence_value) { return This->lpVtbl->EnqueueMakeResident(This, flags, num_objects, objects, fence, fence_value); }
static inline HRESULT ID3D12Device11_CreateCommandList1(ID3D12Device11 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList1(This, node_mask, type, flags, riid, command_list); }
static inline HRESULT ID3D12Device11_CreateProtectedResourceSession(ID3D12Device11 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device11_CreateCommittedResource1(ID3D12Device11 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource1(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device11_CreateHeap1(ID3D12Device11 *This, const D3D12_HEAP_DESC *desc, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap1(This, desc, protected_session, riid, heap); }
static inline HRESULT ID3D12Device11_CreateReservedResource1(ID3D12Device11 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource1(This, desc, initial_state, optimized_clear_value, protected_session, riid, resource); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device11_GetResourceAllocationInfo1(ID3D12Device11 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo1(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device11_CreateLifetimeTracker(ID3D12Device11 *This, ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) { return This->lpVtbl->CreateLifetimeTracker(This, owner, riid, tracker); }
static inline void ID3D12Device11_RemoveDevice(ID3D12Device11 *This) { This->lpVtbl->RemoveDevice(This); }
static inline HRESULT ID3D12Device11_EnumerateMetaCommands(ID3D12Device11 *This, UINT *meta_commands_count, D3D12_META_COMMAND_DESC *descs) { return This->lpVtbl->EnumerateMetaCommands(This, meta_commands_count, descs); }
static inline HRESULT ID3D12Device11_EnumerateMetaCommandParameters(ID3D12Device11 *This, REFGUID command_id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_structure_size_in_bytes, UINT *parameter_count, D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) { return This->lpVtbl->EnumerateMetaCommandParameters(This, command_id, stage, total_structure_size_in_bytes, parameter_count, parameter_descs); }
static inline HRESULT ID3D12Device11_CreateMetaCommand(ID3D12Device11 *This, REFGUID command_id, UINT node_mask, const void *creation_parameters_data, SIZE_T creation_parameters_data_size_in_bytes, REFIID riid, void **meta_command) { return This->lpVtbl->CreateMetaCommand(This, command_id, node_mask, creation_parameters_data, creation_parameters_data_size_in_bytes, riid, meta_command); }
static inline HRESULT ID3D12Device11_CreateStateObject(ID3D12Device11 *This, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) { return This->lpVtbl->CreateStateObject(This, desc, riid, state_object); }
static inline void ID3D12Device11_GetRaytracingAccelerationStructurePrebuildInfo(ID3D12Device11 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) { This->lpVtbl->GetRaytracingAccelerationStructurePrebuildInfo(This, desc, info); }
static inline D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS ID3D12Device11_CheckDriverMatchingIdentifier(ID3D12Device11 *This, D3D12_SERIALIZED_DATA_TYPE serialized_data_type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check) { return This->lpVtbl->CheckDriverMatchingIdentifier(This, serialized_data_type, identifier_to_check); }
static inline HRESULT ID3D12Device11_SetBackgroundProcessingMode(ID3D12Device11 *This, D3D12_BACKGROUND_PROCESSING_MODE mode, D3D12_MEASUREMENTS_ACTION action, HANDLE event, WINBOOL *further_measurements_desired) { return This->lpVtbl->SetBackgroundProcessingMode(This, mode, action, event, further_measurements_desired); }
static inline HRESULT ID3D12Device11_AddToStateObject(ID3D12Device11 *This, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *state_object_to_grow_from, REFIID riid, void **new_state_object) { return This->lpVtbl->AddToStateObject(This, addition, state_object_to_grow_from, riid, new_state_object); }
static inline HRESULT ID3D12Device11_CreateProtectedResourceSession1(ID3D12Device11 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession1(This, desc, riid, session); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device11_GetResourceAllocationInfo2(ID3D12Device11 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo2(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device11_CreateCommittedResource2(ID3D12Device11 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource2(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device11_CreatePlacedResource1(ID3D12Device11 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource1(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline void ID3D12Device11_CreateSamplerFeedbackUnorderedAccessView(ID3D12Device11 *This, ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource, D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) { This->lpVtbl->CreateSamplerFeedbackUnorderedAccessView(This, targeted_resource, feedback_resource, dst_descriptor); }
static inline void ID3D12Device11_GetCopyableFootprints1(ID3D12Device11 *This, const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource, UINT subresources_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count, UINT64 *row_size_in_bytes, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints1(This, resource_desc, first_subresource, subresources_count, base_offset, layouts, rows_count, row_size_in_bytes, total_bytes); }
static inline HRESULT ID3D12Device11_CreateShaderCacheSession(ID3D12Device11 *This, const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateShaderCacheSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device11_ShaderCacheControl(ID3D12Device11 *This, D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control) { return This->lpVtbl->ShaderCacheControl(This, kinds, control); }
static inline HRESULT ID3D12Device11_CreateCommandQueue1(ID3D12Device11 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue1(This, desc, creator_id, riid, command_queue); }
static inline HRESULT ID3D12Device11_CreateCommittedResource3(ID3D12Device11 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource3(This, heap_properties, heap_flags, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid_resource, resource); }
static inline HRESULT ID3D12Device11_CreatePlacedResource2(ID3D12Device11 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource2(This, heap, heap_offset, desc, initial_layout, optimized_clear_value, castable_formats_count, castable_formats, riid, resource); }
static inline HRESULT ID3D12Device11_CreateReservedResource2(ID3D12Device11 *This, const D3D12_RESOURCE_DESC *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource2(This, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid, resource); }
static inline void ID3D12Device11_CreateSampler2(ID3D12Device11 *This, const D3D12_SAMPLER_DESC2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) { This->lpVtbl->CreateSampler2(This, desc, dst); }
static inline HRESULT ID3D12Device12_QueryInterface(ID3D12Device12 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12Device12_AddRef(ID3D12Device12 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12Device12_Release(ID3D12Device12 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12Device12_GetPrivateData(ID3D12Device12 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device12_SetPrivateData(ID3D12Device12 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device12_SetPrivateDataInterface(ID3D12Device12 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12Device12_SetName(ID3D12Device12 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline UINT ID3D12Device12_GetNodeCount(ID3D12Device12 *This) { return This->lpVtbl->GetNodeCount(This); }
static inline HRESULT ID3D12Device12_CreateCommandQueue(ID3D12Device12 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue(This, desc, riid, command_queue); }
static inline HRESULT ID3D12Device12_CreateCommandAllocator(ID3D12Device12 *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **command_allocator) { return This->lpVtbl->CreateCommandAllocator(This, type, riid, command_allocator); }
static inline HRESULT ID3D12Device12_CreateGraphicsPipelineState(ID3D12Device12 *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateGraphicsPipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device12_CreateComputePipelineState(ID3D12Device12 *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateComputePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device12_CreateCommandList(ID3D12Device12 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *command_allocator, ID3D12PipelineState *initial_pipeline_state, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList(This, node_mask, type, command_allocator, initial_pipeline_state, riid, command_list); }
static inline HRESULT ID3D12Device12_CheckFeatureSupport(ID3D12Device12 *This, D3D12_FEATURE feature, void *feature_data, UINT feature_data_size) { return This->lpVtbl->CheckFeatureSupport(This, feature, feature_data, feature_data_size); }
static inline HRESULT ID3D12Device12_CreateDescriptorHeap(ID3D12Device12 *This, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **descriptor_heap) { return This->lpVtbl->CreateDescriptorHeap(This, desc, riid, descriptor_heap); }
static inline UINT ID3D12Device12_GetDescriptorHandleIncrementSize(ID3D12Device12 *This, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { return This->lpVtbl->GetDescriptorHandleIncrementSize(This, descriptor_heap_type); }
static inline HRESULT ID3D12Device12_CreateRootSignature(ID3D12Device12 *This, UINT node_mask, const void *bytecode, SIZE_T bytecode_length, REFIID riid, void **root_signature) { return This->lpVtbl->CreateRootSignature(This, node_mask, bytecode, bytecode_length, riid, root_signature); }
static inline void ID3D12Device12_CreateConstantBufferView(ID3D12Device12 *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateConstantBufferView(This, desc, descriptor); }
static inline void ID3D12Device12_CreateShaderResourceView(ID3D12Device12 *This, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateShaderResourceView(This, resource, desc, descriptor); }
static inline void ID3D12Device12_CreateUnorderedAccessView(ID3D12Device12 *This, ID3D12Resource *resource, ID3D12Resource *counter_resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateUnorderedAccessView(This, resource, counter_resource, desc, descriptor); }
static inline void ID3D12Device12_CreateRenderTargetView(ID3D12Device12 *This, ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateRenderTargetView(This, resource, desc, descriptor); }
static inline void ID3D12Device12_CreateDepthStencilView(ID3D12Device12 *This, ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateDepthStencilView(This, resource, desc, descriptor); }
static inline void ID3D12Device12_CreateSampler(ID3D12Device12 *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateSampler(This, desc, descriptor); }
static inline void ID3D12Device12_CopyDescriptors(ID3D12Device12 *This, UINT dst_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets, const UINT *dst_descriptor_range_sizes, UINT src_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets, const UINT *src_descriptor_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptors(This, dst_descriptor_range_count, dst_descriptor_range_offsets, dst_descriptor_range_sizes, src_descriptor_range_count, src_descriptor_range_offsets, src_descriptor_range_sizes, descriptor_heap_type); }
static inline void ID3D12Device12_CopyDescriptorsSimple(ID3D12Device12 *This, UINT descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset, const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptorsSimple(This, descriptor_count, dst_descriptor_range_offset, src_descriptor_range_offset, descriptor_heap_type); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device12_GetResourceAllocationInfo(ID3D12Device12 *This, UINT visible_mask, UINT reource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo(This, &__ret, visible_mask, reource_desc_count, resource_descs); }
static inline D3D12_HEAP_PROPERTIES ID3D12Device12_GetCustomHeapProperties(ID3D12Device12 *This, UINT node_mask, D3D12_HEAP_TYPE heap_type) { D3D12_HEAP_PROPERTIES __ret; return *This->lpVtbl->GetCustomHeapProperties(This, &__ret, node_mask, heap_type); }
static inline HRESULT ID3D12Device12_CreateCommittedResource(ID3D12Device12 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateCommittedResource(This, heap_properties, heap_flags, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device12_CreateHeap(ID3D12Device12 *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device12_CreatePlacedResource(ID3D12Device12 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device12_CreateReservedResource(ID3D12Device12 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource(This, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device12_CreateSharedHandle(ID3D12Device12 *This, ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) { return This->lpVtbl->CreateSharedHandle(This, object, attributes, access, name, handle); }
static inline HRESULT ID3D12Device12_OpenSharedHandle(ID3D12Device12 *This, HANDLE handle, REFIID riid, void **object) { return This->lpVtbl->OpenSharedHandle(This, handle, riid, object); }
static inline HRESULT ID3D12Device12_OpenSharedHandleByName(ID3D12Device12 *This, const WCHAR *name, DWORD access, HANDLE *handle) { return This->lpVtbl->OpenSharedHandleByName(This, name, access, handle); }
static inline HRESULT ID3D12Device12_MakeResident(ID3D12Device12 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->MakeResident(This, object_count, objects); }
static inline HRESULT ID3D12Device12_Evict(ID3D12Device12 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->Evict(This, object_count, objects); }
static inline HRESULT ID3D12Device12_CreateFence(ID3D12Device12 *This, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **fence) { return This->lpVtbl->CreateFence(This, initial_value, flags, riid, fence); }
static inline HRESULT ID3D12Device12_GetDeviceRemovedReason(ID3D12Device12 *This) { return This->lpVtbl->GetDeviceRemovedReason(This); }
static inline void ID3D12Device12_GetCopyableFootprints(ID3D12Device12 *This, const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource, UINT sub_resource_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count, UINT64 *row_size, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints(This, desc, first_sub_resource, sub_resource_count, base_offset, layouts, row_count, row_size, total_bytes); }
static inline HRESULT ID3D12Device12_CreateQueryHeap(ID3D12Device12 *This, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateQueryHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device12_SetStablePowerState(ID3D12Device12 *This, WINBOOL enable) { return This->lpVtbl->SetStablePowerState(This, enable); }
static inline HRESULT ID3D12Device12_CreateCommandSignature(ID3D12Device12 *This, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root_signature, REFIID riid, void **command_signature) { return This->lpVtbl->CreateCommandSignature(This, desc, root_signature, riid, command_signature); }
static inline void ID3D12Device12_GetResourceTiling(ID3D12Device12 *This, ID3D12Resource *resource, UINT *total_tile_count, D3D12_PACKED_MIP_INFO *packed_mip_info, D3D12_TILE_SHAPE *standard_tile_shape, UINT *sub_resource_tiling_count, UINT first_sub_resource_tiling, D3D12_SUBRESOURCE_TILING *sub_resource_tilings) { This->lpVtbl->GetResourceTiling(This, resource, total_tile_count, packed_mip_info, standard_tile_shape, sub_resource_tiling_count, first_sub_resource_tiling, sub_resource_tilings); }
static inline LUID ID3D12Device12_GetAdapterLuid(ID3D12Device12 *This) { LUID __ret; return *This->lpVtbl->GetAdapterLuid(This, &__ret); }
static inline HRESULT ID3D12Device12_CreatePipelineLibrary(ID3D12Device12 *This, const void *blob, SIZE_T blob_size, REFIID iid, void **lib) { return This->lpVtbl->CreatePipelineLibrary(This, blob, blob_size, iid, lib); }
static inline HRESULT ID3D12Device12_SetEventOnMultipleFenceCompletion(ID3D12Device12 *This, ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) { return This->lpVtbl->SetEventOnMultipleFenceCompletion(This, fences, values, fence_count, flags, event); }
static inline HRESULT ID3D12Device12_SetResidencyPriority(ID3D12Device12 *This, UINT object_count, ID3D12Pageable *const *objects, const D3D12_RESIDENCY_PRIORITY *priorities) { return This->lpVtbl->SetResidencyPriority(This, object_count, objects, priorities); }
static inline HRESULT ID3D12Device12_CreatePipelineState(ID3D12Device12 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreatePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device12_OpenExistingHeapFromAddress(ID3D12Device12 *This, const void *address, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromAddress(This, address, riid, heap); }
static inline HRESULT ID3D12Device12_OpenExistingHeapFromFileMapping(ID3D12Device12 *This, HANDLE file_mapping, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromFileMapping(This, file_mapping, riid, heap); }
static inline HRESULT ID3D12Device12_EnqueueMakeResident(ID3D12Device12 *This, D3D12_RESIDENCY_FLAGS flags, UINT num_objects, ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 fence_value) { return This->lpVtbl->EnqueueMakeResident(This, flags, num_objects, objects, fence, fence_value); }
static inline HRESULT ID3D12Device12_CreateCommandList1(ID3D12Device12 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList1(This, node_mask, type, flags, riid, command_list); }
static inline HRESULT ID3D12Device12_CreateProtectedResourceSession(ID3D12Device12 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device12_CreateCommittedResource1(ID3D12Device12 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource1(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device12_CreateHeap1(ID3D12Device12 *This, const D3D12_HEAP_DESC *desc, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap1(This, desc, protected_session, riid, heap); }
static inline HRESULT ID3D12Device12_CreateReservedResource1(ID3D12Device12 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource1(This, desc, initial_state, optimized_clear_value, protected_session, riid, resource); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device12_GetResourceAllocationInfo1(ID3D12Device12 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo1(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device12_CreateLifetimeTracker(ID3D12Device12 *This, ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) { return This->lpVtbl->CreateLifetimeTracker(This, owner, riid, tracker); }
static inline void ID3D12Device12_RemoveDevice(ID3D12Device12 *This) { This->lpVtbl->RemoveDevice(This); }
static inline HRESULT ID3D12Device12_EnumerateMetaCommands(ID3D12Device12 *This, UINT *meta_commands_count, D3D12_META_COMMAND_DESC *descs) { return This->lpVtbl->EnumerateMetaCommands(This, meta_commands_count, descs); }
static inline HRESULT ID3D12Device12_EnumerateMetaCommandParameters(ID3D12Device12 *This, REFGUID command_id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_structure_size_in_bytes, UINT *parameter_count, D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) { return This->lpVtbl->EnumerateMetaCommandParameters(This, command_id, stage, total_structure_size_in_bytes, parameter_count, parameter_descs); }
static inline HRESULT ID3D12Device12_CreateMetaCommand(ID3D12Device12 *This, REFGUID command_id, UINT node_mask, const void *creation_parameters_data, SIZE_T creation_parameters_data_size_in_bytes, REFIID riid, void **meta_command) { return This->lpVtbl->CreateMetaCommand(This, command_id, node_mask, creation_parameters_data, creation_parameters_data_size_in_bytes, riid, meta_command); }
static inline HRESULT ID3D12Device12_CreateStateObject(ID3D12Device12 *This, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) { return This->lpVtbl->CreateStateObject(This, desc, riid, state_object); }
static inline void ID3D12Device12_GetRaytracingAccelerationStructurePrebuildInfo(ID3D12Device12 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) { This->lpVtbl->GetRaytracingAccelerationStructurePrebuildInfo(This, desc, info); }
static inline D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS ID3D12Device12_CheckDriverMatchingIdentifier(ID3D12Device12 *This, D3D12_SERIALIZED_DATA_TYPE serialized_data_type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check) { return This->lpVtbl->CheckDriverMatchingIdentifier(This, serialized_data_type, identifier_to_check); }
static inline HRESULT ID3D12Device12_SetBackgroundProcessingMode(ID3D12Device12 *This, D3D12_BACKGROUND_PROCESSING_MODE mode, D3D12_MEASUREMENTS_ACTION action, HANDLE event, WINBOOL *further_measurements_desired) { return This->lpVtbl->SetBackgroundProcessingMode(This, mode, action, event, further_measurements_desired); }
static inline HRESULT ID3D12Device12_AddToStateObject(ID3D12Device12 *This, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *state_object_to_grow_from, REFIID riid, void **new_state_object) { return This->lpVtbl->AddToStateObject(This, addition, state_object_to_grow_from, riid, new_state_object); }
static inline HRESULT ID3D12Device12_CreateProtectedResourceSession1(ID3D12Device12 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession1(This, desc, riid, session); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device12_GetResourceAllocationInfo2(ID3D12Device12 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo2(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device12_CreateCommittedResource2(ID3D12Device12 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource2(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device12_CreatePlacedResource1(ID3D12Device12 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource1(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline void ID3D12Device12_CreateSamplerFeedbackUnorderedAccessView(ID3D12Device12 *This, ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource, D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) { This->lpVtbl->CreateSamplerFeedbackUnorderedAccessView(This, targeted_resource, feedback_resource, dst_descriptor); }
static inline void ID3D12Device12_GetCopyableFootprints1(ID3D12Device12 *This, const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource, UINT subresources_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count, UINT64 *row_size_in_bytes, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints1(This, resource_desc, first_subresource, subresources_count, base_offset, layouts, rows_count, row_size_in_bytes, total_bytes); }
static inline HRESULT ID3D12Device12_CreateShaderCacheSession(ID3D12Device12 *This, const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateShaderCacheSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device12_ShaderCacheControl(ID3D12Device12 *This, D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control) { return This->lpVtbl->ShaderCacheControl(This, kinds, control); }
static inline HRESULT ID3D12Device12_CreateCommandQueue1(ID3D12Device12 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue1(This, desc, creator_id, riid, command_queue); }
static inline HRESULT ID3D12Device12_CreateCommittedResource3(ID3D12Device12 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource3(This, heap_properties, heap_flags, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid_resource, resource); }
static inline HRESULT ID3D12Device12_CreatePlacedResource2(ID3D12Device12 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource2(This, heap, heap_offset, desc, initial_layout, optimized_clear_value, castable_formats_count, castable_formats, riid, resource); }
static inline HRESULT ID3D12Device12_CreateReservedResource2(ID3D12Device12 *This, const D3D12_RESOURCE_DESC *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource2(This, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid, resource); }
static inline void ID3D12Device12_CreateSampler2(ID3D12Device12 *This, const D3D12_SAMPLER_DESC2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) { This->lpVtbl->CreateSampler2(This, desc, dst); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device12_GetResourceAllocationInfo3(ID3D12Device12 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, const UINT32 *castable_formats_counts, const DXGI_FORMAT *const *castable_formats, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo3(This, &__ret, visible_mask, resource_descs_count, resource_descs, castable_formats_counts, castable_formats, resource_allocation_info1); }
static inline HRESULT ID3D12Device13_QueryInterface(ID3D12Device13 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12Device13_AddRef(ID3D12Device13 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12Device13_Release(ID3D12Device13 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12Device13_GetPrivateData(ID3D12Device13 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device13_SetPrivateData(ID3D12Device13 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device13_SetPrivateDataInterface(ID3D12Device13 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12Device13_SetName(ID3D12Device13 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline UINT ID3D12Device13_GetNodeCount(ID3D12Device13 *This) { return This->lpVtbl->GetNodeCount(This); }
static inline HRESULT ID3D12Device13_CreateCommandQueue(ID3D12Device13 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue(This, desc, riid, command_queue); }
static inline HRESULT ID3D12Device13_CreateCommandAllocator(ID3D12Device13 *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **command_allocator) { return This->lpVtbl->CreateCommandAllocator(This, type, riid, command_allocator); }
static inline HRESULT ID3D12Device13_CreateGraphicsPipelineState(ID3D12Device13 *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateGraphicsPipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device13_CreateComputePipelineState(ID3D12Device13 *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateComputePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device13_CreateCommandList(ID3D12Device13 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *command_allocator, ID3D12PipelineState *initial_pipeline_state, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList(This, node_mask, type, command_allocator, initial_pipeline_state, riid, command_list); }
static inline HRESULT ID3D12Device13_CheckFeatureSupport(ID3D12Device13 *This, D3D12_FEATURE feature, void *feature_data, UINT feature_data_size) { return This->lpVtbl->CheckFeatureSupport(This, feature, feature_data, feature_data_size); }
static inline HRESULT ID3D12Device13_CreateDescriptorHeap(ID3D12Device13 *This, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **descriptor_heap) { return This->lpVtbl->CreateDescriptorHeap(This, desc, riid, descriptor_heap); }
static inline UINT ID3D12Device13_GetDescriptorHandleIncrementSize(ID3D12Device13 *This, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { return This->lpVtbl->GetDescriptorHandleIncrementSize(This, descriptor_heap_type); }
static inline HRESULT ID3D12Device13_CreateRootSignature(ID3D12Device13 *This, UINT node_mask, const void *bytecode, SIZE_T bytecode_length, REFIID riid, void **root_signature) { return This->lpVtbl->CreateRootSignature(This, node_mask, bytecode, bytecode_length, riid, root_signature); }
static inline void ID3D12Device13_CreateConstantBufferView(ID3D12Device13 *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateConstantBufferView(This, desc, descriptor); }
static inline void ID3D12Device13_CreateShaderResourceView(ID3D12Device13 *This, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateShaderResourceView(This, resource, desc, descriptor); }
static inline void ID3D12Device13_CreateUnorderedAccessView(ID3D12Device13 *This, ID3D12Resource *resource, ID3D12Resource *counter_resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateUnorderedAccessView(This, resource, counter_resource, desc, descriptor); }
static inline void ID3D12Device13_CreateRenderTargetView(ID3D12Device13 *This, ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateRenderTargetView(This, resource, desc, descriptor); }
static inline void ID3D12Device13_CreateDepthStencilView(ID3D12Device13 *This, ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateDepthStencilView(This, resource, desc, descriptor); }
static inline void ID3D12Device13_CreateSampler(ID3D12Device13 *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateSampler(This, desc, descriptor); }
static inline void ID3D12Device13_CopyDescriptors(ID3D12Device13 *This, UINT dst_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets, const UINT *dst_descriptor_range_sizes, UINT src_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets, const UINT *src_descriptor_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptors(This, dst_descriptor_range_count, dst_descriptor_range_offsets, dst_descriptor_range_sizes, src_descriptor_range_count, src_descriptor_range_offsets, src_descriptor_range_sizes, descriptor_heap_type); }
static inline void ID3D12Device13_CopyDescriptorsSimple(ID3D12Device13 *This, UINT descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset, const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptorsSimple(This, descriptor_count, dst_descriptor_range_offset, src_descriptor_range_offset, descriptor_heap_type); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device13_GetResourceAllocationInfo(ID3D12Device13 *This, UINT visible_mask, UINT reource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo(This, &__ret, visible_mask, reource_desc_count, resource_descs); }
static inline D3D12_HEAP_PROPERTIES ID3D12Device13_GetCustomHeapProperties(ID3D12Device13 *This, UINT node_mask, D3D12_HEAP_TYPE heap_type) { D3D12_HEAP_PROPERTIES __ret; return *This->lpVtbl->GetCustomHeapProperties(This, &__ret, node_mask, heap_type); }
static inline HRESULT ID3D12Device13_CreateCommittedResource(ID3D12Device13 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateCommittedResource(This, heap_properties, heap_flags, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device13_CreateHeap(ID3D12Device13 *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device13_CreatePlacedResource(ID3D12Device13 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device13_CreateReservedResource(ID3D12Device13 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource(This, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device13_CreateSharedHandle(ID3D12Device13 *This, ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) { return This->lpVtbl->CreateSharedHandle(This, object, attributes, access, name, handle); }
static inline HRESULT ID3D12Device13_OpenSharedHandle(ID3D12Device13 *This, HANDLE handle, REFIID riid, void **object) { return This->lpVtbl->OpenSharedHandle(This, handle, riid, object); }
static inline HRESULT ID3D12Device13_OpenSharedHandleByName(ID3D12Device13 *This, const WCHAR *name, DWORD access, HANDLE *handle) { return This->lpVtbl->OpenSharedHandleByName(This, name, access, handle); }
static inline HRESULT ID3D12Device13_MakeResident(ID3D12Device13 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->MakeResident(This, object_count, objects); }
static inline HRESULT ID3D12Device13_Evict(ID3D12Device13 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->Evict(This, object_count, objects); }
static inline HRESULT ID3D12Device13_CreateFence(ID3D12Device13 *This, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **fence) { return This->lpVtbl->CreateFence(This, initial_value, flags, riid, fence); }
static inline HRESULT ID3D12Device13_GetDeviceRemovedReason(ID3D12Device13 *This) { return This->lpVtbl->GetDeviceRemovedReason(This); }
static inline void ID3D12Device13_GetCopyableFootprints(ID3D12Device13 *This, const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource, UINT sub_resource_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count, UINT64 *row_size, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints(This, desc, first_sub_resource, sub_resource_count, base_offset, layouts, row_count, row_size, total_bytes); }
static inline HRESULT ID3D12Device13_CreateQueryHeap(ID3D12Device13 *This, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateQueryHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device13_SetStablePowerState(ID3D12Device13 *This, WINBOOL enable) { return This->lpVtbl->SetStablePowerState(This, enable); }
static inline HRESULT ID3D12Device13_CreateCommandSignature(ID3D12Device13 *This, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root_signature, REFIID riid, void **command_signature) { return This->lpVtbl->CreateCommandSignature(This, desc, root_signature, riid, command_signature); }
static inline void ID3D12Device13_GetResourceTiling(ID3D12Device13 *This, ID3D12Resource *resource, UINT *total_tile_count, D3D12_PACKED_MIP_INFO *packed_mip_info, D3D12_TILE_SHAPE *standard_tile_shape, UINT *sub_resource_tiling_count, UINT first_sub_resource_tiling, D3D12_SUBRESOURCE_TILING *sub_resource_tilings) { This->lpVtbl->GetResourceTiling(This, resource, total_tile_count, packed_mip_info, standard_tile_shape, sub_resource_tiling_count, first_sub_resource_tiling, sub_resource_tilings); }
static inline LUID ID3D12Device13_GetAdapterLuid(ID3D12Device13 *This) { LUID __ret; return *This->lpVtbl->GetAdapterLuid(This, &__ret); }
static inline HRESULT ID3D12Device13_CreatePipelineLibrary(ID3D12Device13 *This, const void *blob, SIZE_T blob_size, REFIID iid, void **lib) { return This->lpVtbl->CreatePipelineLibrary(This, blob, blob_size, iid, lib); }
static inline HRESULT ID3D12Device13_SetEventOnMultipleFenceCompletion(ID3D12Device13 *This, ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) { return This->lpVtbl->SetEventOnMultipleFenceCompletion(This, fences, values, fence_count, flags, event); }
static inline HRESULT ID3D12Device13_SetResidencyPriority(ID3D12Device13 *This, UINT object_count, ID3D12Pageable *const *objects, const D3D12_RESIDENCY_PRIORITY *priorities) { return This->lpVtbl->SetResidencyPriority(This, object_count, objects, priorities); }
static inline HRESULT ID3D12Device13_CreatePipelineState(ID3D12Device13 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreatePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device13_OpenExistingHeapFromAddress(ID3D12Device13 *This, const void *address, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromAddress(This, address, riid, heap); }
static inline HRESULT ID3D12Device13_OpenExistingHeapFromFileMapping(ID3D12Device13 *This, HANDLE file_mapping, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromFileMapping(This, file_mapping, riid, heap); }
static inline HRESULT ID3D12Device13_EnqueueMakeResident(ID3D12Device13 *This, D3D12_RESIDENCY_FLAGS flags, UINT num_objects, ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 fence_value) { return This->lpVtbl->EnqueueMakeResident(This, flags, num_objects, objects, fence, fence_value); }
static inline HRESULT ID3D12Device13_CreateCommandList1(ID3D12Device13 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList1(This, node_mask, type, flags, riid, command_list); }
static inline HRESULT ID3D12Device13_CreateProtectedResourceSession(ID3D12Device13 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device13_CreateCommittedResource1(ID3D12Device13 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource1(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device13_CreateHeap1(ID3D12Device13 *This, const D3D12_HEAP_DESC *desc, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap1(This, desc, protected_session, riid, heap); }
static inline HRESULT ID3D12Device13_CreateReservedResource1(ID3D12Device13 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource1(This, desc, initial_state, optimized_clear_value, protected_session, riid, resource); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device13_GetResourceAllocationInfo1(ID3D12Device13 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo1(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device13_CreateLifetimeTracker(ID3D12Device13 *This, ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) { return This->lpVtbl->CreateLifetimeTracker(This, owner, riid, tracker); }
static inline void ID3D12Device13_RemoveDevice(ID3D12Device13 *This) { This->lpVtbl->RemoveDevice(This); }
static inline HRESULT ID3D12Device13_EnumerateMetaCommands(ID3D12Device13 *This, UINT *meta_commands_count, D3D12_META_COMMAND_DESC *descs) { return This->lpVtbl->EnumerateMetaCommands(This, meta_commands_count, descs); }
static inline HRESULT ID3D12Device13_EnumerateMetaCommandParameters(ID3D12Device13 *This, REFGUID command_id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_structure_size_in_bytes, UINT *parameter_count, D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) { return This->lpVtbl->EnumerateMetaCommandParameters(This, command_id, stage, total_structure_size_in_bytes, parameter_count, parameter_descs); }
static inline HRESULT ID3D12Device13_CreateMetaCommand(ID3D12Device13 *This, REFGUID command_id, UINT node_mask, const void *creation_parameters_data, SIZE_T creation_parameters_data_size_in_bytes, REFIID riid, void **meta_command) { return This->lpVtbl->CreateMetaCommand(This, command_id, node_mask, creation_parameters_data, creation_parameters_data_size_in_bytes, riid, meta_command); }
static inline HRESULT ID3D12Device13_CreateStateObject(ID3D12Device13 *This, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) { return This->lpVtbl->CreateStateObject(This, desc, riid, state_object); }
static inline void ID3D12Device13_GetRaytracingAccelerationStructurePrebuildInfo(ID3D12Device13 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) { This->lpVtbl->GetRaytracingAccelerationStructurePrebuildInfo(This, desc, info); }
static inline D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS ID3D12Device13_CheckDriverMatchingIdentifier(ID3D12Device13 *This, D3D12_SERIALIZED_DATA_TYPE serialized_data_type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check) { return This->lpVtbl->CheckDriverMatchingIdentifier(This, serialized_data_type, identifier_to_check); }
static inline HRESULT ID3D12Device13_SetBackgroundProcessingMode(ID3D12Device13 *This, D3D12_BACKGROUND_PROCESSING_MODE mode, D3D12_MEASUREMENTS_ACTION action, HANDLE event, WINBOOL *further_measurements_desired) { return This->lpVtbl->SetBackgroundProcessingMode(This, mode, action, event, further_measurements_desired); }
static inline HRESULT ID3D12Device13_AddToStateObject(ID3D12Device13 *This, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *state_object_to_grow_from, REFIID riid, void **new_state_object) { return This->lpVtbl->AddToStateObject(This, addition, state_object_to_grow_from, riid, new_state_object); }
static inline HRESULT ID3D12Device13_CreateProtectedResourceSession1(ID3D12Device13 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession1(This, desc, riid, session); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device13_GetResourceAllocationInfo2(ID3D12Device13 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo2(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device13_CreateCommittedResource2(ID3D12Device13 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource2(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device13_CreatePlacedResource1(ID3D12Device13 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource1(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline void ID3D12Device13_CreateSamplerFeedbackUnorderedAccessView(ID3D12Device13 *This, ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource, D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) { This->lpVtbl->CreateSamplerFeedbackUnorderedAccessView(This, targeted_resource, feedback_resource, dst_descriptor); }
static inline void ID3D12Device13_GetCopyableFootprints1(ID3D12Device13 *This, const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource, UINT subresources_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count, UINT64 *row_size_in_bytes, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints1(This, resource_desc, first_subresource, subresources_count, base_offset, layouts, rows_count, row_size_in_bytes, total_bytes); }
static inline HRESULT ID3D12Device13_CreateShaderCacheSession(ID3D12Device13 *This, const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateShaderCacheSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device13_ShaderCacheControl(ID3D12Device13 *This, D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control) { return This->lpVtbl->ShaderCacheControl(This, kinds, control); }
static inline HRESULT ID3D12Device13_CreateCommandQueue1(ID3D12Device13 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue1(This, desc, creator_id, riid, command_queue); }
static inline HRESULT ID3D12Device13_CreateCommittedResource3(ID3D12Device13 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource3(This, heap_properties, heap_flags, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid_resource, resource); }
static inline HRESULT ID3D12Device13_CreatePlacedResource2(ID3D12Device13 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource2(This, heap, heap_offset, desc, initial_layout, optimized_clear_value, castable_formats_count, castable_formats, riid, resource); }
static inline HRESULT ID3D12Device13_CreateReservedResource2(ID3D12Device13 *This, const D3D12_RESOURCE_DESC *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource2(This, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid, resource); }
static inline void ID3D12Device13_CreateSampler2(ID3D12Device13 *This, const D3D12_SAMPLER_DESC2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) { This->lpVtbl->CreateSampler2(This, desc, dst); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device13_GetResourceAllocationInfo3(ID3D12Device13 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, const UINT32 *castable_formats_counts, const DXGI_FORMAT *const *castable_formats, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo3(This, &__ret, visible_mask, resource_descs_count, resource_descs, castable_formats_counts, castable_formats, resource_allocation_info1); }
static inline HRESULT ID3D12Device13_OpenExistingHeapFromAddress1(ID3D12Device13 *This, const void *address, SIZE_T size, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromAddress1(This, address, size, riid, heap); }
static inline HRESULT ID3D12Device14_QueryInterface(ID3D12Device14 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12Device14_AddRef(ID3D12Device14 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12Device14_Release(ID3D12Device14 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12Device14_GetPrivateData(ID3D12Device14 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device14_SetPrivateData(ID3D12Device14 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12Device14_SetPrivateDataInterface(ID3D12Device14 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12Device14_SetName(ID3D12Device14 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline UINT ID3D12Device14_GetNodeCount(ID3D12Device14 *This) { return This->lpVtbl->GetNodeCount(This); }
static inline HRESULT ID3D12Device14_CreateCommandQueue(ID3D12Device14 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue(This, desc, riid, command_queue); }
static inline HRESULT ID3D12Device14_CreateCommandAllocator(ID3D12Device14 *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **command_allocator) { return This->lpVtbl->CreateCommandAllocator(This, type, riid, command_allocator); }
static inline HRESULT ID3D12Device14_CreateGraphicsPipelineState(ID3D12Device14 *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateGraphicsPipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device14_CreateComputePipelineState(ID3D12Device14 *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreateComputePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device14_CreateCommandList(ID3D12Device14 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *command_allocator, ID3D12PipelineState *initial_pipeline_state, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList(This, node_mask, type, command_allocator, initial_pipeline_state, riid, command_list); }
static inline HRESULT ID3D12Device14_CheckFeatureSupport(ID3D12Device14 *This, D3D12_FEATURE feature, void *feature_data, UINT feature_data_size) { return This->lpVtbl->CheckFeatureSupport(This, feature, feature_data, feature_data_size); }
static inline HRESULT ID3D12Device14_CreateDescriptorHeap(ID3D12Device14 *This, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **descriptor_heap) { return This->lpVtbl->CreateDescriptorHeap(This, desc, riid, descriptor_heap); }
static inline UINT ID3D12Device14_GetDescriptorHandleIncrementSize(ID3D12Device14 *This, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { return This->lpVtbl->GetDescriptorHandleIncrementSize(This, descriptor_heap_type); }
static inline HRESULT ID3D12Device14_CreateRootSignature(ID3D12Device14 *This, UINT node_mask, const void *bytecode, SIZE_T bytecode_length, REFIID riid, void **root_signature) { return This->lpVtbl->CreateRootSignature(This, node_mask, bytecode, bytecode_length, riid, root_signature); }
static inline void ID3D12Device14_CreateConstantBufferView(ID3D12Device14 *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateConstantBufferView(This, desc, descriptor); }
static inline void ID3D12Device14_CreateShaderResourceView(ID3D12Device14 *This, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateShaderResourceView(This, resource, desc, descriptor); }
static inline void ID3D12Device14_CreateUnorderedAccessView(ID3D12Device14 *This, ID3D12Resource *resource, ID3D12Resource *counter_resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateUnorderedAccessView(This, resource, counter_resource, desc, descriptor); }
static inline void ID3D12Device14_CreateRenderTargetView(ID3D12Device14 *This, ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateRenderTargetView(This, resource, desc, descriptor); }
static inline void ID3D12Device14_CreateDepthStencilView(ID3D12Device14 *This, ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateDepthStencilView(This, resource, desc, descriptor); }
static inline void ID3D12Device14_CreateSampler(ID3D12Device14 *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { This->lpVtbl->CreateSampler(This, desc, descriptor); }
static inline void ID3D12Device14_CopyDescriptors(ID3D12Device14 *This, UINT dst_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets, const UINT *dst_descriptor_range_sizes, UINT src_descriptor_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets, const UINT *src_descriptor_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptors(This, dst_descriptor_range_count, dst_descriptor_range_offsets, dst_descriptor_range_sizes, src_descriptor_range_count, src_descriptor_range_offsets, src_descriptor_range_sizes, descriptor_heap_type); }
static inline void ID3D12Device14_CopyDescriptorsSimple(ID3D12Device14 *This, UINT descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset, const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset, D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) { This->lpVtbl->CopyDescriptorsSimple(This, descriptor_count, dst_descriptor_range_offset, src_descriptor_range_offset, descriptor_heap_type); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device14_GetResourceAllocationInfo(ID3D12Device14 *This, UINT visible_mask, UINT reource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo(This, &__ret, visible_mask, reource_desc_count, resource_descs); }
static inline D3D12_HEAP_PROPERTIES ID3D12Device14_GetCustomHeapProperties(ID3D12Device14 *This, UINT node_mask, D3D12_HEAP_TYPE heap_type) { D3D12_HEAP_PROPERTIES __ret; return *This->lpVtbl->GetCustomHeapProperties(This, &__ret, node_mask, heap_type); }
static inline HRESULT ID3D12Device14_CreateCommittedResource(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateCommittedResource(This, heap_properties, heap_flags, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device14_CreateHeap(ID3D12Device14 *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device14_CreatePlacedResource(ID3D12Device14 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device14_CreateReservedResource(ID3D12Device14 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource(This, desc, initial_state, optimized_clear_value, riid, resource); }
static inline HRESULT ID3D12Device14_CreateSharedHandle(ID3D12Device14 *This, ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) { return This->lpVtbl->CreateSharedHandle(This, object, attributes, access, name, handle); }
static inline HRESULT ID3D12Device14_OpenSharedHandle(ID3D12Device14 *This, HANDLE handle, REFIID riid, void **object) { return This->lpVtbl->OpenSharedHandle(This, handle, riid, object); }
static inline HRESULT ID3D12Device14_OpenSharedHandleByName(ID3D12Device14 *This, const WCHAR *name, DWORD access, HANDLE *handle) { return This->lpVtbl->OpenSharedHandleByName(This, name, access, handle); }
static inline HRESULT ID3D12Device14_MakeResident(ID3D12Device14 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->MakeResident(This, object_count, objects); }
static inline HRESULT ID3D12Device14_Evict(ID3D12Device14 *This, UINT object_count, ID3D12Pageable *const *objects) { return This->lpVtbl->Evict(This, object_count, objects); }
static inline HRESULT ID3D12Device14_CreateFence(ID3D12Device14 *This, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **fence) { return This->lpVtbl->CreateFence(This, initial_value, flags, riid, fence); }
static inline HRESULT ID3D12Device14_GetDeviceRemovedReason(ID3D12Device14 *This) { return This->lpVtbl->GetDeviceRemovedReason(This); }
static inline void ID3D12Device14_GetCopyableFootprints(ID3D12Device14 *This, const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource, UINT sub_resource_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count, UINT64 *row_size, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints(This, desc, first_sub_resource, sub_resource_count, base_offset, layouts, row_count, row_size, total_bytes); }
static inline HRESULT ID3D12Device14_CreateQueryHeap(ID3D12Device14 *This, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **heap) { return This->lpVtbl->CreateQueryHeap(This, desc, riid, heap); }
static inline HRESULT ID3D12Device14_SetStablePowerState(ID3D12Device14 *This, WINBOOL enable) { return This->lpVtbl->SetStablePowerState(This, enable); }
static inline HRESULT ID3D12Device14_CreateCommandSignature(ID3D12Device14 *This, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root_signature, REFIID riid, void **command_signature) { return This->lpVtbl->CreateCommandSignature(This, desc, root_signature, riid, command_signature); }
static inline void ID3D12Device14_GetResourceTiling(ID3D12Device14 *This, ID3D12Resource *resource, UINT *total_tile_count, D3D12_PACKED_MIP_INFO *packed_mip_info, D3D12_TILE_SHAPE *standard_tile_shape, UINT *sub_resource_tiling_count, UINT first_sub_resource_tiling, D3D12_SUBRESOURCE_TILING *sub_resource_tilings) { This->lpVtbl->GetResourceTiling(This, resource, total_tile_count, packed_mip_info, standard_tile_shape, sub_resource_tiling_count, first_sub_resource_tiling, sub_resource_tilings); }
static inline LUID ID3D12Device14_GetAdapterLuid(ID3D12Device14 *This) { LUID __ret; return *This->lpVtbl->GetAdapterLuid(This, &__ret); }
static inline HRESULT ID3D12Device14_CreatePipelineLibrary(ID3D12Device14 *This, const void *blob, SIZE_T blob_size, REFIID iid, void **lib) { return This->lpVtbl->CreatePipelineLibrary(This, blob, blob_size, iid, lib); }
static inline HRESULT ID3D12Device14_SetEventOnMultipleFenceCompletion(ID3D12Device14 *This, ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) { return This->lpVtbl->SetEventOnMultipleFenceCompletion(This, fences, values, fence_count, flags, event); }
static inline HRESULT ID3D12Device14_SetResidencyPriority(ID3D12Device14 *This, UINT object_count, ID3D12Pageable *const *objects, const D3D12_RESIDENCY_PRIORITY *priorities) { return This->lpVtbl->SetResidencyPriority(This, object_count, objects, priorities); }
static inline HRESULT ID3D12Device14_CreatePipelineState(ID3D12Device14 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pipeline_state) { return This->lpVtbl->CreatePipelineState(This, desc, riid, pipeline_state); }
static inline HRESULT ID3D12Device14_OpenExistingHeapFromAddress(ID3D12Device14 *This, const void *address, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromAddress(This, address, riid, heap); }
static inline HRESULT ID3D12Device14_OpenExistingHeapFromFileMapping(ID3D12Device14 *This, HANDLE file_mapping, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromFileMapping(This, file_mapping, riid, heap); }
static inline HRESULT ID3D12Device14_EnqueueMakeResident(ID3D12Device14 *This, D3D12_RESIDENCY_FLAGS flags, UINT num_objects, ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 fence_value) { return This->lpVtbl->EnqueueMakeResident(This, flags, num_objects, objects, fence, fence_value); }
static inline HRESULT ID3D12Device14_CreateCommandList1(ID3D12Device14 *This, UINT node_mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **command_list) { return This->lpVtbl->CreateCommandList1(This, node_mask, type, flags, riid, command_list); }
static inline HRESULT ID3D12Device14_CreateProtectedResourceSession(ID3D12Device14 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device14_CreateCommittedResource1(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource1(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device14_CreateHeap1(ID3D12Device14 *This, const D3D12_HEAP_DESC *desc, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **heap) { return This->lpVtbl->CreateHeap1(This, desc, protected_session, riid, heap); }
static inline HRESULT ID3D12Device14_CreateReservedResource1(ID3D12Device14 *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource1(This, desc, initial_state, optimized_clear_value, protected_session, riid, resource); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device14_GetResourceAllocationInfo1(ID3D12Device14 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo1(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device14_CreateLifetimeTracker(ID3D12Device14 *This, ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) { return This->lpVtbl->CreateLifetimeTracker(This, owner, riid, tracker); }
static inline void ID3D12Device14_RemoveDevice(ID3D12Device14 *This) { This->lpVtbl->RemoveDevice(This); }
static inline HRESULT ID3D12Device14_EnumerateMetaCommands(ID3D12Device14 *This, UINT *meta_commands_count, D3D12_META_COMMAND_DESC *descs) { return This->lpVtbl->EnumerateMetaCommands(This, meta_commands_count, descs); }
static inline HRESULT ID3D12Device14_EnumerateMetaCommandParameters(ID3D12Device14 *This, REFGUID command_id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_structure_size_in_bytes, UINT *parameter_count, D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) { return This->lpVtbl->EnumerateMetaCommandParameters(This, command_id, stage, total_structure_size_in_bytes, parameter_count, parameter_descs); }
static inline HRESULT ID3D12Device14_CreateMetaCommand(ID3D12Device14 *This, REFGUID command_id, UINT node_mask, const void *creation_parameters_data, SIZE_T creation_parameters_data_size_in_bytes, REFIID riid, void **meta_command) { return This->lpVtbl->CreateMetaCommand(This, command_id, node_mask, creation_parameters_data, creation_parameters_data_size_in_bytes, riid, meta_command); }
static inline HRESULT ID3D12Device14_CreateStateObject(ID3D12Device14 *This, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) { return This->lpVtbl->CreateStateObject(This, desc, riid, state_object); }
static inline void ID3D12Device14_GetRaytracingAccelerationStructurePrebuildInfo(ID3D12Device14 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) { This->lpVtbl->GetRaytracingAccelerationStructurePrebuildInfo(This, desc, info); }
static inline D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS ID3D12Device14_CheckDriverMatchingIdentifier(ID3D12Device14 *This, D3D12_SERIALIZED_DATA_TYPE serialized_data_type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check) { return This->lpVtbl->CheckDriverMatchingIdentifier(This, serialized_data_type, identifier_to_check); }
static inline HRESULT ID3D12Device14_SetBackgroundProcessingMode(ID3D12Device14 *This, D3D12_BACKGROUND_PROCESSING_MODE mode, D3D12_MEASUREMENTS_ACTION action, HANDLE event, WINBOOL *further_measurements_desired) { return This->lpVtbl->SetBackgroundProcessingMode(This, mode, action, event, further_measurements_desired); }
static inline HRESULT ID3D12Device14_AddToStateObject(ID3D12Device14 *This, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *state_object_to_grow_from, REFIID riid, void **new_state_object) { return This->lpVtbl->AddToStateObject(This, addition, state_object_to_grow_from, riid, new_state_object); }
static inline HRESULT ID3D12Device14_CreateProtectedResourceSession1(ID3D12Device14 *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid, void **session) { return This->lpVtbl->CreateProtectedResourceSession1(This, desc, riid, session); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device14_GetResourceAllocationInfo2(ID3D12Device14 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo2(This, &__ret, visible_mask, resource_descs_count, resource_descs, resource_allocation_info1); }
static inline HRESULT ID3D12Device14_CreateCommittedResource2(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_resource_state, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource2(This, heap_properties, heap_flags, desc, initial_resource_state, optimized_clear_value, protected_session, riid_resource, resource); }
static inline HRESULT ID3D12Device14_CreatePlacedResource1(ID3D12Device14 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource1(This, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource); }
static inline void ID3D12Device14_CreateSamplerFeedbackUnorderedAccessView(ID3D12Device14 *This, ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource, D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) { This->lpVtbl->CreateSamplerFeedbackUnorderedAccessView(This, targeted_resource, feedback_resource, dst_descriptor); }
static inline void ID3D12Device14_GetCopyableFootprints1(ID3D12Device14 *This, const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource, UINT subresources_count, UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count, UINT64 *row_size_in_bytes, UINT64 *total_bytes) { This->lpVtbl->GetCopyableFootprints1(This, resource_desc, first_subresource, subresources_count, base_offset, layouts, rows_count, row_size_in_bytes, total_bytes); }
static inline HRESULT ID3D12Device14_CreateShaderCacheSession(ID3D12Device14 *This, const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid, void **session) { return This->lpVtbl->CreateShaderCacheSession(This, desc, riid, session); }
static inline HRESULT ID3D12Device14_ShaderCacheControl(ID3D12Device14 *This, D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control) { return This->lpVtbl->ShaderCacheControl(This, kinds, control); }
static inline HRESULT ID3D12Device14_CreateCommandQueue1(ID3D12Device14 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id, REFIID riid, void **command_queue) { return This->lpVtbl->CreateCommandQueue1(This, desc, creator_id, riid, command_queue); }
static inline HRESULT ID3D12Device14_CreateCommittedResource3(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid_resource, void **resource) { return This->lpVtbl->CreateCommittedResource3(This, heap_properties, heap_flags, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid_resource, resource); }
static inline HRESULT ID3D12Device14_CreatePlacedResource2(ID3D12Device14 *This, ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreatePlacedResource2(This, heap, heap_offset, desc, initial_layout, optimized_clear_value, castable_formats_count, castable_formats, riid, resource); }
static inline HRESULT ID3D12Device14_CreateReservedResource2(ID3D12Device14 *This, const D3D12_RESOURCE_DESC *desc, D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE *optimized_clear_value, ID3D12ProtectedResourceSession *protected_session, UINT32 castable_formats_count, DXGI_FORMAT *castable_formats, REFIID riid, void **resource) { return This->lpVtbl->CreateReservedResource2(This, desc, initial_layout, optimized_clear_value, protected_session, castable_formats_count, castable_formats, riid, resource); }
static inline void ID3D12Device14_CreateSampler2(ID3D12Device14 *This, const D3D12_SAMPLER_DESC2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) { This->lpVtbl->CreateSampler2(This, desc, dst); }
static inline D3D12_RESOURCE_ALLOCATION_INFO ID3D12Device14_GetResourceAllocationInfo3(ID3D12Device14 *This, UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs, const UINT32 *castable_formats_counts, const DXGI_FORMAT *const *castable_formats, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) { D3D12_RESOURCE_ALLOCATION_INFO __ret; return *This->lpVtbl->GetResourceAllocationInfo3(This, &__ret, visible_mask, resource_descs_count, resource_descs, castable_formats_counts, castable_formats, resource_allocation_info1); }
static inline HRESULT ID3D12Device14_OpenExistingHeapFromAddress1(ID3D12Device14 *This, const void *address, SIZE_T size, REFIID riid, void **heap) { return This->lpVtbl->OpenExistingHeapFromAddress1(This, address, size, riid, heap); }
static inline HRESULT ID3D12Device14_CreateRootSignatureFromSubobjectInLibrary(ID3D12Device14 *This, UINT node_mask, const void *library_blob, SIZE_T blob_length, LPCWSTR subobject_name, REFIID riid, void **root_signature) { return This->lpVtbl->CreateRootSignatureFromSubobjectInLibrary(This, node_mask, library_blob, blob_length, subobject_name, riid, root_signature); }
static inline HRESULT ID3D12GraphicsCommandList8_QueryInterface(ID3D12GraphicsCommandList8 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12GraphicsCommandList8_AddRef(ID3D12GraphicsCommandList8 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12GraphicsCommandList8_Release(ID3D12GraphicsCommandList8 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12GraphicsCommandList8_GetPrivateData(ID3D12GraphicsCommandList8 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12GraphicsCommandList8_SetPrivateData(ID3D12GraphicsCommandList8 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12GraphicsCommandList8_SetPrivateDataInterface(ID3D12GraphicsCommandList8 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12GraphicsCommandList8_SetName(ID3D12GraphicsCommandList8 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline HRESULT ID3D12GraphicsCommandList8_GetDevice(ID3D12GraphicsCommandList8 *This, REFIID riid, void **device) { return This->lpVtbl->GetDevice(This, riid, device); }
static inline D3D12_COMMAND_LIST_TYPE ID3D12GraphicsCommandList8_GetType(ID3D12GraphicsCommandList8 *This) { return This->lpVtbl->GetType(This); }
static inline HRESULT ID3D12GraphicsCommandList8_Close(ID3D12GraphicsCommandList8 *This) { return This->lpVtbl->Close(This); }
static inline HRESULT ID3D12GraphicsCommandList8_Reset(ID3D12GraphicsCommandList8 *This, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_state) { return This->lpVtbl->Reset(This, allocator, initial_state); }
static inline void ID3D12GraphicsCommandList8_ClearState(ID3D12GraphicsCommandList8 *This, ID3D12PipelineState *pipeline_state) { This->lpVtbl->ClearState(This, pipeline_state); }
static inline void ID3D12GraphicsCommandList8_DrawInstanced(ID3D12GraphicsCommandList8 *This, UINT vertex_count_per_instance, UINT instance_count, UINT start_vertex_location, UINT start_instance_location) { This->lpVtbl->DrawInstanced(This, vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location); }
static inline void ID3D12GraphicsCommandList8_DrawIndexedInstanced(ID3D12GraphicsCommandList8 *This, UINT index_count_per_instance, UINT instance_count, UINT start_vertex_location, INT base_vertex_location, UINT start_instance_location) { This->lpVtbl->DrawIndexedInstanced(This, index_count_per_instance, instance_count, start_vertex_location, base_vertex_location, start_instance_location); }
static inline void ID3D12GraphicsCommandList8_Dispatch(ID3D12GraphicsCommandList8 *This, UINT x, UINT u, UINT z) { This->lpVtbl->Dispatch(This, x, u, z); }
static inline void ID3D12GraphicsCommandList8_CopyBufferRegion(ID3D12GraphicsCommandList8 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT64 byte_count) { This->lpVtbl->CopyBufferRegion(This, dst_buffer, dst_offset, src_buffer, src_offset, byte_count); }
static inline void ID3D12GraphicsCommandList8_CopyTextureRegion(ID3D12GraphicsCommandList8 *This, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box) { This->lpVtbl->CopyTextureRegion(This, dst, dst_x, dst_y, dst_z, src, src_box); }
static inline void ID3D12GraphicsCommandList8_CopyResource(ID3D12GraphicsCommandList8 *This, ID3D12Resource *dst_resource, ID3D12Resource *src_resource) { This->lpVtbl->CopyResource(This, dst_resource, src_resource); }
static inline void ID3D12GraphicsCommandList8_CopyTiles(ID3D12GraphicsCommandList8 *This, ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate, const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset, D3D12_TILE_COPY_FLAGS flags) { This->lpVtbl->CopyTiles(This, tiled_resource, tile_region_start_coordinate, tile_region_size, buffer, buffer_offset, flags); }
static inline void ID3D12GraphicsCommandList8_ResolveSubresource(ID3D12GraphicsCommandList8 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource, ID3D12Resource *src_resource, UINT src_sub_resource, DXGI_FORMAT format) { This->lpVtbl->ResolveSubresource(This, dst_resource, dst_sub_resource, src_resource, src_sub_resource, format); }
static inline void ID3D12GraphicsCommandList8_IASetPrimitiveTopology(ID3D12GraphicsCommandList8 *This, D3D12_PRIMITIVE_TOPOLOGY primitive_topology) { This->lpVtbl->IASetPrimitiveTopology(This, primitive_topology); }
static inline void ID3D12GraphicsCommandList8_RSSetViewports(ID3D12GraphicsCommandList8 *This, UINT viewport_count, const D3D12_VIEWPORT *viewports) { This->lpVtbl->RSSetViewports(This, viewport_count, viewports); }
static inline void ID3D12GraphicsCommandList8_RSSetScissorRects(ID3D12GraphicsCommandList8 *This, UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->RSSetScissorRects(This, rect_count, rects); }
static inline void ID3D12GraphicsCommandList8_OMSetBlendFactor(ID3D12GraphicsCommandList8 *This, const FLOAT blend_factor[4]) { This->lpVtbl->OMSetBlendFactor(This, blend_factor); }
static inline void ID3D12GraphicsCommandList8_OMSetStencilRef(ID3D12GraphicsCommandList8 *This, UINT stencil_ref) { This->lpVtbl->OMSetStencilRef(This, stencil_ref); }
static inline void ID3D12GraphicsCommandList8_SetPipelineState(ID3D12GraphicsCommandList8 *This, ID3D12PipelineState *pipeline_state) { This->lpVtbl->SetPipelineState(This, pipeline_state); }
static inline void ID3D12GraphicsCommandList8_ResourceBarrier(ID3D12GraphicsCommandList8 *This, UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) { This->lpVtbl->ResourceBarrier(This, barrier_count, barriers); }
static inline void ID3D12GraphicsCommandList8_ExecuteBundle(ID3D12GraphicsCommandList8 *This, ID3D12GraphicsCommandList *command_list) { This->lpVtbl->ExecuteBundle(This, command_list); }
static inline void ID3D12GraphicsCommandList8_SetDescriptorHeaps(ID3D12GraphicsCommandList8 *This, UINT heap_count, ID3D12DescriptorHeap *const *heaps) { This->lpVtbl->SetDescriptorHeaps(This, heap_count, heaps); }
static inline void ID3D12GraphicsCommandList8_SetComputeRootSignature(ID3D12GraphicsCommandList8 *This, ID3D12RootSignature *root_signature) { This->lpVtbl->SetComputeRootSignature(This, root_signature); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRootSignature(ID3D12GraphicsCommandList8 *This, ID3D12RootSignature *root_signature) { This->lpVtbl->SetGraphicsRootSignature(This, root_signature); }
static inline void ID3D12GraphicsCommandList8_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) { This->lpVtbl->SetComputeRootDescriptorTable(This, root_parameter_index, base_descriptor); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) { This->lpVtbl->SetGraphicsRootDescriptorTable(This, root_parameter_index, base_descriptor); }
static inline void ID3D12GraphicsCommandList8_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, UINT data, UINT dst_offset) { This->lpVtbl->SetComputeRoot32BitConstant(This, root_parameter_index, data, dst_offset); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, UINT data, UINT dst_offset) { This->lpVtbl->SetGraphicsRoot32BitConstant(This, root_parameter_index, data, dst_offset); }
static inline void ID3D12GraphicsCommandList8_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset) { This->lpVtbl->SetComputeRoot32BitConstants(This, root_parameter_index, constant_count, data, dst_offset); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset) { This->lpVtbl->SetGraphicsRoot32BitConstants(This, root_parameter_index, constant_count, data, dst_offset); }
static inline void ID3D12GraphicsCommandList8_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootConstantBufferView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootConstantBufferView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList8_SetComputeRootShaderResourceView(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootShaderResourceView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRootShaderResourceView(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootShaderResourceView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList8_SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootUnorderedAccessView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList8_SetGraphicsRootUnorderedAccessView(ID3D12GraphicsCommandList8 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootUnorderedAccessView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList8_IASetIndexBuffer(ID3D12GraphicsCommandList8 *This, const D3D12_INDEX_BUFFER_VIEW *view) { This->lpVtbl->IASetIndexBuffer(This, view); }
static inline void ID3D12GraphicsCommandList8_IASetVertexBuffers(ID3D12GraphicsCommandList8 *This, UINT start_slot, UINT view_count, const D3D12_VERTEX_BUFFER_VIEW *views) { This->lpVtbl->IASetVertexBuffers(This, start_slot, view_count, views); }
static inline void ID3D12GraphicsCommandList8_SOSetTargets(ID3D12GraphicsCommandList8 *This, UINT start_slot, UINT view_count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) { This->lpVtbl->SOSetTargets(This, start_slot, view_count, views); }
static inline void ID3D12GraphicsCommandList8_OMSetRenderTargets(ID3D12GraphicsCommandList8 *This, UINT render_target_descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors, WINBOOL single_descriptor_handle, const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) { This->lpVtbl->OMSetRenderTargets(This, render_target_descriptor_count, render_target_descriptors, single_descriptor_handle, depth_stencil_descriptor); }
static inline void ID3D12GraphicsCommandList8_ClearDepthStencilView(ID3D12GraphicsCommandList8 *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearDepthStencilView(This, dsv, flags, depth, stencil, rect_count, rects); }
static inline void ID3D12GraphicsCommandList8_ClearRenderTargetView(ID3D12GraphicsCommandList8 *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearRenderTargetView(This, rtv, color, rect_count, rects); }
static inline void ID3D12GraphicsCommandList8_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList8 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const UINT values[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearUnorderedAccessViewUint(This, gpu_handle, cpu_handle, resource, values, rect_count, rects); }
static inline void ID3D12GraphicsCommandList8_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList8 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const float values[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearUnorderedAccessViewFloat(This, gpu_handle, cpu_handle, resource, values, rect_count, rects); }
static inline void ID3D12GraphicsCommandList8_DiscardResource(ID3D12GraphicsCommandList8 *This, ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) { This->lpVtbl->DiscardResource(This, resource, region); }
static inline void ID3D12GraphicsCommandList8_BeginQuery(ID3D12GraphicsCommandList8 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { This->lpVtbl->BeginQuery(This, heap, type, index); }
static inline void ID3D12GraphicsCommandList8_EndQuery(ID3D12GraphicsCommandList8 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { This->lpVtbl->EndQuery(This, heap, type, index); }
static inline void ID3D12GraphicsCommandList8_ResolveQueryData(ID3D12GraphicsCommandList8 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index, UINT query_count, ID3D12Resource *dst_buffer, UINT64 aligned_dst_buffer_offset) { This->lpVtbl->ResolveQueryData(This, heap, type, start_index, query_count, dst_buffer, aligned_dst_buffer_offset); }
static inline void ID3D12GraphicsCommandList8_SetPredication(ID3D12GraphicsCommandList8 *This, ID3D12Resource *buffer, UINT64 aligned_buffer_offset, D3D12_PREDICATION_OP operation) { This->lpVtbl->SetPredication(This, buffer, aligned_buffer_offset, operation); }
static inline void ID3D12GraphicsCommandList8_SetMarker(ID3D12GraphicsCommandList8 *This, UINT metadata, const void *data, UINT size) { This->lpVtbl->SetMarker(This, metadata, data, size); }
static inline void ID3D12GraphicsCommandList8_BeginEvent(ID3D12GraphicsCommandList8 *This, UINT metadata, const void *data, UINT size) { This->lpVtbl->BeginEvent(This, metadata, data, size); }
static inline void ID3D12GraphicsCommandList8_EndEvent(ID3D12GraphicsCommandList8 *This) { This->lpVtbl->EndEvent(This); }
static inline void ID3D12GraphicsCommandList8_ExecuteIndirect(ID3D12GraphicsCommandList8 *This, ID3D12CommandSignature *command_signature, UINT max_command_count, ID3D12Resource *arg_buffer, UINT64 arg_buffer_offset, ID3D12Resource *count_buffer, UINT64 count_buffer_offset) { This->lpVtbl->ExecuteIndirect(This, command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset); }
static inline void ID3D12GraphicsCommandList8_AtomicCopyBufferUINT(ID3D12GraphicsCommandList8 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) { This->lpVtbl->AtomicCopyBufferUINT(This, dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges); }
static inline void ID3D12GraphicsCommandList8_AtomicCopyBufferUINT64(ID3D12GraphicsCommandList8 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) { This->lpVtbl->AtomicCopyBufferUINT64(This, dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges); }
static inline void ID3D12GraphicsCommandList8_OMSetDepthBounds(ID3D12GraphicsCommandList8 *This, FLOAT min, FLOAT max) { This->lpVtbl->OMSetDepthBounds(This, min, max); }
static inline void ID3D12GraphicsCommandList8_SetSamplePositions(ID3D12GraphicsCommandList8 *This, UINT sample_count, UINT pixel_count, D3D12_SAMPLE_POSITION *sample_positions) { This->lpVtbl->SetSamplePositions(This, sample_count, pixel_count, sample_positions); }
static inline void ID3D12GraphicsCommandList8_ResolveSubresourceRegion(ID3D12GraphicsCommandList8 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource_idx, UINT dst_x, UINT dst_y, ID3D12Resource *src_resource, UINT src_sub_resource_idx, D3D12_RECT *src_rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) { This->lpVtbl->ResolveSubresourceRegion(This, dst_resource, dst_sub_resource_idx, dst_x, dst_y, src_resource, src_sub_resource_idx, src_rect, format, mode); }
static inline void ID3D12GraphicsCommandList8_SetViewInstanceMask(ID3D12GraphicsCommandList8 *This, UINT mask) { This->lpVtbl->SetViewInstanceMask(This, mask); }
static inline void ID3D12GraphicsCommandList8_WriteBufferImmediate(ID3D12GraphicsCommandList8 *This, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters, const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) { This->lpVtbl->WriteBufferImmediate(This, count, parameters, modes); }
static inline void ID3D12GraphicsCommandList8_SetProtectedResourceSession(ID3D12GraphicsCommandList8 *This, ID3D12ProtectedResourceSession *protected_resource_session) { This->lpVtbl->SetProtectedResourceSession(This, protected_resource_session); }
static inline void ID3D12GraphicsCommandList8_BeginRenderPass(ID3D12GraphicsCommandList8 *This, UINT render_targets_count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil, D3D12_RENDER_PASS_FLAGS flags) { This->lpVtbl->BeginRenderPass(This, render_targets_count, render_targets, depth_stencil, flags); }
static inline void ID3D12GraphicsCommandList8_EndRenderPass(ID3D12GraphicsCommandList8 *This) { This->lpVtbl->EndRenderPass(This); }
static inline void ID3D12GraphicsCommandList8_InitializeMetaCommand(ID3D12GraphicsCommandList8 *This, ID3D12MetaCommand *meta_command, const void *initialization_parameters_data, SIZE_T initialization_parameters_data_size_in_bytes) { This->lpVtbl->InitializeMetaCommand(This, meta_command, initialization_parameters_data, initialization_parameters_data_size_in_bytes); }
static inline void ID3D12GraphicsCommandList8_ExecuteMetaCommand(ID3D12GraphicsCommandList8 *This, ID3D12MetaCommand *meta_command, const void *execution_parameters_data, SIZE_T execution_parameters_data_size_in_bytes) { This->lpVtbl->ExecuteMetaCommand(This, meta_command, execution_parameters_data, execution_parameters_data_size_in_bytes); }
static inline void ID3D12GraphicsCommandList8_BuildRaytracingAccelerationStructure(ID3D12GraphicsCommandList8 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT postbuild_info_descs_count, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs) { This->lpVtbl->BuildRaytracingAccelerationStructure(This, desc, postbuild_info_descs_count, postbuild_info_descs); }
static inline void ID3D12GraphicsCommandList8_EmitRaytracingAccelerationStructurePostbuildInfo(ID3D12GraphicsCommandList8 *This, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT src_acceleration_structures_count, const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data) { This->lpVtbl->EmitRaytracingAccelerationStructurePostbuildInfo(This, desc, src_acceleration_structures_count, src_acceleration_structure_data); }
static inline void ID3D12GraphicsCommandList8_CopyRaytracingAccelerationStructure(ID3D12GraphicsCommandList8 *This, D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data, D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) { This->lpVtbl->CopyRaytracingAccelerationStructure(This, dst_acceleration_structure_data, src_acceleration_structure_data, mode); }
static inline void ID3D12GraphicsCommandList8_SetPipelineState1(ID3D12GraphicsCommandList8 *This, ID3D12StateObject *state_object) { This->lpVtbl->SetPipelineState1(This, state_object); }
static inline void ID3D12GraphicsCommandList8_DispatchRays(ID3D12GraphicsCommandList8 *This, const D3D12_DISPATCH_RAYS_DESC *desc) { This->lpVtbl->DispatchRays(This, desc); }
static inline void ID3D12GraphicsCommandList8_RSSetShadingRate(ID3D12GraphicsCommandList8 *This, D3D12_SHADING_RATE base_shading_rate, const D3D12_SHADING_RATE_COMBINER *combiners) { This->lpVtbl->RSSetShadingRate(This, base_shading_rate, combiners); }
static inline void ID3D12GraphicsCommandList8_RSSetShadingRateImage(ID3D12GraphicsCommandList8 *This, ID3D12Resource *shading_rate_image) { This->lpVtbl->RSSetShadingRateImage(This, shading_rate_image); }
static inline void ID3D12GraphicsCommandList8_DispatchMesh(ID3D12GraphicsCommandList8 *This, UINT thread_group_count_x, UINT thread_group_count_y, UINT thread_group_count_z) { This->lpVtbl->DispatchMesh(This, thread_group_count_x, thread_group_count_y, thread_group_count_z); }
static inline void ID3D12GraphicsCommandList8_Barrier(ID3D12GraphicsCommandList8 *This, UINT32 barrier_groups_count, const D3D12_BARRIER_GROUP *barrier_groups) { This->lpVtbl->Barrier(This, barrier_groups_count, barrier_groups); }
static inline void ID3D12GraphicsCommandList8_OMSetFrontAndBackStencilRef(ID3D12GraphicsCommandList8 *This, UINT front_stencil_ref, UINT back_stencil_ref) { This->lpVtbl->OMSetFrontAndBackStencilRef(This, front_stencil_ref, back_stencil_ref); }
static inline HRESULT ID3D12GraphicsCommandList9_QueryInterface(ID3D12GraphicsCommandList9 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12GraphicsCommandList9_AddRef(ID3D12GraphicsCommandList9 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12GraphicsCommandList9_Release(ID3D12GraphicsCommandList9 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12GraphicsCommandList9_GetPrivateData(ID3D12GraphicsCommandList9 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12GraphicsCommandList9_SetPrivateData(ID3D12GraphicsCommandList9 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12GraphicsCommandList9_SetPrivateDataInterface(ID3D12GraphicsCommandList9 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12GraphicsCommandList9_SetName(ID3D12GraphicsCommandList9 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline HRESULT ID3D12GraphicsCommandList9_GetDevice(ID3D12GraphicsCommandList9 *This, REFIID riid, void **device) { return This->lpVtbl->GetDevice(This, riid, device); }
static inline D3D12_COMMAND_LIST_TYPE ID3D12GraphicsCommandList9_GetType(ID3D12GraphicsCommandList9 *This) { return This->lpVtbl->GetType(This); }
static inline HRESULT ID3D12GraphicsCommandList9_Close(ID3D12GraphicsCommandList9 *This) { return This->lpVtbl->Close(This); }
static inline HRESULT ID3D12GraphicsCommandList9_Reset(ID3D12GraphicsCommandList9 *This, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_state) { return This->lpVtbl->Reset(This, allocator, initial_state); }
static inline void ID3D12GraphicsCommandList9_ClearState(ID3D12GraphicsCommandList9 *This, ID3D12PipelineState *pipeline_state) { This->lpVtbl->ClearState(This, pipeline_state); }
static inline void ID3D12GraphicsCommandList9_DrawInstanced(ID3D12GraphicsCommandList9 *This, UINT vertex_count_per_instance, UINT instance_count, UINT start_vertex_location, UINT start_instance_location) { This->lpVtbl->DrawInstanced(This, vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location); }
static inline void ID3D12GraphicsCommandList9_DrawIndexedInstanced(ID3D12GraphicsCommandList9 *This, UINT index_count_per_instance, UINT instance_count, UINT start_vertex_location, INT base_vertex_location, UINT start_instance_location) { This->lpVtbl->DrawIndexedInstanced(This, index_count_per_instance, instance_count, start_vertex_location, base_vertex_location, start_instance_location); }
static inline void ID3D12GraphicsCommandList9_Dispatch(ID3D12GraphicsCommandList9 *This, UINT x, UINT u, UINT z) { This->lpVtbl->Dispatch(This, x, u, z); }
static inline void ID3D12GraphicsCommandList9_CopyBufferRegion(ID3D12GraphicsCommandList9 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT64 byte_count) { This->lpVtbl->CopyBufferRegion(This, dst_buffer, dst_offset, src_buffer, src_offset, byte_count); }
static inline void ID3D12GraphicsCommandList9_CopyTextureRegion(ID3D12GraphicsCommandList9 *This, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box) { This->lpVtbl->CopyTextureRegion(This, dst, dst_x, dst_y, dst_z, src, src_box); }
static inline void ID3D12GraphicsCommandList9_CopyResource(ID3D12GraphicsCommandList9 *This, ID3D12Resource *dst_resource, ID3D12Resource *src_resource) { This->lpVtbl->CopyResource(This, dst_resource, src_resource); }
static inline void ID3D12GraphicsCommandList9_CopyTiles(ID3D12GraphicsCommandList9 *This, ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate, const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset, D3D12_TILE_COPY_FLAGS flags) { This->lpVtbl->CopyTiles(This, tiled_resource, tile_region_start_coordinate, tile_region_size, buffer, buffer_offset, flags); }
static inline void ID3D12GraphicsCommandList9_ResolveSubresource(ID3D12GraphicsCommandList9 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource, ID3D12Resource *src_resource, UINT src_sub_resource, DXGI_FORMAT format) { This->lpVtbl->ResolveSubresource(This, dst_resource, dst_sub_resource, src_resource, src_sub_resource, format); }
static inline void ID3D12GraphicsCommandList9_IASetPrimitiveTopology(ID3D12GraphicsCommandList9 *This, D3D12_PRIMITIVE_TOPOLOGY primitive_topology) { This->lpVtbl->IASetPrimitiveTopology(This, primitive_topology); }
static inline void ID3D12GraphicsCommandList9_RSSetViewports(ID3D12GraphicsCommandList9 *This, UINT viewport_count, const D3D12_VIEWPORT *viewports) { This->lpVtbl->RSSetViewports(This, viewport_count, viewports); }
static inline void ID3D12GraphicsCommandList9_RSSetScissorRects(ID3D12GraphicsCommandList9 *This, UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->RSSetScissorRects(This, rect_count, rects); }
static inline void ID3D12GraphicsCommandList9_OMSetBlendFactor(ID3D12GraphicsCommandList9 *This, const FLOAT blend_factor[4]) { This->lpVtbl->OMSetBlendFactor(This, blend_factor); }
static inline void ID3D12GraphicsCommandList9_OMSetStencilRef(ID3D12GraphicsCommandList9 *This, UINT stencil_ref) { This->lpVtbl->OMSetStencilRef(This, stencil_ref); }
static inline void ID3D12GraphicsCommandList9_SetPipelineState(ID3D12GraphicsCommandList9 *This, ID3D12PipelineState *pipeline_state) { This->lpVtbl->SetPipelineState(This, pipeline_state); }
static inline void ID3D12GraphicsCommandList9_ResourceBarrier(ID3D12GraphicsCommandList9 *This, UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) { This->lpVtbl->ResourceBarrier(This, barrier_count, barriers); }
static inline void ID3D12GraphicsCommandList9_ExecuteBundle(ID3D12GraphicsCommandList9 *This, ID3D12GraphicsCommandList *command_list) { This->lpVtbl->ExecuteBundle(This, command_list); }
static inline void ID3D12GraphicsCommandList9_SetDescriptorHeaps(ID3D12GraphicsCommandList9 *This, UINT heap_count, ID3D12DescriptorHeap *const *heaps) { This->lpVtbl->SetDescriptorHeaps(This, heap_count, heaps); }
static inline void ID3D12GraphicsCommandList9_SetComputeRootSignature(ID3D12GraphicsCommandList9 *This, ID3D12RootSignature *root_signature) { This->lpVtbl->SetComputeRootSignature(This, root_signature); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRootSignature(ID3D12GraphicsCommandList9 *This, ID3D12RootSignature *root_signature) { This->lpVtbl->SetGraphicsRootSignature(This, root_signature); }
static inline void ID3D12GraphicsCommandList9_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) { This->lpVtbl->SetComputeRootDescriptorTable(This, root_parameter_index, base_descriptor); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) { This->lpVtbl->SetGraphicsRootDescriptorTable(This, root_parameter_index, base_descriptor); }
static inline void ID3D12GraphicsCommandList9_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, UINT data, UINT dst_offset) { This->lpVtbl->SetComputeRoot32BitConstant(This, root_parameter_index, data, dst_offset); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, UINT data, UINT dst_offset) { This->lpVtbl->SetGraphicsRoot32BitConstant(This, root_parameter_index, data, dst_offset); }
static inline void ID3D12GraphicsCommandList9_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset) { This->lpVtbl->SetComputeRoot32BitConstants(This, root_parameter_index, constant_count, data, dst_offset); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset) { This->lpVtbl->SetGraphicsRoot32BitConstants(This, root_parameter_index, constant_count, data, dst_offset); }
static inline void ID3D12GraphicsCommandList9_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootConstantBufferView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootConstantBufferView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList9_SetComputeRootShaderResourceView(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootShaderResourceView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRootShaderResourceView(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootShaderResourceView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList9_SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootUnorderedAccessView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList9_SetGraphicsRootUnorderedAccessView(ID3D12GraphicsCommandList9 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootUnorderedAccessView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList9_IASetIndexBuffer(ID3D12GraphicsCommandList9 *This, const D3D12_INDEX_BUFFER_VIEW *view) { This->lpVtbl->IASetIndexBuffer(This, view); }
static inline void ID3D12GraphicsCommandList9_IASetVertexBuffers(ID3D12GraphicsCommandList9 *This, UINT start_slot, UINT view_count, const D3D12_VERTEX_BUFFER_VIEW *views) { This->lpVtbl->IASetVertexBuffers(This, start_slot, view_count, views); }
static inline void ID3D12GraphicsCommandList9_SOSetTargets(ID3D12GraphicsCommandList9 *This, UINT start_slot, UINT view_count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) { This->lpVtbl->SOSetTargets(This, start_slot, view_count, views); }
static inline void ID3D12GraphicsCommandList9_OMSetRenderTargets(ID3D12GraphicsCommandList9 *This, UINT render_target_descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors, WINBOOL single_descriptor_handle, const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) { This->lpVtbl->OMSetRenderTargets(This, render_target_descriptor_count, render_target_descriptors, single_descriptor_handle, depth_stencil_descriptor); }
static inline void ID3D12GraphicsCommandList9_ClearDepthStencilView(ID3D12GraphicsCommandList9 *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearDepthStencilView(This, dsv, flags, depth, stencil, rect_count, rects); }
static inline void ID3D12GraphicsCommandList9_ClearRenderTargetView(ID3D12GraphicsCommandList9 *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearRenderTargetView(This, rtv, color, rect_count, rects); }
static inline void ID3D12GraphicsCommandList9_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList9 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const UINT values[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearUnorderedAccessViewUint(This, gpu_handle, cpu_handle, resource, values, rect_count, rects); }
static inline void ID3D12GraphicsCommandList9_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList9 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const float values[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearUnorderedAccessViewFloat(This, gpu_handle, cpu_handle, resource, values, rect_count, rects); }
static inline void ID3D12GraphicsCommandList9_DiscardResource(ID3D12GraphicsCommandList9 *This, ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) { This->lpVtbl->DiscardResource(This, resource, region); }
static inline void ID3D12GraphicsCommandList9_BeginQuery(ID3D12GraphicsCommandList9 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { This->lpVtbl->BeginQuery(This, heap, type, index); }
static inline void ID3D12GraphicsCommandList9_EndQuery(ID3D12GraphicsCommandList9 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { This->lpVtbl->EndQuery(This, heap, type, index); }
static inline void ID3D12GraphicsCommandList9_ResolveQueryData(ID3D12GraphicsCommandList9 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index, UINT query_count, ID3D12Resource *dst_buffer, UINT64 aligned_dst_buffer_offset) { This->lpVtbl->ResolveQueryData(This, heap, type, start_index, query_count, dst_buffer, aligned_dst_buffer_offset); }
static inline void ID3D12GraphicsCommandList9_SetPredication(ID3D12GraphicsCommandList9 *This, ID3D12Resource *buffer, UINT64 aligned_buffer_offset, D3D12_PREDICATION_OP operation) { This->lpVtbl->SetPredication(This, buffer, aligned_buffer_offset, operation); }
static inline void ID3D12GraphicsCommandList9_SetMarker(ID3D12GraphicsCommandList9 *This, UINT metadata, const void *data, UINT size) { This->lpVtbl->SetMarker(This, metadata, data, size); }
static inline void ID3D12GraphicsCommandList9_BeginEvent(ID3D12GraphicsCommandList9 *This, UINT metadata, const void *data, UINT size) { This->lpVtbl->BeginEvent(This, metadata, data, size); }
static inline void ID3D12GraphicsCommandList9_EndEvent(ID3D12GraphicsCommandList9 *This) { This->lpVtbl->EndEvent(This); }
static inline void ID3D12GraphicsCommandList9_ExecuteIndirect(ID3D12GraphicsCommandList9 *This, ID3D12CommandSignature *command_signature, UINT max_command_count, ID3D12Resource *arg_buffer, UINT64 arg_buffer_offset, ID3D12Resource *count_buffer, UINT64 count_buffer_offset) { This->lpVtbl->ExecuteIndirect(This, command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset); }
static inline void ID3D12GraphicsCommandList9_AtomicCopyBufferUINT(ID3D12GraphicsCommandList9 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) { This->lpVtbl->AtomicCopyBufferUINT(This, dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges); }
static inline void ID3D12GraphicsCommandList9_AtomicCopyBufferUINT64(ID3D12GraphicsCommandList9 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) { This->lpVtbl->AtomicCopyBufferUINT64(This, dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges); }
static inline void ID3D12GraphicsCommandList9_OMSetDepthBounds(ID3D12GraphicsCommandList9 *This, FLOAT min, FLOAT max) { This->lpVtbl->OMSetDepthBounds(This, min, max); }
static inline void ID3D12GraphicsCommandList9_SetSamplePositions(ID3D12GraphicsCommandList9 *This, UINT sample_count, UINT pixel_count, D3D12_SAMPLE_POSITION *sample_positions) { This->lpVtbl->SetSamplePositions(This, sample_count, pixel_count, sample_positions); }
static inline void ID3D12GraphicsCommandList9_ResolveSubresourceRegion(ID3D12GraphicsCommandList9 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource_idx, UINT dst_x, UINT dst_y, ID3D12Resource *src_resource, UINT src_sub_resource_idx, D3D12_RECT *src_rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) { This->lpVtbl->ResolveSubresourceRegion(This, dst_resource, dst_sub_resource_idx, dst_x, dst_y, src_resource, src_sub_resource_idx, src_rect, format, mode); }
static inline void ID3D12GraphicsCommandList9_SetViewInstanceMask(ID3D12GraphicsCommandList9 *This, UINT mask) { This->lpVtbl->SetViewInstanceMask(This, mask); }
static inline void ID3D12GraphicsCommandList9_WriteBufferImmediate(ID3D12GraphicsCommandList9 *This, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters, const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) { This->lpVtbl->WriteBufferImmediate(This, count, parameters, modes); }
static inline void ID3D12GraphicsCommandList9_SetProtectedResourceSession(ID3D12GraphicsCommandList9 *This, ID3D12ProtectedResourceSession *protected_resource_session) { This->lpVtbl->SetProtectedResourceSession(This, protected_resource_session); }
static inline void ID3D12GraphicsCommandList9_BeginRenderPass(ID3D12GraphicsCommandList9 *This, UINT render_targets_count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil, D3D12_RENDER_PASS_FLAGS flags) { This->lpVtbl->BeginRenderPass(This, render_targets_count, render_targets, depth_stencil, flags); }
static inline void ID3D12GraphicsCommandList9_EndRenderPass(ID3D12GraphicsCommandList9 *This) { This->lpVtbl->EndRenderPass(This); }
static inline void ID3D12GraphicsCommandList9_InitializeMetaCommand(ID3D12GraphicsCommandList9 *This, ID3D12MetaCommand *meta_command, const void *initialization_parameters_data, SIZE_T initialization_parameters_data_size_in_bytes) { This->lpVtbl->InitializeMetaCommand(This, meta_command, initialization_parameters_data, initialization_parameters_data_size_in_bytes); }
static inline void ID3D12GraphicsCommandList9_ExecuteMetaCommand(ID3D12GraphicsCommandList9 *This, ID3D12MetaCommand *meta_command, const void *execution_parameters_data, SIZE_T execution_parameters_data_size_in_bytes) { This->lpVtbl->ExecuteMetaCommand(This, meta_command, execution_parameters_data, execution_parameters_data_size_in_bytes); }
static inline void ID3D12GraphicsCommandList9_BuildRaytracingAccelerationStructure(ID3D12GraphicsCommandList9 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT postbuild_info_descs_count, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs) { This->lpVtbl->BuildRaytracingAccelerationStructure(This, desc, postbuild_info_descs_count, postbuild_info_descs); }
static inline void ID3D12GraphicsCommandList9_EmitRaytracingAccelerationStructurePostbuildInfo(ID3D12GraphicsCommandList9 *This, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT src_acceleration_structures_count, const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data) { This->lpVtbl->EmitRaytracingAccelerationStructurePostbuildInfo(This, desc, src_acceleration_structures_count, src_acceleration_structure_data); }
static inline void ID3D12GraphicsCommandList9_CopyRaytracingAccelerationStructure(ID3D12GraphicsCommandList9 *This, D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data, D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) { This->lpVtbl->CopyRaytracingAccelerationStructure(This, dst_acceleration_structure_data, src_acceleration_structure_data, mode); }
static inline void ID3D12GraphicsCommandList9_SetPipelineState1(ID3D12GraphicsCommandList9 *This, ID3D12StateObject *state_object) { This->lpVtbl->SetPipelineState1(This, state_object); }
static inline void ID3D12GraphicsCommandList9_DispatchRays(ID3D12GraphicsCommandList9 *This, const D3D12_DISPATCH_RAYS_DESC *desc) { This->lpVtbl->DispatchRays(This, desc); }
static inline void ID3D12GraphicsCommandList9_RSSetShadingRate(ID3D12GraphicsCommandList9 *This, D3D12_SHADING_RATE base_shading_rate, const D3D12_SHADING_RATE_COMBINER *combiners) { This->lpVtbl->RSSetShadingRate(This, base_shading_rate, combiners); }
static inline void ID3D12GraphicsCommandList9_RSSetShadingRateImage(ID3D12GraphicsCommandList9 *This, ID3D12Resource *shading_rate_image) { This->lpVtbl->RSSetShadingRateImage(This, shading_rate_image); }
static inline void ID3D12GraphicsCommandList9_DispatchMesh(ID3D12GraphicsCommandList9 *This, UINT thread_group_count_x, UINT thread_group_count_y, UINT thread_group_count_z) { This->lpVtbl->DispatchMesh(This, thread_group_count_x, thread_group_count_y, thread_group_count_z); }
static inline void ID3D12GraphicsCommandList9_Barrier(ID3D12GraphicsCommandList9 *This, UINT32 barrier_groups_count, const D3D12_BARRIER_GROUP *barrier_groups) { This->lpVtbl->Barrier(This, barrier_groups_count, barrier_groups); }
static inline void ID3D12GraphicsCommandList9_OMSetFrontAndBackStencilRef(ID3D12GraphicsCommandList9 *This, UINT front_stencil_ref, UINT back_stencil_ref) { This->lpVtbl->OMSetFrontAndBackStencilRef(This, front_stencil_ref, back_stencil_ref); }
static inline void ID3D12GraphicsCommandList9_RSSetDepthBias(ID3D12GraphicsCommandList9 *This, FLOAT depth_bias, FLOAT depth_bias_clamp, FLOAT slope_scaled_depth_bias) { This->lpVtbl->RSSetDepthBias(This, depth_bias, depth_bias_clamp, slope_scaled_depth_bias); }
static inline void ID3D12GraphicsCommandList9_IASetIndexBufferStripCutValue(ID3D12GraphicsCommandList9 *This, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value) { This->lpVtbl->IASetIndexBufferStripCutValue(This, strip_cut_value); }
static inline HRESULT ID3D12GraphicsCommandList10_QueryInterface(ID3D12GraphicsCommandList10 *This, REFIID riid, void **ppvObject) { return This->lpVtbl->QueryInterface(This, riid, ppvObject); }
static inline ULONG ID3D12GraphicsCommandList10_AddRef(ID3D12GraphicsCommandList10 *This) { return This->lpVtbl->AddRef(This); }
static inline ULONG ID3D12GraphicsCommandList10_Release(ID3D12GraphicsCommandList10 *This) { return This->lpVtbl->Release(This); }
static inline HRESULT ID3D12GraphicsCommandList10_GetPrivateData(ID3D12GraphicsCommandList10 *This, REFGUID guid, UINT *data_size, void *data) { return This->lpVtbl->GetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12GraphicsCommandList10_SetPrivateData(ID3D12GraphicsCommandList10 *This, REFGUID guid, UINT data_size, const void *data) { return This->lpVtbl->SetPrivateData(This, guid, data_size, data); }
static inline HRESULT ID3D12GraphicsCommandList10_SetPrivateDataInterface(ID3D12GraphicsCommandList10 *This, REFGUID guid, const IUnknown *data) { return This->lpVtbl->SetPrivateDataInterface(This, guid, data); }
static inline HRESULT ID3D12GraphicsCommandList10_SetName(ID3D12GraphicsCommandList10 *This, const WCHAR *name) { return This->lpVtbl->SetName(This, name); }
static inline HRESULT ID3D12GraphicsCommandList10_GetDevice(ID3D12GraphicsCommandList10 *This, REFIID riid, void **device) { return This->lpVtbl->GetDevice(This, riid, device); }
static inline D3D12_COMMAND_LIST_TYPE ID3D12GraphicsCommandList10_GetType(ID3D12GraphicsCommandList10 *This) { return This->lpVtbl->GetType(This); }
static inline HRESULT ID3D12GraphicsCommandList10_Close(ID3D12GraphicsCommandList10 *This) { return This->lpVtbl->Close(This); }
static inline HRESULT ID3D12GraphicsCommandList10_Reset(ID3D12GraphicsCommandList10 *This, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_state) { return This->lpVtbl->Reset(This, allocator, initial_state); }
static inline void ID3D12GraphicsCommandList10_ClearState(ID3D12GraphicsCommandList10 *This, ID3D12PipelineState *pipeline_state) { This->lpVtbl->ClearState(This, pipeline_state); }
static inline void ID3D12GraphicsCommandList10_DrawInstanced(ID3D12GraphicsCommandList10 *This, UINT vertex_count_per_instance, UINT instance_count, UINT start_vertex_location, UINT start_instance_location) { This->lpVtbl->DrawInstanced(This, vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location); }
static inline void ID3D12GraphicsCommandList10_DrawIndexedInstanced(ID3D12GraphicsCommandList10 *This, UINT index_count_per_instance, UINT instance_count, UINT start_vertex_location, INT base_vertex_location, UINT start_instance_location) { This->lpVtbl->DrawIndexedInstanced(This, index_count_per_instance, instance_count, start_vertex_location, base_vertex_location, start_instance_location); }
static inline void ID3D12GraphicsCommandList10_Dispatch(ID3D12GraphicsCommandList10 *This, UINT x, UINT u, UINT z) { This->lpVtbl->Dispatch(This, x, u, z); }
static inline void ID3D12GraphicsCommandList10_CopyBufferRegion(ID3D12GraphicsCommandList10 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT64 byte_count) { This->lpVtbl->CopyBufferRegion(This, dst_buffer, dst_offset, src_buffer, src_offset, byte_count); }
static inline void ID3D12GraphicsCommandList10_CopyTextureRegion(ID3D12GraphicsCommandList10 *This, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box) { This->lpVtbl->CopyTextureRegion(This, dst, dst_x, dst_y, dst_z, src, src_box); }
static inline void ID3D12GraphicsCommandList10_CopyResource(ID3D12GraphicsCommandList10 *This, ID3D12Resource *dst_resource, ID3D12Resource *src_resource) { This->lpVtbl->CopyResource(This, dst_resource, src_resource); }
static inline void ID3D12GraphicsCommandList10_CopyTiles(ID3D12GraphicsCommandList10 *This, ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate, const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset, D3D12_TILE_COPY_FLAGS flags) { This->lpVtbl->CopyTiles(This, tiled_resource, tile_region_start_coordinate, tile_region_size, buffer, buffer_offset, flags); }
static inline void ID3D12GraphicsCommandList10_ResolveSubresource(ID3D12GraphicsCommandList10 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource, ID3D12Resource *src_resource, UINT src_sub_resource, DXGI_FORMAT format) { This->lpVtbl->ResolveSubresource(This, dst_resource, dst_sub_resource, src_resource, src_sub_resource, format); }
static inline void ID3D12GraphicsCommandList10_IASetPrimitiveTopology(ID3D12GraphicsCommandList10 *This, D3D12_PRIMITIVE_TOPOLOGY primitive_topology) { This->lpVtbl->IASetPrimitiveTopology(This, primitive_topology); }
static inline void ID3D12GraphicsCommandList10_RSSetViewports(ID3D12GraphicsCommandList10 *This, UINT viewport_count, const D3D12_VIEWPORT *viewports) { This->lpVtbl->RSSetViewports(This, viewport_count, viewports); }
static inline void ID3D12GraphicsCommandList10_RSSetScissorRects(ID3D12GraphicsCommandList10 *This, UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->RSSetScissorRects(This, rect_count, rects); }
static inline void ID3D12GraphicsCommandList10_OMSetBlendFactor(ID3D12GraphicsCommandList10 *This, const FLOAT blend_factor[4]) { This->lpVtbl->OMSetBlendFactor(This, blend_factor); }
static inline void ID3D12GraphicsCommandList10_OMSetStencilRef(ID3D12GraphicsCommandList10 *This, UINT stencil_ref) { This->lpVtbl->OMSetStencilRef(This, stencil_ref); }
static inline void ID3D12GraphicsCommandList10_SetPipelineState(ID3D12GraphicsCommandList10 *This, ID3D12PipelineState *pipeline_state) { This->lpVtbl->SetPipelineState(This, pipeline_state); }
static inline void ID3D12GraphicsCommandList10_ResourceBarrier(ID3D12GraphicsCommandList10 *This, UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) { This->lpVtbl->ResourceBarrier(This, barrier_count, barriers); }
static inline void ID3D12GraphicsCommandList10_ExecuteBundle(ID3D12GraphicsCommandList10 *This, ID3D12GraphicsCommandList *command_list) { This->lpVtbl->ExecuteBundle(This, command_list); }
static inline void ID3D12GraphicsCommandList10_SetDescriptorHeaps(ID3D12GraphicsCommandList10 *This, UINT heap_count, ID3D12DescriptorHeap *const *heaps) { This->lpVtbl->SetDescriptorHeaps(This, heap_count, heaps); }
static inline void ID3D12GraphicsCommandList10_SetComputeRootSignature(ID3D12GraphicsCommandList10 *This, ID3D12RootSignature *root_signature) { This->lpVtbl->SetComputeRootSignature(This, root_signature); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRootSignature(ID3D12GraphicsCommandList10 *This, ID3D12RootSignature *root_signature) { This->lpVtbl->SetGraphicsRootSignature(This, root_signature); }
static inline void ID3D12GraphicsCommandList10_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) { This->lpVtbl->SetComputeRootDescriptorTable(This, root_parameter_index, base_descriptor); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) { This->lpVtbl->SetGraphicsRootDescriptorTable(This, root_parameter_index, base_descriptor); }
static inline void ID3D12GraphicsCommandList10_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, UINT data, UINT dst_offset) { This->lpVtbl->SetComputeRoot32BitConstant(This, root_parameter_index, data, dst_offset); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, UINT data, UINT dst_offset) { This->lpVtbl->SetGraphicsRoot32BitConstant(This, root_parameter_index, data, dst_offset); }
static inline void ID3D12GraphicsCommandList10_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset) { This->lpVtbl->SetComputeRoot32BitConstants(This, root_parameter_index, constant_count, data, dst_offset); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset) { This->lpVtbl->SetGraphicsRoot32BitConstants(This, root_parameter_index, constant_count, data, dst_offset); }
static inline void ID3D12GraphicsCommandList10_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootConstantBufferView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootConstantBufferView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList10_SetComputeRootShaderResourceView(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootShaderResourceView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRootShaderResourceView(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootShaderResourceView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList10_SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetComputeRootUnorderedAccessView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList10_SetGraphicsRootUnorderedAccessView(ID3D12GraphicsCommandList10 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) { This->lpVtbl->SetGraphicsRootUnorderedAccessView(This, root_parameter_index, address); }
static inline void ID3D12GraphicsCommandList10_IASetIndexBuffer(ID3D12GraphicsCommandList10 *This, const D3D12_INDEX_BUFFER_VIEW *view) { This->lpVtbl->IASetIndexBuffer(This, view); }
static inline void ID3D12GraphicsCommandList10_IASetVertexBuffers(ID3D12GraphicsCommandList10 *This, UINT start_slot, UINT view_count, const D3D12_VERTEX_BUFFER_VIEW *views) { This->lpVtbl->IASetVertexBuffers(This, start_slot, view_count, views); }
static inline void ID3D12GraphicsCommandList10_SOSetTargets(ID3D12GraphicsCommandList10 *This, UINT start_slot, UINT view_count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) { This->lpVtbl->SOSetTargets(This, start_slot, view_count, views); }
static inline void ID3D12GraphicsCommandList10_OMSetRenderTargets(ID3D12GraphicsCommandList10 *This, UINT render_target_descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors, WINBOOL single_descriptor_handle, const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) { This->lpVtbl->OMSetRenderTargets(This, render_target_descriptor_count, render_target_descriptors, single_descriptor_handle, depth_stencil_descriptor); }
static inline void ID3D12GraphicsCommandList10_ClearDepthStencilView(ID3D12GraphicsCommandList10 *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearDepthStencilView(This, dsv, flags, depth, stencil, rect_count, rects); }
static inline void ID3D12GraphicsCommandList10_ClearRenderTargetView(ID3D12GraphicsCommandList10 *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearRenderTargetView(This, rtv, color, rect_count, rects); }
static inline void ID3D12GraphicsCommandList10_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList10 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const UINT values[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearUnorderedAccessViewUint(This, gpu_handle, cpu_handle, resource, values, rect_count, rects); }
static inline void ID3D12GraphicsCommandList10_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList10 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const float values[4], UINT rect_count, const D3D12_RECT *rects) { This->lpVtbl->ClearUnorderedAccessViewFloat(This, gpu_handle, cpu_handle, resource, values, rect_count, rects); }
static inline void ID3D12GraphicsCommandList10_DiscardResource(ID3D12GraphicsCommandList10 *This, ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) { This->lpVtbl->DiscardResource(This, resource, region); }
static inline void ID3D12GraphicsCommandList10_BeginQuery(ID3D12GraphicsCommandList10 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { This->lpVtbl->BeginQuery(This, heap, type, index); }
static inline void ID3D12GraphicsCommandList10_EndQuery(ID3D12GraphicsCommandList10 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { This->lpVtbl->EndQuery(This, heap, type, index); }
static inline void ID3D12GraphicsCommandList10_ResolveQueryData(ID3D12GraphicsCommandList10 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index, UINT query_count, ID3D12Resource *dst_buffer, UINT64 aligned_dst_buffer_offset) { This->lpVtbl->ResolveQueryData(This, heap, type, start_index, query_count, dst_buffer, aligned_dst_buffer_offset); }
static inline void ID3D12GraphicsCommandList10_SetPredication(ID3D12GraphicsCommandList10 *This, ID3D12Resource *buffer, UINT64 aligned_buffer_offset, D3D12_PREDICATION_OP operation) { This->lpVtbl->SetPredication(This, buffer, aligned_buffer_offset, operation); }
static inline void ID3D12GraphicsCommandList10_SetMarker(ID3D12GraphicsCommandList10 *This, UINT metadata, const void *data, UINT size) { This->lpVtbl->SetMarker(This, metadata, data, size); }
static inline void ID3D12GraphicsCommandList10_BeginEvent(ID3D12GraphicsCommandList10 *This, UINT metadata, const void *data, UINT size) { This->lpVtbl->BeginEvent(This, metadata, data, size); }
static inline void ID3D12GraphicsCommandList10_EndEvent(ID3D12GraphicsCommandList10 *This) { This->lpVtbl->EndEvent(This); }
static inline void ID3D12GraphicsCommandList10_ExecuteIndirect(ID3D12GraphicsCommandList10 *This, ID3D12CommandSignature *command_signature, UINT max_command_count, ID3D12Resource *arg_buffer, UINT64 arg_buffer_offset, ID3D12Resource *count_buffer, UINT64 count_buffer_offset) { This->lpVtbl->ExecuteIndirect(This, command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset); }
static inline void ID3D12GraphicsCommandList10_AtomicCopyBufferUINT(ID3D12GraphicsCommandList10 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) { This->lpVtbl->AtomicCopyBufferUINT(This, dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges); }
static inline void ID3D12GraphicsCommandList10_AtomicCopyBufferUINT64(ID3D12GraphicsCommandList10 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) { This->lpVtbl->AtomicCopyBufferUINT64(This, dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges); }
static inline void ID3D12GraphicsCommandList10_OMSetDepthBounds(ID3D12GraphicsCommandList10 *This, FLOAT min, FLOAT max) { This->lpVtbl->OMSetDepthBounds(This, min, max); }
static inline void ID3D12GraphicsCommandList10_SetSamplePositions(ID3D12GraphicsCommandList10 *This, UINT sample_count, UINT pixel_count, D3D12_SAMPLE_POSITION *sample_positions) { This->lpVtbl->SetSamplePositions(This, sample_count, pixel_count, sample_positions); }
static inline void ID3D12GraphicsCommandList10_ResolveSubresourceRegion(ID3D12GraphicsCommandList10 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource_idx, UINT dst_x, UINT dst_y, ID3D12Resource *src_resource, UINT src_sub_resource_idx, D3D12_RECT *src_rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) { This->lpVtbl->ResolveSubresourceRegion(This, dst_resource, dst_sub_resource_idx, dst_x, dst_y, src_resource, src_sub_resource_idx, src_rect, format, mode); }
static inline void ID3D12GraphicsCommandList10_SetViewInstanceMask(ID3D12GraphicsCommandList10 *This, UINT mask) { This->lpVtbl->SetViewInstanceMask(This, mask); }
static inline void ID3D12GraphicsCommandList10_WriteBufferImmediate(ID3D12GraphicsCommandList10 *This, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters, const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) { This->lpVtbl->WriteBufferImmediate(This, count, parameters, modes); }
static inline void ID3D12GraphicsCommandList10_SetProtectedResourceSession(ID3D12GraphicsCommandList10 *This, ID3D12ProtectedResourceSession *protected_resource_session) { This->lpVtbl->SetProtectedResourceSession(This, protected_resource_session); }
static inline void ID3D12GraphicsCommandList10_BeginRenderPass(ID3D12GraphicsCommandList10 *This, UINT render_targets_count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil, D3D12_RENDER_PASS_FLAGS flags) { This->lpVtbl->BeginRenderPass(This, render_targets_count, render_targets, depth_stencil, flags); }
static inline void ID3D12GraphicsCommandList10_EndRenderPass(ID3D12GraphicsCommandList10 *This) { This->lpVtbl->EndRenderPass(This); }
static inline void ID3D12GraphicsCommandList10_InitializeMetaCommand(ID3D12GraphicsCommandList10 *This, ID3D12MetaCommand *meta_command, const void *initialization_parameters_data, SIZE_T initialization_parameters_data_size_in_bytes) { This->lpVtbl->InitializeMetaCommand(This, meta_command, initialization_parameters_data, initialization_parameters_data_size_in_bytes); }
static inline void ID3D12GraphicsCommandList10_ExecuteMetaCommand(ID3D12GraphicsCommandList10 *This, ID3D12MetaCommand *meta_command, const void *execution_parameters_data, SIZE_T execution_parameters_data_size_in_bytes) { This->lpVtbl->ExecuteMetaCommand(This, meta_command, execution_parameters_data, execution_parameters_data_size_in_bytes); }
static inline void ID3D12GraphicsCommandList10_BuildRaytracingAccelerationStructure(ID3D12GraphicsCommandList10 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT postbuild_info_descs_count, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs) { This->lpVtbl->BuildRaytracingAccelerationStructure(This, desc, postbuild_info_descs_count, postbuild_info_descs); }
static inline void ID3D12GraphicsCommandList10_EmitRaytracingAccelerationStructurePostbuildInfo(ID3D12GraphicsCommandList10 *This, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT src_acceleration_structures_count, const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data) { This->lpVtbl->EmitRaytracingAccelerationStructurePostbuildInfo(This, desc, src_acceleration_structures_count, src_acceleration_structure_data); }
static inline void ID3D12GraphicsCommandList10_CopyRaytracingAccelerationStructure(ID3D12GraphicsCommandList10 *This, D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data, D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) { This->lpVtbl->CopyRaytracingAccelerationStructure(This, dst_acceleration_structure_data, src_acceleration_structure_data, mode); }
static inline void ID3D12GraphicsCommandList10_SetPipelineState1(ID3D12GraphicsCommandList10 *This, ID3D12StateObject *state_object) { This->lpVtbl->SetPipelineState1(This, state_object); }
static inline void ID3D12GraphicsCommandList10_DispatchRays(ID3D12GraphicsCommandList10 *This, const D3D12_DISPATCH_RAYS_DESC *desc) { This->lpVtbl->DispatchRays(This, desc); }
static inline void ID3D12GraphicsCommandList10_RSSetShadingRate(ID3D12GraphicsCommandList10 *This, D3D12_SHADING_RATE base_shading_rate, const D3D12_SHADING_RATE_COMBINER *combiners) { This->lpVtbl->RSSetShadingRate(This, base_shading_rate, combiners); }
static inline void ID3D12GraphicsCommandList10_RSSetShadingRateImage(ID3D12GraphicsCommandList10 *This, ID3D12Resource *shading_rate_image) { This->lpVtbl->RSSetShadingRateImage(This, shading_rate_image); }
static inline void ID3D12GraphicsCommandList10_DispatchMesh(ID3D12GraphicsCommandList10 *This, UINT thread_group_count_x, UINT thread_group_count_y, UINT thread_group_count_z) { This->lpVtbl->DispatchMesh(This, thread_group_count_x, thread_group_count_y, thread_group_count_z); }
static inline void ID3D12GraphicsCommandList10_Barrier(ID3D12GraphicsCommandList10 *This, UINT32 barrier_groups_count, const D3D12_BARRIER_GROUP *barrier_groups) { This->lpVtbl->Barrier(This, barrier_groups_count, barrier_groups); }
static inline void ID3D12GraphicsCommandList10_OMSetFrontAndBackStencilRef(ID3D12GraphicsCommandList10 *This, UINT front_stencil_ref, UINT back_stencil_ref) { This->lpVtbl->OMSetFrontAndBackStencilRef(This, front_stencil_ref, back_stencil_ref); }
static inline void ID3D12GraphicsCommandList10_RSSetDepthBias(ID3D12GraphicsCommandList10 *This, FLOAT depth_bias, FLOAT depth_bias_clamp, FLOAT slope_scaled_depth_bias) { This->lpVtbl->RSSetDepthBias(This, depth_bias, depth_bias_clamp, slope_scaled_depth_bias); }
static inline void ID3D12GraphicsCommandList10_IASetIndexBufferStripCutValue(ID3D12GraphicsCommandList10 *This, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value) { This->lpVtbl->IASetIndexBufferStripCutValue(This, strip_cut_value); }
static inline void ID3D12GraphicsCommandList10_SetProgram(ID3D12GraphicsCommandList10 *This, const D3D12_SET_PROGRAM_DESC *desc) { This->lpVtbl->SetProgram(This, desc); }
static inline void ID3D12GraphicsCommandList10_DispatchGraph(ID3D12GraphicsCommandList10 *This, const D3D12_DISPATCH_GRAPH_DESC *desc) { This->lpVtbl->DispatchGraph(This, desc); }
#endif
